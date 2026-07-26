#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cache/player_parts.h"
#include "sdk/instance.h"
#include "sdk/mesh_part.h"

namespace cache
{
    struct player_state
    {
        std::uintptr_t address = 0;
        std::string name;
        std::string display_name;
        std::uint64_t user_id = 0;
        std::uintptr_t team = 0;
        bool has_team_billboard = false;
        float health = 0.0f;
        float max_health = 0.0f;
        rbx::instance_t humanoid;
        struct body_effects_state
        {
            rbx::instance_t container;
            bool reload = false;
            int armor = 0;
            bool grabbed = false;
            bool gun_firing = false;
            bool knocked = false;
            bool dead = false;
            bool cuffed = false;
        } body_effects;
        struct movement_state
        {
            rbx::Vector3 move_direction{};
            bool has_move_direction = false;
            bool moving = false;
            bool jumping = false;
            bool has_jump_state = false;
        } movement;
        rbx::instance_t player;
        rbx::instance_t character;
        character_parts parts;
        double last_parts_refresh = 0.0;

        void clear()
        {
            *this = {};
        }
    };

    struct dummy_state
    {
        std::uintptr_t address = 0;
        std::string name;
        float health = 0.0f;
        float max_health = 0.0f;
        rbx::instance_t model;
        rbx::instance_t character;
        rbx::instance_t humanoid;
        rbx::instance_t humanoid_root_part;
        character_parts parts;
        player_state::body_effects_state body_effects;

        void clear()
        {
            *this = {};
        }
    };

    class player_cache
    {
    public:
        ~player_cache();

        bool start();
        void stop();
        std::shared_ptr<const std::vector<player_state>> snapshot();
        std::shared_ptr<const dummy_state> dummy_snapshot();

    private:
        void run();
        bool refresh_core_instances();
        void update_state();
        void fill_player_state(player_state& out_state, const rbx::instance_t& player, const player_state* previous_state);
        void update_character_parts(player_state& out_state, const rbx::instance_t& character);
        void fill_dummy_state(dummy_state& out_state, const rbx::instance_t& model);
        void update_character_parts(character_parts& out_parts, const rbx::instance_t& character);

        std::atomic<bool> running{ false };
        std::thread worker;
        std::mutex mutex;
        std::shared_ptr<std::vector<player_state>> players{ std::make_shared<std::vector<player_state>>() };
        std::shared_ptr<dummy_state> dummy{ std::make_shared<dummy_state>() };
    };

    inline std::unique_ptr<player_cache> players_cache = std::make_unique<player_cache>();

    std::vector<std::uint64_t> collect_player_mesh_asset_ids();
    void download_mesh_assets_to_temp();
    bool get_mesh_data(std::uint64_t asset_id, rbx::mesh_parse::mesh_data& out_mesh);
    bool ensure_mesh_data(std::uint64_t asset_id, rbx::mesh_parse::mesh_data& out_mesh);
}
