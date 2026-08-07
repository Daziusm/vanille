#include "cache/team_utils.h"

#include "globals/globals_fixed.h"

namespace cache::team_utils
{
    bool is_phantom_forces()
    {
        return globals && globals->game_id == phantom_forces_place_id;
    }

    bool is_lostfront()
    {
        return globals && globals->game_id == lostfront_place_id;
    }

    static bool is_teammate_phantom_forces(const player_state& local, const player_state& other)
    {
        if (local.pf_enemy_known && other.pf_enemy_known)
        {
            return local.pf_enemy == other.pf_enemy;
        }

        if (other.pf_enemy_known)
        {
            return !other.pf_enemy;
        }

        if (local.team != 0 && other.team != 0)
        {
            return local.team == other.team;
        }

        return false;
    }

    static bool is_teammate_lostfront(const player_state& other)
    {
        return other.has_team_billboard;
    }

    static bool is_teammate_default(const player_state& local, const player_state& other)
    {
        return local.team != 0 && other.team != 0 && local.team == other.team;
    }

    bool is_teammate(const player_state& local, const player_state& other)
    {
        if (local.address == 0 || other.address == 0 || local.address == other.address)
        {
            return false;
        }

        if (is_phantom_forces())
        {
            return is_teammate_phantom_forces(local, other);
        }

        if (is_lostfront())
        {
            return is_teammate_lostfront(other);
        }

        return is_teammate_default(local, other);
    }

    bool is_teammate(const local_player_state& local, const player_state& other)
    {
        if (local.address == 0 || other.address == 0)
        {
            return false;
        }

        if (local.address == other.address)
        {
            return true;
        }

        player_state local_as_player{};
        local_as_player.address = local.address;
        local_as_player.team = local.team;
        local_as_player.pf_enemy = local.pf_enemy;
        local_as_player.pf_enemy_known = local.pf_enemy_known;
        local_as_player.has_team_billboard = local.has_team_billboard;

        return is_teammate(local_as_player, other);
    }
}
