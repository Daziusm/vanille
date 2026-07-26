#include "hacks.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <thread>
#include <Windows.h>

#include "cache/local_player_cache.h"
#include "globals/globals.h"
#include "gui/overlay.hpp"
#include "cache/player_parts.h"
#include "cache/player_cache.h"
#include "memory/memory.h"
#include "sdk/offsets.h"
#include "sdk/humanoid.h"
#include "sdk/part.h"
#include "utils/logger.h"

namespace
{
    using clock = std::chrono::steady_clock;

    enum class keyboard_layout_t
    {
        qwerty,
        azerty
    };

    keyboard_layout_t get_keyboard_layout(HWND reference_window)
    {
        DWORD thread_id = 0;
        if (reference_window)
        {
            thread_id = GetWindowThreadProcessId(reference_window, nullptr);
        }

        const HKL layout = GetKeyboardLayout(thread_id);
        const LANGID lang_id = LOWORD(reinterpret_cast<UINT_PTR>(layout));
        if (PRIMARYLANGID(lang_id) == LANG_FRENCH)
        {
            return keyboard_layout_t::azerty;
        }

        return keyboard_layout_t::qwerty;
    }

    bool chat_is_focused()
    {
        if (!globals->chat_input_bar_configuration.is_valid())
        {
            if (!globals->text_chat_service.is_valid() && globals->datamodel.is_valid())
            {
                globals->text_chat_service = globals->datamodel.find_first_child("TextChatService");
            }

            if (globals->text_chat_service.is_valid())
            {
                globals->chat_input_bar_configuration = globals->text_chat_service.find_first_child("ChatInputBarConfiguration");
            }
        }

        if (!globals->chat_input_bar_configuration.is_valid() || !roblox::offsets::chat::is_focused)
        {
            return false;
        }

        const auto addr = globals->chat_input_bar_configuration.get_address() + roblox::offsets::chat::is_focused;
        const std::uint32_t flags = memory->read<std::uint32_t>(addr);
        return (flags & 0x10000u) != 0;
    }

    bool roblox_has_focus()
    {
        const HWND window = vanille::overlay::g_rbx_window;
        return window && ::IsWindow(window) && ::GetForegroundWindow() == window;
    }

    rbx::Vector3 get_forward(const DirectX::XMFLOAT3X3& rot)
    {
        rbx::Vector3 forward(-rot._13, -rot._23, -rot._33);
        const float len_sq = forward.LengthSquared();
        if (len_sq < 1e-4f || !std::isfinite(len_sq))
        {
            return { 0.0f, 0.0f, -1.0f };
        }
        forward.Normalize();
        return forward;
    }

    rbx::Vector3 get_right(const DirectX::XMFLOAT3X3& rot)
    {
        rbx::Vector3 right(-rot._11, rot._21, -rot._31);
        const float len_sq = right.LengthSquared();
        if (len_sq < 1e-4f || !std::isfinite(len_sq))
        {
            return { 1.0f, 0.0f, 0.0f };
        }
        right.Normalize();
        return right;
    }

    rbx::Vector3 get_up(const DirectX::XMFLOAT3X3& rot)
    {
        rbx::Vector3 up(rot._12, rot._22, rot._32);
        const float len_sq = up.LengthSquared();
        if (len_sq < 1e-4f || !std::isfinite(len_sq))
        {
            return { 0.0f, 1.0f, 0.0f };
        }
        up.Normalize();
        return up;
    }

    std::atomic<bool> fly_running{ false };
    std::atomic<bool> fly_gravity_running{ false };
    std::atomic<bool> fly_active_state{ false };
    std::atomic<bool> walkspeed_running{ false };
    std::atomic<bool> bhop_running{ false };
    std::atomic<bool> noclip_running{ false };
    std::atomic<bool> desync_running{ false };
    std::atomic<bool> freeze_running{ false };
    std::atomic<bool> tickrate_running{ false };
    std::thread fly_worker;
    std::thread fly_gravity_worker;
    std::thread walkspeed_worker;
    std::thread bhop_worker;
    std::thread noclip_worker;
    std::thread desync_worker;
    std::thread freeze_worker;
    std::thread tickrate_worker;

