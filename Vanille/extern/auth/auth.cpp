#include "auth.h"

#include <Windows.h>
#include <sdk/offsets.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    std::filesystem::path module_directory()
    {
        char module_path[MAX_PATH]{};
        const DWORD size = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
        if (size == 0)
            return std::filesystem::current_path();
        return std::filesystem::path(module_path).parent_path();
    }

    std::unordered_map<std::string, std::string> parse_values_file(const std::filesystem::path& path)
    {
        std::unordered_map<std::string, std::string> offsets;
        std::ifstream input(path);
        if (!input.is_open())
            return offsets;

        std::string line;
        while (std::getline(input, line))
        {
            const auto first_quote = line.find('\'');
            if (first_quote == std::string::npos)
                continue;

            const auto second_quote = line.find('\'', first_quote + 1);
            if (second_quote == std::string::npos)
                continue;

            const auto value_marker = line.find("'0x", second_quote + 1);
            if (value_marker == std::string::npos)
                continue;

            const auto value_start = value_marker + 1;
            const auto value_end = line.find('\'', value_start);
            if (value_end == std::string::npos)
                continue;

            std::string key = line.substr(first_quote + 1, second_quote - first_quote - 1);
            std::string value = line.substr(value_start, value_end - value_start);
            if (!key.empty() && !value.empty())
                offsets.emplace(std::move(key), std::move(value));
        }

        return offsets;
    }

    std::filesystem::path find_values_file()
    {
        const auto exe_dir = module_directory();
        const std::filesystem::path candidates[] = {
            exe_dir / "values.txt",
            exe_dir / ".." / "values.txt",
            exe_dir / ".." / ".." / "values.txt",
        };

        for (const auto& candidate : candidates)
        {
            std::error_code ec;
            const auto resolved = std::filesystem::weakly_canonical(candidate, ec);
            if (!ec && std::filesystem::is_regular_file(resolved, ec))
                return resolved;
        }

        return {};
    }

    bool apply_offsets_from_map(const std::unordered_map<std::string, std::string>& offset_map)
    {
        const auto& descriptors = offset::get_descriptors();
        if (descriptors.empty())
        {
            std::cerr << "[vanille] No offsets registered.\n";
            return false;
        }

        auto normalize_hex = [](std::string value) -> std::string {
            if (value.rfind("0x", 0) == 0)
                value.erase(0, 2);
            return value;
        };

        auto is_optional_offset = [](const offset::descriptor& descriptor) -> bool {
            if (!descriptor.ns || !descriptor.name)
                return false;
            const std::string ns(descriptor.ns);
            const std::string name(descriptor.name);
            return ns == "datamodel" && (name == "datamodel_ptr0" || name == "datamodel_ptr1");
        };

        bool missing_any = false;
        bool parse_failed = false;

        for (const auto& descriptor : descriptors)
        {
            std::string matched_key;
            std::string hex_value;

            std::vector<std::string> candidates;
            if (descriptor.ns && *descriptor.ns)
            {
                std::string ns(descriptor.ns);
                candidates.emplace_back(ns + "::" + descriptor.name);
                candidates.emplace_back(ns + "." + descriptor.name);
                candidates.emplace_back(ns + "_" + descriptor.name);
            }
            candidates.emplace_back(descriptor.name);

            for (const auto& candidate : candidates)
            {
                const auto it = offset_map.find(candidate);
                if (it == offset_map.end() || it->second.empty())
                    continue;

                matched_key = candidate;
                hex_value = normalize_hex(it->second);
                break;
            }

            if (hex_value.empty())
            {
                if (is_optional_offset(descriptor))
                    continue;

                std::cerr << "[vanille] missing offset key for '" << descriptor.ns << "::" << descriptor.name << "'\n";
                missing_any = true;
                continue;
            }

            try
            {
                *descriptor.value = std::stoull(hex_value, nullptr, 16);
            }
            catch (...)
            {
                std::cerr << "[vanille] Failed to parse offset '" << descriptor.ns << "::" << descriptor.name << "' from '" << matched_key << "'\n";
                parse_failed = true;
            }
        }

        return !missing_any && !parse_failed;
    }
}

bool LoadOffsets()
{
    const auto values_path = find_values_file();
    if (values_path.empty())
    {
        std::cerr << "[vanille] values.txt not found next to executable.\n";
        return false;
    }

    const auto offset_map = parse_values_file(values_path);
    if (offset_map.empty())
    {
        std::cerr << "[vanille] values.txt is empty or unreadable: " << values_path.string() << "\n";
        return false;
    }

    if (!apply_offsets_from_map(offset_map))
    {
        std::cerr << "[vanille] failed to apply offsets from " << values_path.string() << "\n";
        return false;
    }

    std::cout << "[vanille] loaded offsets from " << values_path.string() << "\n";
    return true;
}
