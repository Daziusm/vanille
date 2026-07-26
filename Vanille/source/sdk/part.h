#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <DirectXMath.h>

#include "sdk/instance.h"
#include "sdk/math_types.h"
#include "sdk/offsets.h"
#include "memory/memory.h"

namespace rbx::part
{
    inline std::optional<float> get_transparency(std::uintptr_t primitive)
    {
        if (!primitive || !roblox::offsets::base_part::transparency)
        {
            return std::nullopt;
        }

        const float transparency = memory->read<float>(primitive + roblox::offsets::base_part::transparency);
        if (!std::isfinite(transparency))
        {
            return std::nullopt;
        }

        return std::clamp(transparency, 0.0f, 1.0f);
    }

    inline std::optional<float> get_transparency(const rbx::instance_t& part_instance, std::uintptr_t primitive = 0)
    {
        if (!part_instance.is_valid())
        {
            return get_transparency(primitive);
        }

        if (!roblox::offsets::base_part::transparency)
        {
            return get_transparency(primitive);
        }

        const float transparency = memory->read<float>(part_instance.get_address() + roblox::offsets::base_part::transparency);
        if (std::isfinite(transparency))
        {
            return std::clamp(transparency, 0.0f, 1.0f);
        }

        return get_transparency(primitive);
    }

    inline std::uintptr_t get_primitive(const rbx::instance_t& part_instance)
    {
        if (!part_instance.is_valid() || !roblox::offsets::base_part::primitive)
        {
            return 0;
        }

        return memory->read<std::uintptr_t>(part_instance.get_address() + roblox::offsets::base_part::primitive);
    }

    inline std::optional<rbx::Vector3> get_size(std::uintptr_t primitive)
    {
        if (!primitive || !roblox::offsets::base_part::size)
        {
            return std::nullopt;
        }

        const auto size = memory->read<rbx::Vector3>(primitive + roblox::offsets::base_part::size);
        if (!std::isfinite(size.x) || !std::isfinite(size.y) || !std::isfinite(size.z))
        {
            return std::nullopt;
        }

        return size;
    }

    inline std::optional<sdk::math::color3> get_color(const rbx::instance_t& part_instance)
    {
        if (!part_instance.is_valid() || !roblox::offsets::base_part::color3)
        {
            return std::nullopt;
        }

        if (const std::uintptr_t primitive = rbx::part::get_primitive(part_instance))
        {
            const auto color = memory->read<sdk::math::color3>(primitive + roblox::offsets::base_part::color3);
            if (std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b))
            {
                return color;
            }
        }

        const auto fallback = memory->read<sdk::math::color3>(part_instance.get_address() + roblox::offsets::base_part::color3);
        if (!std::isfinite(fallback.r) || !std::isfinite(fallback.g) || !std::isfinite(fallback.b))
        {
            return std::nullopt;
        }

