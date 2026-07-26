#include "features/pf_silent.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <thread>
#include <DirectXMath.h>

#include "features/free_aim.h"
#include "globals/globals.h"
#include "memory/memory.h"
#include "sdk/offsets.h"
#include "sdk/part.h"

namespace
{
    constexpr std::int64_t k_pf_place_id = 292439477;

    std::atomic<bool> running{ false };
    std::thread worker;

    struct pf_state
    {
        rbx::instance_t camera;
        rbx::instance_t part;
        std::uintptr_t part_primitive = 0;
        std::uintptr_t workspace = 0;
    };

    bool write_rotation_matrix(std::uintptr_t primitive, const DirectX::XMFLOAT3X3& rotation)
    {
        if (!primitive || !roblox::offsets::base_part::cframe_rotation)
        {
            return false;
        }

        return memory->write<DirectX::XMFLOAT3X3>(primitive + roblox::offsets::base_part::cframe_rotation, rotation);
    }

    struct raw_vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        raw_vec3() = default;
        raw_vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        raw_vec3 operator-(const raw_vec3& other) const
        {
            return raw_vec3(x - other.x, y - other.y, z - other.z);
        }

        raw_vec3 operator*(float scalar) const
        {
            return raw_vec3(x * scalar, y * scalar, z * scalar);
        }

        float length_sq() const
        {
            return x * x + y * y + z * z;
        }

        raw_vec3 normalize() const
        {
            const float len_sq = length_sq();
            if (!std::isfinite(len_sq) || len_sq <= 1e-8f)
            {
                return raw_vec3();
            }
            const float inv_len = 1.0f / std::sqrt(len_sq);
            return raw_vec3(x * inv_len, y * inv_len, z * inv_len);
        }

        raw_vec3 cross(const raw_vec3& other) const
        {
            return raw_vec3(
                y * other.z - z * other.y,
                z * other.x - x * other.z,
                x * other.y - y * other.x
            );
        }
    };

    struct raw_matrix3
    {
        float m[3][3]{};
    };

    std::optional<DirectX::XMFLOAT3X3> build_camera_part_rotation(const rbx::Vector3& origin, const rbx::Vector3& destination)
    {
        const raw_vec3 origin_v(origin.x, origin.y, origin.z);
        const raw_vec3 destination_v(destination.x, destination.y, destination.z);

        raw_vec3 aim_vec = destination_v - origin_v;
        if (!std::isfinite(aim_vec.x) || !std::isfinite(aim_vec.y) || !std::isfinite(aim_vec.z) || aim_vec.length_sq() <= 1e-6f)
        {
            return std::nullopt;
        }

        raw_vec3 back_vec = (aim_vec * -1.0f).normalize();
        if (!std::isfinite(back_vec.x) || !std::isfinite(back_vec.y) || !std::isfinite(back_vec.z) || back_vec.length_sq() <= 1e-6f)
        {
            return std::nullopt;
        }

        raw_vec3 up_vec(0.0f, 0.01f, 0.0f);
        raw_vec3 side_vec = back_vec.cross(up_vec).normalize();
        if (!std::isfinite(side_vec.x) || !std::isfinite(side_vec.y) || !std::isfinite(side_vec.z) || side_vec.length_sq() <= 1e-8f)
        {
            return std::nullopt;
        }

        raw_matrix3 rot_mat{};
        rot_mat.m[0][0] = side_vec.x; rot_mat.m[0][1] = up_vec.x; rot_mat.m[0][2] = back_vec.x;
        rot_mat.m[1][0] = side_vec.y; rot_mat.m[1][1] = up_vec.y; rot_mat.m[1][2] = back_vec.y;
        rot_mat.m[2][0] = side_vec.z; rot_mat.m[2][1] = up_vec.z; rot_mat.m[2][2] = back_vec.z;

        DirectX::XMFLOAT3X3 rot{};
        rot._11 = rot_mat.m[0][0]; rot._12 = rot_mat.m[0][1]; rot._13 = rot_mat.m[0][2];
        rot._21 = rot_mat.m[1][0]; rot._22 = rot_mat.m[1][1]; rot._23 = rot_mat.m[1][2];
        rot._31 = rot_mat.m[2][0]; rot._32 = rot_mat.m[2][1]; rot._33 = rot_mat.m[2][2];
        return rot;
    }

    bool refresh_pf_state(pf_state& state)
    {
        if (!globals->workspace.is_valid())
        {
            state = {};
            return false;
        }

        const auto camera_instance = globals->workspace.find_first_child("Camera");
        if (!camera_instance.is_valid())
        {
            state = {};
            return false;
        }

        const auto part_instance = camera_instance.find_first_child("Part");
        if (!part_instance.is_valid())
        {
            state = {};
            return false;
        }

        const std::uintptr_t part_primitive = rbx::part::get_primitive(part_instance);
        if (!part_primitive)
        {
            state = {};
            return false;
        }

        state.camera = camera_instance;
        state.part = part_instance;
        state.part_primitive = part_primitive;
        state.workspace = globals->workspace.get_address();
        return true;
    }

    void run_loop()
    {
        pf_state state{};
        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<float> jitter_delay_ms(3.675f, 5.0f);

        while (running.load(std::memory_order_relaxed))
        {
            if (globals->game_id != k_pf_place_id)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            const auto& bind = features->free_aim_keybind;
            const bool key_active = bind.type == c_keybind::ALWAYS ? true : bind.enabled;
            if (!features->enable_free_aim || !key_active)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            if (!refresh_pf_state(state))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            const auto target_position = free_aim::get_target_world_position();
            if (!target_position)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            const auto part_position = state.part.get_position(state.part_primitive);
            if (!part_position)
            {
                refresh_pf_state(state);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (const auto rotation = build_camera_part_rotation(*part_position, *target_position))
            {
                write_rotation_matrix(state.part_primitive, *rotation);
            }

            std::this_thread::sleep_for(std::chrono::duration<float, std::milli>(jitter_delay_ms(rng)));
        }
    }
}

namespace pf_silent
{
    bool start()
    {
        if (running.load(std::memory_order_relaxed))
        {
            return true;
        }

        running = true;
        worker = std::thread(run_loop);
        worker.detach();
        return true;
    }

    void stop()
    {
        if (!running.exchange(false))
        {
            return;
        }
    }
}
