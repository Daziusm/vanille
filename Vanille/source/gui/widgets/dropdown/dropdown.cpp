#include "../widgets.h"
#include "../../colors/colors_new.h"
#include "imgui_internal.h"

namespace
{
    constexpr float kBorderThickness = 1.0f;
    constexpr float kPopupPadding = 6.0f;

    inline ImVec4 scale_color(const ImVec4& color, float factor)
    {
        ImVec4 result = color;
        result.x = ImClamp(result.x * factor, 0.0f, 1.0f);
        result.y = ImClamp(result.y * factor, 0.0f, 1.0f);
        result.z = ImClamp(result.z * factor, 0.0f, 1.0f);
        return result;
    }

    inline ImRect shrink_rect(const ImRect& rect, float amount)
    {
        return ImRect(
            ImVec2(rect.Min.x + amount, rect.Min.y + amount),
            ImVec2(rect.Max.x - amount, rect.Max.y - amount)
        );
    }

    inline ImVec4 dropdown_preview_color(const ImVec4& base_text_col, bool active_state, bool hovered_state)
    {
        if (active_state)
            return ImVec4(1.0f, 1.0f, 1.0f, base_text_col.w);
        if (hovered_state)
            return scale_color(base_text_col, 1.0f);
        return scale_color(base_text_col, 0.65f);
    }

    inline ImVec4 dropdown_popup_text_color(const ImVec4& base_text_col, bool selected, bool hovered)
    {
        if (selected)
        {
            ImVec4 accent = c_colors::top_accent_color;
            return hovered ? scale_color(accent, 1.15f) : accent;
        }
        return hovered ? ImVec4(1.0f, 1.0f, 1.0f, base_text_col.w) : scale_color(base_text_col, 0.7f);
    }

}