    constexpr auto k_idle_sleep = std::chrono::milliseconds(8);
    constexpr auto k_walkspeed_sleep = std::chrono::milliseconds(5);
    constexpr auto k_bhop_sleep = std::chrono::milliseconds(5);
    constexpr auto k_bhop_jump_pulse = std::chrono::milliseconds(16);
    constexpr auto k_bhop_jump_cooldown = std::chrono::milliseconds(90);
    constexpr auto k_tickrate_sleep = std::chrono::milliseconds(50);
    constexpr float k_walkspeed_controller_speed = 0.1f;
    constexpr float k_default_walkspeed = 16.0f;
    constexpr float k_tickrate_default = 60.0f;
    constexpr float k_tickrate_multiplier = 4.0f;
    constexpr std::uintptr_t k_tickrate_steps_per_second_offset = 0x660;
    constexpr int k_walkspeed_position_repetitions = 16;
    constexpr float k_bhop_ground_velocity_threshold = 1.25f;
    constexpr float k_bhop_ground_snap_height = 0.75f;
    constexpr float k_bhop_airborne_height_threshold = 1.0f;
    constexpr int k_bhop_air_step_repetitions = 4;

    enum class walkspeed_mode_t
    {
        position = 0,
        humanoid
    };

    std::uintptr_t resolve_world_address()
    {
        if (!globals->workspace.is_valid() || !roblox::offsets::workspace::gravity_container)
        {
            return 0;
        }

        return memory->read<std::uintptr_t>(globals->workspace.get_address() + roblox::offsets::workspace::gravity_container);
    }

    bool write_world_tickrate(std::uintptr_t world_address, float tickrate)
    {
        if (!world_address)
        {
            return false;
        }

        const float world_steps_per_second = tickrate * k_tickrate_multiplier;
        return memory->write<float>(world_address + k_tickrate_steps_per_second_offset, world_steps_per_second);
    }

    std::optional<rbx::Vector3> get_part_position(const cache::primitive_part& part)
    {
        if (!part.instance.is_valid())
        {
            return std::nullopt;
        }
        return part.instance.get_position(part.primitive);
    }

    std::optional<float> get_humanoid_walkspeed(const rbx::instance_t& humanoid_instance)
    {
        if (!humanoid_instance.is_valid() || !roblox::offsets::humanoid::walk_speed)
        {
            return std::nullopt;
        }

        const float value = memory->read<float>(humanoid_instance.get_address() + roblox::offsets::humanoid::walk_speed);
        if (!std::isfinite(value))
        {
            return std::nullopt;
        }

        return value;
    }

    bool set_humanoid_walkspeed(const rbx::instance_t& humanoid_instance, float speed)
    {
        if (!humanoid_instance.is_valid() || !roblox::offsets::humanoid::walk_speed || !std::isfinite(speed))
        {
            return false;
        }

        bool success = memory->write<float>(humanoid_instance.get_address() + roblox::offsets::humanoid::walk_speed, speed);
        if (roblox::offsets::humanoid::walk_speed_check)
        {
            success |= memory->write<float>(humanoid_instance.get_address() + roblox::offsets::humanoid::walk_speed_check, speed);
        }

        return success;
    }

    bool set_humanoid_jump(const rbx::instance_t& humanoid_instance, bool jump)
    {
        if (!humanoid_instance.is_valid() || !roblox::offsets::humanoid::jump)
        {
            return false;
        }

        return memory->write<bool>(humanoid_instance.get_address() + roblox::offsets::humanoid::jump, jump);
    }

    bool get_humanoid_move_velocity(const rbx::instance_t& humanoid_instance, float speed, rbx::Vector3& out_velocity)
    {
        out_velocity = {};

        if (!humanoid_instance.is_valid() || speed <= 0.0f)
        {
            return false;
        }

        const auto move_direction = rbx::humanoid::get_move_direction(humanoid_instance);
        if (!move_direction)
        {
            return false;
        }

        rbx::Vector3 direction(move_direction->x, 0.0f, move_direction->z);
        const float len_sq = direction.LengthSquared();
        if (!std::isfinite(len_sq) || len_sq < 1e-4f)
        {
            return false;
        }

        const float len = std::sqrt(len_sq);
        if (!std::isfinite(len) || len < 1e-4f)
        {
            return false;
        }

        if (len > 1.0f)
        {
            direction /= len;
        }

        out_velocity = direction * speed;
        return true;
    }

    bool write_horizontal_position_components(std::uintptr_t address, float x, float z)
    {
        if (!address)
        {
            return false;
        }

        bool success = memory->write<float>(address, x);
        success |= memory->write<float>(address + sizeof(float) * 2, z);
        return success;
    }

