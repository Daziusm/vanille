#pragma once

#include <cstdint>
#include <optional>

#include "sdk/math_types.h"

namespace free_aim
{
    void render_aim_frame();
    bool start();
    void stop();
    bool is_locked_target(std::uintptr_t player_address);
    std::uintptr_t get_locked_player();
    std::optional<rbx::Vector3> get_target_world_position();
    std::optional<rbx::Vector2> get_target_screen_position();
}
