#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "cache/player_cache.h"
#include "cache/local_player_cache.h"
#include "sdk/math_types.h"

namespace visibility
{
    struct visibility_result
    {
        bool visible = true;
        float coverage = 1.0f;
    };

    struct debug_occluder_primitive
    {
        std::uintptr_t primitive = 0;
        rbx::Vector3 position{};
        rbx::Vector3 half_size{};
        float rotation[9]{};
    };

    bool is_raycast_engine_enabled();
    bool is_any_occluded_check_enabled();
    bool should_show_raycast_engine_warning();
    bool can_run_visibility_check(bool feature_visibility_check_enabled);

    visibility_result is_player_visible(const cache::player_state& player, const cache::local_player_state& local, const rbx::Matrix& view_matrix);
    void get_debug_occluder_primitives(std::vector<debug_occluder_primitive>& out_primitives, std::size_t max_count = 0);
    void reset_occluder_cache();
}
