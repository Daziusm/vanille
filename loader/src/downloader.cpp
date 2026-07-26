#include "downloader.h"

#include <shlwapi.h>
#include <winhttp.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")

namespace
{
    std::wstring trim(const std::wstring& value)
    {
        size_t begin = 0;
        while (begin < value.size() && iswspace(value[begin]))
            ++begin;

        size_t end = value.size();
        while (end > begin && iswspace(value[end - 1]))
            --end;

        return value.substr(begin, end - begin);
    }

    bool run_hidden_tar_extract(const std::wstring& zip_path, const std::wstring& destination)
    {
        wchar_t command_line[1024]{};
        swprintf_s(command_line, L"tar.exe -xf \"%s\" -C \"%s\"", zip_path.c_str(), destination.c_str());

        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);
        startup_info.dwFlags = STARTF_USESHOWWINDOW;
        startup_info.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION process_info{};
        if (!CreateProcessW(nullptr, command_line, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info))
            return false;

        WaitForSingleObject(process_info.hProcess, INFINITE);

        DWORD exit_code = 1;
        GetExitCodeProcess(process_info.hProcess, &exit_code);

        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return exit_code == 0;
    }
}

namespace downloader
{
    bool parse_github_url(const std::wstring& url, github_target& out)
    {
        std::wstring normalized = trim(url);
        const std::wstring prefix = L"https://github.com/";
        if (normalized.rfind(prefix, 0) != 0)
            return false;

        normalized = normalized.substr(prefix.size());
        while (!normalized.empty() && normalized.back() == L'/')
            normalized.pop_back();

        const size_t slash = normalized.find(L'/');
        if (slash == std::wstring::npos)
            return false;

        out.owner = normalized.substr(0, slash);
        std::wstring repo = normalized.substr(slash + 1);
        const size_t extra = repo.find(L'/');
        if (extra != std::wstring::npos)
            repo = repo.substr(0, extra);

        if (out.owner.empty() || repo.empty())
            return false;

        out.repo = repo;
        if (out.branch.empty())
            out.branch = L"main";
        return true;
    }

    bool download_repository_zip(const github_target& target, const std::wstring& zip_path, std::wstring& out_error)
    {
        const std::wstring host = L"github.com";
        const std::wstring path = L"/" + target.owner + L"/" + target.repo + L"/archive/refs/heads/" + target.branch + L".zip";

        HINTERNET session = WinHttpOpen(
            L"ChocolaLoader/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!session)
        {
            out_error = L"WinHttpOpen failed.";
            return false;
        }

        HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection)
        {
            WinHttpCloseHandle(session);
            out_error = L"WinHttpConnect failed.";
            return false;
        }

        HINTERNET request = WinHttpOpenRequest(
            connection,
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (!request)
        {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            out_error = L"WinHttpOpenRequest failed.";
            return false;
        }

        const BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (!sent || !WinHttpReceiveResponse(request, nullptr))
        {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            out_error = L"GitHub download request failed.";
            return false;
        }

        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_size,
            WINHTTP_NO_HEADER_INDEX);

        if (status_code != 200)
        {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            out_error = L"GitHub returned HTTP " + std::to_wstring(status_code) + L". Check repo URL and branch.";
            return false;
        }

        std::ofstream output(zip_path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            out_error = L"Could not create download file.";
            return false;
        }

        std::vector<char> buffer(64 * 1024);
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available))
                break;
            if (available == 0)
                break;

            if (buffer.size() < available)
                buffer.resize(available);

            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), available, &read) || read == 0)
                break;

            output.write(buffer.data(), static_cast<std::streamsize>(read));
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return true;
    }

    bool extract_zip_to_directory(const std::wstring& zip_path, const std::wstring& destination, std::wstring& out_error)
    {
        std::error_code ec;
        std::filesystem::create_directories(destination, ec);
        if (!run_hidden_tar_extract(zip_path, destination))
        {
            out_error = L"Failed to extract downloaded archive.";
            return false;
        }
        return true;
    }

    bool install_sources(
        const std::wstring& repo_url,
        const std::wstring& branch,
        const std::wstring& destination,
        std::wstring& out_error)
    {
        github_target target;
        target.branch = branch.empty() ? L"main" : branch;
        if (!parse_github_url(repo_url, target))
        {
            out_error = L"Invalid GitHub URL. Use https://github.com/owner/repo";
            return false;
        }

        wchar_t temp_dir[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temp_dir))
        {
            out_error = L"Could not access temp directory.";
            return false;
        }

        wchar_t zip_path[MAX_PATH]{};
        PathCombineW(zip_path, temp_dir, L"chocola_source.zip");

        if (!download_repository_zip(target, zip_path, out_error))
            return false;

        const std::wstring staging = destination + L"\\.download_staging";
        std::error_code ec;
        std::filesystem::remove_all(staging, ec);
        std::filesystem::create_directories(staging, ec);

        if (!extract_zip_to_directory(zip_path, staging, out_error))
        {
            DeleteFileW(zip_path);
            return false;
        }

        DeleteFileW(zip_path);

        std::filesystem::path extracted_root;
        bool found_root = false;
        for (const auto& entry : std::filesystem::directory_iterator(staging, ec))
        {
            if (!entry.is_directory())
                continue;
            extracted_root = entry.path();
            found_root = true;
            break;
        }

        if (!found_root)
        {
            out_error = L"Downloaded archive did not contain a source folder.";
            std::filesystem::remove_all(staging, ec);
            return false;
        }

        std::filesystem::create_directories(destination, ec);
        for (const auto& entry : std::filesystem::directory_iterator(extracted_root, ec))
        {
            const auto target_path = std::filesystem::path(destination) / entry.path().filename();
            std::filesystem::remove_all(target_path, ec);
            std::filesystem::copy(
                entry.path(),
                target_path,
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                ec);
        }

        std::filesystem::remove_all(staging, ec);
        return true;
    }
}
