#include "utils/logger.h"

#include <format>
#include <fstream>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <windows.h>

namespace {
    HANDLE resolve_handle(std::FILE* stream)
    {
        return stream == stderr ? GetStdHandle(STD_ERROR_HANDLE) : GetStdHandle(STD_OUTPUT_HANDLE);
    }

    WORD read_default_attributes(HANDLE handle)
    {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(handle, &info))
        {
            return info.wAttributes;
        }
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }

    WORD default_stdout_attributes = 0;
    WORD default_stderr_attributes = 0;
    bool stdout_attributes_ready = false;
    bool stderr_attributes_ready = false;

    WORD fetch_default_attributes(bool is_error, HANDLE handle)
    {
        WORD& attributes = is_error ? default_stderr_attributes : default_stdout_attributes;
        bool& ready = is_error ? stderr_attributes_ready : stdout_attributes_ready;
        if (!ready)
        {
            attributes = read_default_attributes(handle);
            ready = true;
        }
        return attributes;
    }

    WORD select_color(std::string_view prefix, WORD fallback)
    {
        if (prefix == "[error]")
        {
            return FOREGROUND_RED | FOREGROUND_INTENSITY;
        }
        if (prefix == "[warning]")
        {
            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        }
        if (prefix == "[success]")
        {
            return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        }
        if (prefix == "[info]")
        {
            return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        }
        return fallback;
    }

    std::string make_timestamp()
    {
        SYSTEMTIME system_time{};
        GetLocalTime(&system_time);
        return std::format("{:02}:{:02}:{:02}", system_time.wHour, system_time.wMinute, system_time.wSecond);
    }

    std::mutex g_log_file_mutex;
    std::string g_log_file_path;
    bool g_log_file_ready = false;

    void ensure_log_file_path()
    {
        if (g_log_file_ready)
        {
            return;
        }

        char module_path[MAX_PATH]{};
        if (GetModuleFileNameA(nullptr, module_path, MAX_PATH) != 0)
        {
            const auto parent = std::string(module_path);
            const auto slash = parent.find_last_of("\\/");
            if (slash != std::string::npos)
            {
                g_log_file_path = parent.substr(0, slash + 1) + "vanille-debug.log";
            }
        }

        g_log_file_ready = true;
    }

    void append_log_file(std::string_view prefix, std::string_view message)
    {
        ensure_log_file_path();
        if (g_log_file_path.empty())
        {
            return;
        }

        std::lock_guard lock(g_log_file_mutex);
        std::ofstream output(g_log_file_path, std::ios::app);
        if (!output.is_open())
        {
            return;
        }

        output << '[' << make_timestamp() << "] " << prefix << ' ' << message << '\n';
    }
}

namespace logger_core
{
    void log_prompt(std::string_view prefix, std::string_view message)
    {
        const HANDLE handle = resolve_handle(stdout);
        const WORD base_attributes = fetch_default_attributes(false, handle);
        const WORD color = select_color(prefix, base_attributes);
        const WORD background = base_attributes & 0xF0;
        constexpr WORD timestamp_color = FOREGROUND_INTENSITY;

        const auto stamp = make_timestamp();

        if (handle != INVALID_HANDLE_VALUE)
        {
            SetConsoleTextAttribute(handle, timestamp_color | background);
        }
        std::print(stdout, "[{}] ", stamp);

        if (handle != INVALID_HANDLE_VALUE)
        {
            SetConsoleTextAttribute(handle, color | background);
        }
        std::print(stdout, "{} ", prefix);

        if (handle != INVALID_HANDLE_VALUE)
        {
            SetConsoleTextAttribute(handle, base_attributes);
        }
        std::print(stdout, "{}", message);
        std::fflush(stdout);
    }

    void log_message(std::FILE* stream, std::string_view prefix, std::string_view message)
    {
        const HANDLE handle = resolve_handle(stream);
        const bool is_error = stream == stderr;
        const WORD base_attributes = fetch_default_attributes(is_error, handle);
        const WORD color = select_color(prefix, base_attributes);
        constexpr WORD timestamp_color = FOREGROUND_INTENSITY;
        const WORD background = base_attributes & 0xF0;

        const auto stamp = make_timestamp();

        if (handle != INVALID_HANDLE_VALUE)
        {
            SetConsoleTextAttribute(handle, timestamp_color | background);
        }
        std::print(stream, "[{}] ", stamp);

        if (handle != INVALID_HANDLE_VALUE)
        {
            SetConsoleTextAttribute(handle, color | background);
        }
        std::print(stream, "{} ", prefix);

        if (handle != INVALID_HANDLE_VALUE)
        {
            SetConsoleTextAttribute(handle, base_attributes);
        }
        std::print(stream, "{}\n", message);
        append_log_file(prefix, message);
    }
}
