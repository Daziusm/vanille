#include "features/aimbot.h"

#include <atomic>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <thread>
#include <unordered_map>
#include <vector>
#include <DirectXMath.h>
#include <Windows.h>
#include <imgui.h>

#include "cache/local_player_cache.h"
#include "cache/player_cache.h"
#include "cache/team_utils.h"
#include "globals/globals_fixed.h"
#include "gui/overlay.hpp"
#include "features/visibility.h"
#include "sdk/camera.h"
#include "sdk/mouse.h"
#include "sdk/offsets.h"
#include "sdk/part.h"
#include "sdk/mesh_part.h"
#include "memory/memory.h"

namespace
{
    struct aim_target_state
    {
        std::uintptr_t player_address = 0;
        cache::primitive_part part{};
        rbx::Vector3 world_position{};
        rbx::Vector2 screen_position{};
        float screen_distance = 0.0f;
        bool has_screen = false;
    };

    cache::player_state make_dummy_player_state(const cache::dummy_state& dummy)
    {
        cache::player_state state{};
        state.address = dummy.address;
        state.name = !dummy.name.empty() ? dummy.name : "Dummy";
        state.display_name = state.name;
        state.health = dummy.health;
        state.max_health = dummy.max_health;
        state.character = dummy.character;
        state.humanoid = dummy.humanoid;
        state.parts = dummy.parts;
        state.body_effects = dummy.body_effects;
        state.team = static_cast<std::uintptr_t>(-1);
        return state;
    }

    enum class player_relation
    {
        neutral,
        friendly,
        enemy
    };
    constexpr std::int64_t lostfront_place_id = 102871156420149;

    player_relation determine_relation(const cache::player_state& player, std::uintptr_t local_team)
    {
        (void)local_team;
        const int status_override = vanille::overlay::get_player_status(player.user_id, player.name);
        if (status_override == 1)
        {
            return player_relation::enemy;
        }
        if (status_override == 2)
        {
            return player_relation::friendly;
        }

        return player_relation::neutral;
    }

    struct motion_sample
    {
        rbx::Vector3 position{};
        rbx::Vector3 velocity{};
        rbx::Vector2 screen{};
        double time = 0.0;
    };

    std::atomic<bool> running{ false };
    std::thread worker;
    std::mutex target_mutex;
    aim_target_state current_target{};
    std::atomic<std::uintptr_t> locked_player_address{ 0 };
    std::mutex chat_mutex;
    std::unordered_map<std::uintptr_t, motion_sample> motion_map;

    std::optional<rbx::Vector2> get_cursor_client_position(const rbx::Vector2& dims)
    {
        if (dims.x <= 0.0f || dims.y <= 0.0f)
        {
            return std::nullopt;
        }

        const HWND window = vanille::overlay::g_rbx_window;
        if (!window || !::IsWindow(window))
        {
            return std::nullopt;
        }

        POINT cursor{};
        if (!::GetCursorPos(&cursor))
        {
            return std::nullopt;
        }

        POINT client = cursor;
        if (!::ScreenToClient(window, &client))
        {
            return std::nullopt;
        }

        RECT client_rect{};
        if (!::GetClientRect(window, &client_rect))
        {
            return std::nullopt;
        }

        if (client_rect.right <= client_rect.left || client_rect.bottom <= client_rect.top)
        {
            return std::nullopt;
        }

        if (client.x < client_rect.left || client.x > client_rect.right ||
            client.y < client_rect.top || client.y > client_rect.bottom)
        {
            return std::nullopt;
        }

        const float x = std::clamp(static_cast<float>(client.x), 0.0f, dims.x);
        const float y = std::clamp(static_cast<float>(client.y), 0.0f, dims.y);
        return rbx::Vector2{ x, y };
    }

    void refresh_chat_instances()
    {
        std::lock_guard<std::mutex> lock(chat_mutex);
        if (!globals->text_chat_service.is_valid() && globals->datamodel.is_valid())
        {
            globals->text_chat_service = globals->datamodel.find_first_child("TextChatService");
        }

        if (!globals->chat_input_bar_configuration.is_valid() && globals->text_chat_service.is_valid())
        {
            globals->chat_input_bar_configuration = globals->text_chat_service.find_first_child("ChatInputBarConfiguration");
        }
    }

    bool chat_is_focused()
    {
        refresh_chat_instances();
        if (!globals->chat_input_bar_configuration.is_valid() || !roblox::offsets::chat::is_focused)
        {
            return false;
        }

        const auto addr = globals->chat_input_bar_configuration.get_address() + roblox::offsets::chat::is_focused;
        const std::uint32_t flags = memory->read<std::uint32_t>(addr);
        return (flags & 0x10000u) != 0;
    }

    void clear_target_locked()
    {
        current_target = {};
        locked_player_address.store(0, std::memory_order_relaxed);
    }

