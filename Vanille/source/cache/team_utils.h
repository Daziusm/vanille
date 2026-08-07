#pragma once

#include "cache/local_player_cache.h"
#include "cache/player_cache.h"

namespace cache::team_utils
{
    constexpr std::int64_t phantom_forces_place_id = 292439477;
    constexpr std::int64_t lostfront_place_id = 102871156420149;

    bool is_phantom_forces();
    bool is_lostfront();

    bool is_teammate(const player_state& local, const player_state& other);
    bool is_teammate(const local_player_state& local, const player_state& other);
}
