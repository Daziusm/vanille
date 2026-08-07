#include "features/triggerbot.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <Windows.h>

#include "cache/local_player_cache.h"
#include "cache/player_cache.h"
#include "cache/team_utils.h"
#include "features/visibility.h"
#include "globals/globals_fixed.h"
#include "gui/overlay.hpp"
#include "memory/memory.h"
#include "sdk/camera.h"
#include "sdk/mesh_part.h"
#include "sdk/offsets.h"
#include "sdk/part.h"

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

    std::atomic<bool> running{ false };
    std::thread worker;
    std::mutex target_mutex;
    aim_target_state current_target{};
    std::atomic<std::uintptr_t> locked_player_address{ 0 };
    std::mutex chat_mutex;

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

    rbx::Vector2 get_reference_point(const rbx::Vector2& dimensions)
    {
        if (features->triggerbot_fov_mode == 1)
        {
            if (const auto cursor = get_cursor_client_position(dimensions))
            {
                return *cursor;
            }
        }
        return dimensions * 0.5f;
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

    bool is_reference_inside_part(const cache::primitive_part& part, const rbx::Matrix& view_matrix, const rbx::Vector2& dimensions, const rbx::Vector2& reference, float inflate);

    std::optional<aim_target_state> build_candidate(const cache::player_state& player, int hitbox_mode, bool nearest_part, bool limit_fov, float fov_radius, const rbx::Matrix& view, const rbx::Vector2& dimensions, const rbx::Vector2& reference)
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

            const auto screen = rbx::camera::world_to_screen(world, view, dimensions);
            if (!screen)
            {
                return;
            }

            float distance = (*screen - reference).Length();
            if (is_reference_inside_part(part, view, dimensions, reference, 0.0f))
            {
                distance = 0.0f;
            }
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

    bool is_reference_inside_part(const cache::primitive_part& part, const rbx::Matrix& view_matrix, const rbx::Vector2& dimensions, const rbx::Vector2& reference, float inflate)
    {
        if (!part.instance.is_valid())
        {
            return false;
        }

        rbx::Vector3 size = part.size;
        if (!(std::isfinite(size.x) && std::isfinite(size.y) && std::isfinite(size.z)) ||
            size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f)
        {
            return false;
        }

        rbx::mesh_part::transform transform{};
        bool has_transform = false;
        if (const auto tr = rbx::mesh_part::get_transform(part.instance, part.primitive))
        {
            transform = *tr;
            has_transform = true;
        }
        else if (rbx::Vector3 pos{}; get_part_position(part, pos))
        {
            transform.position = pos;
            transform.has_rotation = false;
            has_transform = true;
        }

        if (!has_transform)
        {
            return false;
        }

        rbx::Vector3 half = size * 0.5f;
        if (inflate > 0.0f)
        {
            half.x += inflate;
            half.y += inflate;
            half.z += inflate;
        }

        float min_x = (std::numeric_limits<float>::max)();
        float min_y = (std::numeric_limits<float>::max)();
        float max_x = (std::numeric_limits<float>::lowest)();
        float max_y = (std::numeric_limits<float>::lowest)();
        int projected = 0;

        const float signs[] = { -1.0f, 1.0f };
        for (float sx : signs)
        {
            for (float sy : signs)
            {
                for (float sz : signs)
                {
                    rbx::Vector3 local{ half.x * sx, half.y * sy, half.z * sz };
                    if (transform.has_rotation)
                    {
                        local = rotate_local_point(transform, local);
                    }
                    const rbx::Vector3 world = transform.position + local;
                    if (const auto screen = rbx::camera::world_to_screen(world, view_matrix, dimensions))
                    {
                        min_x = (std::min)(min_x, screen->x);
                        min_y = (std::min)(min_y, screen->y);
                        max_x = (std::max)(max_x, screen->x);
                        max_y = (std::max)(max_y, screen->y);
                        ++projected;
                    }
                }
            }
        }

        if (projected == 0)
        {
            return false;
        }

        return reference.x >= min_x && reference.x <= max_x &&
            reference.y >= min_y && reference.y <= max_y;
    }

    bool is_reference_inside_any_part(const cache::player_state& player, const rbx::Matrix& view_matrix, const rbx::Vector2& dimensions, const rbx::Vector2& reference)
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
            if (!part)
            {
                continue;
            }

            if (is_reference_inside_part(*part, view_matrix, dimensions, reference, 0.0f))
            {
                return true;
            }
        }

        return false;
    }

    bool player_blocked(const cache::player_state& player, const cache::local_player_state& local)
    {
        const player_relation relation = determine_relation(player, local.team);
        if (relation == player_relation::friendly)
        {
            return true;
        }

        if (features->triggerbot_only_enemies && relation != player_relation::enemy)
        {
            return true;
        }
        if (features->triggerbot_check_team && cache::team_utils::is_teammate(local, player))
        {
            return true;
        }
        if (features->triggerbot_check_health && player.health <= 0.0f)
        {
            return true;
        }
        if (features->triggerbot_check_knocked && player.body_effects.knocked)
        {
            return true;
        }
        if (features->triggerbot_check_grabbed && player.body_effects.grabbed)
        {
            return true;
        }
        if (features->triggerbot_check_reloading && player.body_effects.reload)
        {
            return true;
        }
        return false;
    }

    void send_left_down()
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        SendInput(1, &input, sizeof(INPUT));
    }

    void send_left_up()
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &input, sizeof(INPUT));
    }

    void run_loop()
    {
        using clock = std::chrono::steady_clock;
        std::uintptr_t last_locked_address = 0;
        std::uintptr_t last_target_address = 0;
        auto next_click = clock::now();
        bool left_held = false;

        auto release_left = [&]()
        {
            if (left_held)
            {
                send_left_up();
                left_held = false;
            }
        };

        while (running.load(std::memory_order_relaxed))
        {
            const HWND foreground = ::GetForegroundWindow();
            const bool roblox_active = (foreground == vanille::overlay::g_rbx_window);
            const bool menu_open = vanille::overlay::is_menu_open();

            if (menu_open)
            {
                release_left();
                clear_target();
                last_locked_address = 0;
                last_target_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            auto& keybind = features->triggerbot_keybind;
            if (features->triggerbot_check_typing)
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

            if (!features->enable_triggerbot || !key_active)
            {
                release_left();
                clear_target();
                last_locked_address = 0;
                last_target_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (!roblox_active)
            {
                release_left();
                clear_target();
                last_locked_address = 0;
                last_target_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (features->triggerbot_check_reloading && is_local_reloading())
            {
                release_left();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            const auto local = cache::localplayer->snapshot();
            if (local.address == 0 || !local.camera.is_valid())
            {
                release_left();
                clear_target();
                last_locked_address = 0;
                last_target_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            rbx::visualengine_t visual(globals->visualengine.get_address());
            auto view_matrix = visual.get_view_matrix();
            auto dimensions = visual.get_dimensions();
            if (!view_matrix || !dimensions)
            {
                release_left();
                clear_target();
                last_locked_address = 0;
                last_target_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            const bool limit_fov = features->triggerbot_limit_fov && features->triggerbot_fov_radius > 0.0f;
            const auto cursor_ref = get_cursor_client_position(*dimensions);
            const rbx::Vector2 reference = cursor_ref.value_or(get_reference_point(*dimensions));

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

            const float max_distance = features->triggerbot_max_distance;
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
            const bool visibility_enabled = visibility::can_run_visibility_check(features->triggerbot_visibility_check);

            aim_target_state best{};
            float best_distance = (std::numeric_limits<float>::max)();
            bool target_visible = false;
            bool preserve_locked = false;
            const cache::player_state* best_player_ptr = nullptr;

            if (features->triggerbot_sticky && last_locked_address != 0)
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
                            features->triggerbot_hitbox,
                            features->triggerbot_nearest_part,
                            limit_fov,
                            features->triggerbot_fov_radius,
                            *view_matrix,
                            *dimensions,
                            reference);

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
                        best_player_ptr = &player;
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

            if (!features->triggerbot_sticky || last_locked_address == 0)
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
                            features->triggerbot_hitbox,
                            features->triggerbot_nearest_part,
                            limit_fov,
                            features->triggerbot_fov_radius,
                            *view_matrix,
                            *dimensions,
                            reference);

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
                        best_player_ptr = &player;
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
                }
                else
                {
                    if (!preserve_locked)
                    {
                        clear_target_locked();
                        last_locked_address = 0;
                    }
                }
            }

            if (features->enable_triggerbot_closest_point && best.has_screen)
            {
                const rbx::Vector3 prediction_offset{};
                if (auto closest = find_closest_point_on_part(best.part, prediction_offset, *view_matrix, *dimensions, reference, 10))
                {
                    if (const auto screen = rbx::camera::world_to_screen(*closest, *view_matrix, *dimensions))
                    {
                        best.world_position = *closest;
                        best.screen_position = *screen;
                    }
                }
            }

            bool on_screen = false;
            if (best.has_screen)
            {
                on_screen =
                    best.screen_position.x >= 0.0f && best.screen_position.y >= 0.0f &&
                    best.screen_position.x <= dimensions->x && best.screen_position.y <= dimensions->y;
            }

            bool target_blocked = false;
            if (best.player_address != 0)
            {
                if (best_player_ptr)
                {
                    target_blocked = player_blocked(*best_player_ptr, local);
                }
                else
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
            }

            bool cursor_inside = false;
            if (cursor_ref && best.player_address != 0)
            {
                if (best_player_ptr)
                {
                    cursor_inside = is_reference_inside_any_part(*best_player_ptr, *view_matrix, *dimensions, *cursor_ref);
                }
                else
                {
                    for_each_player([&](const cache::player_state& player)
                        {
                            if (player.address != best.player_address)
                            {
                                return true;
                            }

                            cursor_inside = is_reference_inside_any_part(player, *view_matrix, *dimensions, *cursor_ref);
                            return false;
                        });
                }
            }

            const bool active_target = best.has_screen && on_screen && !target_blocked && target_visible && cursor_inside;
            const bool hold_fire = features->triggerbot_hold_fire;

            const int delay_ms = static_cast<int>(std::lround(std::clamp(features->triggerbot_delay_ms, 0.0f, 300.0f)));
            const auto now = clock::now();

            if (active_target)
            {
                if (hold_fire)
                {
                    if (!left_held)
                    {
                        send_left_down();
                        left_held = true;
                    }
                    last_target_address = best.player_address;
                }
                else
                {
                    release_left();
                    if (best.player_address != last_target_address)
                    {
                        last_target_address = best.player_address;
                        next_click = now + std::chrono::milliseconds(delay_ms);
                    }

                    if (now >= next_click)
                    {
                        send_left_down();
                        send_left_up();
                        next_click = now + std::chrono::milliseconds(delay_ms);
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            else
            {
                release_left();
                last_target_address = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        release_left();
    }
}

namespace triggerbot
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
