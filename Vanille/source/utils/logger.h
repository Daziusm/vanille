#pragma once

#include <cstdio>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

namespace logger_core
{
    void log_message(std::FILE* stream, std::string_view prefix, std::string_view message);

    template <typename... Args>
    void log_error(std::format_string<Args...> fmt, Args&&... args)
    {
        const auto text = std::format(fmt, std::forward<Args>(args)...);
        log_message(stderr, "[error]", text);
    }

    template <typename... Args>
    void log_warning(std::format_string<Args...> fmt, Args&&... args)
    {
        const auto text = std::format(fmt, std::forward<Args>(args)...);
        log_message(stderr, "[warning]", text);
    }

    template <typename... Args>
    void log_success(std::format_string<Args...> fmt, Args&&... args)
    {
        const auto text = std::format(fmt, std::forward<Args>(args)...);
        log_message(stdout, "[success]", text);
    }

    template <typename... Args>
    void log_info(std::format_string<Args...> fmt, Args&&... args)
    {
        const auto text = std::format(fmt, std::forward<Args>(args)...);
        log_message(stdout, "[info]", text);
    }

    void log_prompt(std::string_view prefix, std::string_view message);
}
