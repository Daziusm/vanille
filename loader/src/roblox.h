#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace roblox
{
    struct instance
    {
        std::uint32_t pid = 0;
        std::wstring title;
    };

    std::vector<instance> enumerate_instances();
    bool is_vanille_running();
}
