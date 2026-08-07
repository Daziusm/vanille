#include "../widgets.h"
#include "../../colors/colors_new.h"
#include <imgui/imgui_internal.h>

namespace
{
    constexpr float kBorderThickness = 1.0f;

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

    inline void draw_gradient_rect(ImDrawList* draw_list, const ImRect& rect, const ImVec4& top_col, const ImVec4& bottom_col)
    {
        draw_list->AddRectFilledMultiColor(
            rect.Min,
            rect.Max,
            ImGui::GetColorU32(bottom_col),
            ImGui::GetColorU32(bottom_col),
            ImGui::GetColorU32(top_col),
            ImGui::GetColorU32(top_col)
        );
    }

    inline ImGuiID slider_smooth_value_key(ImGuiID base_id)
    {
        static const ImGuiID seed = ImHashStr("c_widgets::slider_smooth_value");
        return ImHashData(&base_id, sizeof(base_id), seed);
    }
}

bool c_widgets::slider_float(const char* label, float* v, float v_min, float v_max, const char* format)
{
    if (!v || v_min == v_max)
        return false;

    if (!format)
        format = "%.3f";

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO();
    const float full_width = ImGui::GetContentRegionAvail().x;

    const bool show_label = label && label[0] != '\0' && label[0] != '#';
    const char* slider_label = show_label ? "##slider" : (label ? label : "##slider");
    ImVec2 label_screen_pos(0.0f, 0.0f);
    ImVec2 label_size(0.0f, 0.0f);
    const float label_spacing = style.ItemSpacing.y * 0.25f;

    if (show_label)
    {
        label_screen_pos = ImGui::GetCursorScreenPos();
        label_size = ImGui::CalcTextSize(label);

        char value_buf_head[64];
        ImGui::DataTypeFormatString(value_buf_head, IM_ARRAYSIZE(value_buf_head), ImGuiDataType_Float, v, format);
        const ImVec2 value_size = ImGui::CalcTextSize(value_buf_head);

        ImDrawList* head_draw = window->DrawList;
        ImVec4 label_col = scale_color(ImGui::GetStyle().Colors[ImGuiCol_Text], 0.65f);
        head_draw->AddText(label_screen_pos, ImGui::GetColorU32(label_col), label);
        const float value_x = label_screen_pos.x + full_width - value_size.x;
        head_draw->AddText(ImVec2(value_x, label_screen_pos.y), ImGui::GetColorU32(c_colors::top_accent_color), value_buf_head);

        ImGui::Dummy(ImVec2(0.0f, label_size.y));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + label_spacing);
    }

    ImVec2 padding(style.FramePadding.x, style.FramePadding.y * 0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, c_colors::widget_rounding);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));

    ImGui::SetNextItemWidth(full_width);
    if (show_label)
        ImGui::PushID(label);
    bool value_changed = ImGui::SliderFloat(slider_label, v, v_min, v_max, format, ImGuiSliderFlags_NoInput);
    if (show_label)
        ImGui::PopID();

    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);

    ImRect frame_bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    frame_bb.Min.y -= 1.0f;
    frame_bb.Max.y += 1.0f;

    const ImGuiID slider_id = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const float target_value = *v;
    float display_value = target_value;

    if (ImGuiStorage* storage = ImGui::GetStateStorage())
    {
        const ImGuiID smooth_key = slider_smooth_value_key(slider_id);
        float stored_value = storage->GetFloat(smooth_key, target_value);

        const float smoothing_speed = active ? 24.0f : (hovered ? 14.0f : 10.0f);
        const float lerp_alpha = ImClamp(io.DeltaTime * smoothing_speed, 0.0f, 1.0f);
        stored_value = ImLerp(stored_value, target_value, lerp_alpha);
        if (ImFabs(stored_value - target_value) <= 0.0001f)
            stored_value = target_value;

        storage->SetFloat(smooth_key, stored_value);
        display_value = stored_value;
    }

    ImDrawList* draw_list = window->DrawList;
    ImRect outer = frame_bb;
    ImRect inner = shrink_rect(outer, kBorderThickness);
    ImRect base_bb = shrink_rect(inner, 1.0f);
    ImRect fill_bb = base_bb;

    auto snap_rect = [](ImRect rect) -> ImRect
    {
        rect.Min.x = IM_ROUND(rect.Min.x);
        rect.Min.y = IM_ROUND(rect.Min.y);
        rect.Max.x = IM_ROUND(rect.Max.x);
        rect.Max.y = IM_ROUND(rect.Max.y);
        return rect;
    };

    const bool prev_aa = (draw_list->Flags & ImDrawListFlags_AntiAliasedLines) != 0;
    draw_list->Flags &= ~ImDrawListFlags_AntiAliasedLines;

    outer = snap_rect(outer);
    inner = snap_rect(inner);
    base_bb = snap_rect(base_bb);
    fill_bb = snap_rect(fill_bb);

    auto adjust_base_col = [&](const ImVec4& col) -> ImVec4
    {
        if (active)
            return scale_color(col, 0.9f);
        if (hovered)
            return scale_color(col, 1.1f);
        return col;
    };

    const ImVec4 base_top = adjust_base_col(c_colors::surface_inset);
    const ImVec4 base_bottom = adjust_base_col(c_colors::surface_inset);

    draw_list->AddRect(outer.Min, outer.Max, ImGui::GetColorU32(c_colors::outter_border), c_colors::widget_rounding, 0,
                       kBorderThickness);
    draw_list->AddRect(inner.Min, inner.Max, ImGui::GetColorU32(c_colors::main_border),
                       ImMax(0.0f, c_colors::widget_rounding - 1.0f), 0, kBorderThickness);
    draw_gradient_rect(draw_list, base_bb, base_top, base_bottom);

    if (prev_aa)
        draw_list->Flags |= ImDrawListFlags_AntiAliasedLines;

    const float range = v_max - v_min;
    const float t = (range != 0.0f) ? ImClamp((display_value - v_min) / range, 0.0f, 1.0f) : 0.0f;
    if (t > 0.0f)
    {
        ImRect value_rect = fill_bb;
        value_rect.Max.x = ImLerp(fill_bb.Min.x, fill_bb.Max.x, t);
        if (value_rect.Max.x - value_rect.Min.x < 2.0f)
            value_rect.Max.x = value_rect.Min.x + 2.0f;

        ImVec4 accent_top = c_colors::top_accent_color;
        ImVec4 accent_bottom = c_colors::bottom_accent_color;
        if (hovered || active)
        {
            const float factor = active ? 1.08f : 1.02f;
            accent_top = scale_color(accent_top, factor);
            accent_bottom = scale_color(accent_bottom, factor);
        }

        draw_gradient_rect(draw_list, value_rect, accent_bottom, accent_top);
    }

    {
        const float thumb_radius = 8.0f;
        const float thumb_border = 2.0f;
        const float center_x = ImLerp(fill_bb.Min.x, fill_bb.Max.x, t);
        const float center_y = (fill_bb.Min.y + fill_bb.Max.y) * 0.5f;
        const ImVec2 center(center_x, center_y);

        draw_list->AddCircleFilled(center, thumb_radius + 1.0f, IM_COL32(0, 0, 0, 51));
        draw_list->AddCircleFilled(center, thumb_radius, ImGui::GetColorU32(c_colors::top_window_background));
        draw_list->AddCircleFilled(center, thumb_radius - thumb_border, ImGui::GetColorU32(c_colors::top_accent_color));
    }

    char value_buf[64];
    ImGui::DataTypeFormatString(value_buf, IM_ARRAYSIZE(value_buf), ImGuiDataType_Float, &display_value, format);

    if (!show_label)
    {
        const ImVec2 text_size = ImGui::CalcTextSize(value_buf);
        const ImVec2 text_pos(
            base_bb.Min.x + (base_bb.GetWidth() - text_size.x) * 0.5f,
            base_bb.Min.y + (base_bb.GetHeight() - text_size.y) * 0.5f
        );

        ImVec4 text_col = hovered || active ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : scale_color(ImGui::GetStyle().Colors[ImGuiCol_Text], 0.65f);
        draw_list->AddText(text_pos, ImGui::GetColorU32(text_col), value_buf);
    }

    return value_changed;
}
