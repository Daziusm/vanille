#include "features/free_aim.h"

#include <atomic>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
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
#include "sdk/mesh_part.h"
#include "memory/memory.h"
#include "utils/logger.h"
#include "features/aimbot.h"

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
    constexpr std::int64_t phantom_forces_place_id = 292439477;

    bool is_phantom_forces_silent_mode()
    {
        return globals && globals->game_id == phantom_forces_place_id;
    }

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

        if (is_phantom_forces_silent_mode() && player.pf_enemy_known)
        {
            return player.pf_enemy ? player_relation::enemy : player_relation::friendly;
        }

        return player_relation::neutral;
    }

    std::atomic<bool> running{ false };
    std::thread worker;
    std::thread mouse_spoof_worker;
    std::mutex target_mutex;
    aim_target_state current_target{};
    std::atomic<std::uintptr_t> locked_player_address{ 0 };
    std::mutex chat_mutex;
    std::atomic<bool> block_aim_gui_writes{ false };

    struct udim_data
    {
        float scale = 0.0f;
        std::int32_t offset = 0;
    };

    std::optional<udim_data> read_frame_dim(const rbx::instance_t& gui, std::uintptr_t offset)
    {
        if (!gui.is_valid() || offset == 0)
        {
            return std::nullopt;
        }
        return memory->read<udim_data>(gui.get_address() + offset);
    }

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

    inline bool get_part_position(const cache::primitive_part& part, rbx::Vector3& out)
    {
        if (!part.instance.is_valid()) return false;
        auto pos = part.instance.get_position(part.primitive);
        if (!pos) return false;
        out = *pos;
        return true;
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
        constexpr float base_distance = 500.0f;
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
        if (!features->free_aim_enable_prediction)
        {
            return position;
        }

        float div_xz = (std::max)(features->free_aim_prediction_x, 0.001f);
        float div_y = (std::max)(features->free_aim_prediction_y, 0.001f);
        if (features->free_aim_prediction_mode == 1 && target_distance)
        {
            compute_ballistic_dividers(*target_distance, div_xz, div_y);
        }

        rbx::Vector3 offset{};
        offset.x = velocity.x / div_xz;
        offset.z = velocity.z / div_xz;
        offset.y = velocity.y / div_y;
        if (features->free_aim_prediction_mode == 1 && local_origin)
        {
            const float distance = target_distance.value_or((position - *local_origin).Length());
            if (distance > 0.01f)
            {
                offset.y += compute_ballistic_offset(distance, div_y, features->free_aim_max_distance);
            }
        }
        if (prefer_head)
        {
            offset.y += compute_head_bias(target_distance, local_origin, position);
        }
        return position + offset;
    }

    rbx::Vector3 get_velocity(const cache::primitive_part& part)
    {
        if (!part.primitive || !roblox::offsets::base_part::assembly_linear_velocity)
        {
            return {};
        }
        return memory->read<rbx::Vector3>(part.primitive + roblox::offsets::base_part::assembly_linear_velocity);
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

        constexpr float inset_ratio = 0.375f;
        rbx::Vector3 inset_size{
            (std::max)(size.x * inset_ratio, 0.001f),
            (std::max)(size.y * inset_ratio, 0.001f),
            (std::max)(size.z * inset_ratio, 0.001f)
        };

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

        const rbx::Vector3 half = inset_size * 0.5f;
        const rbx::Vector3 origin = transform->position + prediction_offset;

        float best_dist_sq = (std::numeric_limits<float>::max)();
        rbx::Vector3 best_world{};

        for (const auto& face : faces)
        {
            const float offset = face.offset_sign * get_component(half, face.normal_axis);

            float min_x = (std::numeric_limits<float>::max)();
            float min_y = (std::numeric_limits<float>::max)();
            float max_x = -(std::numeric_limits<float>::max)();
            float max_y = -(std::numeric_limits<float>::max)();
            const float u_corners[2] = { 0.0f, 1.0f };
            const float v_corners[2] = { 0.0f, 1.0f };
            for (float u_c : u_corners)
            {
                for (float v_c : v_corners)
                {
                    rbx::Vector3 local_corner{};
                    set_component(local_corner, face.normal_axis, offset);
                    const float t1c = u_c * get_component(inset_size, face.tangent1) - get_component(half, face.tangent1);
                    const float t2c = v_c * get_component(inset_size, face.tangent2) - get_component(half, face.tangent2);
                    set_component(local_corner, face.tangent1, t1c);
                    set_component(local_corner, face.tangent2, t2c);

                    const rbx::Vector3 world_corner = origin + rotate_local_point(*transform, local_corner);
                    if (const auto screen_corner = rbx::camera::world_to_screen(world_corner, view_matrix, dimensions))
                    {
                        min_x = (std::min)(min_x, screen_corner->x);
                        max_x = (std::max)(max_x, screen_corner->x);
                        min_y = (std::min)(min_y, screen_corner->y);
                        max_y = (std::max)(max_y, screen_corner->y);
                    }
                }
            }

            constexpr float u_min = 0.0f;
            constexpr float u_max = 1.0f;
            constexpr float v_min = 0.0f;
            constexpr float v_max = 1.0f;

            const float step_u = (u_max - u_min) / static_cast<float>(samples_per_edge - 1);
            const float step_v = (v_max - v_min) / static_cast<float>(samples_per_edge - 1);

            for (int i = 0; i < samples_per_edge; ++i)
            {
                for (int j = 0; j < samples_per_edge; ++j)
                {
                    rbx::Vector3 local_point{};
                    set_component(local_point, face.normal_axis, offset);

                    const float u = u_min + i * step_u;
                    const float v = v_min + j * step_v;
                    const float t1 = u * get_component(inset_size, face.tangent1) - get_component(half, face.tangent1);
                    const float t2 = v * get_component(inset_size, face.tangent2) - get_component(half, face.tangent2);
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

    std::optional<aim_target_state> build_candidate(const cache::player_state& player, int hitbox_mode, bool nearest_part, bool limit_fov, float fov_radius, const rbx::Matrix& view, const rbx::Vector2& dimensions, const rbx::Vector2& reference, const std::optional<rbx::Vector3>& local_origin)
    {
        bool has_candidate = false;
        float best_distance = (std::numeric_limits<float>::max)();
        rbx::Vector2 best_screen{};
        rbx::Vector3 best_world{};
        cache::primitive_part best_part{};
        std::optional<float> head_distance;
        if (local_origin)
        {
            rbx::Vector3 head_pos{};
            if (get_part_position(player.parts.head, head_pos))
            {
                head_distance = (head_pos - *local_origin).Length();
            }
        }

        auto consider_part = [&](const cache::primitive_part& part)
            {
                rbx::Vector3 world{};
                if (!get_part_position(part, world))
                {
                    return;
                }

                const rbx::Vector3 velocity = get_velocity(part);
                bool prefer_head = (!nearest_part && hitbox_mode == 0);
                if (!prefer_head && part.instance.is_valid() && player.parts.head.instance.is_valid() &&
                    player.parts.head.instance.get_address() == part.instance.get_address())
                {
                    prefer_head = true;
                }
                std::optional<float> target_distance = head_distance;
                if (!target_distance && local_origin)
                {
                    target_distance = (world - *local_origin).Length();
                }
                world = apply_prediction(world, velocity, local_origin, target_distance, prefer_head);

                const auto screen = rbx::camera::world_to_screen(world, view, dimensions);
                if (!screen)
                {
                    return;
                }

                const float distance = (*screen - reference).Length();
                if (distance < best_distance)
                {
                    best_distance = distance;
                    best_screen = *screen;
                    best_world = world;
                    best_part = part;
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

        if (limit_fov && best_distance > fov_radius)
        {
            return std::nullopt;
        }

        aim_target_state result{};
        result.player_address = player.address;
        result.part = best_part;
        result.world_position = best_world;
        result.screen_position = best_screen;
        result.screen_distance = best_distance;
        result.has_screen = true;
        return result;
    }

    std::optional<aim_target_state> get_target_copy()
    {
        std::lock_guard<std::mutex> lock(target_mutex);
        if (current_target.player_address == 0)
        {
            return std::nullopt;
        }

        if (!current_target.has_screen && !is_phantom_forces_silent_mode())
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

    void run_mouse_spoof()
    {
        std::uint64_t last_spoof_pos_x = 0;
        std::uint64_t last_spoof_pos_y = 0;

        while (running.load(std::memory_order_relaxed))
        {
            if (is_phantom_forces_silent_mode() || features->free_aim_silent_mode == 1)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (!features->enable_free_aim || !features->free_aim_mouse_spoof)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            const bool right_click_down = (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            const bool has_target = get_target_copy().has_value();

            rbx::visualengine_t visual(globals->visualengine.get_address());
            const auto screen = visual.get_dimensions();
            if (!screen)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            const float screen_x = screen->x;
            const float screen_y = screen->y;

            bool have_pos = false;
            if (right_click_down)
            {
                const float center_x = screen_x * 0.5f;
                const float center_y = std::clamp(screen_y * 0.5f - 58.0f, 0.0f, screen_y);
                last_spoof_pos_x = static_cast<std::uint64_t>(center_x);
                last_spoof_pos_y = static_cast<std::uint64_t>(center_y);
                have_pos = true;
            }
            else if (const auto cursor = get_cursor_client_position(*screen))
            {
                const float adj_y = std::clamp(cursor->y - 58.0f, 0.0f, screen_y);
                last_spoof_pos_x = static_cast<std::uint64_t>(cursor->x);
                last_spoof_pos_y = static_cast<std::uint64_t>(adj_y);
                have_pos = true;
            }

            if (have_pos)
            {
                const auto local = cache::localplayer->snapshot();
                if (local.address != 0 && local.aim_gui.is_valid() && !block_aim_gui_writes.load(std::memory_order_relaxed))
                {
                    const bool ok_x = local.aim_gui.set_frame_pos_x(last_spoof_pos_x);
                    const bool ok_y = local.aim_gui.set_frame_pos_y(last_spoof_pos_y);
                    if (!ok_x || !ok_y)
                    {
                        block_aim_gui_writes.store(true, std::memory_order_relaxed);
                        logger_core::log_warning("disabled aim_gui writes after failure (spoof loop)");
                    }
                }
            }
        }
    }

    void run_loop()
    {
        std::uintptr_t last_locked_address = 0;
        bool last_viewport_mode = false;

        while (running.load(std::memory_order_relaxed))
        {
            auto& keybind = features->free_aim_keybind;
            if (features->free_aim_check_typing)
            {
                const bool chat_focused = chat_is_focused();
                const HWND foreground = ::GetForegroundWindow();
                const bool roblox_active = (foreground == vanille::overlay::g_rbx_window);
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
            const auto local = cache::localplayer->snapshot();
            const bool pf_camera_mode = is_phantom_forces_silent_mode();
            const bool viewport_mode = features->free_aim_silent_mode == 1 && !pf_camera_mode;
            if (last_viewport_mode && !viewport_mode && local.camera.is_valid())
            {
                rbx::visualengine_t visual(globals->visualengine.get_address());
                if (const auto dims = visual.get_dimensions())
                {
                    rbx::camera::set_viewport(local.camera.get_address(), rbx::camera::viewport_from_size(*dims));
                }
            }
            last_viewport_mode = viewport_mode;
            if (!features->enable_free_aim || !key_active)
            {
                if (viewport_mode && local.camera.is_valid())
                {
                    rbx::visualengine_t visual(globals->visualengine.get_address());
                    if (const auto dims = visual.get_dimensions())
                    {
                        rbx::camera::set_viewport(local.camera.get_address(), rbx::camera::viewport_from_size(*dims));
                    }
                }
                clear_target();
                locked_player_address.store(0, std::memory_order_relaxed);
                last_locked_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (features->aimbot_check_reloading && is_local_reloading())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (local.address == 0 && !(pf_camera_mode && local.camera.is_valid()))
            {
                if (viewport_mode && local.camera.is_valid())
                {
                    rbx::visualengine_t visual(globals->visualengine.get_address());
                    if (const auto dims = visual.get_dimensions())
                    {
                        rbx::camera::set_viewport(local.camera.get_address(), rbx::camera::viewport_from_size(*dims));
                    }
                }
                clear_target();
                last_locked_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            const std::uintptr_t local_team = local.team;

            rbx::visualengine_t visual(globals->visualengine.get_address());
            auto view_matrix = visual.get_view_matrix();
            auto dimensions = visual.get_dimensions();
            if (!view_matrix || !dimensions)
            {
                clear_target();
                last_locked_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            const rbx::Vector2 reference = pf_camera_mode
                ? (*dimensions * 0.5f)
                : get_cursor_client_position(*dimensions).value_or(*dimensions * 0.5f);
            const bool limit_fov = features->free_aim_limit_fov && features->free_aim_fov_radius > 0.0f;

            std::optional<rbx::Vector3> local_origin;
            rbx::Vector3 origin_temp{};
            if (local.camera.is_valid())
            {
                local_origin = local.camera.get_camera_position();
            }
            else if (get_part_position(local.parts.humanoid_root_part, origin_temp))
            {
                local_origin = origin_temp;
            }
            std::optional<rbx::Vector3> prediction_origin = local_origin;
            if (local.camera.is_valid())
            {
                prediction_origin = local.camera.get_camera_position();
            }

            const float max_distance = features->free_aim_max_distance;
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
            const bool visibility_enabled = visibility::can_run_visibility_check(features->free_aim_visibility_check);

            aim_target_state best{};
            float best_distance = (std::numeric_limits<float>::max)();
            bool target_visible = false;
            bool preserve_locked = false;
            bool on_screen = false;

            auto is_blocked = [&](const cache::player_state& player) -> bool
                {
                    const player_relation relation = determine_relation(player, local_team);
                    if (relation == player_relation::friendly)
                    {
                        return true;
                    }
                    if (features->free_aim_only_enemies && relation != player_relation::enemy)
                    {
                        if (is_phantom_forces_silent_mode())
                        {
                            if (player.pf_enemy_known)
                            {
                                return true;
                            }
                            if (cache::team_utils::is_teammate(local, player))
                            {
                                return true;
                            }
                            return false;
                        }
                        return true;
                    }
                    if (features->free_aim_check_team && cache::team_utils::is_teammate(local, player))
                    {
                        return true;
                    }
                    if (features->free_aim_check_health && player.health <= 0.0f)
                    {
                        return true;
                    }
                    if (features->free_aim_check_knocked && player.body_effects.knocked)
                    {
                        return true;
                    }
                    if (features->free_aim_check_grabbed && player.body_effects.grabbed)
                    {
                        return true;
                    }
                    return false;
                };

            if (features->free_aim_sticky && last_locked_address != 0)
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
                        if (is_blocked(player))
                        {
                            locked_blocked = true;
                            return false;
                        }

                        const auto candidate = build_candidate(
                            player,
                            features->free_aim_hitbox,
                            features->free_aim_nearest_part,
                            limit_fov,
                            features->free_aim_fov_radius,
                            *view_matrix,
                            *dimensions,
                            reference,
                            prediction_origin);

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

            if (!features->free_aim_sticky || last_locked_address == 0)
            {
                for_each_player([&](const cache::player_state& player)
                    {
                        if (player.address == 0 || player.address == local.address)
                        {
                            return true;
                        }

                        if (is_blocked(player))
                        {
                            return true;
                        }

                        const auto candidate = build_candidate(
                            player,
                            features->free_aim_hitbox,
                            features->free_aim_nearest_part,
                            limit_fov,
                            features->free_aim_fov_radius,
                            *view_matrix,
                            *dimensions,
                            reference,
                            prediction_origin);

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
            if (features->enable_free_aim_closest_point && best.has_screen)
            {
                rbx::Vector3 base_position{};
                if (get_part_position(best.part, base_position))
                {
                    const rbx::Vector3 prediction_offset = best.world_position - base_position;
                    if (auto closest = find_closest_point_on_part(best.part, prediction_offset, *view_matrix, *dimensions, reference, 80))
                    {
                        if (const auto screen = rbx::camera::world_to_screen(*closest, *view_matrix, *dimensions))
                        {
                            best.world_position = *closest;
                            best.screen_position = *screen;
                        }
                    }
                }
            }

            if (best.has_screen)
            {
                on_screen =
                    best.screen_position.x >= 0.0f && best.screen_position.y >= 0.0f &&
                    best.screen_position.x <= dimensions->x && best.screen_position.y <= dimensions->y;
            }

            {
                std::lock_guard<std::mutex> lock(target_mutex);
                if (best.player_address != 0 && in_distance(best) && (best.has_screen || pf_camera_mode))
                {
                    current_target = best;
                    last_locked_address = best.player_address;
                    locked_player_address.store(best.player_address, std::memory_order_relaxed);
                }
                else if (!preserve_locked)
                {
                    clear_target_locked();
                    last_locked_address = 0;
                }
            }

            if (!on_screen && !pf_camera_mode)
            {
                clear_target();
                locked_player_address.store(0, std::memory_order_relaxed);
                last_locked_address = 0;
                target_visible = false;
                best.has_screen = false;
                best.player_address = 0;
            }

            bool target_blocked = false;
            if (best.player_address != 0)
            {
                for_each_player([&](const cache::player_state& player)
                    {
                        if (player.address == best.player_address)
                        {
                            if (is_blocked(player))
                            {
                                target_blocked = true;
                            }
                            return false;
                        }
                        return true;
                    });
            }

            if (pf_camera_mode)
            {
                if (!features->enable_aimbot && local.camera.is_valid() &&
                    best.player_address != 0 && !target_blocked)
                {
                    const DirectX::XMFLOAT3X3 rotation = local.camera.look_at(best.world_position);
                    for (int i = 0; i < 16; ++i)
                    {
                        local.camera.set_rotation(rotation);
                    }
                }
                continue;
            }

            const bool active_target = best.has_screen && target_visible && on_screen;

            if (viewport_mode)
            {
                if (local.camera.is_valid())
                {
                    if (active_target && !target_blocked)
                    {
                        const auto viewport = rbx::camera::calculate_viewport(best.screen_position, *dimensions);
                        rbx::camera::set_viewport(local.camera.get_address(), viewport);
                    }
                    else
                    {
                        rbx::camera::set_viewport(local.camera.get_address(), rbx::camera::viewport_from_size(*dimensions));
                    }
                }
                continue;
            }

            const auto mouse_service_addr = globals->mouse_service.get_address();
            if (mouse_service_addr != 0)
            {
                globals->mouse_service.initialise_mouse(mouse_service_addr);
                std::uint64_t input_obj = globals->mouse_service.get_cached_input_object();
                const std::uint64_t invalid_ptr = (std::numeric_limits<std::uint64_t>::max)();
                if (input_obj != 0 && input_obj != invalid_ptr)
                {
                    rbx::Vector2 write_pos{};
                    bool wrote = false;
                    bool from_mouse_read = false;

                    if (active_target)
                    {
                        write_pos = rbx::Vector2{ best.screen_position.x, best.screen_position.y };
                        wrote = true;
                    }
                    else if (features->free_aim_mouse_spoof && active_target)
                    {
                        if (const auto cursor = get_cursor_client_position(*dimensions))
                        {
                            const float adj_y = std::clamp(cursor->y - 50.0f, 0.0f, dimensions->y);
                            write_pos = rbx::Vector2{ cursor->x, adj_y };
                            wrote = true;
                            from_mouse_read = true;
                        }
                    }

                    if (wrote && !target_blocked)
                    {
                        rbx::mouse_service::write_mouse_position(input_obj, write_pos);
                        if (!target_visible && from_mouse_read && !block_aim_gui_writes.load(std::memory_order_relaxed))
                        {
                            if (const auto local = cache::localplayer->snapshot(); local.address != 0 && local.aim_gui.is_valid())
                            {
                                const bool ok_x = local.aim_gui.set_frame_pos_x(static_cast<std::uint64_t>(write_pos.x));
                                const bool ok_y = local.aim_gui.set_frame_pos_y(static_cast<std::uint64_t>(write_pos.y));
                                if (!ok_x || !ok_y)
                                {
                                    block_aim_gui_writes.store(true, std::memory_order_relaxed);
                                    logger_core::log_warning("disabled aim_gui writes after failure (mouse spoof)");
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

namespace free_aim
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
        mouse_spoof_worker = std::thread(run_mouse_spoof);
        mouse_spoof_worker.detach();
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
        if (target && target->player_address == player_address)
        {
            return true;
        }

        const auto aimbot_locked = aimbot::get_locked_player();
        return aimbot_locked != 0 && aimbot_locked == player_address;
    }

    std::uintptr_t get_locked_player()
    {
        return locked_player_address.load(std::memory_order_relaxed);
    }

    std::optional<rbx::Vector3> get_target_world_position()
    {
        const auto target = get_target_copy();
        if (!target || target->player_address == 0)
        {
            return std::nullopt;
        }

        if (!target->has_screen && !(globals && globals->game_id == 292439477))
        {
            return std::nullopt;
        }

        const rbx::Vector3& world = target->world_position;
        if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z))
        {
            return std::nullopt;
        }

        return target->world_position;
    }

    std::optional<rbx::Vector2> get_target_screen_position()
    {
        const auto target = get_target_copy();
        if (!target || target->player_address == 0 || !target->has_screen)
        {
            return std::nullopt;
        }

        const rbx::Vector2& screen = target->screen_position;
        if (!std::isfinite(screen.x) || !std::isfinite(screen.y))
        {
            return std::nullopt;
        }

        return screen;
    }
}
