#include "lua/lua_console.h"

#include <deque>
#include <mutex>

namespace lua_console
{
    namespace
    {
        std::mutex g_mutex;
        std::deque<console_line> g_lines;
        constexpr std::size_t g_max_lines = 4096;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_lines.clear();
    }

    void push(log_level level, const std::string& text)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_lines.push_back(console_line{ level, text });
        while (g_lines.size() > g_max_lines)
        {
            g_lines.pop_front();
        }
    }

    void push_info(const std::string& text)
    {
        push(log_level::info, text);
    }

    void push_warning(const std::string& text)
    {
        push(log_level::warning, text);
    }

    void push_error(const std::string& text)
    {
        push(log_level::error, text);
    }

    std::vector<console_line> get_lines_snapshot()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return std::vector<console_line>(g_lines.begin(), g_lines.end());
    }
}
