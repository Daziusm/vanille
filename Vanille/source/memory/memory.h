#pragma once
#include <windows.h>
#include <TlHelp32.h>
#include <vector>
#include <string>
#include <memory>
#include <atomic>

extern "C" intptr_t
Luck_ReadVirtualMemory
(
	HANDLE ProcessHandle,
	PVOID BaseAddress,
	PVOID Buffer,
	ULONG NumberOfBytesToRead,
	PULONG NumberOfBytesRead
);

extern "C" intptr_t
Luck_WriteVirtualMemory
(
	HANDLE Processhandle,
	PVOID BaseAddress,
	PVOID Buffer,
	ULONG NumberOfBytesToWrite,
	PULONG NumberOfBytesWritten
);

struct memory_stats
{
	std::uint64_t read_ops = 0;
	std::uint64_t read_failures = 0;
	std::uint64_t write_ops = 0;
	std::uint64_t write_failures = 0;
	std::uint64_t bytes_read = 0;
	std::uint64_t bytes_written = 0;
};

class memory_t final
{
public:
	memory_t() = default;
	~memory_t() = default;

	std::uint32_t find_process_id(const std::string& process_name);
	std::uint64_t find_module_address(const std::string& module_name);

	bool attach_to_process(const std::string& process_name);

	std::uintptr_t allocate(std::size_t size);

	std::string read_string(std::uint64_t address);
	bool read_raw(void* buffer, uint64_t address, size_t size);

	template <typename T>
	T read(std::uint64_t address);

	template <typename T>
	bool write(std::uint64_t address, const T& value);

	bool write_string(std::uint64_t address, const std::string& value);
	bool is_process_alive() const;

	std::uint32_t get_process_id();
	std::uint64_t get_module_address();
	HANDLE get_process_handle();
	memory_stats get_stats() const;

	bool write_raw(std::uint64_t address, const void* buffer, size_t size);
private:
	bool write_bytes(std::uint64_t address, const void* buffer, size_t size);
	
	std::uint32_t process_id{};
	std::uint64_t base_address{};
	HANDLE process_handle{};

	std::atomic<std::uint64_t> read_ops{ 0 };
	std::atomic<std::uint64_t> read_failures{ 0 };
	std::atomic<std::uint64_t> write_ops{ 0 };
	std::atomic<std::uint64_t> write_failures{ 0 };
	std::atomic<std::uint64_t> bytes_read_total{ 0 };
	std::atomic<std::uint64_t> bytes_written_total{ 0 };
};

template <typename T>
T memory_t::read(uint64_t address)
{
	T buffer{};

	const auto status = Luck_ReadVirtualMemory(process_handle, reinterpret_cast<void*>(address), &buffer, sizeof(T), nullptr);

	read_ops.fetch_add(1, std::memory_order_relaxed);
	if (status < 0)
	{
		read_failures.fetch_add(1, std::memory_order_relaxed);
	}
	bytes_read_total.fetch_add(sizeof(T), std::memory_order_relaxed);

	return buffer;
}

template <typename T>
bool memory_t::write(uint64_t address, const T& value)
{
	return write_bytes(address, &value, sizeof(T));
}

inline std::unique_ptr<memory_t> memory = std::make_unique<memory_t>();