bool c_widgets::dropdown(const char* label, int* current_item, const char* const items[], int items_count)
{
    if (!label || !current_item || !items || items_count <= 0)
        return false;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    if (*current_item < 0 || *current_item >= items_count)
        *current_item = 0;

    ImGuiStyle& style = ImGui::GetStyle();
    const float frame_height = ImGui::GetFrameHeight();
    const float full_width = IM_FLOOR(ImGui::GetContentRegionAvail().x);

    const ImVec2 pos(IM_FLOOR(window->DC.CursorPos.x), IM_FLOOR(window->DC.CursorPos.y));
    const ImRect total_bb(pos, ImVec2(pos.x + full_width, pos.y + frame_height));

    ImGui::ItemSize(total_bb, style.FramePadding.y);

    ImGui::PushID(label);
    const ImGuiID id = window->GetID("##dropdown_field");
    if (!ImGui::ItemAdd(total_bb, id))
    {
        ImGui::PopID();
        return false;
    }

    bool hovered = false;
    bool held = false;
    bool pressed = ImGui::ButtonBehavior(total_bb, id, &hovered, &held);
    if (pressed)
        ImGui::OpenPopup("##dropdown_popup");

    const bool popup_open = ImGui::IsPopupOpen("##dropdown_popup");
    bool popup_padding_pushed = false;
    if (popup_open)
    {
        ImGui::SetNextWindowPos(ImVec2(total_bb.Min.x, total_bb.Max.y + 1.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(total_bb.GetWidth(), 0.0f), ImVec2(total_bb.GetWidth(), 240.0f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
        ImVec2 popup_padding = style.WindowPadding;
        if (popup_padding.x < 1.0f)
        {
            popup_padding.x = kPopupPadding;
            popup_padding_pushed = true;
        }
        if (popup_padding.y < 1.0f)
        {
            popup_padding.y = kPopupPadding;
            popup_padding_pushed = true;
        }
        if (popup_padding_pushed)
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popup_padding);
    }

    ImDrawList* draw_list = window->DrawList;
    ImRect outer_bb = total_bb;
    ImRect inner_bb = shrink_rect(outer_bb, kBorderThickness);
    ImRect fill_bb = shrink_rect(inner_bb, 1.0f);

    ImVec4 fill_color = c_colors::top_child_background;
    if (held)
        fill_color = scale_color(fill_color, 0.9f);
    else if (hovered)
        fill_color = scale_color(fill_color, 1.15f);

    draw_list->AddRect(outer_bb.Min, outer_bb.Max, ImGui::GetColorU32(c_colors::outter_border), c_colors::widget_rounding, 0, kBorderThickness);
    draw_list->AddRect(inner_bb.Min, inner_bb.Max, ImGui::GetColorU32(c_colors::main_border), ImMax(0.0f, c_colors::widget_rounding - 1.0f), 0, kBorderThickness);
    draw_list->AddRectFilled(fill_bb.Min, fill_bb.Max, ImGui::GetColorU32(fill_color));

    const float indicator_width = frame_height * 0.9f;
    ImRect indicator_bb(ImVec2(fill_bb.Max.x - indicator_width, fill_bb.Min.y), fill_bb.Max);
    ImRect text_bb(fill_bb.Min, ImVec2(indicator_bb.Min.x, fill_bb.Max.y));
    const float divider_x = indicator_bb.Min.x;

    const char* preview = (*current_item >= 0 && *current_item < items_count) ? items[*current_item] : "";
    const ImVec2 text_size = ImGui::CalcTextSize(preview);
    const ImVec2 text_pos(
        text_bb.Min.x + style.FramePadding.x,
        text_bb.Min.y + ImMax(0.0f, (text_bb.GetHeight() - text_size.y) * 0.5f)
    );
    const ImVec4 base_text_col = style.Colors[ImGuiCol_Text];

    const bool text_overflow = (text_pos.x + text_size.x) > (text_bb.Max.x - style.FramePadding.x);
    ImVec2 clip_min = text_bb.Min;
    ImVec2 clip_max = text_bb.Max;
    if (text_overflow)
        clip_max.x = divider_x - style.FramePadding.x * 0.5f;

    ImVec4 preview_col = dropdown_preview_color(base_text_col, popup_open || held, hovered);
    draw_list->PushClipRect(clip_min, clip_max, true);
    draw_list->AddText(text_pos, ImGui::GetColorU32(preview_col), preview);
    draw_list->PopClipRect();

    if (text_overflow)
    {
        const float fade_width = ImMin(style.FramePadding.x * 3.0f, text_bb.GetWidth() * 0.6f);
        const ImVec2 fade_min(divider_x - fade_width, fill_bb.Min.y + 1.0f);
        const ImVec2 fade_max(divider_x, fill_bb.Max.y - 1.0f);
        ImVec4 solid_col = fill_color;
        solid_col.w = ImClamp(solid_col.w * 0.95f, 0.0f, 1.0f);
        ImVec4 transparent_col = solid_col;
        transparent_col.w = 0.0f;
        draw_list->AddRectFilledMultiColor(
            fade_min,
            fade_max,
            ImGui::GetColorU32(transparent_col),
            ImGui::GetColorU32(solid_col),
            ImGui::GetColorU32(solid_col),
            ImGui::GetColorU32(transparent_col));
    }

    draw_list->AddLine(ImVec2(divider_x, indicator_bb.Min.y), ImVec2(divider_x, indicator_bb.Max.y), ImGui::GetColorU32(c_colors::main_border));

    const ImVec2 icon_center(
        indicator_bb.Min.x + indicator_bb.GetWidth() * 0.5f,
        indicator_bb.Min.y + indicator_bb.GetHeight() * 0.5f
    );
    const float icon_half = indicator_bb.GetHeight() * 0.20f;
    ImVec4 indicator_col = dropdown_preview_color(base_text_col, popup_open || held, hovered);
    const ImU32 indicator_u32 = ImGui::GetColorU32(indicator_col);
    draw_list->AddLine(ImVec2(icon_center.x - icon_half, icon_center.y), ImVec2(icon_center.x + icon_half, icon_center.y), indicator_u32, 1.25f);
    if (!popup_open)
        draw_list->AddLine(ImVec2(icon_center.x, icon_center.y - icon_half), ImVec2(icon_center.x, icon_center.y + icon_half), indicator_u32, 1.25f);

    bool value_changed = false;
    if (popup_open && ImGui::BeginPopup("##dropdown_popup"))
    {
        ImDrawList* popup_draw_list = ImGui::GetWindowDrawList();
        const ImVec2 popup_pos = ImGui::GetWindowPos();
        const ImVec2 popup_size = ImGui::GetWindowSize();
        const ImVec2 popup_max = ImVec2(popup_pos.x + popup_size.x, popup_pos.y + popup_size.y);

        popup_draw_list->AddRectFilled(popup_pos, popup_max, ImGui::GetColorU32(c_colors::top_child_background));
        popup_draw_list->AddRect(popup_pos, popup_max, ImGui::GetColorU32(c_colors::outter_border), c_colors::widget_rounding, 0, kBorderThickness);

        ImRect popup_inner = shrink_rect(ImRect(popup_pos, popup_max), kBorderThickness);
        popup_draw_list->AddRect(popup_inner.Min, popup_inner.Max, ImGui::GetColorU32(c_colors::main_border),
                                 ImMax(0.0f, c_colors::widget_rounding - 1.0f), 0, kBorderThickness);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * 0.5f, style.ItemSpacing.y * 0.35f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
        const float row_height = ImGui::GetFontSize() + style.FramePadding.y * 0.8f;
        bool first_item = true;
        for (int i = 0; i < items_count; ++i)
        {
            ImGui::PushID(i);
            const bool is_selected = (i == *current_item);
            const ImGuiSelectableFlags selectable_flags = ImGuiSelectableFlags_DontClosePopups | ImGuiSelectableFlags_SpanAvailWidth | ImGuiSelectableFlags_AllowItemOverlap;
            const bool item_pressed = ImGui::Selectable(items[i], is_selected, selectable_flags, ImVec2(0.0f, row_height));
            const bool item_hovered = ImGui::IsItemHovered();
            ImRect item_bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImVec4 item_text_col = dropdown_popup_text_color(base_text_col, is_selected, item_hovered);
            const ImVec2 item_text_pos(
                item_bb.Min.x + style.FramePadding.x,
                item_bb.Min.y + (item_bb.GetHeight() - ImGui::GetFontSize()) * 0.5f
            );
            popup_draw_list->AddText(item_text_pos, ImGui::GetColorU32(item_text_col), items[i]);
            if (item_pressed)
            {
                if (*current_item != i)
                {
                    *current_item = i;
                    value_changed = true;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
            if (i + 1 < items_count)
            {
                if (first_item)
                    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 0.05f));
                else
                    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 0.2f));
            }
            first_item = false;
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        ImGui::EndPopup();
    }

    if (popup_open)
    {
        if (popup_padding_pushed)
            ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ImGui::PopID();
    return value_changed;
}

bool c_widgets::multi_dropdown(const char* label, bool* selections, const char* const items[], int items_count)
{
    if (!label || !selections || !items || items_count <= 0)
        return false;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiStyle& style = ImGui::GetStyle();
    const float frame_height = ImGui::GetFrameHeight();
    const float full_width = ImGui::GetContentRegionAvail().x;

    const ImVec2 pos = window->DC.CursorPos;
    const ImRect total_bb(pos, ImVec2(pos.x + full_width, pos.y + frame_height));

    ImGui::ItemSize(total_bb, style.FramePadding.y);

    ImGui::PushID(label);
    const ImGuiID id = window->GetID("##multi_dropdown_field");
    if (!ImGui::ItemAdd(total_bb, id))
    {
        ImGui::PopID();
        return false;
    }

    bool hovered = false;
    bool held = false;
    bool pressed = ImGui::ButtonBehavior(total_bb, id, &hovered, &held);
    if (pressed)
        ImGui::OpenPopup("##multi_dropdown_popup");

    const bool popup_open = ImGui::IsPopupOpen("##multi_dropdown_popup");
    bool popup_padding_pushed = false;
    if (popup_open)
    {
        ImGui::SetNextWindowPos(ImVec2(total_bb.Min.x, total_bb.Max.y + 1.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(total_bb.GetWidth(), 0.0f), ImVec2(total_bb.GetWidth(), 240.0f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
        ImVec2 popup_padding = style.WindowPadding;
        if (popup_padding.x < 1.0f)
        {
            popup_padding.x = kPopupPadding;
            popup_padding_pushed = true;
        }
        if (popup_padding.y < 1.0f)
        {
            popup_padding.y = kPopupPadding;
            popup_padding_pushed = true;
        }
        if (popup_padding_pushed)
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popup_padding);
    }

    ImDrawList* draw_list = window->DrawList;
    ImRect outer_bb = total_bb;
    ImRect inner_bb = shrink_rect(outer_bb, kBorderThickness);
    ImRect fill_bb = shrink_rect(inner_bb, 1.0f);

    ImVec4 fill_color = c_colors::top_child_background;
    if (held)
        fill_color = scale_color(fill_color, 0.9f);
    else if (hovered)
        fill_color = scale_color(fill_color, 1.15f);

    draw_list->AddRect(outer_bb.Min, outer_bb.Max, ImGui::GetColorU32(c_colors::outter_border), c_colors::widget_rounding, 0, kBorderThickness);
    draw_list->AddRect(inner_bb.Min, inner_bb.Max, ImGui::GetColorU32(c_colors::main_border), ImMax(0.0f, c_colors::widget_rounding - 1.0f), 0, kBorderThickness);
    draw_list->AddRectFilled(fill_bb.Min, fill_bb.Max, ImGui::GetColorU32(fill_color));

    const float indicator_width = frame_height * 0.9f;
    ImRect indicator_bb(ImVec2(fill_bb.Max.x - indicator_width, fill_bb.Min.y), fill_bb.Max);
    ImRect text_bb(fill_bb.Min, ImVec2(indicator_bb.Min.x, fill_bb.Max.y));
    const float divider_x = indicator_bb.Min.x;

    const ImVec4 base_text_col = style.Colors[ImGuiCol_Text];
    int selected_count = 0;
    std::string preview_text;
    int appended = 0;
    for (int i = 0; i < items_count; ++i)
    {
        if (!selections[i])
            continue;

        ++selected_count;
        if (appended < 2)
        {
            if (!preview_text.empty())
                preview_text.append(", ");
            preview_text.append(items[i]);
            ++appended;
        }
    }

    if (selected_count == 0)
    {
        preview_text = "None";
    }
    else if (selected_count > appended)
    {
        if (!preview_text.empty())
            preview_text.append(", ");
        preview_text.append(std::to_string(selected_count - appended));
        preview_text.append(" more");
    }

    const ImVec2 text_size = ImGui::CalcTextSize(preview_text.c_str());

    const float base_x = text_bb.Min.x + style.FramePadding.x;
    const ImVec2 text_pos(
        base_x,
        text_bb.Min.y + ImMax(0.0f, (text_bb.GetHeight() - text_size.y) * 0.5f)
    );

    const bool text_overflow = (text_pos.x + text_size.x) > (text_bb.Max.x - style.FramePadding.x);
    ImVec2 clip_min = text_bb.Min;
    ImVec2 clip_max = text_bb.Max;
    if (text_overflow)
        clip_max.x = divider_x - style.FramePadding.x * 0.5f;

    ImVec4 preview_col = dropdown_preview_color(base_text_col, popup_open || held, hovered);
    draw_list->PushClipRect(clip_min, clip_max, true);
    draw_list->AddText(text_pos, ImGui::GetColorU32(preview_col), preview_text.c_str());
    draw_list->PopClipRect();

    if (text_overflow)
    {
        const float fade_width = ImMin(style.FramePadding.x * 3.0f, text_bb.GetWidth() * 0.6f);
        const ImVec2 fade_min(divider_x - fade_width, fill_bb.Min.y + 1.0f);
        const ImVec2 fade_max(divider_x, fill_bb.Max.y - 1.0f);
        ImVec4 solid_col = fill_color;
        solid_col.w = ImClamp(solid_col.w * 0.95f, 0.0f, 1.0f);
        ImVec4 transparent_col = solid_col;
        transparent_col.w = 0.0f;
        draw_list->AddRectFilledMultiColor(
            fade_min,
            fade_max,
            ImGui::GetColorU32(transparent_col),
            ImGui::GetColorU32(solid_col),
            ImGui::GetColorU32(solid_col),
            ImGui::GetColorU32(transparent_col));
    }

    draw_list->AddLine(ImVec2(divider_x, indicator_bb.Min.y), ImVec2(divider_x, indicator_bb.Max.y), ImGui::GetColorU32(c_colors::main_border));

    const ImVec2 icon_center(
        indicator_bb.Min.x + indicator_bb.GetWidth() * 0.5f,
        indicator_bb.Min.y + indicator_bb.GetHeight() * 0.5f
    );
    const float icon_half = indicator_bb.GetHeight() * 0.20f;
    ImVec4 indicator_col = dropdown_preview_color(base_text_col, popup_open || held, hovered);
    const ImU32 indicator_u32 = ImGui::GetColorU32(indicator_col);
    draw_list->AddLine(ImVec2(icon_center.x - icon_half, icon_center.y), ImVec2(icon_center.x + icon_half, icon_center.y), indicator_u32, 1.25f);
    if (!popup_open)
        draw_list->AddLine(ImVec2(icon_center.x, icon_center.y - icon_half), ImVec2(icon_center.x, icon_center.y + icon_half), indicator_u32, 1.25f);

    bool value_changed = false;
    if (popup_open && ImGui::BeginPopup("##multi_dropdown_popup"))
    {
        ImDrawList* popup_draw_list = ImGui::GetWindowDrawList();
        const ImVec2 popup_pos = ImGui::GetWindowPos();
        const ImVec2 popup_size = ImGui::GetWindowSize();
        const ImVec2 popup_max = ImVec2(popup_pos.x + popup_size.x, popup_pos.y + popup_size.y);

        popup_draw_list->AddRectFilled(popup_pos, popup_max, ImGui::GetColorU32(c_colors::top_child_background));
        popup_draw_list->AddRect(popup_pos, popup_max, ImGui::GetColorU32(c_colors::outter_border), c_colors::widget_rounding, 0, kBorderThickness);

        ImRect popup_inner = shrink_rect(ImRect(popup_pos, popup_max), kBorderThickness);
        popup_draw_list->AddRect(popup_inner.Min, popup_inner.Max, ImGui::GetColorU32(c_colors::main_border),
                                 ImMax(0.0f, c_colors::widget_rounding - 1.0f), 0, kBorderThickness);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * 0.5f, style.ItemSpacing.y * 0.35f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
        const float row_height = ImGui::GetFontSize() + style.FramePadding.y * 0.8f;
        bool first_item = true;

        for (int i = 0; i < items_count; ++i)
        {
            ImGui::PushID(i);
            const bool is_selected = selections[i];
            ImGuiStorage* storage = ImGui::GetStateStorage();
            const ImGuiID anim_id = ImGui::GetID("anim");
            const float target = is_selected ? 1.0f : 0.0f;
            float anim = storage ? storage->GetFloat(anim_id, target) : target;
            const float anim_speed = 18.0f;
            anim = ImLerp(anim, target, ImClamp(ImGui::GetIO().DeltaTime * anim_speed, 0.0f, 1.0f));
            if (ImFabs(anim - target) < 0.001f)
                anim = target;
            if (storage)
                storage->SetFloat(anim_id, anim);

            const ImGuiSelectableFlags selectable_flags = ImGuiSelectableFlags_DontClosePopups | ImGuiSelectableFlags_SpanAvailWidth | ImGuiSelectableFlags_AllowItemOverlap;
            const bool item_pressed = ImGui::Selectable(items[i], is_selected, selectable_flags, ImVec2(0.0f, row_height));
            const bool item_hovered = ImGui::IsItemHovered();
            ImRect item_bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            ImVec4 item_text_col = dropdown_popup_text_color(base_text_col, is_selected, item_hovered);
            const float circle_radius = ImGui::GetFontSize() * 0.22f;
            const float left_padding = 4.0f;
            const float slide_distance = 10.0f;
            const float base_offset = left_padding;
            const ImVec2 item_text_pos(
                item_bb.Min.x + base_offset + anim * slide_distance,
                item_bb.Min.y + (item_bb.GetHeight() - ImGui::GetFontSize()) * 0.5f
            );
            popup_draw_list->AddText(item_text_pos, ImGui::GetColorU32(item_text_col), items[i]);

            const float circle_alpha = ImClamp(anim, 0.0f, 1.0f);
            if (circle_alpha > 0.01f)
            {
                const float desired_x = item_text_pos.x - (circle_radius * 2.0f + 2.0f);
                const float min_x = item_bb.Min.x + circle_radius + 3.0f;
                ImVec2 marker_center(
                    ImMax(min_x, desired_x),
                    item_bb.Min.y + (item_bb.GetHeight() * 0.5f)
                );
                ImVec4 marker_col = c_colors::top_accent_color;
                marker_col.w *= circle_alpha;
                popup_draw_list->AddCircleFilled(marker_center, circle_radius, ImGui::GetColorU32(marker_col));
            }

            if (item_pressed)
            {
                selections[i] = !selections[i];
                value_changed = true;
            }
            ImGui::PopID();

            if (i + 1 < items_count)
            {
                if (first_item)
                    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 0.05f));
                else
                    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 0.2f));
            }
            first_item = false;
        }

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    if (popup_open)
    {
        if (popup_padding_pushed)
            ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ImGui::PopID();
    return value_changed;
}
