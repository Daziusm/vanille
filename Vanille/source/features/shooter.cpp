#include "features/shooter.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <Windows.h>

#include "cache/local_player_cache.h"
#include "cache/player_cache.h"
#include "globals/globals.h"
#include "gui/overlay.hpp"

namespace
{
    std::atomic<bool> g_running{ false };
    std::thread g_worker;

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

    using clock = std::chrono::steady_clock;

    std::string to_lower_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
        return value;
    }

    bool is_gun_tool_name(const std::string& name)
    {
        if (name.empty())
        {
            return false;
        }

        const std::string lower = to_lower_ascii(name);
        static constexpr std::string_view gun_tokens[] = {
            "revolver",
            "double barrel",
            "doublebarrel",
            "tacticalshotgun",
            "tactical shotgun",
            "shotgun",
            "rifle",
            "pistol",
            "smg",
            "glock",
            "uzi",
            "ak",
            "ar",
            "sniper",
            "silencer"
        };

        for (const auto token : gun_tokens)
        {
            if (lower.find(token) != std::string::npos)
            {
                return true;
            }
        }

        return false;
    }

    bool has_rbx_focus()
    {
        const HWND window = vanille::overlay::g_rbx_window;
        return window && ::IsWindow(window) && ::GetForegroundWindow() == window;
    }

    int get_bullets_to_send(const cache::local_player_state& local)
    {
        if (!local.equipped_tool.is_valid())
        {
            return 0;
        }

        if (!is_gun_tool_name(local.equipped_tool_name))
        {
            return 0;
        }

        const int max_ammo = local.tool_max_ammo;
        if (max_ammo <= 0)
        {
            return 0;
        }

        const int ammo = local.tool_ammo;
        if (ammo <= 0)
        {
            return 0;
        }

        int desired = static_cast<int>(std::lround(features->bullets_sent));
        desired = std::clamp(desired, 1, max_ammo);
        desired = (std::min)(desired, ammo);
        return desired;
    }

    struct host_state
    {
        bool was_firing = false;
        bool active = false;
        int target_ammo = 0;
        clock::time_point next_click = clock::now();
    };

    void run_loop()
    {
        std::unordered_map<std::uint64_t, host_state> state_by_key;
        while (g_running.load(std::memory_order_relaxed))
        {
            if (!features->enable_auto_shooter)
            {
                state_by_key.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            if (!has_rbx_focus())
            {
                state_by_key.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            const auto local = cache::localplayer->snapshot();
            const int bullets_to_send = get_bullets_to_send(local);
            if (bullets_to_send <= 0)
            {
                state_by_key.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            const auto players_snapshot = cache::players_cache->snapshot();
            std::unordered_set<std::uint64_t> seen_keys;
            const int click_delay = static_cast<int>(std::clamp(features->host_click_delay_ms, 1.0f, 1000.0f));

            if (players_snapshot)
            {
                for (const auto& player : *players_snapshot)
                {
                    if (!vanille::overlay::is_host(player.user_id, player.name))
                    {
                        continue;
                    }

                    const std::uint64_t key = (player.user_id != 0) ? player.user_id : player.address;
                    seen_keys.insert(key);

                    auto& state = state_by_key[key];
                    const bool firing = player.body_effects.gun_firing;
                    const auto now = clock::now();

                    if (firing)
                    {
                        if (!state.was_firing && !state.active)
                        {
                            const int start_ammo = (std::max)(0, local.tool_ammo);
                            const int target_ammo = (std::max)(0, start_ammo - bullets_to_send);
                            state.active = true;
                            state.was_firing = true;
                            state.target_ammo = target_ammo;
                            state.next_click = now;
                        }
                    }
                    else
                    {
                        state.was_firing = false;
                    }

                    if (state.active)
                    {
                        const int current_ammo = (std::max)(0, local.tool_ammo);
                        if (current_ammo <= state.target_ammo)
                        {
                            state.active = false;
                        }
                        else if (now >= state.next_click)
                        {
                            send_left_down();
                            send_left_up();
                            state.next_click = now + std::chrono::milliseconds(click_delay);
                        }
                    }
                }
            }

            for (auto it = state_by_key.begin(); it != state_by_key.end();)
            {
                if (seen_keys.find(it->first) == seen_keys.end())
                {
                    it = state_by_key.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

    }
}

namespace shooter
{
    void start()
    {
        if (g_running.exchange(true))
        {
            return;
        }
        g_worker = std::thread(run_loop);
    }

    void stop()
    {
        if (!g_running.exchange(false))
        {
            return;
        }
        if (g_worker.joinable())
        {
            g_worker.join();
        }
    }

    bool is_running()
    {
        return g_running.load(std::memory_order_relaxed);
    }
}
