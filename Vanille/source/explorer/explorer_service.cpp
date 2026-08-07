#include "explorer/explorer_service.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

#include "globals/globals_fixed.h"
#include "memory/memory.h"
#include "sdk/engine.h"
#include "sdk/offsets.h"

namespace vanille::explorer
{
    namespace
    {
        std::string sanitize_label(std::string text)
        {
            if (text.empty())
            {
                return {};
            }

            std::string out;
            out.reserve(text.size());
            for (unsigned char c : text)
            {
                if (c >= 32 && c <= 126)
                {
                    out.push_back(static_cast<char>(c));
                }
            }

            constexpr std::size_t k_max_label_len = 96;
            if (out.size() > k_max_label_len)
            {
                out.resize(k_max_label_len);
            }
            return out;
        }

        rbx::instance_t read_parent(const rbx::instance_t& instance)
        {
            if (!instance.is_valid() || !roblox::offsets::instance::parent)
            {
                return {};
            }

            try
            {
                const std::uintptr_t parent_ptr = memory->read<std::uintptr_t>(instance.get_address() + roblox::offsets::instance::parent);
                if (!parent_ptr)
                {
                    return {};
                }
                return rbx::instance_t(parent_ptr);
            }
            catch (...)
            {
                return {};
            }
        }

        std::string build_path(const rbx::instance_t& instance)
        {
            if (!instance.is_valid())
            {
                return {};
            }

            struct path_node
            {
                std::uintptr_t address = 0;
                std::string label;
            };

            std::vector<path_node> nodes;
            nodes.reserve(16);
            std::unordered_set<std::uintptr_t> seen;
            constexpr std::size_t k_max_depth = 128;
            const std::uintptr_t datamodel_addr = globals->datamodel.is_valid() ? globals->datamodel.get_address() : 0;

            rbx::instance_t current = instance;
            for (std::size_t depth = 0; depth < k_max_depth && current.is_valid(); ++depth)
            {
                const std::uintptr_t address = current.get_address();
                if (address == 0 || !seen.insert(address).second)
                {
                    break;
                }

                std::string label;
                try { label = current.get_name(); }
                catch (...) {}
                if (label.empty())
                {
                    try { label = current.get_class_name(); }
                    catch (...) {}
                }
                if (label.empty())
                {
                    label = "Instance";
                }

                nodes.push_back({ address, std::move(label) });
                if (datamodel_addr != 0 && address == datamodel_addr)
                {
                    break;
                }

                current = read_parent(current);
            }

            if (nodes.empty())
            {
                return {};
            }

            if (datamodel_addr != 0)
            {
                for (auto& node_item : nodes)
                {
                    if (node_item.address == datamodel_addr)
                    {
                        node_item.label = "game";
                        break;
                    }
                }
            }

            std::reverse(nodes.begin(), nodes.end());
            std::string path;
            path.reserve(nodes.size() * 12);
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (i > 0)
                {
                    path.push_back('.');
                }
                path += nodes[i].label;
            }
            return path;
        }

        std::string json_escape(std::string_view text)
        {
            std::string out;
            out.reserve(text.size() + 8);
            for (char c : text)
            {
                switch (c)
                {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out.push_back(c); break;
                }
            }
            return out;
        }

        node capture_node(const rbx::instance_t& instance, std::size_t depth, const capture_options& options, std::size_t& counter, bool& truncated)
        {
            node result{};
            if (!instance.is_valid())
            {
                return result;
            }

            result.address = instance.get_address();
            try { result.name = sanitize_label(instance.get_name()); }
            catch (...) {}
            try { result.class_name = sanitize_label(instance.get_class_name()); }
            catch (...) {}
            result.path = build_path(instance);
            ++counter;

            if (options.max_nodes > 0 && counter >= options.max_nodes)
            {
                truncated = true;
                return result;
            }

            if (options.max_depth > 0 && depth >= options.max_depth)
            {
                return result;
            }

            std::vector<rbx::instance_t> children;
            try
            {
                children = instance.get_children();
            }
            catch (...)
            {
                children.clear();
            }

            result.children.reserve(children.size());
            for (const auto& child : children)
            {
                if (!child.is_valid())
                {
                    continue;
                }
                if (options.max_nodes > 0 && counter >= options.max_nodes)
                {
                    truncated = true;
                    break;
                }
                result.children.push_back(capture_node(child, depth + 1, options, counter, truncated));
            }

            return result;
        }

