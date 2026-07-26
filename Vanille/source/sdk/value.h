#pragma once

#include <cstdint>
#include <optional>

#include "memory/memory.h"
#include "sdk/instance.h"
#include "sdk/offsets.h"

namespace rbx::value
{
    inline std::optional<bool> get_bool(const rbx::instance_t& instance)
    {
        if (!instance.is_valid() || !roblox::offsets::value_bool::value)
        {
            return std::nullopt;
        }

        return memory->read<bool>(instance.get_address() + roblox::offsets::value_bool::value);
    }

    inline std::optional<int> get_int(const rbx::instance_t& instance)
    {
        if (!instance.is_valid() || !roblox::offsets::value_int::value)
        {
            return std::nullopt;
        }

        return memory->read<int>(instance.get_address() + roblox::offsets::value_int::value);
    }

    inline std::optional<double> get_number(const rbx::instance_t& instance)
    {
        if (!instance.is_valid() || !roblox::offsets::value_number::value)
        {
            return std::nullopt;
        }

        return memory->read<double>(instance.get_address() + roblox::offsets::value_number::value);
    }
}
