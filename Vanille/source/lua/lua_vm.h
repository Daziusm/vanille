#pragma once

#include <filesystem>
#include <string>

struct ImVec2;

struct HWND__;
using overlay_hwnd_t = HWND__*;

namespace lua_vm
{
    bool initialize();
    void shutdown();
    bool is_ready();

    bool execute_string(const std::string& script, const std::string& chunk_name = "lua_script");
    bool execute_file(const std::filesystem::path& file_path);

    void on_frame(float delta_time);

    void begin_aux_window_hittest_frame();
    void register_aux_window_hittest_rect(const ImVec2& screen_pos, const ImVec2& size, overlay_hwnd_t overlay_hwnd);
    bool cursor_over_aux_windows(overlay_hwnd_t overlay_hwnd);
    bool client_point_over_aux_windows(overlay_hwnd_t overlay_hwnd, int client_x, int client_y);

    void render_editor_window(bool menu_has_frame, const ImVec2& menu_pos, const ImVec2& menu_size);
    void render_console_window(bool menu_has_frame, const ImVec2& menu_pos, const ImVec2& menu_size);
}
