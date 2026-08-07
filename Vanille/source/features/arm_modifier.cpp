#include "features/arm_modifier.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "globals/globals_fixed.h"
#include "memory/memory.h"
#include "sdk/offsets.h"
#include "sdk/part.h"

namespace
{
    bool is_finite_vec(const rbx::Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool size_is_reasonable(const rbx::Vector3& size)
    {
        if (!is_finite_vec(size))
        {
            return false;
        }

        constexpr float k_min_size = 0.1f;
        constexpr float k_max_size = 5000.0f;
        if (size.x < k_min_size || size.y < k_min_size || size.z < k_min_size)
        {
            return false;
        }
        if (size.x > k_max_size || size.y > k_max_size || size.z > k_max_size)
        {
            return false;
        }
        return true;
    }

    bool is_valid_primitive(std::uintptr_t primitive)
    {
        if (!primitive || !roblox::offsets::base_part::validate)
        {
            return false;
        }

        constexpr int k_validate_value = 0x6;
        try
        {
            return memory->read<int>(primitive + roblox::offsets::base_part::validate) == k_validate_value;
        }
        catch (...)
        {
            return false;
        }
    }

    bool write_part_color_packed(const rbx::instance_t& instance, std::uint8_t r, std::uint8_t g, std::uint8_t b)
    {
        if (!instance.is_valid() || !roblox::offsets::base_part::color3)
        {
            return false;
        }

        const auto color_offset = roblox::offsets::base_part::color3;
        std::uint32_t packed = static_cast<std::uint32_t>(r)
            | (static_cast<std::uint32_t>(g) << 8)
            | (static_cast<std::uint32_t>(b) << 16);

        try
        {
            const std::uint32_t current = memory->read<std::uint32_t>(instance.get_address() + color_offset);
            packed |= (current & 0xFF000000u);
        }
        catch (...)
        {
        }

        try
        {
            return memory->write<std::uint32_t>(instance.get_address() + color_offset, packed);
        }
        catch (...)
        {
            return false;
        }
    }
}

namespace arm_modifier
{
    void run()
    {
        if (!features->enable_arm_modifier)
        {
            return;
        }

        rbx::instance_t workspace = globals->workspace;
        if (!workspace.is_valid() && globals->datamodel.is_valid())
        {
            workspace = globals->datamodel.find_first_child_by_class("Workspace");
        }
        if (!workspace.is_valid())
        {
            return;
        }

        rbx::instance_t camera = workspace.find_first_child("CurrentCamera");
        if (!camera.is_valid())
        {
            camera = workspace.find_first_child("Camera");
        }
        if (!camera.is_valid())
        {
            camera = workspace.get_current_camera();
        }
        if (!camera.is_valid())
        {
            return;
        }

        const auto descendants = camera.get_descendants();
        if (descendants.empty())
        {
            return;
        }

        const auto material_offset = roblox::offsets::base_part::material;
        if (!material_offset)
        {
            return;
        }

        const std::int16_t selected_material = static_cast<std::int16_t>(features->arm_modifier_material);
        const bool apply_material = selected_material > 0;
        const ImVec4 color = features->arm_modifier_color;

        auto to_byte = [](float value) -> std::uint8_t
        {
            return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        };

        const std::uint8_t r = to_byte(color.x);
        const std::uint8_t g = to_byte(color.y);
        const std::uint8_t b = to_byte(color.z);

        for (const auto& child : descendants)
        {
            if (!child.is_valid())
            {
                continue;
            }

            const auto primitive = rbx::part::get_primitive(child);
            if (!primitive)
            {
                continue;
            }

            if (!is_valid_primitive(primitive))
            {
                continue;
            }

            const auto size_opt = rbx::part::get_size(primitive);
            if (!size_opt || !size_is_reasonable(*size_opt))
            {
                continue;
            }

            if (apply_material)
            {
                memory->write<std::int16_t>(primitive + material_offset, selected_material);
            }

            write_part_color_packed(child, r, g, b);
        }
    }
}
