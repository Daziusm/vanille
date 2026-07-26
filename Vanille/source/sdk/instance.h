#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <atomic>
#include <vector>
#include <DirectXMath.h>

#include "sdk/math_types.h"

namespace rbx
{
    class instance_t
    {
    public:
        instance_t() = default;
        explicit instance_t(std::uintptr_t address);
        instance_t(const instance_t& other);
        instance_t& operator=(const instance_t& other);
        instance_t(instance_t&& other) noexcept;
        instance_t& operator=(instance_t&& other) noexcept;

        [[nodiscard]] bool is_valid() const;
        [[nodiscard]] std::uintptr_t get_address() const;

        [[nodiscard]] std::int64_t get_game_id() const;

        [[nodiscard]] std::vector<instance_t> get_children() const;
        [[nodiscard]] std::vector<instance_t> get_descendants() const;
        [[nodiscard]] instance_t find_first_child(std::string_view name) const;
        [[nodiscard]] instance_t find_first_child_by_class(std::string_view class_name) const;
        [[nodiscard]] instance_t get_current_camera() const;
        [[nodiscard]] bool IsA(std::string_view class_name) const;
        [[nodiscard]] std::string get_name() const;
        [[nodiscard]] std::string get_class_name() const;
        [[nodiscard]] std::optional<Vector3> get_position(std::uintptr_t primitive_override = 0) const;
        [[nodiscard]] std::uint64_t get_cached_input_object() const;
        void cache_input_object() const;
        void initialise_mouse(std::uintptr_t address) const;
        bool set_frame_pos_x(std::uint64_t position) const;
        bool set_frame_pos_y(std::uint64_t position) const;
        bool set_base_part_color(std::uint8_t r, std::uint8_t g, std::uint8_t b) const;
        [[nodiscard]] Vector3 get_camera_position() const;
        [[nodiscard]] DirectX::XMFLOAT3X3 get_rotation() const;
        [[nodiscard]] DirectX::XMFLOAT3X3 look_at(const Vector3& target) const;
        void set_rotation(const DirectX::XMFLOAT3X3& rotation) const;

    private:
        std::uintptr_t address = 0;
        mutable std::atomic<std::uint64_t> input_object_cache{ 0 };
    };
}
