#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace script_storage
{
    struct script_entry
    {
        std::string name;
        std::filesystem::path path;
    };

    void initialize();
    void set_scripts_directory(const std::filesystem::path& directory);
    std::filesystem::path get_scripts_directory();
    void refresh();
    const std::vector<script_entry>& get_scripts();
    std::string sanitize_script_name(const std::string& name);
    std::string make_unique_script_name(const std::string& base_name);
    bool load_script(const std::string& name, std::string& out_script);
    bool save_script(const std::string& name, const std::string& script, std::string* out_saved_name = nullptr);
    bool delete_script(const std::string& name);
    bool rename_script(const std::string& source_name, const std::string& target_name, std::string* out_new_name = nullptr);
    bool duplicate_script(const std::string& source_name, const std::string& target_name, std::string* out_new_name = nullptr);
}
