#include "../widgets.h"
#include "../../colors/colors_new.h"
#include <imgui/imgui_internal.h>

namespace
{
    struct checkbox_spacing_state
    {
        ImGuiID last_window_id = 0;
        int     last_frame = -1;
        bool    last_was_checkbox = false;
        float   last_bottom = 0.0f;
    };

    inline checkbox_spacing_state& get_spacing_state()
    {
        static checkbox_spacing_state state;
        return state;
    }

    inline ImGuiID checkbox_anim_key(ImGuiID base_id)
    {
        static const ImGuiID seed = ImHashStr("c_widgets::checkbox_anim");
        return ImHashData(&base_id, sizeof(base_id), seed);
    }

    inline ImGuiID checkbox_state_key(ImGuiID base_id)
    {
        static const ImGuiID seed = ImHashStr("c_widgets::checkbox_state");
        return ImHashData(&base_id, sizeof(base_id), seed);
    }

    inline float ease_out_expo(float t)
    {
        if (t <= 0.0f)
            return 0.0f;
        if (t >= 1.0f)
            return 1.0f;
        return 1.0f - ImPow(2.0f, -10.0f * t);
    }

    inline void draw_checkmark(ImDrawList* draw_list, const ImRect& rect, ImU32 color, float progress)
    {
        if (progress <= 0.01f)
            return;

        const ImVec2 center = rect.GetCenter();
        const float scale = progress;
        const ImVec2 p1(center.x - 4.0f * scale, center.y + 0.5f * scale);
        const ImVec2 p2(center.x - 1.0f * scale, center.y + 3.5f * scale);
        const ImVec2 p3(center.x + 4.5f * scale, center.y - 3.0f * scale);
        draw_list->AddLine(p1, p2, color, 2.0f);
        draw_list->AddLine(p2, p3, color, 2.0f);
    }
}

bool c_widgets::checkbox(const char* label, bool* v)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImVec2 original_item_spacing = g.Style.ItemSpacing;
    constexpr float spacing_ratio = 0.7f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(original_item_spacing.x, original_item_spacing.y * spacing_ratio));
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);

    checkbox_spacing_state& spacing = get_spacing_state();
    constexpr float square_sz = 16.0f;
    const bool last_item_was_checkbox =
        spacing.last_was_checkbox &&
        spacing.last_frame == g.FrameCount &&
        spacing.last_window_id == window->ID;
    const bool can_stack = !window->DC.IsSameLine;
    if (can_stack && last_item_was_checkbox)
    {
        const float desired_spacing = original_item_spacing.y * spacing_ratio + 0.5f;
        const float current_gap = ImGui::GetCursorScreenPos().y - spacing.last_bottom;
        const float max_correction = square_sz;
        if (current_gap > desired_spacing + 0.1f)
        {
            const float delta = current_gap - desired_spacing;
            if (delta < max_correction)
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - delta);
        }
        else if (current_gap + 0.1f < desired_spacing)
        {
            ImGui::Dummy(ImVec2(0.0f, desired_spacing - current_gap));
        }
    }

    ImVec2 pos = window->DC.CursorPos;
    const float label_gap = label_size.x > 0.0f ? 10.0f : 0.0f;
    float total_width = square_sz + (label_size.x > 0.0f ? label_gap + label_size.x : 0.0f);
    const float total_height = ImMax(label_size.y, square_sz);
    ImRect total_bb(pos, pos + ImVec2(total_width, total_height));

    ImGui::ItemSize(total_bb, style.FramePadding.y);
    ImGui::SetNextItemAllowOverlap();
    if (!ImGui::ItemAdd(total_bb, id))
    {
        ImGui::PopStyleVar();
        spacing.last_was_checkbox = false;
        return false;
    }

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(total_bb, id, &hovered, &held);
    if (pressed)
    {
        *v = !*v;
        ImGui::MarkItemEdited(id);
    }

    ImRect check_bb(pos, pos + ImVec2(square_sz, square_sz));
    ImDrawList* draw_list = window->DrawList;
    const float rounding = c_colors::widget_rounding;

    const bool is_active = *v;
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID anim_key = checkbox_anim_key(id);
    const ImGuiID state_key = checkbox_state_key(id);

    float active_t = storage->GetFloat(anim_key, 1.0f);
    bool last_state = storage->GetBool(state_key, is_active);
    if (last_state != is_active)
        active_t = 0.0f;
    storage->SetBool(state_key, is_active);

    const float active_speed = 4.0f;
    active_t = ImMin(active_t + g.IO.DeltaTime * active_speed, 1.0f);
    storage->SetFloat(anim_key, active_t);
    const float active_progress = is_active ? ease_out_expo(active_t) : (1.0f - ease_out_expo(active_t));

    ImVec4 fill_col = ImLerp(c_colors::surface_inset, c_colors::top_accent_color, active_progress);
    ImVec4 border_col = ImLerp(c_colors::border_soft, c_colors::top_accent_color, active_progress);

    draw_list->AddRectFilled(check_bb.Min, check_bb.Max, ImGui::GetColorU32(fill_col), rounding);
    draw_list->AddRect(check_bb.Min, check_bb.Max, ImGui::GetColorU32(border_col), rounding, 0, 1.5f);
    draw_checkmark(draw_list, check_bb, ImGui::GetColorU32(c_colors::accent_on), active_progress);

    if (label_size.x > 0.0f)
    {
        ImVec2 label_pos = ImVec2(check_bb.Max.x + label_gap, check_bb.Min.y + (square_sz - label_size.y) * 0.5f);
        ImVec4 text_col = is_active ? c_colors::white : (hovered ? c_colors::white : c_colors::text_muted);
        ImGui::PushStyleColor(ImGuiCol_Text, text_col);
        ImGui::RenderText(label_pos, label);
        ImGui::PopStyleColor();
    }

    spacing.last_frame = g.FrameCount;
    spacing.last_window_id = window->ID;
    spacing.last_was_checkbox = true;
    spacing.last_bottom = total_bb.Max.y;

    ImGui::PopStyleVar();
    return pressed;
}