        return fallback;
    }

    inline std::optional<rbx::Vector3> get_linear_velocity(std::uintptr_t primitive)
    {
        if (!primitive || !roblox::offsets::base_part::assembly_linear_velocity)
        {
            return std::nullopt;
        }

        const auto vel = memory->read<rbx::Vector3>(primitive + roblox::offsets::base_part::assembly_linear_velocity);
        if (!std::isfinite(vel.x) || !std::isfinite(vel.y) || !std::isfinite(vel.z))
        {
            return std::nullopt;
        }
        return vel;
    }

    inline bool set_linear_velocity(std::uintptr_t primitive, const rbx::Vector3& velocity)
    {
        if (!primitive || !roblox::offsets::base_part::assembly_linear_velocity)
        {
            return false;
        }

        memory->write<rbx::Vector3>(primitive + roblox::offsets::base_part::assembly_linear_velocity, velocity);
        if (roblox::offsets::base_part::assembly_angular_velocity)
        {
            const rbx::Vector3 zero{};
            memory->write<rbx::Vector3>(primitive + roblox::offsets::base_part::assembly_angular_velocity, zero);
        }
        return true;
    }

    inline bool clear_velocity(std::uintptr_t primitive)
    {
        const rbx::Vector3 zero{};
        return set_linear_velocity(primitive, zero);
    }

    inline bool set_can_collide(std::uintptr_t primitive, bool enable)
    {
        if (!primitive || !roblox::offsets::base_part::can_collide || !roblox::offsets::base_part::can_collide_mask)
        {
            return false;
        }

        const std::uintptr_t addr = primitive + roblox::offsets::base_part::can_collide;
        std::uint8_t flags = memory->read<std::uint8_t>(addr);
        const std::uint8_t mask = static_cast<std::uint8_t>(roblox::offsets::base_part::can_collide_mask);
        const std::uint8_t desired = enable ? static_cast<std::uint8_t>(flags | mask)
                                            : static_cast<std::uint8_t>(flags & ~mask);
        if (flags != desired)
        {
            memory->write<std::uint8_t>(addr, desired);
        }

        memory->write<std::uint8_t>(addr, enable ? static_cast<std::uint8_t>(1) : static_cast<std::uint8_t>(0));
        return true;
    }

    inline bool set_position(std::uintptr_t primitive, const rbx::Vector3& position)
    {
        if (!primitive)
        {
            return false;
        }

        bool success = false;

        if (roblox::offsets::base_part::position)
        {
            success |= memory->write<rbx::Vector3>(primitive + roblox::offsets::base_part::position, position);
        }

        if (roblox::offsets::base_part::primitive_properties && roblox::offsets::base_part::primitive_position)
        {
            const auto is_valid_ptr = [](std::uintptr_t ptr) -> bool
            {
                constexpr std::uintptr_t k_min = 0x10000;
                constexpr std::uintptr_t k_max = 0x00007FFFFFFFFFFF;
                return ptr >= k_min && ptr <= k_max;
            };

            const auto properties_offset = roblox::offsets::base_part::primitive_properties;
            const auto position_offset = roblox::offsets::base_part::primitive_position;
            const std::uintptr_t props_ptr = memory->read<std::uintptr_t>(primitive + properties_offset);
            if (is_valid_ptr(props_ptr))
            {
                success |= memory->write<rbx::Vector3>(props_ptr + position_offset, position);
            }
            else
            {
                success |= memory->write<rbx::Vector3>(primitive + properties_offset + position_offset, position);
            }
        }

        return success;
    }

    inline bool set_position_repeated(std::uintptr_t primitive, const rbx::Vector3& position, int repetitions = 105)
    {
        repetitions = std::clamp(repetitions, 1, 100);
        bool any_success = false;
        for (int i = 0; i < repetitions; ++i)
        {
            any_success |= set_position(primitive, position);
        }
        return any_success;
    }

    inline bool step_position(const rbx::instance_t& part_instance, std::uintptr_t primitive, const rbx::Vector3& velocity, float dt, int repetitions = 105)
    {
        if (dt <= 0.0f)
        {
            return false;
        }

        if (!primitive)
        {
            primitive = rbx::part::get_primitive(part_instance);
        }

        if (!primitive)
        {
            return false;
        }

        const auto pos = part_instance.get_position(primitive);
        if (!pos)
        {
            return false;
        }

        const rbx::Vector3 next = *pos + velocity * dt;
        return set_position_repeated(primitive, next, repetitions);
    }

    inline bool set_rotation_from_forward(std::uintptr_t primitive, const rbx::Vector3& forward_dir, const rbx::Vector3& up_hint = { 0.0f, 1.0f, 0.0f })
    {
        if (!primitive || !roblox::offsets::base_part::cframe_rotation)
        {
            return false;
        }

        rbx::Vector3 forward = forward_dir;
        forward.Normalize();
        if (forward.LengthSquared() < 1e-4f || !std::isfinite(forward.x) || !std::isfinite(forward.y) || !std::isfinite(forward.z))
        {
            return false;
        }

        rbx::Vector3 up = up_hint;
        if (up.LengthSquared() < 1e-4f || !std::isfinite(up.x) || !std::isfinite(up.y) || !std::isfinite(up.z))
        {
            up = { 0.0f, 1.0f, 0.0f };
        }
        up.Normalize();

        rbx::Vector3 right = up.Cross(forward);
        if (right.LengthSquared() < 1e-6f)
        {
            right = rbx::Vector3(1.0f, 0.0f, 0.0f).Cross(forward);
        }
        if (right.LengthSquared() < 1e-6f)
        {
            return false;
        }
        right.Normalize();
        up = forward.Cross(right);
        up.Normalize();

        DirectX::XMFLOAT3X3 rot{};
        rot._11 = -right.x;  rot._12 = up.x;   rot._13 = -forward.x;
        rot._21 = right.y;   rot._22 = up.y;   rot._23 = -forward.y;
        rot._31 = -right.z;  rot._32 = up.z;   rot._33 = -forward.z;

        memory->write<DirectX::XMFLOAT3X3>(primitive + roblox::offsets::base_part::cframe_rotation, rot);
        return true;
    }

}
