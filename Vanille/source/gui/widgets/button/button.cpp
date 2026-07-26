#include "../widgets.h"
#include "../../colors/colors.h"
#include <imgui/imgui_internal.h>

namespace
{
    constexpr float kBorderThickness = 1.0f;

    inline ImRect round_rect(const ImRect& rect)
    {
        return ImRect(
            ImVec2(IM_ROUND(rect.Min.x), IM_ROUND(rect.Min.y)),
            ImVec2(IM_ROUND(rect.Max.x), IM_ROUND(rect.Max.y))
        );
    }

    inline ImRect shrink_rect(const ImRect& rect, float amount)
    {
        return ImRect(
            ImVec2(rect.Min.x + amount, rect.Min.y + amount),
            ImVec2(rect.Max.x - amount, rect.Max.y - amount)
        );
    }

    inline ImVec4 scale_color(const ImVec4& color, float factor)
    {
        ImVec4 result = color;
        result.x = ImClamp(result.x * factor, 0.0f, 1.0f);
        result.y = ImClamp(result.y * factor, 0.0f, 1.0f);
        result.z = ImClamp(result.z * factor, 0.0f, 1.0f);
        return result;
    }

    bool draw_button_impl(const char* label, const ImVec2& size_arg, bool primary)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (!window || window->SkipItems)
            return false;

        ImVec2 size = size_arg;
        if (size.x <= 0.0f)
            size.x = ImGui::GetContentRegionAvail().x;
        if (size.y <= 0.0f)
            size.y = ImGui::GetFrameHeight();

        const ImVec2 pos = window->DC.CursorPos;
        const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));

        ImGui::ItemSize(bb);
        const ImGuiID id = window->GetID(label);
        if (!ImGui::ItemAdd(bb, id))
            return false;

        bool hovered = false;
        bool held = false;
        const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_None);

        ImDrawList* draw_list = window->DrawList;
        ImRect outer = round_rect(bb);
        ImRect inner = round_rect(shrink_rect(outer, kBorderThickness));
        ImRect fill = round_rect(shrink_rect(inner, 1.0f));

        const ImDrawListFlags old_flags = draw_list->Flags;
        draw_list->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;

        ImVec4 fill_col;
        ImVec4 border_col;
        ImVec4 text_col;

        if (primary)
        {
            fill_col = held ? scale_color(c_colors::top_accent_color, 0.94f)
                            : (hovered ? scale_color(c_colors::top_accent_color, 1.06f) : c_colors::top_accent_color);
            border_col = c_colors::top_accent_color;
            text_col = c_colors::accent_on;
        }
        else
        {
            fill_col = held ? scale_color(c_colors::surface_raised, 0.92f)
                            : (hovered ? scale_color(c_colors::surface_raised, 1.04f) : c_colors::surface_raised);
            border_col = hovered ? c_colors::border_soft : c_colors::main_border;
            text_col = hovered ? c_colors::white : c_colors::text_muted;
        }

        draw_list->AddRect(outer.Min, outer.Max, ImGui::GetColorU32(c_colors::outter_border), c_colors::widget_rounding, 0,
                           kBorderThickness);
        draw_list->AddRect(inner.Min, inner.Max, ImGui::GetColorU32(border_col),
                           ImMax(0.0f, c_colors::widget_rounding - 1.0f), 0, kBorderThickness);
        draw_list->AddRectFilled(fill.Min, fill.Max, ImGui::GetColorU32(fill_col), ImMax(0.0f, c_colors::widget_rounding - 1.0f));

        const char* text_begin = label ? label : "";
        const char* text_end = ImGui::FindRenderedTextEnd(text_begin);
        if (text_begin != text_end)
        {
            const ImVec2 text_size = ImGui::CalcTextSize(text_begin, text_end);
            const ImVec2 text_pos(
                fill.Min.x + (fill.GetWidth() - text_size.x) * 0.5f,
                fill.Min.y + (fill.GetHeight() - text_size.y) * 0.5f
            );
            draw_list->AddText(text_pos, ImGui::GetColorU32(text_col), text_begin, text_end);
        }

        draw_list->Flags = old_flags;
        return pressed;
    }
}

bool c_widgets::button(const char* label, const ImVec2& size_arg)
{
    return draw_button_impl(label, size_arg, false);
}

bool c_widgets::button_primary(const char* label, const ImVec2& size_arg)
{
    return draw_button_impl(label, size_arg, true);
}
