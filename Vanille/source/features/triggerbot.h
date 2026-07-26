#pragma once

#include <cstdint>

namespace triggerbot
{
    bool start();
    void stop();
    bool is_locked_target(std::uintptr_t player_address);
    std::uintptr_t get_locked_player();
}