    void clear_target()
    {
        std::lock_guard<std::mutex> lock(target_mutex);
        clear_target_locked();
    }

    std::optional<aim_target_state> get_target_copy()
    {
        std::lock_guard<std::mutex> lock(target_mutex);
        if (!current_target.has_screen || current_target.player_address == 0)
        {
            return std::nullopt;
        }
        return current_target;
    }

    std::uintptr_t get_locked_player()
    {
        return locked_player_address.load(std::memory_order_relaxed);
    }

    bool is_local_reloading()
    {
        const auto local_snapshot = cache::localplayer->snapshot();
        if (local_snapshot.address == 0)
        {
            return false;
        }

        const std::uint64_t local_user_id = local_snapshot.user_id;
        const auto players_snapshot = cache::players_cache->snapshot();
        if (players_snapshot)
        {
            for (const auto& p : *players_snapshot)
            {
                if (p.address == local_snapshot.address || (local_user_id != 0 && p.user_id == local_user_id))
                {
                    return p.body_effects.reload;
                }
            }
        }

        return local_snapshot.body_effects.reload;
    }

    inline bool get_part_position(const cache::primitive_part& part, rbx::Vector3& out)
    {
        if (!part.instance.is_valid()) return false;
        auto pos = part.instance.get_position(part.primitive);
        if (!pos) return false;
        out = *pos;
        return true;
    }

