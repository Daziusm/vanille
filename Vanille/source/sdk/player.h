#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "sdk/instance.h"
#include "sdk/offsets.h"
#include "memory/memory.h"

namespace rbx::player
{
    inline std::optional<rbx::instance_t> get_local_player(const rbx::instance_t& players_instance)
    {
        if (!players_instance.is_valid() || !roblox::offsets::players::local_player)
        {
            return std::nullopt;
        }

        const auto player_ptr = memory->read<std::uintptr_t>(players_instance.get_address() + roblox::offsets::players::local_player);
        if (!player_ptr)
        {
            return std::nullopt;
        }

        return rbx::instance_t(player_ptr);
    }

    inline std::optional<rbx::instance_t> get_character(const rbx::instance_t& player_instance)
    {
        if (!player_instance.is_valid() || !roblox::offsets::player::character)
        {
            return std::nullopt;
        }

        const auto character_ptr = memory->read<std::uintptr_t>(player_instance.get_address() + roblox::offsets::player::character);
        if (!character_ptr)
        {
            return std::nullopt;
        }

        return rbx::instance_t(character_ptr);
    }

    inline std::optional<std::uint64_t> get_user_id(const rbx::instance_t& player_instance)
    {
        if (!player_instance.is_valid() || !roblox::offsets::player::user_id)
        {
            return std::nullopt;
        }

        return memory->read<std::uint64_t>(player_instance.get_address() + roblox::offsets::player::user_id);
    }

    inline std::optional<std::uintptr_t> get_team(const rbx::instance_t& player_instance)
    {
        if (!player_instance.is_valid())
        {
            return std::nullopt;
        }

        if (roblox::offsets::player::team)
        {
            const auto team_ptr = memory->read<std::uintptr_t>(player_instance.get_address() + roblox::offsets::player::team);
            if (team_ptr)
            {
                return team_ptr;
            }
        }

        if (roblox::offsets::player::team_color)
        {
            const auto team_color = memory->read<std::uintptr_t>(player_instance.get_address() + roblox::offsets::player::team_color);
            if (team_color)
            {
                return team_color;
            }
        }

        return std::nullopt;
    }

    inline std::optional<std::string> get_team_color_name(const rbx::instance_t& player_instance)
    {
        const auto team_color = get_team(player_instance);
        if (!team_color || *team_color == 0)
        {
            return std::nullopt;
        }

        const auto team_color_instance = rbx::instance_t(*team_color);
        if (!team_color_instance.is_valid())
        {
            return std::nullopt;
        }

        const auto team_color_name = team_color_instance.get_name();
        if (team_color_name.empty())
        {
            return std::nullopt;
        }

        return team_color_name;
    }


    inline std::optional<std::string> get_display_name(const rbx::instance_t& player_instance)
    {
        if (!player_instance.is_valid() || !roblox::offsets::player::display_name)
        {
            return std::nullopt;
        }

        auto read_c_string = [](std::uintptr_t ptr) -> std::string
        {
            if (!ptr)
            {
                return {};
            }

            char buffer[256] = {};
            if (!memory->read_raw(buffer, ptr, sizeof(buffer) - 1))
            {
                return {};
            }
            buffer[sizeof(buffer) - 1] = '\0';
            return std::string(buffer);
        };

        const auto display_name_ptr = memory->read<std::uintptr_t>(player_instance.get_address() + roblox::offsets::player::display_name);
        if (display_name_ptr)
        {
            const auto pointer_value = memory->read_string(display_name_ptr);
            if (!pointer_value.empty() && pointer_value != "Unknown")
            {
                return pointer_value;
            }

            const auto raw_pointer_value = read_c_string(display_name_ptr);
            if (!raw_pointer_value.empty())
            {
                return raw_pointer_value;
            }
        }

        const auto inline_value = memory->read_string(player_instance.get_address() + roblox::offsets::player::display_name);
        if (!inline_value.empty() && inline_value != "Unknown")
        {
            return inline_value;
        }

        const auto raw_inline_value = read_c_string(player_instance.get_address() + roblox::offsets::player::display_name);
        if (!raw_inline_value.empty())
        {
            return raw_inline_value;
        }

        return std::nullopt;
    }
}
