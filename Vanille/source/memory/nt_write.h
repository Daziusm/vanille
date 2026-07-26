#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <windows.h>

inline bool nt_fast_write(HANDLE process, std::uint64_t address, const void* buffer, std::size_t size)
{
    using NtWriteVirtualMemory_t = LONG(__stdcall*)(
        HANDLE ProcessHandle,
        PVOID BaseAddress,
        PVOID Buffer,
        ULONG NumberOfBytesToWrite,
        PULONG NumberOfBytesWritten
    );

    static NtWriteVirtualMemory_t nt_write = nullptr;
    static std::once_flag load_flag;
    std::call_once(load_flag, []()
        {
            HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            if (!ntdll)
            {
                ntdll = ::LoadLibraryW(L"ntdll.dll");
            }
            if (ntdll)
            {
                nt_write = reinterpret_cast<NtWriteVirtualMemory_t>(::GetProcAddress(ntdll, "NtWriteVirtualMemory"));
            }
        });

    if (!process || address == 0 || buffer == nullptr || size == 0 || size > static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()))
    {
        return false;
    }

    auto write_fallback = [&]() -> bool
    {
        SIZE_T written = 0;
        return ::WriteProcessMemory(
            process,
            reinterpret_cast<PVOID>(address),
            buffer,
            size,
            &written
        ) != 0 && written == size;
    };

    if (!nt_write)
    {
        return write_fallback();
    }

    LONG status = -1;
    __try
    {
        status = nt_write(
            process,
            reinterpret_cast<PVOID>(address),
            const_cast<void*>(buffer),
            static_cast<ULONG>(size),
            nullptr
        );
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = -1;
    }

    if (status >= 0)
    {
        return true;
    }

    return write_fallback();
}
