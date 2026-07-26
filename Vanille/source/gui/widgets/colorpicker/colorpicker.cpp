#include "../widgets.h"
#include "../../colors/colors.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <cfloat>

namespace
{
    constexpr float kBorderThickness = 1.0f;
    constexpr float kInnerPadding = 1.0f;
    bool g_has_copied_color = false;
    ImVec4 g_copied_color(0.0f, 0.0f, 0.0f, 0.0f);

    ImRect make_rect_from_pos_size(const ImVec2& pos, const ImVec2& size)
    {
        return ImRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    }

    bool render_context_action_text_only(const char* label)
    {
        ImGuiWindow* popup_window = ImGui::GetCurrentWindow();
        if (!popup_window)
            return false;

        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 item_pos = ImGui::GetCursorScreenPos();
        const ImVec2 text_size = ImGui::CalcTextSize(label, nullptr, true);
        const float item_height = ImMax(ImGui::GetFrameHeight(), text_size.y + style.FramePadding.y * 2.0f);
        const float item_width = ImMax(text_size.x + style.FramePadding.x * 2.0f, ImGui::GetContentRegionAvail().x);

        ImGui::PushID(label);
        const bool clicked = ImGui::InvisibleButton("##context_action", ImVec2(item_width, item_height));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();

        const ImVec4 normal_text = style.Colors[ImGuiCol_Text];
        const ImVec4 hover_text = c_colors::scale_color(c_colors::top_accent_color, 1.10f);
        const ImVec4 active_text = c_colors::scale_color(c_colors::top_accent_color, 0.90f);
        const ImU32 text_col = ImGui::GetColorU32(active ? active_text : (hovered ? hover_text : normal_text));

        const float text_y = item_pos.y + (item_height - text_size.y) * 0.5f;
        popup_window->DrawList->AddText(ImVec2(item_pos.x + style.FramePadding.x, text_y), text_col, label);
        ImGui::PopID();

        return clicked;
    }

    bool render_color_context_popup(const char* popup_id, ImVec4& color)
    {
        bool changed = false;
        if (ImGui::BeginPopup(popup_id))
        {
            bool close_popup = false;
            if (render_context_action_text_only("Copy"))
            {
                g_copied_color = color;
                g_has_copied_color = true;
                close_popup = true;
            }

            if (!close_popup && g_has_copied_color && render_context_action_text_only("Paste"))
            {
                color = g_copied_color;
                changed = true;
                close_popup = true;
            }

            if (close_popup)
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        return changed;
    }
}

bool c_widgets::colorpicker(const char* label, ImVec4& color, float target_height)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGui::SameLine();

    ImGuiStyle& style = ImGui::GetStyle();
    const float frame_height = target_height > 0.0f ? target_height : ImGui::GetFrameHeight();
    const float preview_width = frame_height * 1.45f;
    const float preview_height = frame_height * 0.86f;
    const float font_height = ImGui::GetFontSize();
    const float pad_y = ImMax(0.0f, (frame_height - font_height) * 0.5f);

    const float full_width = ImGui::GetContentRegionAvail().x;
    const ImVec2 cursor_pos = window->DC.CursorPos;
    const ImVec2 total_size(full_width, frame_height);

    const ImGuiID row_id = window->GetID(label);
    ImRect total_bb = make_rect_from_pos_size(cursor_pos, total_size);
    ImGui::ItemSize(total_bb, pad_y);
    if (!ImGui::ItemAdd(total_bb, row_id))
        return false;

    const float label_right = total_bb.Max.x - preview_width - style.ItemInnerSpacing.x;
    ImRect label_bb(total_bb.Min, ImVec2(ImMax(label_right, total_bb.Min.x), total_bb.Max.y));

    ImVec2 preview_max(total_bb.Max.x, total_bb.Max.y);
    ImVec2 preview_min(preview_max.x - preview_width, total_bb.Min.y);
    preview_min.x = ImMax(preview_min.x, total_bb.Min.x);
    const float preview_offset_y = (frame_height - preview_height) * 0.5f;
    ImRect preview_bb(
        ImVec2(preview_min.x, preview_min.y + preview_offset_y),
        ImVec2(preview_max.x, preview_min.y + preview_offset_y + preview_height));

    ImDrawList* draw_list = window->DrawList;

    if (label && label[0] != '\0')
    {
        const ImVec2 text_offset(style.FramePadding.x, pad_y);
        ImGui::RenderTextClipped(label_bb.Min + text_offset, label_bb.Max, label, nullptr, nullptr, ImVec2(0.0f, 0.5f));
    }

