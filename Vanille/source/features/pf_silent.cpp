#include "features/pf_silent.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

#include "cache/local_player_cache.h"
#include "cache/team_utils.h"
#include "features/free_aim.h"
#include "globals/globals_fixed.h"
#include "sdk/camera.h"
#include "sdk/mouse.h"
#include "sdk/offsets.h"

namespace
{
    std::atomic<bool> running{ false };
    std::thread worker;

    constexpr int k_rotation_write_repeats = 16;

    void apply_pf_mouse_spoof(const rbx::Vector2& screen_position)
    {
        const std::uintptr_t mouse_service_addr = globals->mouse_service.get_address();
        if (mouse_service_addr == 0)
        {
            return;
        }

        globals->mouse_service.initialise_mouse(mouse_service_addr);
        const std::uint64_t input_obj = globals->mouse_service.get_cached_input_object();
        const std::uint64_t invalid_ptr = (std::numeric_limits<std::uint64_t>::max)();
        if (input_obj == 0 || input_obj == invalid_ptr)
        {
            return;
        }

        rbx::mouse_service::write_mouse_position(input_obj, screen_position);
    }

    void run_loop()
    {
        while (running.load(std::memory_order_relaxed))
        {
            if (!cache::team_utils::is_phantom_forces())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (features->enable_aimbot)
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

            const auto target_position = free_aim::get_target_world_position();
            const auto local = cache::localplayer->snapshot();
            if (!target_position || !local.camera.is_valid())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            const DirectX::XMFLOAT3X3 rotation = local.camera.look_at(*target_position);
            for (int i = 0; i < k_rotation_write_repeats; ++i)
            {
                local.camera.set_rotation(rotation);
            }

            if (const auto screen_position = free_aim::get_target_screen_position())
            {
                apply_pf_mouse_spoof(*screen_position);
            }
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
