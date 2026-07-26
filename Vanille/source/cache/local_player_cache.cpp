#include "cache/local_player_cache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cstddef>
#include <exception>

#include "cache/player_parts.h"
#include "globals/globals.h"
#include "memory/memory.h"
#include "sdk/engine.h"
#include "sdk/part.h"
#include "sdk/offsets.h"
#include "sdk/player.h"
#include "sdk/value.h"
#include "cache/player_cache.h"

namespace
{
    constexpr double k_parts_refresh_interval = 0.5;
    constexpr std::int64_t k_phantom_forces_place_id = 292439477;

    double now_seconds()
    {
        using clock = std::chrono::steady_clock;
        return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    int compute_cache_sleep_ms(int min_ms, int max_ms, int fallback_ms)
    {
        const float dt = g_overlay_dt.load(std::memory_order_relaxed);
        if (!std::isfinite(dt) || dt <= 0.0f)
        {
            return min_ms;
        }

        const int target_ms = static_cast<int>(std::lround(dt * 1000.0f));
        return std::clamp(target_ms, min_ms, max_ms);
    }

    void fill_part(cache::primitive_part& target, const rbx::instance_t& part)
    {
        target.instance = part;
        target.primitive = rbx::part::get_primitive(part);
        target.size = {};

        if (const auto size = rbx::part::get_size(target.primitive))
        {
            target.size = *size;
        }
    }

    constexpr std::size_t k_max_children = 2048;

    std::vector<rbx::instance_t> get_children_safely(const rbx::instance_t& instance, std::string_view context)
    {
        if (!instance.is_valid())
        {
            return {};
        }

        try
        {
            auto children = instance.get_children();
            if (children.size() > k_max_children)
            {
                children.resize(k_max_children);
            }
            return children;
        }
        catch (const std::exception& ex)
        {
            return {};
        }
        catch (...)
        {
            return {};
        }
    }

    bool is_player_instance(const rbx::instance_t& instance)
    {
        if (!instance.is_valid())
        {
            return false;
        }

        try
        {
            return instance.IsA("Player");
        }
        catch (...)
        {
            return false;
        }
    }

    bool is_model_instance(const rbx::instance_t& instance)
    {
        if (!instance.is_valid())
        {
            return false;
        }

        try
        {
            return instance.IsA("Model");
        }
        catch (...)
        {
            return false;
        }
    }

    bool has_humanoid_root_part(const rbx::instance_t& model)
    {
        if (!model.is_valid())
        {
            return false;
        }

        if (!is_model_instance(model))
        {
            return false;
        }

        const auto root = model.find_first_child("HumanoidRootPart");
        return root.is_valid();
    }

    rbx::instance_t resolve_pf_local_model()
    {
        static std::uintptr_t cached_model_address = 0;
        static double last_refresh = 0.0;
        static double next_retry_time = 0.0;
        constexpr double k_refresh_interval = 0.35;
        constexpr double k_missing_retry_interval = 1.0;

        const double now = now_seconds();
        if (cached_model_address == 0 && now < next_retry_time)
        {
            globals->pf_local_player_model = {};
            return {};
        }

        const auto workspace = globals->workspace;
        if (!workspace.is_valid())
        {
            next_retry_time = now + k_missing_retry_interval;
            globals->pf_local_player_model = {};
            return {};
        }

        const auto ignore = workspace.find_first_child("Ignore");
        if (!ignore.is_valid())
        {
            next_retry_time = now + k_missing_retry_interval;
            globals->pf_local_player_model = {};
            return {};
        }

        if (cached_model_address != 0 && (now - last_refresh) < k_refresh_interval)
        {
            const rbx::instance_t cached(cached_model_address);
            if (has_humanoid_root_part(cached))
            {
                globals->pf_local_player_model = cached;
                return cached;
            }
        }

        rbx::instance_t found = ignore.find_first_child_by_class("Model");
        if (!has_humanoid_root_part(found))
        {
            const auto children = get_children_safely(ignore, "pf_local_ignore");
            for (const auto& child : children)
            {
                if (has_humanoid_root_part(child))
                {
                    found = child;
                    break;
                }
            }
        }

        if (has_humanoid_root_part(found))
        {
            cached_model_address = found.get_address();
            last_refresh = now;
            next_retry_time = now;
            globals->pf_local_player_model = found;
            return found;
        }

        cached_model_address = 0;
        last_refresh = now;
        next_retry_time = now + k_missing_retry_interval;
        globals->pf_local_player_model = {};
        return {};
    }

    rbx::instance_t get_player_instance()
    {
        auto players = globals->players;
        if (!players.is_valid())
        {
            return {};
        }

        static std::uintptr_t cached_player = 0;
        const bool is_phantom_forces = (globals->game_id == k_phantom_forces_place_id);
        if (const auto player = rbx::player::get_local_player(players))
        {
            cached_player = player->get_address();
            return *player;
        }

        rbx::instance_t pf_local_model;
        if (is_phantom_forces)
        {
            pf_local_model = resolve_pf_local_model();
        }
        else
        {
            globals->pf_local_player_model = {};
        }

        if (is_phantom_forces && pf_local_model.is_valid())
        {
            return pf_local_model;
        }

        if (is_phantom_forces)
        {
            if (cached_player != 0)
            {
                const rbx::instance_t cached(cached_player);
                if (is_player_instance(cached) || is_model_instance(cached))
                {
                    return cached;
                }
                cached_player = 0;
            }

            return {};
        }

        if (cached_player != 0)
        {
            return rbx::instance_t(cached_player);
        }

        for (const auto& child : players.get_children())
        {
            if (child.get_class_name() == "Player")
            {
                cached_player = child.get_address();
                return child;
            }
        }

        const auto workspace = globals->workspace;
        if (workspace.is_valid())
        {
            const auto model = workspace.find_first_child_by_class("Model");
            if (model.is_valid())
            {
                cached_player = model.get_address();
                return model;
            }
        }

        return {};
    }

    rbx::instance_t resolve_character(const rbx::instance_t& player, const std::string& name)
    {
        if (!player.is_valid())
        {
            return {};
        }

        if (globals->game_id == k_phantom_forces_place_id)
        {
            const auto cached_pf_model = globals->pf_local_player_model;
            if (has_humanoid_root_part(cached_pf_model))
            {
                return cached_pf_model;
            }

            const auto pf_model = resolve_pf_local_model();
            if (pf_model.is_valid())
            {
                return pf_model;
            }

            return {};
        }

        if (is_player_instance(player))
        {
            if (const auto character = rbx::player::get_character(player))
            {
                return *character;
            }
        }
        else if (is_model_instance(player))
        {
            return player;
        }

        const auto workspace = globals->workspace;
        if (workspace.is_valid())
        {
            const auto child = workspace.find_first_child(name);
            if (child.is_valid())
            {
                return child;
            }

            const auto model = workspace.find_first_child_by_class("Model");
            if (model.is_valid())
            {
                return model;
            }
        }

        return {};
    }

    rbx::instance_t get_revolver(const rbx::instance_t& character)
    {
        if (!character.is_valid())
        {
            return {};
        }

        return character.find_first_child("[Revolver]");
    }

    rbx::instance_t get_descendant_by_name(const rbx::instance_t& root, std::string_view name)
    {
        std::vector<rbx::instance_t> stack = get_children_safely(root, "descendant_search");
        if (stack.empty())
        {
            return {};
        }

        while (!stack.empty())
        {
            const auto current = stack.back();
            stack.pop_back();

            const auto current_children = get_children_safely(current, "descendant_walk");
            for (const auto& child : current_children)
            {
                if (child.get_name() == name)
                {
                    return child;
                }

                stack.emplace_back(child);
            }
        }

        return {};
    }

    rbx::instance_t get_player_gui(const rbx::instance_t& player)
    {
        if (!player.is_valid())
        {
            return {};
        }

        return player.find_first_child("PlayerGui");
    }

    rbx::instance_t get_aim_gui(const rbx::instance_t& player_gui)
    {
        if (!player_gui.is_valid())
        {
            return {};
        }

        static std::uintptr_t cached_player_gui = 0;
        static std::uintptr_t cached_aim_gui = 0;
        static double last_refresh = 0.0;
        constexpr double k_refresh_interval = 1.0;

        const double now = now_seconds();
        const std::uintptr_t player_gui_addr = player_gui.get_address();
        if (player_gui_addr != 0 && player_gui_addr == cached_player_gui && cached_aim_gui != 0 && (now - last_refresh) < k_refresh_interval)
        {
            return rbx::instance_t(cached_aim_gui);
        }

        auto aim_gui = get_descendant_by_name(player_gui, "Aim");
        cached_player_gui = player_gui_addr;
        cached_aim_gui = aim_gui.get_address();
        last_refresh = now;

        if (aim_gui.is_valid())
        {
            return aim_gui;
        }

        return {};
    }

}

namespace cache
{
    local_player_cache::~local_player_cache()
    {
        stop();
    }