    ImGui::PushID(label);

    const ImGuiID preview_id = window->GetID("##preview");
    bool hovered = false;
    bool held = false;
    bool pressed = false;
    if (ImGui::ItemAdd(preview_bb, preview_id))
    {
        pressed = ImGui::ButtonBehavior(preview_bb, preview_id, &hovered, &held);
        ImGui::OpenPopupOnItemClick("##picker_context", ImGuiPopupFlags_MouseButtonRight);
    }

    if (pressed)
        ImGui::OpenPopup("##picker");

    if (ImGui::IsPopupOpen("##picker"))
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 viewport_min = viewport ? viewport->WorkPos : ImVec2(0.0f, 0.0f);
        ImVec2 viewport_max = viewport ? ImVec2(viewport_min.x + viewport->WorkSize.x, viewport_min.y + viewport->WorkSize.y)
            : ImVec2(FLT_MAX, FLT_MAX);

        ImVec2 popup_pos(preview_bb.Max.x + style.ItemInnerSpacing.x, preview_bb.Min.y);
        popup_pos.x = ImMin(popup_pos.x, viewport_max.x - style.WindowPadding.x);
        popup_pos.y = ImClamp(popup_pos.y, viewport_min.y, viewport_max.y);
        ImGui::SetNextWindowPos(popup_pos, ImGuiCond_Appearing);
    }

    const ImGuiColorEditFlags picker_flags =
        ImGuiColorEditFlags_AlphaBar |
        ImGuiColorEditFlags_NoSidePreview |
        ImGuiColorEditFlags_NoSmallPreview |
        ImGuiColorEditFlags_NoInputs |
        ImGuiColorEditFlags_DisplayRGB |
        ImGuiColorEditFlags_PickerHueBar;

    const bool popup_visible = ImGui::IsPopupOpen("##picker");
    if (popup_visible)
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));

    bool value_changed = false;
    if (ImGui::BeginPopup("##picker"))
    {
        ImDrawList* popup_draw_list = ImGui::GetWindowDrawList();
        const ImVec2 popup_pos = ImGui::GetWindowPos();
        const ImVec2 popup_size = ImGui::GetWindowSize();
        const ImVec2 popup_max = ImVec2(popup_pos.x + popup_size.x, popup_pos.y + popup_size.y);
        const ImU32 top_bg = ImGui::GetColorU32(c_colors::top_child_background);
        const ImU32 bottom_bg = ImGui::GetColorU32(c_colors::bottom_child_background);
        popup_draw_list->AddRectFilledMultiColor(popup_pos, popup_max, bottom_bg, bottom_bg, top_bg, top_bg);
        popup_draw_list->AddRect(popup_pos, popup_max, ImGui::GetColorU32(c_colors::outter_border), 0.0f, 0, kBorderThickness);

        ImRect popup_inner(
            ImVec2(popup_pos.x + kBorderThickness, popup_pos.y + kBorderThickness),
            ImVec2(popup_max.x - kBorderThickness, popup_max.y - kBorderThickness));
        popup_draw_list->AddRect(popup_inner.Min, popup_inner.Max, ImGui::GetColorU32(c_colors::main_border), 0.0f, 0, kBorderThickness);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * 0.8f, style.ItemSpacing.y));
        value_changed |= ImGui::ColorPicker4("##colorpicker_widget", &color.x, picker_flags);
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (popup_visible)
        ImGui::PopStyleColor();

    value_changed |= render_color_context_popup("##picker_context", color);

    const float rounding = c_colors::widget_rounding;
    const ImU32 outer_col = ImGui::GetColorU32(c_colors::outter_border);
    ImVec4 main_border = c_colors::main_border;
    const ImU32 main_col = ImGui::GetColorU32(main_border);

    draw_list->AddRect(preview_bb.Min, preview_bb.Max, outer_col, rounding, 0, kBorderThickness);

    ImRect inner_bb(
        ImVec2(preview_bb.Min.x + kBorderThickness, preview_bb.Min.y + kBorderThickness),
        ImVec2(preview_bb.Max.x - kBorderThickness, preview_bb.Max.y - kBorderThickness));
    draw_list->AddRect(inner_bb.Min, inner_bb.Max, main_col, rounding, 0, kBorderThickness);

    ImRect fill_bb(
        ImVec2(inner_bb.Min.x + kInnerPadding, inner_bb.Min.y + kInnerPadding),
        ImVec2(inner_bb.Max.x - kInnerPadding, inner_bb.Max.y - kInnerPadding));
    fill_bb.ClipWith(inner_bb);

    const float grid_step = ImMax(2.0f, ImMin(fill_bb.GetWidth(), fill_bb.GetHeight()) / 2.8f);
    ImGui::RenderColorRectWithAlphaCheckerboard(draw_list, fill_bb.Min, fill_bb.Max, 0, grid_step, ImVec2(0.0f, 0.0f));
    draw_list->AddRectFilled(fill_bb.Min, fill_bb.Max, ImGui::GetColorU32(color));

    ImGui::PopID();
    return value_changed;
}

