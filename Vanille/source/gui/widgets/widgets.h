#pragma once
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <string>
#include <algorithm>
#include <windows.h>
#include <cstddef>

#include "keybind/keybind.h"

namespace c_widgets
{
    bool colorpicker(const char* label, ImVec4& color, float target_height = 0.0f);
    bool colorpicker_dual(const char* label, ImVec4& color_a, ImVec4& color_b, float target_height = 0.0f);
    bool dropdown(const char* label, int* current_item, const char* const items[], int items_count);
    bool multi_dropdown(const char* label, bool* selections, const char* const items[], int items_count);
    bool slider_float(const char* label, float* v, float v_min, float v_max, const char* format = "%.3f");
    bool button(const char* label, const ImVec2& size = ImVec2(-1.0f, 0.0f));
    bool button_primary(const char* label, const ImVec2& size = ImVec2(-1.0f, 0.0f));
    bool keybind(const char* id, c_keybind& keybind, const ImVec2& size = ImVec2(0.0f, 0.0f));
    bool input_text(const char* label, char* buffer, size_t buffer_size, ImGuiInputTextFlags flags = 0);

    bool begin_padded_child(const char* str_id, ImGuiChildFlags child_flags = 0, ImGuiWindowFlags window_flags = 0, ImVec2 preferred_size = ImVec2(-1.0f, -1.0f), bool use_margin = true, bool use_gradient = true, bool use_accent = false, bool use_clip = false, bool block_parent_drag = false, bool use_accent_background = false, bool remove_accent_top_padding = false, bool allow_scroll = false);
    void end_padded_child();

    float padded_child_margin();

    void window_drag_handle(const char* id, float height = 0.0f);

    bool checkbox(const char* label, bool* v);
    void text(const char* fmt, ...);
    void text_accent(const char* fmt, ...);
    void text_colored(const ImVec4& color, const char* fmt, ...);
    void section_label(const char* fmt, ...);
}