    bool set_horizontal_position(std::uintptr_t primitive, float x, float z)
    {
        if (!primitive)
        {
            return false;
        }

        bool success = false;

        if (roblox::offsets::base_part::position)
        {
            success |= write_horizontal_position_components(primitive + roblox::offsets::base_part::position, x, z);
        }

        if (roblox::offsets::base_part::primitive_properties && roblox::offsets::base_part::primitive_position)
        {
            const auto is_valid_ptr = [](std::uintptr_t ptr) -> bool
            {
                constexpr std::uintptr_t k_min = 0x10000;
                constexpr std::uintptr_t k_max = 0x00007FFFFFFFFFFF;
                return ptr >= k_min && ptr <= k_max;
            };

            const std::uintptr_t props_ptr = memory->read<std::uintptr_t>(primitive + roblox::offsets::base_part::primitive_properties);
            if (is_valid_ptr(props_ptr))
            {
                success |= write_horizontal_position_components(props_ptr + roblox::offsets::base_part::primitive_position, x, z);
            }
            else
            {
                success |= write_horizontal_position_components(
                    primitive + roblox::offsets::base_part::primitive_properties + roblox::offsets::base_part::primitive_position,
                    x,
                    z);
            }
        }

        return success;
    }

    bool step_horizontal_position(const rbx::instance_t& part_instance, std::uintptr_t primitive, const rbx::Vector3& horizontal_velocity, float dt, int repetitions = k_walkspeed_position_repetitions)
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

        repetitions = std::clamp(repetitions, 1, 100);
        const float step_dt = dt / static_cast<float>(repetitions);
        bool success = false;
        for (int i = 0; i < repetitions; ++i)
        {
            const auto pos = part_instance.get_position(primitive);
            if (!pos)
            {
                break;
            }

            const float next_x = pos->x + horizontal_velocity.x * step_dt;
            const float next_z = pos->z + horizontal_velocity.z * step_dt;
            success |= set_horizontal_position(primitive, next_x, next_z);
        }
        return success;
    }

}

namespace hacks
{
    namespace
    {
        void fly_loop()
        {
            bool was_active = false;
            rbx::Vector3 current_velocity{};
            auto last_time = clock::now();

            while (fly_running.load(std::memory_order_relaxed))
            {
                const auto now = clock::now();
                float dt = std::chrono::duration<float>(now - last_time).count();
                if (dt < 0.0001f)
                {
                    dt = 0.0001f;
                }
                last_time = now;

                const bool chat_focused = chat_is_focused();
                const bool roblox_active = roblox_has_focus();
                const bool block_input = features->fly_check_typing && chat_focused;
                const bool allow_toggle = roblox_active && !chat_focused;

                if (allow_toggle && !features->fly_keybind.waiting_for_input)
                {
                    features->fly_keybind.update();
                }
                else if (chat_focused && !features->fly_keybind.waiting_for_input && features->fly_keybind.key)
                {
                    ::GetAsyncKeyState(features->fly_keybind.key);
                }

                const auto local = cache::localplayer->snapshot();
                const bool is_knocked = local.body_effects.knocked;
                const bool fly_active = !is_knocked && features->enable_fly && features->fly_keybind.enabled;
                const bool sleep_when_idle = !fly_active;
                fly_active_state.store(fly_active, std::memory_order_relaxed);
                const auto hrp = local.parts.humanoid_root_part;

                if (local.address == 0 || !hrp.primitive || !local.camera.is_valid() || is_knocked)
                {
                    if (was_active && hrp.primitive)
                    {
                        rbx::part::clear_velocity(hrp.primitive);
                        current_velocity = {};
                    }

                    fly_active_state.store(false, std::memory_order_relaxed);
                    was_active = false;

                    if (sleep_when_idle)
                    {
                        std::this_thread::sleep_for(k_idle_sleep);
                    }
                    continue;
                }

                DirectX::XMFLOAT3X3 rotation = local.camera.get_rotation();
                rbx::Vector3 forward = get_forward(rotation);
                rbx::Vector3 right = get_right(rotation);
                const rbx::Vector3 world_up(0.0f, 1.0f, 0.0f);

                if (!fly_active)
                {
                    if (was_active)
                    {
                        rbx::part::clear_velocity(hrp.primitive);
                        current_velocity = {};
                    }

                    was_active = false;

                    if (sleep_when_idle)
                    {
                        std::this_thread::sleep_for(k_idle_sleep);
                    }
                    continue;
                }

                was_active = true;

                const keyboard_layout_t layout = get_keyboard_layout(vanille::overlay::g_rbx_window);
                const int forward_key = layout == keyboard_layout_t::azerty ? 'Z' : 'W';
                const int left_key = layout == keyboard_layout_t::azerty ? 'Q' : 'A';

                auto key_down = [](int vk) -> bool
                {
                    return (GetAsyncKeyState(vk) & 0x8000) != 0;
                };

                if (forward.LengthSquared() < 1e-6f || right.LengthSquared() < 1e-6f)
                {
                    forward = rbx::Vector3(0.0f, 0.0f, 1.0f);
                    right = rbx::Vector3(1.0f, 0.0f, 0.0f);
                }
                else
                {
                    forward.Normalize();
                    right.Normalize();
                }

                const bool allow_movement_input = roblox_active && !block_input;
                rbx::Vector3 target_velocity = block_input ? current_velocity : rbx::Vector3{};

                const float speed = (std::max)(features->fly_speed, 0.0f);
                const float vertical_speed = speed * features->fly_vertical_boost;

                if (allow_movement_input)
                {
                    if (key_down(forward_key)) target_velocity += forward * speed;
                    if (key_down('S'))         target_velocity -= forward * speed;
                    if (key_down(left_key))    target_velocity += right * speed;
                    if (key_down('D'))         target_velocity -= right * speed;

                    float vertical_input = 0.0f;
                    if (key_down(VK_SPACE)) vertical_input += 1.0f;
                    if (key_down(VK_CONTROL)) vertical_input -= 1.0f;
                    if (std::abs(vertical_input) > 0.0f)
                    {
                        target_velocity += world_up * (vertical_input * vertical_speed);
                    }
                }

                const bool has_input = target_velocity.LengthSquared() > 1e-4f;
                const float damping = (std::max)(features->fly_damping, 0.0f);
                const float clamped_dt = std::clamp(dt, 0.001f, 0.05f);

                if (damping > 0.0f)
                {
                    const float alpha = 1.0f - std::exp(-damping * clamped_dt);
                    current_velocity = current_velocity + (target_velocity - current_velocity) * alpha;
                }
                else
                {
                    current_velocity = target_velocity;
                }

                rbx::part::set_linear_velocity(hrp.primitive, current_velocity);
            }

            if (auto local = cache::localplayer->snapshot(); local.address != 0 && local.parts.humanoid_root_part.primitive)
            {
                rbx::part::clear_velocity(local.parts.humanoid_root_part.primitive);
            }
            fly_active_state.store(false, std::memory_order_relaxed);
        }

