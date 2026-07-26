#include "payload.h"

#include "payload_version.h"
#include "resource.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <functional>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace
{
    std::wstring local_appdata_dir()
    {
        wchar_t path[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path)))
            return {};
        return path;
    }

    std::wstring runtime_dir()
    {
        const std::wstring app_data = local_appdata_dir();
        if (app_data.empty())
            return {};

        wchar_t combined[MAX_PATH]{};
        PathCombineW(combined, app_data.c_str(), L"Chocola");
        return combined;
    }

    bool read_file_text(const std::wstring& path, std::string& out)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    }

    bool write_file_text(const std::wstring& path, const std::string& data)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        return static_cast<bool>(file);
    }

    bool write_file_bytes(const std::wstring& path, const std::vector<std::uint8_t>& data)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return static_cast<bool>(file);
    }

    bool load_embedded_zip(std::vector<std::uint8_t>& out)
    {
        const HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PAYLOAD_ZIP), RT_RCDATA);
        if (!resource)
            return false;

        const HGLOBAL loaded = LoadResource(nullptr, resource);
        if (!loaded)
            return false;

        const void* data = LockResource(loaded);
        const DWORD size = SizeofResource(nullptr, resource);
        if (!data || size == 0)
            return false;

        const auto* bytes = static_cast<const std::uint8_t*>(data);
        out.assign(bytes, bytes + size);
        return true;
    }

    bool run_hidden_process(const wchar_t* application, wchar_t* command_line)
    {
        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);
        startup_info.dwFlags = STARTF_USESHOWWINDOW;
        startup_info.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION process_info{};
        if (!CreateProcessW(
                application,
                command_line,
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup_info,
                &process_info))
        {
            return false;
        }

        WaitForSingleObject(process_info.hProcess, INFINITE);

        DWORD exit_code = 1;
        GetExitCodeProcess(process_info.hProcess, &exit_code);

        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return exit_code == 0;
    }

    bool extract_zip_to_directory(const std::vector<std::uint8_t>& zip_bytes, const std::wstring& dest_dir)
    {
        wchar_t temp_dir[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temp_dir))
            return false;

        wchar_t zip_path[MAX_PATH]{};
        PathCombineW(zip_path, temp_dir, L"chocola_payload.zip");
        if (!write_file_bytes(zip_path, zip_bytes))
            return false;

        std::error_code ec;
        std::filesystem::remove_all(dest_dir, ec);
        std::filesystem::create_directories(dest_dir, ec);

        wchar_t command_line[1024]{};
        swprintf_s(
            command_line,
            L"tar.exe -xf \"%s\" -C \"%s\"",
            zip_path,
            dest_dir.c_str());

        const bool ok = run_hidden_process(nullptr, command_line);
        DeleteFileW(zip_path);
        return ok;
    }

    bool payload_is_current(const std::wstring& install_dir)
    {
        wchar_t marker_path[MAX_PATH]{};
        PathCombineW(marker_path, install_dir.c_str(), L".payload_version");

        wchar_t vanille_path[MAX_PATH]{};
        PathCombineW(vanille_path, install_dir.c_str(), L"vanille.exe");
        if (!PathFileExistsW(vanille_path))
            return false;

        std::string on_disk;
        if (!read_file_text(marker_path, on_disk))
            return false;

        return on_disk == CHOCOLA_PAYLOAD_SHA256;
    }
}

namespace payload
{
    namespace
    {
        std::function<void(const std::wstring&)> g_status_sink;

        void report_status(const std::wstring& message)
        {
            if (g_status_sink)
                g_status_sink(message);
        }

        void show_error(const wchar_t* message)
        {
            report_status(message);
            MessageBoxW(nullptr, message, L"Chocola", MB_OK | MB_ICONERROR);
        }
    }

    void set_status_sink(std::function<void(const std::wstring&)> sink)
    {
        g_status_sink = std::move(sink);
    }

    bool ensure_installed(std::wstring& out_runtime_dir, const std::wstring& preferred_install_dir)
    {
        out_runtime_dir = preferred_install_dir.empty() ? runtime_dir() : preferred_install_dir;
        if (out_runtime_dir.empty())
        {
            show_error(L"Could not resolve install directory.");
            return false;
        }

        std::filesystem::create_directories(out_runtime_dir);
        report_status(L"Checking Vanille payload...");

        if (payload_is_current(out_runtime_dir))
        {
            report_status(L"Payload is up to date.");
            return true;
        }

        report_status(L"Installing embedded Vanille payload...");

        std::vector<std::uint8_t> zip_bytes;
        if (!load_embedded_zip(zip_bytes))
        {
            show_error(L"Embedded Vanille payload is missing from the loader.");
            return false;
        }

        const std::wstring staging_dir = out_runtime_dir + L"\\.staging";
        std::error_code ec;
        std::filesystem::remove_all(staging_dir, ec);

        if (!extract_zip_to_directory(zip_bytes, staging_dir))
        {
            show_error(L"Failed to extract the embedded Vanille payload.");
            return false;
        }

        for (const auto& entry : std::filesystem::directory_iterator(staging_dir, ec))
        {
            if (ec)
                break;

            const auto destination = std::filesystem::path(out_runtime_dir) / entry.path().filename();
            std::filesystem::remove_all(destination, ec);
            std::filesystem::copy(
                entry.path(),
                destination,
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                ec);
        }

        std::filesystem::remove_all(staging_dir, ec);

        wchar_t marker_path[MAX_PATH]{};
        PathCombineW(marker_path, out_runtime_dir.c_str(), L".payload_version");
        if (!write_file_text(marker_path, CHOCOLA_PAYLOAD_SHA256))
        {
            show_error(L"Failed to write payload version marker.");
            return false;
        }

        return payload_is_current(out_runtime_dir);
    }

    bool launch_vanille(const std::wstring& runtime_dir)
    {
        wchar_t vanille_path[MAX_PATH]{};
        PathCombineW(vanille_path, runtime_dir.c_str(), L"vanille.exe");
        if (!PathFileExistsW(vanille_path))
        {
            show_error(L"vanille.exe is missing from the install directory.");
            return false;
        }

        report_status(L"Launching Vanille...");

        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);

        PROCESS_INFORMATION process_info{};
        wchar_t command_line[MAX_PATH * 2]{};
        swprintf_s(command_line, L"\"%s\"", vanille_path);

        if (!CreateProcessW(
                vanille_path,
                command_line,
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                runtime_dir.c_str(),
                &startup_info,
                &process_info))
        {
            show_error(L"Failed to start vanille.exe.");
            return false;
        }

        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        report_status(L"Vanille launched.");
        return true;
    }
}
