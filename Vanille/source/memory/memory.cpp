#include "memory.h"

#include <cstring>
#include <algorithm>

#include "sdk/offsets.h"
#include "utils/logger.h"

std::uint32_t memory_t::find_process_id(const std::string& process_name)
{
	std::uint32_t local_process_id = 0;
	process_id = 0;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);

	if (snapshot == INVALID_HANDLE_VALUE)
	{
		return local_process_id;
	}

	PROCESSENTRY32 process_entry{};
	process_entry.dwSize = sizeof(PROCESSENTRY32);

	if (Process32First(snapshot, &process_entry))
	{
		do
		{
			if (!_stricmp(process_name.c_str(), process_entry.szExeFile))
			{
				local_process_id = process_entry.th32ProcessID;
				process_id = local_process_id;
				break;
			}
		} while (Process32Next(snapshot, &process_entry));
	}

	CloseHandle(snapshot);
	process_id = local_process_id;
	return local_process_id;
}

std::uint64_t memory_t::find_module_address(const std::string& module_name)
{
	std::uint64_t module_address = 0;
	base_address = 0;

	if (!process_handle)
	{
		return module_address;
	}

	DWORD process_id = GetProcessId(process_handle);
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);

	if (snapshot == INVALID_HANDLE_VALUE)
	{
		return module_address;
	}

	MODULEENTRY32 module_entry{};
	module_entry.dwSize = sizeof(MODULEENTRY32);

	if (Module32First(snapshot, &module_entry))
	{
		do
		{
			if (!_stricmp(module_name.c_str(), module_entry.szModule))
			{
				module_address = reinterpret_cast<uint64_t>(module_entry.modBaseAddr);
				base_address = module_address;
				break;
			}
		} while (Module32Next(snapshot, &module_entry));
	}

	CloseHandle(snapshot);
	base_address = module_address;
	return module_address;
}

std::uintptr_t memory_t::allocate(std::size_t size)
{
	if (!process_handle || size == 0)
	{
		return 0;
	}

	void* allocated = VirtualAllocEx(process_handle, nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	return reinterpret_cast<std::uintptr_t>(allocated);
}

bool memory_t::attach_to_process(const std::string& process_name)
{
	const auto target_process_id = find_process_id(process_name);
	if (!target_process_id)
	{
		return false;
	}

	HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, false, target_process_id);
	if (!process)
	{
		return false;
	}

	const HANDLE previous_handle = process_handle;
	process_handle = process;
	process_id = target_process_id;
	base_address = 0;
	if (previous_handle && previous_handle != process_handle)
	{
		CloseHandle(previous_handle);
	}

	return true;
}

std::string memory_t::read_string(std::uint64_t address)
{
	if (!roblox::offsets::rbx_string::length)
	{
		return "Unknown";
	}

	std::int32_t string_length = read<std::int32_t>(address + roblox::offsets::rbx_string::length);
	if (string_length <= 0 || string_length > 255)
	{
		return "Unknown";
	}

	std::uint64_t string_address = (string_length >= 16) ? read<std::uint64_t>(address) : address;

	if (string_address == 0)
	{
		return "Unknown";
	}

	std::vector<char> buffer(string_length + 1, 0);
	Luck_ReadVirtualMemory(process_handle, reinterpret_cast<void*>(string_address), buffer.data(), buffer.size(), nullptr);

	return std::string(buffer.data(), string_length);
}

bool memory_t::read_raw(void* buffer, uint64_t address, size_t size)
{
	const auto status = Luck_ReadVirtualMemory(process_handle,reinterpret_cast<void*>(address), buffer, static_cast<ULONG>(size), nullptr);
	read_ops.fetch_add(1, std::memory_order_relaxed);
	if (status < 0)
	{
		read_failures.fetch_add(1, std::memory_order_relaxed);
	}
	bytes_read_total.fetch_add(size, std::memory_order_relaxed);

	return status >= 0;
}

bool memory_t::write_raw(std::uint64_t address, const void* buffer, size_t size)
{
	return write_bytes(address, buffer, size);
}

