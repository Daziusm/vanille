#pragma once

#include <cstdint>
#include <limits>

#include "sdk/math_types.h"
#include "memory/memory.h"
#include "memory/nt_write.h"

namespace rbx::mouse_service
{
    inline std::uint64_t get_input_object(std::uintptr_t base_address)
    {
        return memory->read<std::uint64_t>(base_address + roblox::offsets::mouse_service::input_object);
    }

    inline bool write_mouse_position(std::uint64_t input_object, const rbx::Vector2& position)
    {
        const std::uint64_t invalid_ptr = (std::numeric_limits<std::uint64_t>::max)();
        if (input_object == 0 || input_object == invalid_ptr)
        {
            return false;
        }

        static HANDLE cached_handle = nullptr;
        HANDLE handle = cached_handle ? cached_handle : memory->get_process_handle();
        if (!handle)
        {
            cached_handle = nullptr;
            return false;
        }
        cached_handle = handle;

        return nt_fast_write(handle, input_object + roblox::offsets::mouse_service::mouse_position, &position, sizeof(position));
    }
}
