#include "settings.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <fstream>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace
{
    std::wstring read_ini_string(const wchar_t* path, const wchar_t* key, const wchar_t* fallback)
    {
        wchar_t buffer[1024]{};
        GetPrivateProfileStringW(L"loader", key, fallback, buffer, static_cast<DWORD>(std::size(buffer)), path);
        return buffer;
    }

    bool read_ini_bool(const wchar_t* path, const wchar_t* key, bool fallback)
    {
        return GetPrivateProfileIntW(L"loader", key, fallback ? 1 : 0, path) != 0;
    }
}

namespace settings
{
    std::wstring default_install_path()
    {
        wchar_t app_data[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, app_data)))
            return L"";

        wchar_t combined[MAX_PATH]{};
        PathCombineW(combined, app_data, L"Chocola");
        return combined;
    }

    std::wstring default_source_path()
    {
        wchar_t documents[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents)))
            return L"";

        wchar_t combined[MAX_PATH]{};
        PathCombineW(combined, documents, L"Vanille-src");
        return combined;
    }

    std::wstring settings_path()
    {
        wchar_t app_data[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, app_data)))
            return L"";

        wchar_t dir[MAX_PATH]{};
        PathCombineW(dir, app_data, L"Chocola");
        CreateDirectoryW(dir, nullptr);

        wchar_t file[MAX_PATH]{};
        PathCombineW(file, dir, L"loader.ini");
        return file;
    }

    loader_settings load()
    {
        loader_settings value;
        value.install_path = default_install_path();
        value.source_path = default_source_path();

        const std::wstring path = settings_path();
        if (path.empty())
            return value;

        value.install_path = read_ini_string(path.c_str(), L"install_path", value.install_path.c_str());
        value.source_path = read_ini_string(path.c_str(), L"source_path", value.source_path.c_str());
        value.repo_url = read_ini_string(path.c_str(), L"repo_url", value.repo_url.c_str());
        value.branch = read_ini_string(path.c_str(), L"branch", value.branch.c_str());
        value.auto_load = read_ini_bool(path.c_str(), L"auto_load", false);
        value.selected_module = GetPrivateProfileIntW(L"loader", L"selected_module", 0, path.c_str());

        if (value.repo_url == L"https://github.com/your-org/vanille"
            || value.repo_url == L"https://github.com/0x1408/vanille")
        {
            value.repo_url = L"https://github.com/Daziusm/vanille";
            WritePrivateProfileStringW(L"loader", L"repo_url", value.repo_url.c_str(), path.c_str());
        }

        if (value.branch == L"main" && value.repo_url == L"https://github.com/Daziusm/vanille")
        {
            value.branch = L"master";
            WritePrivateProfileStringW(L"loader", L"branch", value.branch.c_str(), path.c_str());
        }

        return value;
    }

    bool save(const loader_settings& value)
    {
        const std::wstring path = settings_path();
        if (path.empty())
            return false;

        WritePrivateProfileStringW(L"loader", L"install_path", value.install_path.c_str(), path.c_str());
        WritePrivateProfileStringW(L"loader", L"source_path", value.source_path.c_str(), path.c_str());
        WritePrivateProfileStringW(L"loader", L"repo_url", value.repo_url.c_str(), path.c_str());
        WritePrivateProfileStringW(L"loader", L"branch", value.branch.c_str(), path.c_str());
        WritePrivateProfileStringW(L"loader", L"auto_load", value.auto_load ? L"1" : L"0", path.c_str());

        wchar_t module_buffer[16]{};
        swprintf_s(module_buffer, L"%d", value.selected_module);
        WritePrivateProfileStringW(L"loader", L"selected_module", module_buffer, path.c_str());
        return true;
    }
}