        void append_json_node(std::ostringstream& oss, const node& item, int indent, bool pretty)
        {
            const std::string pad(pretty ? static_cast<std::size_t>(indent), ' ' : 0, ' ');
            const std::string next_pad(pretty ? static_cast<std::size_t>(indent + 2), ' ' : 0, ' ');
            const std::string newline = pretty ? "\n" : "";

            oss << pad << "{" << newline;
            oss << next_pad << "\"address\": \"0x" << std::hex << std::uppercase << item.address << std::dec << "\"," << newline;
            oss << next_pad << "\"name\": \"" << json_escape(item.name) << "\"," << newline;
            oss << next_pad << "\"class_name\": \"" << json_escape(item.class_name) << "\"," << newline;
            oss << next_pad << "\"path\": \"" << json_escape(item.path) << "\"";

            if (!item.children.empty())
            {
                oss << "," << newline << next_pad << "\"children\": [" << newline;
                for (std::size_t i = 0; i < item.children.size(); ++i)
                {
                    append_json_node(oss, item.children[i], indent + 4, pretty);
                    if (i + 1 < item.children.size())
                    {
                        oss << ",";
                    }
                    oss << newline;
                }
                oss << next_pad << "]";
            }

            oss << newline << pad << "}";
        }

        const node* find_node_recursive(const node& current, std::string_view path)
        {
            if (current.path == path)
            {
                return &current;
            }

            for (const auto& child : current.children)
            {
                if (const node* found = find_node_recursive(child, path))
                {
                    return found;
                }
            }
            return nullptr;
        }
    }

    snapshot capture(const capture_options& options)
    {
        snapshot result{};
        if (!globals || !globals->datamodel.is_valid())
        {
            return result;
        }

        bool truncated = false;
        try
        {
            node root = capture_node(globals->datamodel, 0, options, result.node_count, truncated);
            result.roots.push_back(std::move(root));
        }
        catch (...)
        {
            result.roots.clear();
            result.node_count = 0;
        }

        result.truncated = truncated;
        return result;
    }

    std::string to_json(const snapshot& snap, bool pretty)
    {
        std::ostringstream oss;
        const std::string newline = pretty ? "\n" : "";
        const std::string indent = pretty ? "  " : "";

        oss << "{" << newline;
        oss << indent << "\"generated_at_unix_ms\": " << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() << "," << newline;
        oss << indent << "\"game_id\": " << (globals ? globals->game_id : 0) << "," << newline;
        oss << indent << "\"node_count\": " << snap.node_count << "," << newline;
        oss << indent << "\"truncated\": " << (snap.truncated ? "true" : "false") << "," << newline;
        oss << indent << "\"roots\": [" << newline;

        for (std::size_t i = 0; i < snap.roots.size(); ++i)
        {
            append_json_node(oss, snap.roots[i], pretty ? 4 : 0, pretty);
            if (i + 1 < snap.roots.size())
            {
                oss << ",";
            }
            oss << newline;
        }

        oss << indent << "]" << newline << "}";
        return oss.str();
    }

    bool export_json_to_file(const std::string& path, const capture_options& options)
    {
        if (path.empty())
        {
            return false;
        }

        try
        {
            const std::filesystem::path output_path(path);
            if (output_path.has_parent_path())
            {
                std::filesystem::create_directories(output_path.parent_path());
            }

            const snapshot snap = capture(options);
            std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                return false;
            }

            file << to_json(snap, true);
            return file.good();
        }
        catch (...)
        {
            return false;
        }
    }

    std::string default_mcp_snapshot_path()
    {
        char* local_app_data = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&local_app_data, &length, "LOCALAPPDATA") != 0 || !local_app_data || !*local_app_data)
        {
            if (local_app_data)
            {
                free(local_app_data);
            }
            return "explorer_snapshot.json";
        }

        const std::string path = (std::filesystem::path(local_app_data) / "Vanille" / "mcp" / "explorer_snapshot.json").string();
        free(local_app_data);
        return path;
    }

    bool export_mcp_snapshot(const capture_options& options)
    {
        return export_json_to_file(default_mcp_snapshot_path(), options);
    }

    std::optional<node> find_by_path(const snapshot& snap, std::string_view dot_path)
    {
        for (const auto& root : snap.roots)
        {
            if (const node* found = find_node_recursive(root, dot_path))
            {
                return *found;
            }
        }
        return std::nullopt;
    }
}