bool memory_t::write_string(std::uint64_t address, const std::string& value)
{
	if (address == 0)
	{
		return false;
	}

	const auto is_valid_ptr = [](std::uintptr_t ptr) -> bool
	{
		constexpr std::uintptr_t k_min = 0x10000;
		constexpr std::uintptr_t k_max = 0x00007FFFFFFFFFFF;
		return ptr >= k_min && ptr <= k_max;
	};

	if (!is_valid_ptr(static_cast<std::uintptr_t>(address)))
	{
		return false;
	}

	struct rbx_string
	{
		union
		{
			char buffer[16];
			std::uintptr_t pointer;
		} data;
		std::uintptr_t length;
		std::uintptr_t capacity;
	};

	rbx_string str = read<rbx_string>(address);
	if (str.capacity == 0)
	{
		str.capacity = 15;
	}

	constexpr std::uintptr_t k_max_capacity = 1u << 20; // 1MB guard
	if (str.capacity > k_max_capacity)
	{
		return false;
	}

	str.length = static_cast<std::uintptr_t>(value.length());

	if (str.length > 15)
	{
		if (str.length > str.capacity || str.data.pointer == 0)
		{
			while (str.length > str.capacity)
			{
				str.capacity = str.capacity * 2 + 1;
				if (str.capacity > k_max_capacity)
				{
					return false;
				}
			}
			str.data.pointer = allocate(static_cast<std::size_t>(str.capacity));
			if (!str.data.pointer)
			{
				return false;
			}
		}

		if (!is_valid_ptr(str.data.pointer))
		{
			return false;
		}

		if (!write<rbx_string>(address, str))
		{
			return false;
		}

		if (!value.empty())
		{
			if (!write_raw(str.data.pointer, value.c_str(), value.length()))
			{
				return false;
			}
		}
		write<char>(str.data.pointer + str.length, '\0');
	}
	else
	{
		str.capacity = 15;
		std::memset(str.data.buffer, 0, sizeof(str.data.buffer));
		if (!value.empty())
		{
			std::memcpy(str.data.buffer, value.c_str(), value.length());
		}
		if (str.length < sizeof(str.data.buffer))
		{
			str.data.buffer[str.length] = '\0';
		}
		if (!write<rbx_string>(address, str))
		{
			return false;
		}
	}

	return true;
}

bool memory_t::write_bytes(std::uint64_t address, const void* buffer, size_t size)
{
	if (!process_handle)
	{
		logger_core::log_error("memory write failed: missing process handle (addr=0x{:X}, size={})", address, size);
		return false;
	}

	if (address == 0 || buffer == nullptr || size == 0)
	{
		logger_core::log_error("memory write failed: invalid args (addr=0x{:X}, size={})", address, size);
		return false;
	}

	ULONG bytes_written = 0;
	const auto status = Luck_WriteVirtualMemory(
		process_handle,
		reinterpret_cast<void*>(address),
		const_cast<void*>(buffer),
		static_cast<ULONG>(size),
		&bytes_written
	);
	
	const bool ok = status >= 0;
	write_ops.fetch_add(1, std::memory_order_relaxed);
	if (ok)
	{
		bytes_written_total.fetch_add(bytes_written, std::memory_order_relaxed);
	}
	else
	{
		write_failures.fetch_add(1, std::memory_order_relaxed);
	}

	if (!ok)
	{
		const auto error = GetLastError();
		logger_core::log_error(
			"memory write failed @ 0x{:X} ({} bytes) status=0x{:X} last_error={}",
			address,
			size,
			static_cast<unsigned long>(status),
			error
		);
		return false;
	}

	if (bytes_written != size)
	{
		logger_core::log_warning(
			"partial memory write @ 0x{:X}: wrote {} of {} bytes",
			address,
			bytes_written,
			size
		);
		return false;
	}

	return true;
}

bool memory_t::is_process_alive() const
{
	if (!process_handle)
	{
		return false;
	}

	DWORD exit_code = 0;
	if (!GetExitCodeProcess(process_handle, &exit_code))
	{
		return false;
	}

	return exit_code == STILL_ACTIVE;
}

std::uint32_t memory_t::get_process_id()
{
	return process_id;
}

std::uint64_t memory_t::get_module_address()
{
	return base_address;
}

HANDLE memory_t::get_process_handle()
{
	return process_handle;
}

memory_stats memory_t::get_stats() const
{
	memory_stats stats{};
	stats.read_ops = read_ops.load(std::memory_order_relaxed);
	stats.read_failures = read_failures.load(std::memory_order_relaxed);
	stats.write_ops = write_ops.load(std::memory_order_relaxed);
	stats.write_failures = write_failures.load(std::memory_order_relaxed);
	stats.bytes_read = bytes_read_total.load(std::memory_order_relaxed);
	stats.bytes_written = bytes_written_total.load(std::memory_order_relaxed);
	return stats;
}