        void fly_gravity_loop()
        {
            bool gravity_overridden = false;
            float gravity_backup = 0.0f;

            while (fly_gravity_running.load(std::memory_order_relaxed))
            {
                const bool fly_active = fly_active_state.load(std::memory_order_relaxed);
                const bool sleep_when_idle = !fly_active;
                const bool can_write_gravity = globals->workspace.is_valid() && roblox::offsets::workspace::gravity_container && roblox::offsets::workspace::gravity;

                if (fly_active && can_write_gravity)
                {
                    const auto world = memory->read<std::uintptr_t>(globals->workspace.get_address() + roblox::offsets::workspace::gravity_container);
                    if (world)
                    {
                        if (!gravity_overridden)
                        {
                            gravity_backup = memory->read<float>(world + roblox::offsets::workspace::gravity);
                            gravity_overridden = true;
                        }
                        memory->write<float>(world + roblox::offsets::workspace::gravity, 0.0f);
                    }
                }
                else if (!fly_active && gravity_overridden && can_write_gravity)
                {
                    const auto world = memory->read<std::uintptr_t>(globals->workspace.get_address() + roblox::offsets::workspace::gravity_container);
                    if (world)
                    {
                        memory->write<float>(world + roblox::offsets::workspace::gravity, gravity_backup);
                    }
                    gravity_overridden = false;
                }

                if (sleep_when_idle)
                {
                    std::this_thread::sleep_for(k_idle_sleep);
                }
            }

            if (gravity_overridden && globals->workspace.is_valid() && roblox::offsets::workspace::gravity_container && roblox::offsets::workspace::gravity)
            {
                const auto world = memory->read<std::uintptr_t>(globals->workspace.get_address() + roblox::offsets::workspace::gravity_container);
                if (world)
                {
                    memory->write<float>(world + roblox::offsets::workspace::gravity, gravity_backup);
                }
            }
        }

