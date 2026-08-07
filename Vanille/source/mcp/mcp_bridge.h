#pragma once

#include <filesystem>
#include <string>

namespace vanille::mcp_bridge
{
    std::filesystem::path root_dir();
    std::filesystem::path requests_dir();
    std::filesystem::path responses_dir();
    std::filesystem::path status_path();
    std::filesystem::path explorer_snapshot_path();

    void initialize();
    void tick();
}