    std::optional<rbx::Vector2> project_target_to_screen(const rbx::Vector3& world, const rbx::Matrix& view_matrix, const rbx::Vector2& dimensions, bool offscreen_check)
    {
        if (dimensions.x <= 0.0f || dimensions.y <= 0.0f)
        {
            return std::nullopt;
        }

        if (offscreen_check)
        {
            return rbx::camera::world_to_screen(world, view_matrix, dimensions);
        }

        const rbx::Vector4 clip = rbx::camera::transform(world, view_matrix);
        if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z) || !std::isfinite(clip.w))
        {
            return std::nullopt;
        }

        // Offscreen mode intentionally keeps points outside the view frustum (and behind camera)
        // so aimbot can continue rotating toward those targets.
        const float denom = (std::max)(std::fabs(clip.w), 1e-4f);
        const rbx::Vector3 ndc = rbx::Vector3(clip.x, clip.y, clip.z) / denom;
        if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z))
        {
            return std::nullopt;
        }

        rbx::Vector2 screen{
            (ndc.x + 1.0f) * 0.5f * dimensions.x,
            (1.0f - ndc.y) * 0.5f * dimensions.y
        };
        if (!std::isfinite(screen.x) || !std::isfinite(screen.y))
        {
            return std::nullopt;
        }

        return screen;
    }

    std::optional<aim_target_state> build_candidate(const cache::player_state& player, int hitbox_mode, bool nearest_part, bool limit_fov, float fov_radius, const rbx::Matrix& view, const rbx::Vector2& dimensions, const rbx::Vector2& reference, bool offscreen_check)
    {
        bool has_candidate = false;
        float best_distance = (std::numeric_limits<float>::max)();
        aim_target_state best{};

        auto consider_part = [&](const cache::primitive_part& part)
        {
            rbx::Vector3 world{};
            if (!get_part_position(part, world))
            {
                return;
            }

            const auto screen = project_target_to_screen(world, view, dimensions, offscreen_check);
            if (!screen)
            {
                return;
            }

            const float distance = (*screen - reference).Length();
            if (limit_fov && distance > fov_radius)
            {
                return;
            }

            if (distance < best_distance)
            {
                best_distance = distance;
                best.player_address = player.address;
                best.part = part;
                best.world_position = world;
                best.screen_position = *screen;
                best.screen_distance = distance;
                best.has_screen = true;
                has_candidate = true;
            }
        };

        if (nearest_part)
        {
            const cache::primitive_part* parts_to_test[] = {
                &player.parts.head, &player.parts.upper_torso, &player.parts.torso, &player.parts.humanoid_root_part, &player.parts.lower_torso,
                &player.parts.left_arm, &player.parts.right_arm, &player.parts.left_leg, &player.parts.right_leg,
                &player.parts.left_upper_arm, &player.parts.left_lower_arm, &player.parts.left_hand,
                &player.parts.right_upper_arm, &player.parts.right_lower_arm, &player.parts.right_hand,
                &player.parts.left_upper_leg, &player.parts.left_lower_leg, &player.parts.left_foot,
                &player.parts.right_upper_leg, &player.parts.right_lower_leg, &player.parts.right_foot
            };

            for (const auto* part : parts_to_test)
            {
                if (part)
                {
                    consider_part(*part);
                }
            }
        }
        else
        {
            const cache::primitive_part* primary = nullptr;
            switch (hitbox_mode)
            {
            case 1: primary = &player.parts.upper_torso; break;
            case 2: primary = &player.parts.humanoid_root_part; break;
            default: primary = &player.parts.head; break;
            }

            if (primary) consider_part(*primary);

            if (!has_candidate)
            {
                const cache::primitive_part* fallbacks[] = { &player.parts.head, &player.parts.upper_torso, &player.parts.humanoid_root_part, &player.parts.torso, &player.parts.lower_torso };
                for (const auto* fallback : fallbacks)
                {
                    if (!fallback) continue;
                    consider_part(*fallback);
                    if (has_candidate) break;
                }
            }
        }

        if (!has_candidate)
        {
            return std::nullopt;
        }

        return best;
    }

    bool player_blocked(const cache::player_state& player, const cache::local_player_state& local)
    {
        const player_relation relation = determine_relation(player, local.team);
        if (relation == player_relation::friendly)
        {
            return true;
        }

        if (features->aimbot_only_enemies && relation != player_relation::enemy)
        {
            return true;
        }
        if (features->aimbot_check_team && cache::team_utils::is_teammate(local, player))
        {
            return true;
        }
        if (features->aimbot_check_health && player.health <= 0.0f)
        {
            return true;
        }
        if (features->aimbot_check_knocked && player.body_effects.knocked)
        {
            return true;
        }
        if (features->aimbot_check_grabbed && player.body_effects.grabbed)
        {
            return true;
        }
        if (features->aimbot_check_reloading && player.body_effects.reload)
        {
            return true;
        }
        return false;
    }

    rbx::Vector2 get_reference_point(const rbx::Vector2& dimensions)
    {
        if (const auto cursor = get_cursor_client_position(dimensions))
        {
            return *cursor;
        }
        return dimensions * 0.5f;
    }

    double now_seconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void update_motion(std::uintptr_t part_address, const rbx::Vector3& position, const rbx::Vector2& screen, double timestamp, rbx::Vector3& out_velocity)
    {
        if (part_address == 0)
        {
            out_velocity = {};
            return;
        }

        auto it = motion_map.find(part_address);
        if (it != motion_map.end())
        {
            double dt = timestamp - it->second.time;
            if (dt > 0.0001)
            {
                out_velocity = (position - it->second.position) / static_cast<float>(dt);
                it->second.velocity = out_velocity;
            }
            else
            {
                out_velocity = it->second.velocity;
            }
            it->second.position = position;
            it->second.screen = screen;
            it->second.time = timestamp;
        }
        else
        {
            motion_sample sample{};
            sample.position = position;
            sample.velocity = {};
            sample.screen = screen;
            sample.time = timestamp;
            motion_map.emplace(part_address, sample);
            out_velocity = {};
        }
    }

    float compute_ballistic_offset(float distance, float div_y, float max_distance)
    {
        constexpr float min_start = 140.0f;
        constexpr float max_offset = 25.0f;

        float end = max_distance > 0.0f ? max_distance : 800.0f;
        end = std::clamp(end, 600.0f, 900.0f);
        const float start = (std::max)(min_start, end * 0.25f);
        float t = std::clamp((distance - start) / (end - start), 0.0f, 1.0f);
        t = std::pow(t, 1.15f);
        const float scale = 0.16f / div_y;
        const float offset = distance * scale * t;
        return std::clamp(offset, 0.0f, max_offset);
    }

    float compute_head_bias(const std::optional<float>& target_distance, const std::optional<rbx::Vector3>& local_origin, const rbx::Vector3& position)
    {
        float distance = 0.0f;
        if (target_distance)
        {
            distance = *target_distance;
        }
        else if (local_origin)
        {
            distance = (position - *local_origin).Length();
        }

        constexpr float near_bias = 0.35f;
        constexpr float far_bias = 0.95f;
        if (distance <= 0.0f)
        {
            return 0.55f;
        }

        const float t = std::clamp((distance - 100.0f) / 700.0f, 0.0f, 1.0f);
        return near_bias + (far_bias - near_bias) * t;
    }

    void compute_ballistic_dividers(float distance, float& out_div_xz, float& out_div_y)
    {
        constexpr float base_distance = 1000.0f;
        constexpr float min_distance = 60.0f;
        constexpr float base_div_xz = 3.5f;
        constexpr float base_div_y = 12.0f;
        constexpr float min_div_xz = 2.0f;
        constexpr float min_div_y = 7.0f;
        constexpr float max_div = 50.0f;

        const float clamped = (std::max)(distance, min_distance);
        const float scale = base_distance / clamped;
        out_div_xz = base_div_xz * scale;
        out_div_y = base_div_y * scale;

        constexpr float far_start = 600.0f;
        constexpr float far_end = 1400.0f;
        const float t = std::clamp((clamped - far_start) / (far_end - far_start), 0.0f, 1.0f);
        const float boost = 1.0f - 0.12f * t;
        out_div_xz *= boost;
        out_div_y *= boost;

        out_div_xz = std::clamp(out_div_xz, min_div_xz, max_div);
        out_div_y = std::clamp(out_div_y, min_div_y, max_div);
    }

    rbx::Vector3 apply_prediction(const rbx::Vector3& position, const rbx::Vector3& velocity, const std::optional<rbx::Vector3>& local_origin, const std::optional<float>& target_distance, bool prefer_head)
    {
        if (!features->enable_aimbot_prediction)
        {
            return position;
        }

        float div_xz = (std::max)(features->aimbot_prediction_x, 0.001f);
        float div_y  = (std::max)(features->aimbot_prediction_y, 0.001f);
        if (features->aimbot_prediction_mode == 1 && target_distance)
        {
            compute_ballistic_dividers(*target_distance, div_xz, div_y);
        }

        rbx::Vector3 offset{};
        offset.x = velocity.x / div_xz;
        offset.z = velocity.z / div_xz;
        offset.y = velocity.y / div_y;
        if (features->aimbot_prediction_mode == 1 && local_origin)
        {
            const float distance = target_distance.value_or((position - *local_origin).Length());
            if (distance > 0.01f)
            {
                offset.y += compute_ballistic_offset(distance, div_y, features->aimbot_max_distance);
            }
        }
        if (prefer_head)
        {
            offset.y += compute_head_bias(target_distance, local_origin, position);
        }
        return position + offset;
    }

    rbx::Vector3 rotate_local_point(const rbx::mesh_part::transform& tr, const rbx::Vector3& local)
    {
        if (!tr.has_rotation)
        {
            return local;
        }

        rbx::Vector3 out{};
        out.x = tr.rotation[0][0] * local.x + tr.rotation[0][1] * local.y + tr.rotation[0][2] * local.z;
        out.y = tr.rotation[1][0] * local.x + tr.rotation[1][1] * local.y + tr.rotation[1][2] * local.z;
        out.z = tr.rotation[2][0] * local.x + tr.rotation[2][1] * local.y + tr.rotation[2][2] * local.z;
        return out;
    }

    std::optional<rbx::Vector3> find_closest_point_on_part(
        const cache::primitive_part& part,
        const rbx::Vector3& prediction_offset,
        const rbx::Matrix& view_matrix,
        const rbx::Vector2& dimensions,
        const rbx::Vector2& cursor_reference,
        int samples_per_edge)
    {
        if (!part.instance.is_valid() || samples_per_edge < 2)
        {
            return std::nullopt;
        }

        const auto transform = rbx::mesh_part::get_transform(part.instance, part.primitive);
        if (!transform)
        {
            return std::nullopt;
        }

        const rbx::Vector3 size = part.size;
        if (!(std::isfinite(size.x) && std::isfinite(size.y) && std::isfinite(size.z)) ||
            size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f)
        {
            return std::nullopt;
        }

        auto get_component = [](const rbx::Vector3& vec, int axis) -> float
        {
            switch (axis)
            {
            case 0: return vec.x;
            case 1: return vec.y;
            case 2: return vec.z;
            default: return 0.0f;
            }
        };

        auto set_component = [](rbx::Vector3& vec, int axis, float value)
        {
            switch (axis)
            {
            case 0: vec.x = value; break;
            case 1: vec.y = value; break;
            case 2: vec.z = value; break;
            default: break;
            }
        };

        struct face_def
        {
            int normal_axis = 0;
            float offset_sign = 1.0f;
            int tangent1 = 0;
            int tangent2 = 0;
        };

        static constexpr face_def faces[] = {
            {0,  1.0f, 1, 2}, {0, -1.0f, 1, 2},
            {1,  1.0f, 0, 2}, {1, -1.0f, 0, 2},
            {2,  1.0f, 0, 1}, {2, -1.0f, 0, 1}
        };

        const float step = 1.0f / static_cast<float>(samples_per_edge - 1);
        const rbx::Vector3 half = size * 0.5f;
        const rbx::Vector3 origin = transform->position + prediction_offset;

        float best_dist_sq = (std::numeric_limits<float>::max)();
        rbx::Vector3 best_world{};

        for (const auto& face : faces)
        {
            const float offset = face.offset_sign * get_component(half, face.normal_axis);

            for (int i = 0; i < samples_per_edge; ++i)
            {
                for (int j = 0; j < samples_per_edge; ++j)
                {
                    rbx::Vector3 local_point{};
                    set_component(local_point, face.normal_axis, offset);

                    const float t1 = i * step * get_component(size, face.tangent1) - get_component(half, face.tangent1);
                    const float t2 = j * step * get_component(size, face.tangent2) - get_component(half, face.tangent2);
                    set_component(local_point, face.tangent1, t1);
                    set_component(local_point, face.tangent2, t2);

                    const rbx::Vector3 world_point = origin + rotate_local_point(*transform, local_point);
                    if (const auto screen = rbx::camera::world_to_screen(world_point, view_matrix, dimensions))
                    {
                        const float dx = cursor_reference.x - screen->x;
                        const float dy = cursor_reference.y - screen->y;
                        const float dist_sq = dx * dx + dy * dy;
                        if (dist_sq < best_dist_sq)
                        {
                            best_dist_sq = dist_sq;
                            best_world = world_point;
                        }
                    }
                }
            }
        }

        if (best_dist_sq < (std::numeric_limits<float>::max)())
        {
            return best_world;
        }

        return std::nullopt;
    }

    float apply_smoothing_ease(float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        float inv = 1.0f - t;
        return 1.0f - inv * inv * inv;
    }

    DirectX::XMFLOAT3X3 lerp_rotation(const DirectX::XMFLOAT3X3& from, const DirectX::XMFLOAT3X3& to, float alpha)
    {
        DirectX::XMFLOAT3X3 result{};
        float clamped = std::clamp(alpha, 0.0f, 1.0f);
        for (int i = 0; i < 3; ++i)
        {
            result.m[i][0] = from.m[i][0] + (to.m[i][0] - from.m[i][0]) * clamped;
            result.m[i][1] = from.m[i][1] + (to.m[i][1] - from.m[i][1]) * clamped;
            result.m[i][2] = from.m[i][2] + (to.m[i][2] - from.m[i][2]) * clamped;
        }
        return result;
    }

    void apply_camera_rotation(const rbx::Vector3& target_world, const cache::local_player_state& local, float dt)
    {
        if (!local.camera.is_valid())
        {
            return;
        }

        rbx::Vector3 camera_pos = local.camera.get_camera_position();
        rbx::Vector3 to_target = target_world - camera_pos;
        if (to_target.LengthSquared() < 1e-6f)
        {
            return;
        }

        DirectX::XMFLOAT3X3 final_rot = local.camera.look_at(target_world);

        bool smooth_enabled = features->enable_aimbot_smooth;
        auto clamp_smooth = [](float v) -> float
        {
            return std::clamp((std::max)(v, 1.0f), 1.0f, 100.0f);
        };
        const float smooth_x = clamp_smooth(features->aimbot_smooth_x);
        const float smooth_y = clamp_smooth(features->aimbot_smooth_y);
        float smooth_value = (std::max)(smooth_x, smooth_y);
        smooth_value = std::clamp(smooth_value, 1.0f, 100.0f);
        const bool smoothing_active = smooth_enabled && smooth_value > 1.0f;

        if (!smoothing_active)
        {
            local.camera.set_rotation(final_rot);
        }
        else
        {
            DirectX::XMFLOAT3X3 current_rot = local.camera.get_rotation();
            auto forward_from_matrix = [](const DirectX::XMFLOAT3X3& m) -> rbx::Vector3
            {
                return rbx::Vector3{ -m._13, -m._23, -m._33 };
            };

            auto normalize_forward = [](rbx::Vector3 dir) -> rbx::Vector3
            {
                if (dir.LengthSquared() > 1e-6f)
                {
                    dir.Normalize();
                    return dir;
                }
                return {};
            };

            rbx::Vector3 current_forward = normalize_forward(forward_from_matrix(current_rot));
            rbx::Vector3 desired_forward = normalize_forward(to_target);

            if (current_forward.LengthSquared() < 1e-6f || desired_forward.LengthSquared() < 1e-6f)
            {
                local.camera.set_rotation(final_rot);
                return;
            }

            auto to_angles = [](const rbx::Vector3& dir) -> std::pair<float, float>
            {
                const float yaw = std::atan2(dir.x, dir.z);
                const float pitch = std::atan2(dir.y, std::sqrt(dir.x * dir.x + dir.z * dir.z));
                return { yaw, pitch };
            };

            auto wrap_delta = [](float delta) -> float
            {
                const float pi = DirectX::XM_PI;
                const float two_pi = DirectX::XM_2PI;
                while (delta > pi) delta -= two_pi;
                while (delta < -pi) delta += two_pi;
                return delta;
            };

            const auto [current_yaw, current_pitch] = to_angles(current_forward);
            const auto [desired_yaw, desired_pitch] = to_angles(desired_forward);
            const float yaw_delta = std::abs(wrap_delta(desired_yaw - current_yaw));
            const float pitch_delta = std::abs(wrap_delta(desired_pitch - current_pitch));
            const float delta_sum = yaw_delta + pitch_delta;

            float effective_smooth = smooth_value;
            if (delta_sum > 1e-6f)
            {
                const float weighted = (yaw_delta * smooth_x + pitch_delta * smooth_y) / delta_sum;
                effective_smooth = std::clamp(weighted, 1.0f, 100.0f);
            }

            float responsiveness = 50.0f / (effective_smooth + 1.0f);
            responsiveness = std::clamp(responsiveness, 0.50f, 50.0f);
            float base_alpha = 1.0f - std::exp(-dt * responsiveness);
            float alpha = apply_smoothing_ease(base_alpha);

            DirectX::XMFLOAT3X3 smooth_rot = lerp_rotation(current_rot, final_rot, alpha);
            local.camera.set_rotation(smooth_rot);
        }
    }

    void apply_mouse_lock(const rbx::Vector2& target, const rbx::Vector2& dimensions, bool valid_target, float dt)
    {
        static rbx::Vector2 carry{};
        static rbx::Vector2 velocity{};

        if (!valid_target)
        {
            velocity = {};
            carry = {};
            return;
        }

        auto cursor = get_cursor_client_position(dimensions);
        if (!cursor)
        {
            return;
        }

        rbx::Vector2 delta = target - *cursor;
        const float delta_len_sq = delta.x * delta.x + delta.y * delta.y;
        if (delta_len_sq < 0.25f)
        {
            velocity = {};
            return;
        }

        rbx::Vector2 step = delta;
        if (features->enable_aimbot_smooth)
        {
            float smooth_x = (std::max)(features->aimbot_smooth_x, 1.0f);
            float smooth_y = (std::max)(features->aimbot_smooth_y, 1.0f);
            float smooth_value = (smooth_x + smooth_y) * 0.5f;

            float base_rate = dt > 1e-5f ? (1.0f / dt) : 60.0f;
            float follow_rate = base_rate / (smooth_value + 5.0f);
            follow_rate = std::clamp(follow_rate, 2.0f, 60.0f);
            float alpha = 1.0f - std::exp(-(std::max)(dt, 0.0005f) * follow_rate);

            rbx::Vector2 desired{};
            desired.x = delta.x / smooth_x;
            desired.y = delta.y / smooth_y;

            velocity.x = velocity.x * 0.2f + desired.x * 0.8f;
            velocity.y = velocity.y * 0.2f + desired.y * 0.8f;

            step.x = velocity.x * alpha;
            step.y = velocity.y * alpha;
        }
        else
        {
            velocity = {};
        }

        carry.x += step.x;
        carry.y += step.y;

        LONG dx = static_cast<LONG>(std::round(carry.x));
        LONG dy = static_cast<LONG>(std::round(carry.y));
        carry.x -= static_cast<float>(dx);
        carry.y -= static_cast<float>(dy);

        const float max_step = 250.0f;
        float len_sq = static_cast<float>(dx) * static_cast<float>(dx) + static_cast<float>(dy) * static_cast<float>(dy);
        if (len_sq > max_step * max_step)
        {
            float len = std::sqrt(len_sq);
            float scale = max_step / len;
            dx = static_cast<LONG>(std::round(static_cast<float>(dx) * scale));
            dy = static_cast<LONG>(std::round(static_cast<float>(dy) * scale));
            carry = {};
        }

        if (dx == 0 && dy == 0)
        {
            return;
        }

        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = dx;
        input.mi.dy = dy;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(INPUT));
    }

    void run_loop()
    {
        std::uintptr_t last_locked_address = 0;
        std::uintptr_t last_part_address = 0;
        auto t_last = std::chrono::steady_clock::now();

        while (running.load(std::memory_order_relaxed))
        {
            auto t_now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(t_now - t_last).count();
            t_last = t_now;
            if (!std::isfinite(dt) || dt < 0.0f)
            {
                dt = 0.0f;
            }

            const HWND foreground = ::GetForegroundWindow();
            const bool roblox_active = (foreground == vanille::overlay::g_rbx_window);

            auto& keybind = features->aimbot_keybind;
            if (features->aimbot_check_typing)
            {
                const bool chat_focused = chat_is_focused();
                if (!chat_focused && roblox_active && !keybind.waiting_for_input)
                {
                    keybind.update();
                }
                else if ((chat_focused || !roblox_active) && !keybind.waiting_for_input && keybind.key)
                {
                    ::GetAsyncKeyState(keybind.key);
                }
            }
            else if (!keybind.waiting_for_input)
            {
                keybind.update();
            }
            const bool key_active = keybind.type == c_keybind::ALWAYS ? true : keybind.enabled;

            if (!features->enable_aimbot || !key_active)
            {
                clear_target();
                last_locked_address = 0;
                last_part_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (!roblox_active)
            {
                clear_target();
                last_locked_address = 0;
                last_part_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            const auto local = cache::localplayer->snapshot();
            if (features->aimbot_check_reloading && is_local_reloading())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (local.address == 0 || !local.camera.is_valid())
            {
                clear_target();
                last_locked_address = 0;
                last_part_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            rbx::visualengine_t visual(globals->visualengine.get_address());
            auto view_matrix = visual.get_view_matrix();
            auto dimensions = visual.get_dimensions();
            if (!view_matrix || !dimensions)
            {
                clear_target();
                last_locked_address = 0;
                last_part_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            const bool limit_fov = features->aimbot_limit_fov && features->aimbot_fov_radius > 0.0f;
            const rbx::Vector2 reference = get_reference_point(*dimensions);

            std::optional<rbx::Vector3> local_origin;
            rbx::Vector3 origin_temp{};
            if (get_part_position(local.parts.humanoid_root_part, origin_temp))
            {
                local_origin = origin_temp;
            }
            else if (local.camera.is_valid())
            {
                local_origin = local.camera.get_camera_position();
            }
            std::optional<rbx::Vector3> prediction_origin;
            if (local.camera.is_valid())
            {
                prediction_origin = local.camera.get_camera_position();
            }
            else
            {
                prediction_origin = local_origin;
            }

            const float max_distance = features->aimbot_max_distance;
            auto in_distance = [&](const aim_target_state& candidate) -> bool
            {
                if (max_distance <= 0.0f || !local_origin)
                {
                    return true;
                }
                return (candidate.world_position - *local_origin).Length() <= max_distance;
            };

            const auto players_snapshot = cache::players_cache->snapshot();
            const auto dummy = cache::players_cache->dummy_snapshot();
            cache::player_state dummy_player{};
            bool has_dummy = false;
            if (dummy && dummy->address != 0)
            {
                dummy_player = make_dummy_player_state(*dummy);
                has_dummy = true;
            }

            auto for_each_player = [&](auto&& fn) -> bool
            {
                if (players_snapshot)
                {
                    for (const auto& player : *players_snapshot)
                    {
                        if (!fn(player))
                        {
                            return false;
                        }
                    }
                }
                if (has_dummy)
                {
                    if (!fn(dummy_player))
                    {
                        return false;
                    }
                }
                return true;
            };
            const auto last_target_snapshot = get_target_copy();
            const bool visibility_enabled = visibility::can_run_visibility_check(features->aimbot_visibility_check);

        aim_target_state best{};
        float best_distance = (std::numeric_limits<float>::max)();
        bool target_visible = false;
        bool preserve_locked = false;

        if (features->aimbot_sticky && last_locked_address != 0)
        {
            bool locked_valid = false;
            bool locked_present = false;
            bool locked_blocked = false;

            for_each_player([&](const cache::player_state& player)
                {
                    if (player.address != last_locked_address)
                    {
                        return true;
                    }

                    locked_present = true;
                    if (player_blocked(player, local))
                    {
                        locked_blocked = true;
                        return false;
                    }

                    const auto candidate = build_candidate(
                        player,
                        features->aimbot_hitbox,
                        features->aimbot_nearest_part,
                        limit_fov,
                        features->aimbot_fov_radius,
                        *view_matrix,
                        *dimensions,
                        reference,
                        features->aimbot_offscreen_check);

                    if (candidate && in_distance(*candidate))
                    {
                        if (visibility_enabled)
                        {
                            const auto vis = visibility::is_player_visible(player, local, *view_matrix);
                            if (!vis.visible)
                            {
                                return false;
                            }
                        }

                        best = *candidate;
                        best_distance = candidate->screen_distance;
                        locked_valid = true;
                        target_visible = true;
                    }

                    return false;
                });

            if (locked_valid && best.has_screen)
            {
                goto finish_candidate_selection;
            }

            if (locked_blocked)
            {
                preserve_locked = true;
                if (last_target_snapshot && last_target_snapshot->player_address == last_locked_address && in_distance(*last_target_snapshot))
                {
                    best = *last_target_snapshot;
                    target_visible = false;
                }
                goto finish_candidate_selection;
            }

            if (locked_present)
            {
                if (last_target_snapshot && last_target_snapshot->player_address == last_locked_address && in_distance(*last_target_snapshot))
                {
                    best = *last_target_snapshot;
                    best_distance = last_target_snapshot->screen_distance;
                    target_visible = false;
                }
                goto finish_candidate_selection;
            }

            last_locked_address = 0;
        }

        if (!features->aimbot_sticky || last_locked_address == 0)
        {
            for_each_player([&](const cache::player_state& player)
                {
                    if (player.address == 0 || player.address == local.address)
                    {
                        return true;
                    }

                    if (player_blocked(player, local))
                    {
                        return true;
                    }

                    const auto candidate = build_candidate(
                        player,
                        features->aimbot_hitbox,
                        features->aimbot_nearest_part,
                        limit_fov,
                        features->aimbot_fov_radius,
                        *view_matrix,
                        *dimensions,
                        reference,
                        features->aimbot_offscreen_check);

                    if (!candidate || !in_distance(*candidate))
                    {
                        return true;
                    }

                    if (visibility_enabled)
                    {
                        const auto vis = visibility::is_player_visible(player, local, *view_matrix);
                        if (!vis.visible)
                        {
                            return true;
                        }
                    }

                    if (candidate->screen_distance < best_distance)
                    {
                        best = *candidate;
                        best_distance = candidate->screen_distance;
                        target_visible = true;
                    }

                    return true;
                });
        }

        finish_candidate_selection:
            {
                std::lock_guard<std::mutex> lock(target_mutex);
                if (best.has_screen && best.player_address != 0 && in_distance(best))
                {
                    current_target = best;
                    locked_player_address.store(best.player_address, std::memory_order_relaxed);
                    last_locked_address = best.player_address;
                    last_part_address = best.part.instance.get_address();
                    if (last_part_address == 0)
                    {
                        last_part_address = best.part.primitive;
                    }
                }
                else
                {
                    if (!preserve_locked)
                    {
                        clear_target_locked();
                        locked_player_address.store(0, std::memory_order_relaxed);
                        last_locked_address = 0;
                        last_part_address = 0;
                    }
                }
            }

            bool target_blocked = false;
            if (best.player_address != 0)
            {
                for_each_player([&](const cache::player_state& player)
                    {
                        if (player.address == best.player_address)
                        {
                            if (player_blocked(player, local))
                            {
                                target_blocked = true;
                            }
                            return false;
                        }
                        return true;
                    });
            }

            if (!best.has_screen || best.player_address == 0 || target_blocked)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            const rbx::Vector3 position = best.world_position;
            const std::uintptr_t primitive_addr = best.part.primitive ? best.part.primitive : best.part.instance.get_address();
            rbx::Vector3 velocity{};

            if (primitive_addr)
            {
                if (const auto live_vel = rbx::part::get_linear_velocity(primitive_addr))
                {
                    velocity = *live_vel;
                }
            }
            if (velocity.LengthSquared() < 1e-6f)
            {
                update_motion(primitive_addr, position, best.screen_position, now_seconds(), velocity);
            }
            std::optional<float> target_distance;
            bool target_is_head = (!features->aimbot_nearest_part && features->aimbot_hitbox == 0);
            if (prediction_origin)
            {
                auto compute_distance = [&](const cache::player_state& player) -> std::optional<float>
                {
                    rbx::Vector3 head_pos{};
                    if (best.part.instance.is_valid() && player.parts.head.instance.is_valid() &&
                        player.parts.head.instance.get_address() == best.part.instance.get_address())
                    {
                        target_is_head = true;
                    }
                    if (get_part_position(player.parts.head, head_pos))
                    {
                        return (head_pos - *prediction_origin).Length();
                    }
                    return std::nullopt;
                };

                if (players_snapshot)
                {
                    for (const auto& player : *players_snapshot)
                    {
                        if (player.address == best.player_address)
                        {
                            target_distance = compute_distance(player);
                            break;
                        }
                    }
                }
                if (!target_distance && has_dummy && dummy_player.address == best.player_address)
                {
                    target_distance = compute_distance(dummy_player);
                }
                if (!target_distance)
                {
                    target_distance = (position - *prediction_origin).Length();
                }
            }

            rbx::Vector3 predicted = apply_prediction(position, velocity, prediction_origin, target_distance, target_is_head);
            const rbx::Vector3 prediction_offset = predicted - position;
            if (features->enable_aimbot_closest_point)
            {
                if (auto closest = find_closest_point_on_part(best.part, prediction_offset, *view_matrix, *dimensions, reference, 10))
                {
                    predicted = *closest;
                }
            }

            auto predicted_screen = rbx::camera::world_to_screen(predicted, *view_matrix, *dimensions);
            rbx::Vector2 target_point = best.screen_position;
            if (predicted_screen)
            {
                target_point = *predicted_screen;
            }

            float dt_mouse = dt;
            const float io_fps = ImGui::GetIO().Framerate;
            if (io_fps > 1.0f)
            {
                dt_mouse = 1.0f / io_fps;
            }
            dt_mouse = (std::max)(dt_mouse, 0.0001f);

            if (features->aimbot_mode == 1)
            {
                apply_mouse_lock(target_point, *dimensions, target_visible, dt_mouse);
            }
            else
            {
                if (target_visible)
                {
                    apply_camera_rotation(predicted, local, dt);
                }
            }

            bool should_sleep = true;
            if (features->aimbot_mode == 0)
            {
                should_sleep = false;
            }
            else if (features->enable_aimbot_smooth && features->aimbot_mode == 1)
            {
                should_sleep = false;
            }
            if (should_sleep)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
}

namespace aimbot
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

    bool is_locked_target(std::uintptr_t player_address)
    {
        if (player_address == 0)
        {
            return false;
        }

        const auto target = get_target_copy();
        return target && target->player_address == player_address;
    }

    std::uintptr_t get_locked_player()
    {
        return locked_player_address.load(std::memory_order_relaxed);
    }
}