        void walkspeed_loop()
        {
            bool was_active = false;
            bool has_humanoid_backup = false;
            rbx::instance_t backed_up_humanoid{};
            float backed_up_walkspeed = k_default_walkspeed;
            auto last_time = clock::now();

            auto clear_backup = [&]()
            {
                has_humanoid_backup = false;
                backed_up_humanoid = {};
                backed_up_walkspeed = k_default_walkspeed;
            };

            auto restore_humanoid_speed = [&]()
            {
                if (has_humanoid_backup && backed_up_humanoid.is_valid())
                {
                    set_humanoid_walkspeed(backed_up_humanoid, backed_up_walkspeed);
                }

                clear_backup();
            };

            auto backup_humanoid_speed = [&](const rbx::instance_t& humanoid)
            {
                if (!humanoid.is_valid())
                {
                    return;
                }

                const bool humanoid_changed = !has_humanoid_backup || backed_up_humanoid.get_address() != humanoid.get_address();
                if (!humanoid_changed)
                {
                    return;
                }

                restore_humanoid_speed();
                backed_up_walkspeed = get_humanoid_walkspeed(humanoid).value_or(k_default_walkspeed);
                backed_up_humanoid = humanoid;
                has_humanoid_backup = true;
            };

            while (walkspeed_running.load(std::memory_order_relaxed))
            {
                const auto now = clock::now();
                float dt = std::chrono::duration<float>(now - last_time).count();
                if (dt < 0.0001f)
                {
                    dt = 0.0001f;
                }
                last_time = now;

                const bool chat_focused = chat_is_focused();
                const bool allow_toggle = roblox_has_focus() && !chat_focused;

                if (allow_toggle && !features->walkspeed_keybind.waiting_for_input)
                {
                    features->walkspeed_keybind.update();
                }
                else if (chat_focused && !features->walkspeed_keybind.waiting_for_input && features->walkspeed_keybind.key)
                {
                    ::GetAsyncKeyState(features->walkspeed_keybind.key);
                }

                features->walkspeed_value = std::clamp(features->walkspeed_value, 1.0f, 500.0f);
                features->walkspeed_mode = std::clamp(features->walkspeed_mode, 0, 1);

                const auto local = cache::localplayer->snapshot();
                const auto hrp = local.parts.humanoid_root_part;
                const auto humanoid = rbx::humanoid::find_humanoid(local.character);
                const auto mode = static_cast<walkspeed_mode_t>(features->walkspeed_mode);
                const float speed = features->walkspeed_value;
                const bool active = features->enable_walkspeed
                    && features->walkspeed_keybind.enabled
                    && local.address != 0
                    && hrp.primitive
                    && humanoid.is_valid()
                    && !local.body_effects.knocked;

                if (!active)
                {
                    if (was_active)
                    {
                        restore_humanoid_speed();
                    }

                    was_active = false;
                    std::this_thread::sleep_for(k_idle_sleep);
                    continue;
                }

                backup_humanoid_speed(humanoid);

                if (mode == walkspeed_mode_t::humanoid)
                {
                    set_humanoid_walkspeed(humanoid, speed);
                }
                else
                {
                    set_humanoid_walkspeed(humanoid, k_walkspeed_controller_speed);

                    rbx::Vector3 desired_velocity{};
                    const bool has_direction = get_humanoid_move_velocity(humanoid, speed, desired_velocity);
                    if (has_direction)
                    {
                        const float clamped_dt = std::clamp(dt, 0.001f, 0.05f);
                        step_horizontal_position(hrp.instance, hrp.primitive, desired_velocity, clamped_dt, k_walkspeed_position_repetitions);
                    }
                }

                was_active = true;
                std::this_thread::sleep_for(k_walkspeed_sleep);
            }

            restore_humanoid_speed();
        }