bool c_widgets::colorpicker_dual(const char* label, ImVec4& color_a, ImVec4& color_b, float target_height)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGui::SameLine();

    ImGuiStyle& style = ImGui::GetStyle();
    const float frame_height = target_height > 0.0f ? target_height : ImGui::GetFrameHeight();
    const float preview_width = frame_height * 1.45f;
    const float preview_height = frame_height * 0.86f;
    const float preview_spacing = style.ItemInnerSpacing.x * 0.65f;
    const float font_height = ImGui::GetFontSize();
    const float pad_y = ImMax(0.0f, (frame_height - font_height) * 0.5f);

    const float full_width = ImGui::GetContentRegionAvail().x;
    const ImVec2 cursor_pos = window->DC.CursorPos;
    const ImVec2 total_size(full_width, frame_height);

    const ImGuiID row_id = window->GetID(label);
    ImRect total_bb = make_rect_from_pos_size(cursor_pos, total_size);
    ImGui::ItemSize(total_bb, pad_y);
    if (!ImGui::ItemAdd(total_bb, row_id))
        return false;

    const float previews_total = preview_width * 2.0f + preview_spacing;
    const float label_right = total_bb.Max.x - previews_total - style.ItemInnerSpacing.x;
    ImRect label_bb(total_bb.Min, ImVec2(ImMax(label_right, total_bb.Min.x), total_bb.Max.y));

    ImVec2 preview_min_a(total_bb.Max.x - previews_total, total_bb.Min.y);
    ImVec2 preview_max_a(preview_min_a.x + preview_width, total_bb.Max.y);
    ImVec2 preview_min_b(preview_max_a.x + preview_spacing, total_bb.Min.y);
    ImVec2 preview_max_b(preview_min_b.x + preview_width, total_bb.Max.y);

    const float preview_offset_y = (frame_height - preview_height) * 0.5f;
    preview_min_a.y += preview_offset_y;
    preview_max_a.y = preview_min_a.y + preview_height;
    preview_min_b.y += preview_offset_y;
    preview_max_b.y = preview_min_b.y + preview_height;

    ImDrawList* draw_list = window->DrawList;

    if (label && label[0] != '\0')
    {
        const ImVec2 text_offset(style.FramePadding.x, pad_y);
        ImGui::RenderTextClipped(label_bb.Min + text_offset, label_bb.Max, label, nullptr, nullptr, ImVec2(0.0f, 0.5f));
    }

    ImGui::PushID(label);

    auto draw_preview = [&](const ImRect& bb, ImVec4& color, const char* popup_id, const char* context_popup_id) -> bool
        {
            const ImGuiID preview_id = window->GetID(popup_id);
            bool hovered = false;
            bool held = false;
            bool pressed = false;
            if (ImGui::ItemAdd(bb, preview_id))
            {
                pressed = ImGui::ButtonBehavior(bb, preview_id, &hovered, &held);
                ImGui::OpenPopupOnItemClick(context_popup_id, ImGuiPopupFlags_MouseButtonRight);
            }

            if (pressed)
                ImGui::OpenPopup(popup_id);

            if (ImGui::IsPopupOpen(popup_id))
            {
                const ImGuiViewport* viewport = ImGui::GetMainViewport();
                ImVec2 viewport_min = viewport ? viewport->WorkPos : ImVec2(0.0f, 0.0f);
                ImVec2 viewport_max = viewport ? ImVec2(viewport_min.x + viewport->WorkSize.x, viewport_min.y + viewport->WorkSize.y)
                    : ImVec2(FLT_MAX, FLT_MAX);

                ImVec2 popup_pos(bb.Max.x + style.ItemInnerSpacing.x, bb.Min.y);
                popup_pos.x = ImMin(popup_pos.x, viewport_max.x - style.WindowPadding.x);
                popup_pos.y = ImClamp(popup_pos.y, viewport_min.y, viewport_max.y);
                ImGui::SetNextWindowPos(popup_pos, ImGuiCond_Appearing);
            }

            const ImGuiColorEditFlags picker_flags =
                ImGuiColorEditFlags_AlphaBar |
                ImGuiColorEditFlags_NoSidePreview |
                ImGuiColorEditFlags_NoSmallPreview |
                ImGuiColorEditFlags_NoInputs |
                ImGuiColorEditFlags_DisplayRGB |
                ImGuiColorEditFlags_PickerHueBar;

            const bool popup_visible = ImGui::IsPopupOpen(popup_id);
            if (popup_visible)
                ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));

            bool changed = false;
            if (ImGui::BeginPopup(popup_id))
            {
                ImDrawList* popup_draw_list = ImGui::GetWindowDrawList();
                const ImVec2 popup_pos = ImGui::GetWindowPos();
                const ImVec2 popup_size = ImGui::GetWindowSize();
                const ImVec2 popup_max = ImVec2(popup_pos.x + popup_size.x, popup_pos.y + popup_size.y);
                const ImU32 top_bg = ImGui::GetColorU32(c_colors::top_child_background);
                const ImU32 bottom_bg = ImGui::GetColorU32(c_colors::bottom_child_background);
                popup_draw_list->AddRectFilledMultiColor(popup_pos, popup_max, bottom_bg, bottom_bg, top_bg, top_bg);
                popup_draw_list->AddRect(popup_pos, popup_max, ImGui::GetColorU32(c_colors::outter_border), 0.0f, 0, kBorderThickness);

                ImRect popup_inner(
                    ImVec2(popup_pos.x + kBorderThickness, popup_pos.y + kBorderThickness),
                    ImVec2(popup_max.x - kBorderThickness, popup_max.y - kBorderThickness));
                popup_draw_list->AddRect(popup_inner.Min, popup_inner.Max, ImGui::GetColorU32(c_colors::main_border), 0.0f, 0, kBorderThickness);

                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * 0.8f, style.ItemSpacing.y));
                changed |= ImGui::ColorPicker4("##colorpicker_widget", &color.x, picker_flags);
                ImGui::PopStyleVar();
                ImGui::EndPopup();
            }

            if (popup_visible)
                ImGui::PopStyleColor();

            changed |= render_color_context_popup(context_popup_id, color);

            const float rounding = c_colors::widget_rounding;
            const ImU32 outer_col = ImGui::GetColorU32(c_colors::outter_border);
            ImVec4 main_border = c_colors::main_border;
            const ImU32 main_col = ImGui::GetColorU32(main_border);

            draw_list->AddRect(bb.Min, bb.Max, outer_col, rounding, 0, kBorderThickness);

            ImRect inner_bb(
                ImVec2(bb.Min.x + kBorderThickness, bb.Min.y + kBorderThickness),
                ImVec2(bb.Max.x - kBorderThickness, bb.Max.y - kBorderThickness));
            draw_list->AddRect(inner_bb.Min, inner_bb.Max, main_col, rounding, 0, kBorderThickness);

            ImRect fill_bb(
                ImVec2(inner_bb.Min.x + kInnerPadding, inner_bb.Min.y + kInnerPadding),
                ImVec2(inner_bb.Max.x - kInnerPadding, inner_bb.Max.y - kInnerPadding));
            fill_bb.ClipWith(inner_bb);

            const float grid_step = ImMax(2.0f, ImMin(fill_bb.GetWidth(), fill_bb.GetHeight()) / 2.8f);
            ImGui::RenderColorRectWithAlphaCheckerboard(draw_list, fill_bb.Min, fill_bb.Max, 0, grid_step, ImVec2(0.0f, 0.0f));
            draw_list->AddRectFilled(fill_bb.Min, fill_bb.Max, ImGui::GetColorU32(color));

            return changed;
        };

    ImRect preview_bb_a(preview_min_a, preview_max_a);
    ImRect preview_bb_b(preview_min_b, preview_max_b);

    bool changed_a = draw_preview(preview_bb_a, color_a, "##picker_a", "##picker_context_a");
    bool changed_b = draw_preview(preview_bb_b, color_b, "##picker_b", "##picker_context_b");

    ImGui::PopID();
    return changed_a || changed_b;
}
