#pragma once

#include <string>
#include <vector>

namespace lua_console
{
    enum class log_level
    {
        info,
        warning,
        error
    };

    struct console_line
    {
        log_level level = log_level::info;
        std::string text;
    };

    void clear();
    void push(log_level level, const std::string& text);
    void push_info(const std::string& text);
    void push_warning(const std::string& text);
    void push_error(const std::string& text);
    std::vector<console_line> get_lines_snapshot();
}