        void bhop_loop()
        {
            bool jump_latched = false;
            bool has_ground_reference = false;
            bool has_humanoid_backup = false;
            float ground_reference_y = 0.0f;
            float backed_up_walkspeed = k_default_walkspeed;
            std::uintptr_t tracked_character = 0;
            rbx::instance_t backed_up_humanoid{};
            auto last_jump_write = clock::now() - k_bhop_jump_cooldown;
            auto last_time = clock::now();

            auto clear_backup = [&]()
            {
                has_humanoid_backup = false;
                backed_up_humanoid = {};
                backed_up_walkspeed = k_default_walkspeed;
            };

            auto restore_humanoid_speed = [&]()
            {
                if (has_humanoid_backup && backed_up_humanoid.is_valid())
                {
                    set_humanoid_walkspeed(backed_up_humanoid, backed_up_walkspeed);
                }

                clear_backup();
            };

            auto backup_humanoid_speed = [&](const rbx::instance_t& humanoid)
            {
                if (!humanoid.is_valid())
                {
                    return;
                }

                const bool humanoid_changed = !has_humanoid_backup || backed_up_humanoid.get_address() != humanoid.get_address();
                if (!humanoid_changed)
                {
                    return;
                }

                restore_humanoid_speed();
                backed_up_walkspeed = get_humanoid_walkspeed(humanoid).value_or(k_default_walkspeed);
                backed_up_humanoid = humanoid;
                has_humanoid_backup = true;
            };

            while (bhop_running.load(std::memory_order_relaxed))
            {
                const auto now = clock::now();
                float dt = std::chrono::duration<float>(now - last_time).count();
                if (dt < 0.0001f)
                {
                    dt = 0.0001f;
                }
                last_time = now;

                const bool chat_focused = chat_is_focused();
                const bool allow_toggle = roblox_has_focus() && !chat_focused;

                if (allow_toggle && !features->bhop_keybind.waiting_for_input)
                {
                    features->bhop_keybind.update();
                }
                else if (chat_focused && !features->bhop_keybind.waiting_for_input && features->bhop_keybind.key)
                {
                    ::GetAsyncKeyState(features->bhop_keybind.key);
                }

                features->bhop_speed = std::clamp(features->bhop_speed, 1.0f, 500.0f);

                const auto local = cache::localplayer->snapshot();
                const auto hrp = local.parts.humanoid_root_part;
                const auto humanoid = rbx::humanoid::find_humanoid(local.character);
                const bool active = features->enable_bhop
                    && features->bhop_keybind.enabled
                    && allow_toggle
                    && local.address != 0
                    && local.character.is_valid()
                    && humanoid.is_valid()
                    && hrp.primitive
                    && !local.body_effects.knocked;

                if (!active)
                {
                    if (jump_latched && humanoid.is_valid())
                    {
                        set_humanoid_jump(humanoid, false);
                    }

                    restore_humanoid_speed();
                    jump_latched = false;
                    has_ground_reference = false;
                    tracked_character = 0;
                    std::this_thread::sleep_for(k_idle_sleep);
                    continue;
                }

                const std::uintptr_t character_address = local.character.get_address();
                if (tracked_character != character_address)
                {
                    tracked_character = character_address;
                    jump_latched = false;
                    has_ground_reference = false;
                    restore_humanoid_speed();
                }

                if (jump_latched && (now - last_jump_write) >= k_bhop_jump_pulse)
                {
                    set_humanoid_jump(humanoid, false);
                    jump_latched = false;
                }

                const bool walkspeed_active = features->enable_walkspeed && features->walkspeed_keybind.enabled;
                if (walkspeed_active)
                {
                    restore_humanoid_speed();
                }
                else
                {
                    backup_humanoid_speed(humanoid);
                }

                const bool jump_key_held = (::GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
                const float bhop_speed = features->bhop_speed;
                rbx::Vector3 desired_velocity{};
                const bool moving = jump_key_held && get_humanoid_move_velocity(humanoid, bhop_speed, desired_velocity);
                const bool jump_flag = rbx::humanoid::get_jump(humanoid).value_or(false);

                float vertical_velocity = 0.0f;
                if (const auto live_velocity = rbx::part::get_linear_velocity(hrp.primitive))
                {
                    if (std::isfinite(live_velocity->y))
                    {
                        vertical_velocity = live_velocity->y;
                    }
                }

                bool height_airborne = false;
                if (const auto position = hrp.instance.get_position(hrp.primitive))
                {
                    if (!has_ground_reference)
                    {
                        ground_reference_y = position->y;
                        has_ground_reference = true;
                    }

                    const float height_delta = position->y - ground_reference_y;
                    if (!jump_flag
                        && std::abs(vertical_velocity) <= k_bhop_ground_velocity_threshold
                        && std::abs(height_delta) <= k_bhop_ground_snap_height)
                    {
                        ground_reference_y = position->y;
                    }

                    height_airborne = std::abs(position->y - ground_reference_y) > k_bhop_airborne_height_threshold;
                }

                const bool velocity_airborne = std::abs(vertical_velocity) > k_bhop_ground_velocity_threshold;
                const bool airborne = jump_flag || velocity_airborne || height_airborne;
                const bool can_jump = moving && !airborne && (now - last_jump_write) >= k_bhop_jump_cooldown;

                if (!walkspeed_active && has_humanoid_backup && backed_up_humanoid.is_valid())
                {
                    const float target_speed = moving ? (std::max)(backed_up_walkspeed, bhop_speed) : backed_up_walkspeed;
                    set_humanoid_walkspeed(backed_up_humanoid, target_speed);
                }

                if (moving && airborne)
                {
                    const float clamped_dt = std::clamp(dt, 0.001f, 0.03f);
                    step_horizontal_position(hrp.instance, hrp.primitive, desired_velocity, clamped_dt, k_bhop_air_step_repetitions);
                }

                if (can_jump && set_humanoid_jump(humanoid, true))
                {
                    last_jump_write = now;
                    jump_latched = true;
                }

                std::this_thread::sleep_for(k_bhop_sleep);
            }

            if (jump_latched)
            {
                const auto local = cache::localplayer->snapshot();
                if (const auto humanoid = rbx::humanoid::find_humanoid(local.character); humanoid.is_valid())
                {
                    set_humanoid_jump(humanoid, false);
                }
            }

            restore_humanoid_speed();
        }

        void apply_noclip_state(const cache::character_parts& parts, bool enable)
        {
            const std::uintptr_t primitives[] = {
                parts.humanoid_root_part.primitive,
                parts.head.primitive,
                parts.torso.primitive,
                parts.upper_torso.primitive,
                parts.lower_torso.primitive,
                parts.left_arm.primitive,
                parts.right_arm.primitive,
                parts.left_leg.primitive,
                parts.right_leg.primitive,
                parts.left_upper_arm.primitive,
                parts.left_lower_arm.primitive,
                parts.left_hand.primitive,
                parts.right_upper_arm.primitive,
                parts.right_lower_arm.primitive,
                parts.right_hand.primitive,
                parts.left_upper_leg.primitive,
                parts.left_lower_leg.primitive,
                parts.left_foot.primitive,
                parts.right_upper_leg.primitive,
                parts.right_lower_leg.primitive,
                parts.right_foot.primitive
            };

            for (std::uintptr_t primitive : primitives)
            {
                if (primitive)
                {
                    rbx::part::set_can_collide(primitive, enable);
                }
            }
        }

        void noclip_loop()
        {
            bool last_state = false;

            while (noclip_running.load(std::memory_order_relaxed))
            {
                const bool chat_focused = chat_is_focused();
                const bool allow_toggle = roblox_has_focus() && !chat_focused;

                if (allow_toggle && !features->noclip_keybind.waiting_for_input)
                {
                    features->noclip_keybind.update();
                }
                else if (chat_focused && !features->noclip_keybind.waiting_for_input && features->noclip_keybind.key)
                {
                    ::GetAsyncKeyState(features->noclip_keybind.key);
                }

                const auto local = cache::localplayer->snapshot();
                const bool has_char = local.character.is_valid();
                const bool is_knocked = local.body_effects.knocked;
                const bool wants_noclip = features->enable_noclip && features->noclip_keybind.enabled;
                const bool noclip_active = wants_noclip && has_char && !is_knocked;

                if (noclip_active && has_char)
                {
                    apply_noclip_state(local.parts, false);
                    last_state = true;
                }
                else if (last_state)
                {
                    apply_noclip_state(local.parts, true);
                    last_state = false;
                }

                if (!noclip_active)
                {
                    std::this_thread::sleep_for(k_idle_sleep);
                }
            }

            const auto local = cache::localplayer->snapshot();
            if (local.character.is_valid())
            {
                apply_noclip_state(local.parts, true);
        }
    }

    void freeze_players_loop()
    {
        bool last_state = false;
        const int freeze_value = -999999999999999999;
        const int normal_value = 20;

        while (freeze_running.load(std::memory_order_relaxed))
        {
            const bool allow_toggle = roblox_has_focus() && !chat_is_focused();

            if (allow_toggle && !features->freeze_players_keybind.waiting_for_input)
            {
                features->freeze_players_keybind.update();
            }
            else if (!allow_toggle && !features->freeze_players_keybind.waiting_for_input && features->freeze_players_keybind.key)
            {
                ::GetAsyncKeyState(features->freeze_players_keybind.key);
            }

            const bool freeze_active = features->freeze_players && features->freeze_players_keybind.enabled;
            const auto base = memory->get_module_address();
            const auto offset = roblox::offsets::fflags::target_time_delay_facctor_tenths;

            if (base != 0 && offset != 0)
            {
                if (freeze_active != last_state)
                {
                    const int value = freeze_active ? freeze_value : normal_value;
                    memory->write<int>(base + offset, value);
                    last_state = freeze_active;
                }
            }
            else
            {
                last_state = false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        const auto base = memory->get_module_address();
        const auto offset = roblox::offsets::fflags::target_time_delay_facctor_tenths;
        if (base != 0 && offset != 0)
        {
            memory->write<int>(base + offset, normal_value);
        }
    }

    void tickrate_loop()
    {
        bool last_enabled = false;
        float last_slider_value = features->tickrate_modifier_value;
        std::uintptr_t last_world_address = 0;

        while (tickrate_running.load(std::memory_order_relaxed))
        {
            const bool enabled = features->enable_tickrate_modifier;
            features->tickrate_modifier_value = std::clamp(features->tickrate_modifier_value, 1.0f, 660.0f);
            const float slider_value = features->tickrate_modifier_value;
            const std::uintptr_t world_address = resolve_world_address();

            if (world_address)
            {
                const bool value_changed = std::fabs(slider_value - last_slider_value) > 0.001f;
                const bool world_changed = world_address != last_world_address;
                const bool should_apply_override = enabled && (!last_enabled || value_changed || world_changed);
                const bool should_restore_default = !enabled && last_enabled;

                if (should_apply_override)
                {
                    write_world_tickrate(world_address, slider_value);
                }
                else if (should_restore_default)
                {
                    write_world_tickrate(world_address, k_tickrate_default);
                }
            }

            last_enabled = enabled;
            last_slider_value = slider_value;
            last_world_address = world_address;

            std::this_thread::sleep_for(k_tickrate_sleep);
        }

        if (last_enabled)
        {
            if (const std::uintptr_t world_address = resolve_world_address(); world_address)
            {
                write_world_tickrate(world_address, k_tickrate_default);
            }
        }
    }

    void desync_loop()
    {
        bool last_value = false;
        bool has_written = false;

            while (desync_running.load(std::memory_order_relaxed))
            {
                const bool allow_toggle = roblox_has_focus() && !chat_is_focused();

                if (allow_toggle && !features->desync_keybind.waiting_for_input)
                {
                    features->desync_keybind.update();
                }
                else if (!allow_toggle && !features->desync_keybind.waiting_for_input && features->desync_keybind.key)
                {
                    ::GetAsyncKeyState(features->desync_keybind.key);
                }

                const bool desired = features->desync && features->desync_keybind.enabled;
                const std::uint64_t base = memory->get_module_address();
                const std::uintptr_t replicator_offset = roblox::offsets::replicator::nextgen_replicator;

                if (base != 0 && replicator_offset != 0 && (!has_written || desired != last_value))
                {
                    memory->write<bool>(base + replicator_offset, desired);
                    last_value = desired;
                    has_written = true;

                    if (desired)
                    {
                        if (const auto local = cache::localplayer->snapshot(); local.address != 0)
                        {
                            if (const auto pos = get_part_position(local.parts.humanoid_root_part))
                            {
                                features->desync_marker_position = *pos;
                                features->desync_marker_active = true;
                            }
                        }
                    }
                    else
                    {
                        features->desync_marker_active = false;
                    }
                }
                else if (replicator_offset == 0)
                {
                    has_written = false;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    void apply()
    {
        static std::atomic<bool> mesh_parts_logged{ false };

        auto schedule_mesh_log = []()
        {
            std::thread([]()
                {
                    const int max_attempts = 6;
                    for (int attempt = 0; attempt < max_attempts; ++attempt)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(600));
                        auto snap = cache::players_cache->snapshot();
                        int count = static_cast<int>(snap ? snap->size() : 0);
                        const auto dummy = cache::players_cache->dummy_snapshot();
                        if (dummy && dummy->address != 0)
                            ++count;
                        if (count > 1)
                            break;
                    }
                }).detach();
        };

        if (!fly_running.load(std::memory_order_relaxed))
        {
            fly_running = true;
            fly_worker = std::thread(fly_loop);
            fly_worker.detach();
        }

        if (!fly_gravity_running.load(std::memory_order_relaxed))
        {
            fly_gravity_running = true;
            fly_gravity_worker = std::thread(fly_gravity_loop);
            fly_gravity_worker.detach();
        }

        if (!noclip_running.load(std::memory_order_relaxed))
        {
            noclip_running = true;
            noclip_worker = std::thread(noclip_loop);
            noclip_worker.detach();
        }

        if (!walkspeed_running.load(std::memory_order_relaxed))
        {
            walkspeed_running = true;
            walkspeed_worker = std::thread(walkspeed_loop);
            walkspeed_worker.detach();
        }

        if (!bhop_running.load(std::memory_order_relaxed))
        {
            bhop_running = true;
            bhop_worker = std::thread(bhop_loop);
            bhop_worker.detach();
        }

        if (!desync_running.load(std::memory_order_relaxed))
        {
            desync_running = true;
            desync_worker = std::thread(desync_loop);
            desync_worker.detach();
        }

        if (!freeze_running.load(std::memory_order_relaxed))
        {
            freeze_running = true;
            freeze_worker = std::thread(freeze_players_loop);
            freeze_worker.detach();
        }

        if (!tickrate_running.load(std::memory_order_relaxed))
        {
            tickrate_running = true;
            tickrate_worker = std::thread(tickrate_loop);
            tickrate_worker.detach();
        }

        if (!mesh_parts_logged.exchange(true))
        {
            schedule_mesh_log();
            std::thread([]()
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
                    cache::download_mesh_assets_to_temp();
                }).detach();
        }
    }

    void stop()
    {
        fly_running.store(false, std::memory_order_relaxed);
        fly_gravity_running.store(false, std::memory_order_relaxed);
        walkspeed_running.store(false, std::memory_order_relaxed);
        bhop_running.store(false, std::memory_order_relaxed);
        noclip_running.store(false, std::memory_order_relaxed);
        desync_running.store(false, std::memory_order_relaxed);
        freeze_running.store(false, std::memory_order_relaxed);
        tickrate_running.store(false, std::memory_order_relaxed);
        fly_active_state.store(false, std::memory_order_relaxed);
    }
}
