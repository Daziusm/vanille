#include "lua/script_storage.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace script_storage
{
    namespace fs = std::filesystem;

    namespace
    {
        bool g_initialized = false;
        bool g_has_custom_directory = false;
        fs::path g_custom_directory;
        std::vector<script_entry> g_scripts;

        std::string to_lower_copy(const std::string& value)
        {
            std::string lowered = value;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
                });
            return lowered;
        }

        std::string trim_copy(const std::string& value)
        {
            const std::size_t begin = value.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
            {
                return std::string();
            }
            const std::size_t end = value.find_last_not_of(" \t\r\n");
            return value.substr(begin, end - begin + 1);
        }

        fs::path module_directory()
        {
            char module_path[MAX_PATH]{};
            const DWORD size = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
            if (size == 0)
            {
                return fs::current_path();
            }
            return fs::path(module_path).parent_path();
        }

        fs::path scripts_directory()
        {
            if (g_has_custom_directory)
            {
                return g_custom_directory;
            }
            return module_directory() / "scripts";
        }

        bool ensure_directory()
        {
            std::error_code error_code;
            const fs::path directory = scripts_directory();
            if (!fs::exists(directory, error_code))
            {
                fs::create_directories(directory, error_code);
                if (error_code)
                {
                    return false;
                }
            }
            return fs::is_directory(directory, error_code);
        }

        fs::path script_path_from_name(const std::string& raw_name, std::string& sanitized_name)
        {
            sanitized_name = sanitize_script_name(raw_name);
            if (sanitized_name.empty())
            {
                return fs::path();
            }
            return scripts_directory() / (sanitized_name + ".lua");
        }
    }

    void initialize()
    {
        if (g_initialized)
        {
            return;
        }

        if (ensure_directory())
        {
            refresh();
            g_initialized = true;
        }
    }

    void set_scripts_directory(const std::filesystem::path& directory)
    {
        g_custom_directory = directory;
        g_has_custom_directory = true;
        g_initialized = false;
        initialize();
    }

    std::filesystem::path get_scripts_directory()
    {
        return scripts_directory();
    }

    void refresh()
    {
        g_scripts.clear();

        std::error_code error_code;
        const fs::path directory = scripts_directory();
        if (!fs::exists(directory, error_code) || !fs::is_directory(directory, error_code))
        {
            return;
        }

        for (const auto& entry : fs::directory_iterator(directory, error_code))
        {
            if (error_code)
            {
                break;
            }

            if (!entry.is_regular_file())
            {
                continue;
            }

            const fs::path& path = entry.path();
            if (to_lower_copy(path.extension().string()) != ".lua")
            {
                continue;
            }

            script_entry script{};
            script.name = path.stem().string();
            script.path = path;
            g_scripts.push_back(std::move(script));
        }

        std::sort(g_scripts.begin(), g_scripts.end(), [](const script_entry& left, const script_entry& right)
            {
                return to_lower_copy(left.name) < to_lower_copy(right.name);
            });
    }

    const std::vector<script_entry>& get_scripts()
    {
        return g_scripts;
    }

    std::string sanitize_script_name(const std::string& name)
    {
        const std::string trimmed = trim_copy(name);
        std::string sanitized;
        sanitized.reserve(trimmed.size());

        for (char current : trimmed)
        {
            const unsigned char value = static_cast<unsigned char>(current);
            if (std::isalnum(value) || current == '_' || current == '-' || current == ' ')
            {
                sanitized.push_back(current == ' ' ? '_' : current);
            }
        }

        while (!sanitized.empty() && (sanitized.front() == '_' || sanitized.front() == '-'))
        {
            sanitized.erase(sanitized.begin());
        }
        while (!sanitized.empty() && (sanitized.back() == '_' || sanitized.back() == '-'))
        {
            sanitized.pop_back();
        }

        return sanitized;
    }

    std::string make_unique_script_name(const std::string& base_name)
    {
        std::string candidate = sanitize_script_name(base_name);
        if (candidate.empty())
        {
            candidate = "script";
        }

        std::string output_name;
        fs::path output_path = script_path_from_name(candidate, output_name);
        if (!fs::exists(output_path))
        {
            return output_name;
        }

        int suffix = 1;
        while (suffix < 10000)
        {
            const std::string suffix_candidate = candidate + "_" + std::to_string(suffix);
            output_path = script_path_from_name(suffix_candidate, output_name);
            if (!fs::exists(output_path))
            {
                return output_name;
            }
            ++suffix;
        }

        return output_name;
    }

    bool load_script(const std::string& name, std::string& out_script)
    {
        initialize();

        std::string sanitized;
        const fs::path script_path = script_path_from_name(name, sanitized);
        if (sanitized.empty())
        {
            return false;
        }

        std::ifstream input(script_path, std::ios::binary);
        if (!input.is_open())
        {
            return false;
        }

        out_script.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        return true;
    }

    bool save_script(const std::string& name, const std::string& script, std::string* out_saved_name)
    {
        initialize();

        if (!ensure_directory())
        {
            return false;
        }

        std::string sanitized;
        const fs::path script_path = script_path_from_name(name, sanitized);
        if (sanitized.empty())
        {
            return false;
        }

        std::ofstream output(script_path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            return false;
        }

        output.write(script.data(), static_cast<std::streamsize>(script.size()));
        output.close();

        if (out_saved_name)
        {
            *out_saved_name = sanitized;
        }

        refresh();
        return true;
    }

    bool delete_script(const std::string& name)
    {
        initialize();

        std::string sanitized;
        const fs::path script_path = script_path_from_name(name, sanitized);
        if (sanitized.empty())
        {
            return false;
        }

        std::error_code error_code;
        if (!fs::exists(script_path, error_code))
        {
            return false;
        }

        if (!fs::remove(script_path, error_code) || error_code)
        {
            return false;
        }

        refresh();
        return true;
    }

    bool rename_script(const std::string& source_name, const std::string& target_name, std::string* out_new_name)
    {
        initialize();

        std::string source_sanitized;
        std::string target_sanitized;
        const fs::path source_path = script_path_from_name(source_name, source_sanitized);
        const fs::path target_path = script_path_from_name(target_name, target_sanitized);

        if (source_sanitized.empty() || target_sanitized.empty())
        {
            return false;
        }

        std::error_code error_code;
        if (!fs::exists(source_path, error_code) || fs::exists(target_path, error_code))
        {
            return false;
        }

        fs::rename(source_path, target_path, error_code);
        if (error_code)
        {
            return false;
        }

        if (out_new_name)
        {
            *out_new_name = target_sanitized;
        }

        refresh();
        return true;
    }

    bool duplicate_script(const std::string& source_name, const std::string& target_name, std::string* out_new_name)
    {
        std::string source_script;
        if (!load_script(source_name, source_script))
        {
            return false;
        }

        std::string target_sanitized = sanitize_script_name(target_name);
        if (target_sanitized.empty())
        {
            target_sanitized = make_unique_script_name(source_name + "_copy");
        }

        return save_script(target_sanitized, source_script, out_new_name);
    }
}
