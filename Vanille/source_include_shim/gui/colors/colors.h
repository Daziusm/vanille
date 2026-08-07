#pragma once

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <imgui/imgui.h>

namespace c_colors
{
    inline float clamp01(float value)
    {
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }

    inline ImVec4 scale_color(const ImVec4& color, float factor)
    {
        ImVec4 result = color;
        result.x = clamp01(result.x * factor);
        result.y = clamp01(result.y * factor);
        result.z = clamp01(result.z * factor);
        return result;
    }

    inline ImVec4 derive_bottom_accent(const ImVec4& top_color)
    {
        const ImVec4 accent_delta(-0.15f, -0.08f, -0.15f, 0.0f);
        ImVec4 bottom_color;
        bottom_color.x = clamp01(top_color.x + accent_delta.x);
        bottom_color.y = clamp01(top_color.y + accent_delta.y);
        bottom_color.z = clamp01(top_color.z + accent_delta.z);
        bottom_color.w = top_color.w;
        return bottom_color;
    }

    inline ImVec4 derive_bottom_surface(const ImVec4& top_color, float amount = 0.012f)
    {
        (void)amount;
        return top_color;
    }

    // Bloom design tokens (vanille/uitest/bloom/tokens.css)
    inline ImVec4 top_window_background    = ImVec4(24.0f / 255.0f, 24.0f / 255.0f, 24.0f / 255.0f, 1.0f); // #181818
    inline ImVec4 bottom_window_background = top_window_background;

    inline ImVec4 top_child_background    = ImVec4(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f); // #1e1e1e
    inline ImVec4 bottom_child_background = top_child_background;

    inline ImVec4 outter_border           = ImVec4(4.0f / 255.0f, 4.0f / 255.0f, 4.0f / 255.0f, 1.0f);   // #040404
    inline ImVec4 main_border             = ImVec4(38.0f / 255.0f, 38.0f / 255.0f, 38.0f / 255.0f, 1.0f); // #262626

    inline ImVec4 surface                 = ImVec4(28.0f / 255.0f, 28.0f / 255.0f, 28.0f / 255.0f, 1.0f); // #1c1c1c
    inline ImVec4 surface_inset           = ImVec4(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f, 1.0f); // #1a1a1a
    inline ImVec4 surface_raised          = ImVec4(34.0f / 255.0f, 34.0f / 255.0f, 34.0f / 255.0f, 1.0f); // #222222

    inline ImVec4 border_soft             = ImVec4(48.0f / 255.0f, 48.0f / 255.0f, 48.0f / 255.0f, 1.0f); // #303030

    inline ImVec4 top_accent_color        = ImVec4(255.0f / 255.0f, 250.0f / 255.0f, 238.0f / 255.0f, 1.0f); // #fffaee
    inline ImVec4 bottom_accent_color     = derive_bottom_accent(top_accent_color);
    inline ImVec4 accent_dim              = ImVec4(204.0f / 255.0f, 200.0f / 255.0f, 188.0f / 255.0f, 1.0f); // #ccc8bc
    inline ImVec4 accent_soft             = ImVec4(255.0f / 255.0f, 250.0f / 255.0f, 238.0f / 255.0f, 0.10f);
    inline ImVec4 accent_border           = ImVec4(255.0f / 255.0f, 250.0f / 255.0f, 238.0f / 255.0f, 0.22f);
    inline ImVec4 accent_on               = ImVec4(21.0f / 255.0f, 21.0f / 255.0f, 21.0f / 255.0f, 1.0f);   // #151515

    inline ImVec4 white                   = ImVec4(1.000f, 1.000f, 1.000f, 1.000f);
    inline ImVec4 text_muted              = ImVec4(1.000f, 1.000f, 1.000f, 0.640f); // --ds-text-muted
    inline ImVec4 black                   = ImVec4(0.000f, 0.000f, 0.000f, 1.000f);

    inline float window_rounding = 10.0f;
    inline float panel_rounding  = 6.0f;
    inline float widget_rounding = 4.0f;

    inline void draw_rounded_gradient_rect(ImDrawList* draw_list, ImVec2 min, ImVec2 max, ImU32 top_col,
                                           ImU32 bottom_col, float rounding)
    {
        (void)bottom_col;
        draw_list->AddRectFilled(min, max, top_col, rounding);
    }
}
