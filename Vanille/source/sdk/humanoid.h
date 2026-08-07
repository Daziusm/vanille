#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

#include "memory/memory.h"
#include "sdk/instance.h"
#include "sdk/offsets.h"
#include "globals/globals_fixed.h"

namespace rbx::humanoid
{
    inline rbx::instance_t find_humanoid(const rbx::instance_t& character_instance)
    {
        if (!character_instance.is_valid())
        {
            return {};
        }

        return character_instance.find_first_child("Humanoid");
    }

    inline std::optional<float> get_health(const rbx::instance_t& humanoid_instance)
    {
        if (!humanoid_instance.is_valid() || !roblox::offsets::humanoid::health)
        {
            return std::nullopt;
        }

        const float value = memory->read<float>(humanoid_instance.get_address() + roblox::offsets::humanoid::health);
        if (!std::isfinite(value))
        {
            return std::nullopt;
        }

        return value;
    }

    inline std::optional<float> get_max_health(const rbx::instance_t& humanoid_instance)
    {
        if (!humanoid_instance.is_valid() || !roblox::offsets::humanoid::max_health)
        {
            return std::nullopt;
        }

        const float value = memory->read<float>(humanoid_instance.get_address() + roblox::offsets::humanoid::max_health);
        if (!std::isfinite(value))
        {
            return std::nullopt;
        }

        return value;
    }

    inline std::optional<float> set_gravity(float gravity)
    {
        uintptr_t container = memory->read<uintptr_t>(globals->workspace.get_address() + roblox::offsets::workspace::gravity_container);
        memory->write<float>(container + roblox::offsets::workspace::gravity, gravity);
    }

    inline std::optional<rbx::Vector3> get_move_direction(const rbx::instance_t& humanoid_instance)
    {
        if (!humanoid_instance.is_valid() || !roblox::offsets::humanoid::move_direction)
        {
            return std::nullopt;
        }

        const auto value = memory->read<rbx::Vector3>(humanoid_instance.get_address() + roblox::offsets::humanoid::move_direction);
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
        {
            return std::nullopt;
        }

        return value;
    }

    inline std::optional<bool> get_jump(const rbx::instance_t& humanoid_instance)
    {
        if (!humanoid_instance.is_valid() || !roblox::offsets::humanoid::jump)
        {
            return std::nullopt;
        }

        return memory->read<bool>(humanoid_instance.get_address() + roblox::offsets::humanoid::jump);
    }
}
