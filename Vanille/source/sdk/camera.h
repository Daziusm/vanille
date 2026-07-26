#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <algorithm>
#include <limits>

#include "sdk/math_types.h"
#include "sdk/offsets.h"
#include "memory/memory.h"

namespace rbx
{
    inline bool matrix_is_finite(const Matrix& matrix)
    {
        const float* values = reinterpret_cast<const float*>(&matrix._11);
        for (int i = 0; i < 16; ++i)
        {
            if (!std::isfinite(values[i]))
            {
                return false;
            }
        }
        return true;
    }

    class visualengine_t
    {
    public:
        visualengine_t() = default;
        explicit visualengine_t(std::uintptr_t addr) : address(addr) {}

        [[nodiscard]] bool is_valid() const { return address != 0; }

        [[nodiscard]] std::optional<Vector2> get_dimensions() const
        {
            if (!is_valid() || !roblox::offsets::visualengine::dimensions)
            {
                return std::nullopt;
            }

            const auto dimensions = memory->read<Vector2>(address + roblox::offsets::visualengine::dimensions);
            if (!std::isfinite(dimensions.x) || !std::isfinite(dimensions.y) || dimensions.x <= 0.0f || dimensions.y <= 0.0f)
            {
                return std::nullopt;
            }

            return dimensions;
        }

        [[nodiscard]] std::optional<Matrix> get_view_matrix() const
        {
            if (!is_valid() || !roblox::offsets::visualengine::view_matrix)
            {
                return std::nullopt;
            }

            const auto view = memory->read<Matrix>(address + roblox::offsets::visualengine::view_matrix);
            if (!matrix_is_finite(view))
            {
                return std::nullopt;
            }

            return view;
        }

    private:
        std::uintptr_t address = 0;
    };
}

namespace rbx::camera
{
    struct Vector2int16
    {
        std::int16_t x = 0;
        std::int16_t y = 0;
    };

    inline Vector2int16 calculate_viewport(const Vector2& target_screen_pos, const Vector2& screen_size) noexcept
    {
        const float clamped_target_x = std::clamp(target_screen_pos.x, 0.0f, screen_size.x);
        const float clamped_target_y = std::clamp(target_screen_pos.y, 0.0f, screen_size.y);
        const float raw_x = 2.0f * (screen_size.x - clamped_target_x);
        const float raw_y = 2.0f * (screen_size.y - clamped_target_y);
        const int rounded_x = static_cast<int>(std::lround(raw_x));
        const int rounded_y = static_cast<int>(std::lround(raw_y));
        const int min_i16 = static_cast<int>((std::numeric_limits<std::int16_t>::min)());
        const int max_i16 = static_cast<int>((std::numeric_limits<std::int16_t>::max)());
        const int clamped_x = std::clamp(rounded_x, min_i16, max_i16);
        const int clamped_y = std::clamp(rounded_y, min_i16, max_i16);
        return Vector2int16{ static_cast<std::int16_t>(clamped_x), static_cast<std::int16_t>(clamped_y) };
    }

    inline Vector2int16 viewport_from_size(const Vector2& screen_size) noexcept
    {
        const int min_i16 = static_cast<int>((std::numeric_limits<std::int16_t>::min)());
        const int max_i16 = static_cast<int>((std::numeric_limits<std::int16_t>::max)());
        const int clamped_x = std::clamp(static_cast<int>(screen_size.x), min_i16, max_i16);
        const int clamped_y = std::clamp(static_cast<int>(screen_size.y), min_i16, max_i16);
        return Vector2int16{ static_cast<std::int16_t>(clamped_x), static_cast<std::int16_t>(clamped_y) };
    }

    inline void set_viewport(std::uintptr_t camera_address, const Vector2int16& value) noexcept
    {
        if (camera_address == 0 || !roblox::offsets::camera::viewport)
        {
            return;
        }
        memory->write<Vector2int16>(camera_address + roblox::offsets::camera::viewport, value);
    }

    inline Vector4 transform(const Vector3& v, const Matrix& m) noexcept
    {
        return Vector4{
            v.x * m._11 + v.y * m._12 + v.z * m._13 + m._14,
            v.x * m._21 + v.y * m._22 + v.z * m._23 + m._24,
            v.x * m._31 + v.y * m._32 + v.z * m._33 + m._34,
            v.x * m._41 + v.y * m._42 + v.z * m._43 + m._44
        };
    }

    inline std::optional<rbx::Vector2> world_to_screen(const rbx::Vector3& world, const rbx::Matrix& view_matrix, const rbx::Vector2& dimensions) noexcept
    {
        if (dimensions.x <= 0.0f || dimensions.y <= 0.0f)
        {
            return std::nullopt;
        }

        const rbx::Vector4 clip = transform(world, view_matrix);
        if (clip.w <= 0.0f)
        {
            return std::nullopt;
        }

        const rbx::Vector3 ndc = rbx::Vector3(clip.x, clip.y, clip.z) / clip.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f || ndc.z < 0.0f || ndc.z > 1.0f)
        {
            return std::nullopt;
        }

        return rbx::Vector2{ (ndc.x + 1.0f) * 0.5f * dimensions.x, (1.0f - ndc.y) * 0.5f * dimensions.y };
    }
}
