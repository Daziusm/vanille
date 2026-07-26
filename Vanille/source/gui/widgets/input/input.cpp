#include "../widgets.h"
#include "../../colors/colors.h"
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
}

bool c_widgets::input_text(const char* label, char* buffer, size_t buffer_size, ImGuiInputTextFlags flags)
{
    if (!buffer || buffer_size == 0)
        return false;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window || window->SkipItems)
        return false;

    ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* draw_list = window->DrawList;

    const bool show_label = label && label[0] != '\0' && label[0] != '#';
    const char* input_label = show_label ? "##input_text" : (label ? label : "##input_text");

    ImVec2 label_pos = ImVec2(0.0f, 0.0f);
    ImVec2 label_size = ImVec2(0.0f, 0.0f);
    const float label_spacing = style.ItemSpacing.y * 0.25f;
    if (show_label)
    {
        label_pos = ImGui::GetCursorScreenPos();
        label_size = ImGui::CalcTextSize(label);
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
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

    ImDrawListSplitter splitter;
    splitter.Split(draw_list, 2);
    splitter.SetCurrentChannel(draw_list, 1);

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (show_label)
        ImGui::PushID(label);
    const bool value_changed = ImGui::InputText(input_label, buffer, buffer_size, flags);
    if (show_label)
        ImGui::PopID();

    splitter.SetCurrentChannel(draw_list, 0);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);

    ImRect frame_bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    frame_bb.Min.y -= 1.0f;
    frame_bb.Max.y += 1.0f;

    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_RectOnly);
    const bool active = ImGui::IsItemActive();

    ImRect outer = frame_bb;
    ImRect inner = shrink_rect(outer, kBorderThickness);
    ImRect fill = shrink_rect(inner, 1.0f);

    auto adjust_base = [&](const ImVec4& col) -> ImVec4
    {
        if (active)
            return scale_color(col, 0.92f);
        if (hovered)
            return scale_color(col, 1.08f);
        return col;
    };

    ImVec4 fill_top = adjust_base(c_colors::top_child_background);
    ImVec4 fill_bottom = adjust_base(c_colors::bottom_child_background);

    draw_list->AddRect(outer.Min, outer.Max, ImGui::GetColorU32(c_colors::outter_border), 0.0f, 0, kBorderThickness);
    draw_list->AddRect(inner.Min, inner.Max, ImGui::GetColorU32(c_colors::main_border), 0.0f, 0, kBorderThickness);
    draw_list->AddRectFilledMultiColor(
        fill.Min,
        fill.Max,
        ImGui::GetColorU32(fill_bottom),
        ImGui::GetColorU32(fill_bottom),
        ImGui::GetColorU32(fill_top),
        ImGui::GetColorU32(fill_top)
    );

    splitter.Merge(draw_list);

    if (show_label)
    {
        ImVec4 label_col = scale_color(style.Colors[ImGuiCol_Text], hovered ? 1.05f : 0.7f);
        if (active)
            label_col = ImVec4(1.0f, 1.0f, 1.0f, label_col.w);
        draw_list->AddText(label_pos, ImGui::GetColorU32(label_col), label);
    }

    return value_changed;
}