    bool local_player_cache::start()
    {
        if (running.load())
        {
            return true;
        }

        running = true;
        worker = std::thread(&local_player_cache::run, this);
        return true;
    }

    void local_player_cache::stop()
    {
        if (!running.exchange(false))
        {
            return;
        }

        if (worker.joinable())
        {
            worker.join();
        }
    }

    local_player_state local_player_cache::snapshot()
    {
        std::scoped_lock lock(mutex);
        return state;
    }

    void local_player_cache::run()
    {
        while (running.load())
        {
            update_state();
            const int sleep_ms = compute_cache_sleep_ms(4, 200, 200);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }

    bool local_player_cache::refresh_core_instances()
    {
        const bool need_refresh = !globals->datamodel.is_valid() || !globals->players.is_valid() || !globals->workspace.is_valid();
        if (need_refresh)
        {
            if (!rbx::engine->refresh())
            {
                return false;
            }
            globals->datamodel = rbx::engine->get_datamodel();
        }

        if (!globals->players.is_valid() && globals->datamodel.is_valid())
        {
            globals->players = globals->datamodel.find_first_child_by_class("Players");
        }

        if (!globals->workspace.is_valid() && globals->datamodel.is_valid())
        {
            globals->workspace = globals->datamodel.find_first_child_by_class("Workspace");
        }

        if (!globals->datamodel.is_valid())
        {
            return false;
        }

        if (!globals->players.is_valid())
        {
            return false;
        }

        if (!globals->workspace.is_valid())
        {
            return false;
        }

        return true;
    }

    void local_player_cache::update_state()
    {
        static std::uintptr_t last_player_address = 0;
        static std::uintptr_t last_character_address = 0;
        static double last_parts_refresh = 0.0;

        if (!refresh_core_instances())
        {
            std::scoped_lock lock(mutex);
            state.clear();
            if (last_player_address != 0)
            {
                last_player_address = 0;
                last_character_address = 0;
            }
            return;
        }

        cache::player_state::body_effects_state previous_body_effects{};
        cache::character_parts previous_parts{};
        std::uintptr_t previous_character = 0;
        std::uintptr_t previous_player = 0;
        std::string previous_name;
        std::string previous_display_name;
        std::uint64_t previous_user_id = 0;
        {
            std::scoped_lock lock(mutex);
            previous_body_effects = state.body_effects;
            previous_parts = state.parts;
            previous_character = state.character.get_address();
            previous_player = state.player.get_address();
            previous_name = state.name;
            previous_display_name = state.display_name;
            previous_user_id = state.user_id;
        }

        local_player_state next_state;

        const auto player = get_player_instance();
        if (!player.is_valid())
        {
            std::scoped_lock lock(mutex);
            state.clear();
            if (last_player_address != 0)
            {
                last_player_address = 0;
                last_character_address = 0;
            }
            return;
        }

        next_state.player = player;
        next_state.address = player.get_address();
        const bool is_player = is_player_instance(player);
        if (previous_player == next_state.address)
        {
            next_state.name = previous_name;
            next_state.display_name = previous_display_name;
            next_state.user_id = previous_user_id;
        }

        if (next_state.name.empty())
        {
            next_state.name = player.get_name();
        }
        if (is_player)
        {
            if (next_state.display_name.empty())
            {
                if (const auto display_name_value = rbx::player::get_display_name(player))
                {
                    next_state.display_name = *display_name_value;
                }
            }

            if (next_state.user_id == 0)
            {
                if (const auto user_id_value = rbx::player::get_user_id(player))
                {
                    next_state.user_id = *user_id_value;
                }
            }

            if (const auto team_value = rbx::player::get_team(player))
            {
                next_state.team = *team_value;
            }
        }

        if (next_state.display_name.empty())
        {
            next_state.display_name = next_state.name;
        }

        next_state.character = resolve_character(player, next_state.name);
        next_state.revolver = get_revolver(next_state.character);
        next_state.equipped_tool = next_state.character.find_first_child_by_class("Tool");
        next_state.equipped_tool_name.clear();
        next_state.tool_ammo = 0;
        next_state.tool_max_ammo = 0;
        if (next_state.equipped_tool.is_valid())
        {
            next_state.equipped_tool_name = next_state.equipped_tool.get_name();
            const auto ammo_child = next_state.equipped_tool.find_first_child("Ammo");
            if (ammo_child.is_valid())
            {
                if (const auto value = rbx::value::get_int(ammo_child))
                {
                    next_state.tool_ammo = *value;
                }
            }
            const auto max_ammo_child = next_state.equipped_tool.find_first_child("MaxAmmo");
            if (max_ammo_child.is_valid())
            {
                if (const auto value = rbx::value::get_int(max_ammo_child))
                {
                    next_state.tool_max_ammo = *value;
                }
            }
        }
        const std::uintptr_t current_character = next_state.character.get_address();
        const double now = now_seconds();
        const bool character_same = current_character != 0 && current_character == previous_character;
        if (!character_same)
        {
            last_parts_refresh = 0.0;
        }

        const bool reuse_parts = character_same &&
            (previous_parts.head.instance.is_valid() || previous_parts.humanoid_root_part.instance.is_valid());
        const bool refresh_due = (last_parts_refresh <= 0.0)
            || ((now - last_parts_refresh) >= k_parts_refresh_interval);

        if (reuse_parts && !refresh_due)
        {
            next_state.parts = previous_parts;
        }
        else
        {
            update_character_parts(next_state, next_state.character);
            if (next_state.character.is_valid())
            {
                last_parts_refresh = now;
            }
        }
        update_camera(next_state);
        next_state.player_gui = get_player_gui(player);
        next_state.aim_gui = get_aim_gui(next_state.player_gui);
        next_state.body_effects = previous_body_effects;

        if (next_state.character.is_valid())
        {
            auto body_effects = next_state.character.find_first_child("BodyEffects");
            next_state.body_effects.container = body_effects;

            if (body_effects.is_valid())
            {
                auto read_bool = [&](std::string_view name, bool& target)
                {
                    const auto child = body_effects.find_first_child(name);
                    if (child.is_valid())
                    {
                        if (const auto value = rbx::value::get_bool(child))
                        {
                            target = *value;
                        }
                    }
                };

                auto read_armor_value = [&](int& target)
                {
                    auto try_set = [&](const rbx::instance_t& child, int& out_target)
                    {
                        if (const auto as_int = rbx::value::get_int(child))
                        {
                            out_target = *as_int;
                            return true;
                        }
                        if (const auto as_num = rbx::value::get_number(child))
                        {
                            out_target = static_cast<int>(std::round(*as_num));
                            return true;
                        }
                        return false;
                    };

                    const auto armor_child = body_effects.find_first_child("Armor");
                    const auto fire_armor_child = body_effects.find_first_child("FireArmor");
                    const auto defense_child = body_effects.find_first_child("Defense");

                    int new_armor = target;
                    if (armor_child.is_valid()) try_set(armor_child, new_armor);
                    if (new_armor <= 0 && fire_armor_child.is_valid()) try_set(fire_armor_child, new_armor);
                    if (new_armor <= 0 && defense_child.is_valid()) try_set(defense_child, new_armor);
                    target = new_armor;
                };

                auto read_int = [&](std::string_view name, int& target)
                {
                    const auto child = body_effects.find_first_child(name);
                    if (child.is_valid())
                    {
                        if (const auto value = rbx::value::get_int(child))
                        {
                            target = *value;
                        }
                    }
                };

                read_bool("Reload", next_state.body_effects.reload);
                read_armor_value(next_state.body_effects.armor);
                read_bool("Grabbed", next_state.body_effects.grabbed);
                read_bool("GunFiring", next_state.body_effects.gun_firing);
                read_bool("K.O", next_state.body_effects.knocked);
                read_bool("Dead", next_state.body_effects.dead);
                read_bool("Cuff", next_state.body_effects.cuffed);
            }
        }

        last_player_address = next_state.address;
        last_character_address = next_state.character.get_address();

        std::scoped_lock lock(mutex);
        state = std::move(next_state);
    }

    void local_player_cache::update_character_parts(local_player_state& next_state, const rbx::instance_t& character)
    {
        next_state.parts.clear();

        if (!character.is_valid())
        {
            return;
        }

        const auto children = get_children_safely(character, "character");
        std::unordered_map<std::string, rbx::instance_t> part_map;
        part_map.reserve(children.size());
        for (const auto& child : children)
        {
            part_map.emplace(child.get_name(), child);
        }

        auto get_by_name = [&](std::string_view name) -> rbx::instance_t
        {
            auto it = part_map.find(std::string(name));
            if (it != part_map.end())
            {
                return it->second;
            }
            return {};
        };

        const bool has_upper_torso = part_map.find("UpperTorso") != part_map.end();
        const bool has_lower_torso = part_map.find("LowerTorso") != part_map.end();
        next_state.parts.is_r15 = has_upper_torso || has_lower_torso;
        if (next_state.parts.is_r15)
        {
            fill_part(next_state.parts.humanoid_root_part, get_by_name("HumanoidRootPart"));
            fill_part(next_state.parts.head, get_by_name("Head"));
            fill_part(next_state.parts.upper_torso, get_by_name("UpperTorso"));
            fill_part(next_state.parts.lower_torso, get_by_name("LowerTorso"));
            fill_part(next_state.parts.left_upper_arm, get_by_name("LeftUpperArm"));
            fill_part(next_state.parts.left_lower_arm, get_by_name("LeftLowerArm"));
            fill_part(next_state.parts.left_hand, get_by_name("LeftHand"));
            fill_part(next_state.parts.right_upper_arm, get_by_name("RightUpperArm"));
            fill_part(next_state.parts.right_lower_arm, get_by_name("RightLowerArm"));
            fill_part(next_state.parts.right_hand, get_by_name("RightHand"));
            fill_part(next_state.parts.left_upper_leg, get_by_name("LeftUpperLeg"));
            fill_part(next_state.parts.left_lower_leg, get_by_name("LeftLowerLeg"));
            fill_part(next_state.parts.left_foot, get_by_name("LeftFoot"));
            fill_part(next_state.parts.right_upper_leg, get_by_name("RightUpperLeg"));
            fill_part(next_state.parts.right_lower_leg, get_by_name("RightLowerLeg"));
            fill_part(next_state.parts.right_foot, get_by_name("RightFoot"));
        }
        else
        {
            fill_part(next_state.parts.humanoid_root_part, get_by_name("HumanoidRootPart"));
            fill_part(next_state.parts.head, get_by_name("Head"));
            fill_part(next_state.parts.torso, get_by_name("Torso"));
            fill_part(next_state.parts.left_arm, get_by_name("Left Arm"));
            fill_part(next_state.parts.right_arm, get_by_name("Right Arm"));
            fill_part(next_state.parts.left_leg, get_by_name("Left Leg"));
            fill_part(next_state.parts.right_leg, get_by_name("Right Leg"));
        }
    }

    void local_player_cache::update_camera(local_player_state& next_state)
    {
        const auto workspace = globals->workspace;
        if (!workspace.is_valid())
        {
            next_state.camera = {};
            return;
        }

        static rbx::instance_t cached_camera;
        static double last_refresh = 0.0;
        constexpr double k_refresh_interval = 0.25;
        const double now = now_seconds();
        if (cached_camera.is_valid() && (now - last_refresh) < k_refresh_interval)
        {
            next_state.camera = cached_camera;
            return;
        }

        auto camera = workspace.find_first_child("CurrentCamera");
        if (!camera.is_valid())
        {
            camera = workspace.find_first_child("Camera");
        }

        cached_camera = camera;
        last_refresh = now;
        next_state.camera = camera;
    }

    void local_player_state::clear()
    {
        *this = {};
    }
}
