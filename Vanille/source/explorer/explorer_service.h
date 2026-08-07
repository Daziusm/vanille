#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vanille::explorer
{
    struct node
    {
        std::uintptr_t address = 0;
        std::string name;
        std::string class_name;
        std::string path;
        std::vector<node> children;
    };

    struct snapshot
    {
        std::vector<node> roots;
        std::size_t node_count = 0;
        bool truncated = false;
    };

    struct capture_options
    {
        std::size_t max_depth = 0;
        std::size_t max_nodes = 50000;
    };

    snapshot capture(const capture_options& options = {});
    std::string to_json(const snapshot& snap, bool pretty = true);
    bool export_json_to_file(const std::string& path, const capture_options& options = {});
    std::string default_mcp_snapshot_path();
    bool export_mcp_snapshot(const capture_options& options = {});
    std::optional<node> find_by_path(const snapshot& snap, std::string_view dot_path);
}
