#pragma once

#include <string>

namespace settings
{
    struct loader_settings
    {
        std::wstring install_path;
        std::wstring source_path;
        std::wstring repo_url = L"https://github.com/Daziusm/vanille";
        std::wstring branch = L"master";
        bool auto_load = false;
        int selected_module = 0; // 0 = Vanille
    };

    loader_settings load();
    bool save(const loader_settings& value);
    std::wstring default_install_path();
    std::wstring default_source_path();
    std::wstring settings_path();
}
