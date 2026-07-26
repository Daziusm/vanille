#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "sdk/instance.h"
#include "utils/logger.h"
#include "sdk/math_types.h"

namespace rbx::mesh_part
{
    struct transform
    {
        rbx::Vector3 position{};
        float rotation[3][3]{};
        bool has_rotation = false;
    };

    std::optional<std::string> get_mesh_id(const rbx::instance_t& instance);
    std::optional<std::uint64_t> get_mesh_asset_id(const rbx::instance_t& instance);
    std::optional<transform> get_transform(const rbx::instance_t& instance, std::uintptr_t primitive = 0);
}

namespace rbx::mesh_parse
{
    struct mesh_data
    {
        std::vector<float> vertices;
        std::vector<unsigned int> indices;
    };

    mesh_data load_obj(const std::string& file_path, bool debug = false);
    mesh_data load_roblox_mesh(const std::string& file_path, bool debug = false);
}
