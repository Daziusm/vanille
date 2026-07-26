#include "utils/logger.h"

#include <format>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <windows.h>

#ifndef VANILLE_ENABLE_CONSOLE
#include <shlobj.h>
#include <shlwapi.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#endif

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

#ifndef VANILLE_ENABLE_CONSOLE
    std::mutex g_log_mutex;

    void write_release_log(std::string_view prefix, std::string_view message)
    {
        std::lock_guard lock(g_log_mutex);

        wchar_t app_data[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, app_data)))
            return;

        wchar_t log_dir[MAX_PATH]{};
        PathCombineW(log_dir, app_data, L"Chocola");
        CreateDirectoryW(log_dir, nullptr);

        wchar_t log_path[MAX_PATH]{};
        PathCombineW(log_path, log_dir, L"vanille.log");

        FILE* file = nullptr;
        if (_wfopen_s(&file, log_path, L"a") != 0 || !file)
            return;

        const auto stamp = make_timestamp();
        std::fprintf(file, "[%s] %.*s %.*s\n",
            stamp.c_str(),
            static_cast<int>(prefix.size()), prefix.data(),
            static_cast<int>(message.size()), message.data());
        std::fflush(file);
        std::fclose(file);
    }
#endif
}

namespace logger_core
{
    void log_prompt(std::string_view prefix, std::string_view message)
    {
#ifndef VANILLE_ENABLE_CONSOLE
        write_release_log(prefix, message);
        return;
#endif

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
#ifndef VANILLE_ENABLE_CONSOLE
        (void)stream;
        write_release_log(prefix, message);
        return;
#endif

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
    }
}
