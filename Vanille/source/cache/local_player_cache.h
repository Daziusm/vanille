#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "sdk/instance.h"
#include "cache/player_parts.h"
#include "cache/player_cache.h"

namespace cache
{
    struct local_player_state
    {
        std::uintptr_t address = 0;
        std::string name;
        std::string display_name;
        std::uint64_t user_id = 0;
        std::uintptr_t team = 0;
        bool pf_enemy = false;
        bool pf_enemy_known = false;
        bool has_team_billboard = false;
        rbx::instance_t player;
        rbx::instance_t character;
        rbx::instance_t revolver;
        rbx::instance_t equipped_tool;
        std::string equipped_tool_name;
        int tool_ammo = 0;
        int tool_max_ammo = 0;
        rbx::instance_t camera;
        rbx::instance_t player_gui;
        rbx::instance_t aim_gui;
        character_parts parts;
        cache::player_state::body_effects_state body_effects;

        void clear();
    };

    class local_player_cache
    {
    public:
        ~local_player_cache();

        bool start();
        void stop();
        local_player_state snapshot();

    private:
        void run();
        void update_state();
        void update_character_parts(local_player_state& next_state, const rbx::instance_t& character);
        void update_camera(local_player_state& next_state);
        bool refresh_core_instances();

        std::atomic<bool> running{ false };
        std::thread worker;
        std::mutex mutex;
        local_player_state state;
    };

    inline std::unique_ptr<local_player_cache> localplayer = std::make_unique<local_player_cache>();
}
