#include "../widgets.h"
#include "../../colors/colors.h"
#include <imgui/imgui_internal.h>

namespace
{
    struct c_context
    {
        ImVec2 cursor_start;
        ImVec2 child_size;
        float  applied_margin = 0.0f;
        float  bottom_padding_compensation = 0.0f;
        bool   use_accent = false;
        bool   style_color_pushed = false;
        bool   style_padding_pushed = false;
        bool   clip_pushed = false;
        bool   allow_scroll = false;
        ImVec2 child_min;
        ImVec2 child_max;
        ImVec2 clip_min;
        ImVec2 clip_max;
    };

    struct smooth_scroll_keys
    {
        ImGuiID current;
        ImGuiID target;
    };

    inline ImVector<c_context>& get_child_stack()
    {
        static ImVector<c_context> stack;
        return stack;
    }

    inline const smooth_scroll_keys& get_smooth_scroll_keys()
    {
        static const smooth_scroll_keys keys{
            ImHashStr("c_widgets::smooth_scroll_current"),
            ImHashStr("c_widgets::smooth_scroll_target")
        };
        return keys;
    }

    constexpr float child_margin = 2.0f;

    inline ImVec4 with_alpha(ImVec4 color, float alpha)
    {
        color.w = alpha;
        return color;
    }

    void update_smooth_scroll()
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (!window)
            return;

        ImGuiStorage* storage = window->DC.StateStorage;
        if (!storage)
            return;

        const auto& keys = get_smooth_scroll_keys();
        float current = storage->GetFloat(keys.current, window->Scroll.y);
        float target = storage->GetFloat(keys.target, window->Scroll.y);
        const float scroll_max = window->ScrollMax.y;

        if (scroll_max <= 0.0f)
        {
            current = target = window->Scroll.y;
            storage->SetFloat(keys.current, current);
            storage->SetFloat(keys.target, target);
            return;
        }

        current = ImClamp(current, 0.0f, scroll_max);
        target = ImClamp(target, 0.0f, scroll_max);

        if (ImFabs(window->Scroll.y - current) > 0.5f)
        {
            current = window->Scroll.y;
            target = window->Scroll.y;
        }

        ImGuiIO& io = ImGui::GetIO();
        const ImGuiHoveredFlags hovered_flags = ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_AllowWhenBlockedByPopup;
        if (!io.KeyCtrl && ImGui::IsWindowHovered(hovered_flags))
        {
            const float wheel = io.MouseWheel;
            if (wheel != 0.0f)
            {
                const float max_step = window->InnerRect.GetHeight() * 0.67f;
                const float scroll_step = ImTrunc(ImMin(5.0f * window->FontRefSize, max_step));
                if (scroll_step > 0.0f)
                    target = ImClamp(target - wheel * scroll_step, 0.0f, scroll_max);
            }
        }

        const float smoothing_speed = 12.0f;
        const float lerp_alpha = ImClamp(io.DeltaTime * smoothing_speed, 0.0f, 1.0f);
        current = ImLerp(current, target, lerp_alpha);

        if (ImFabs(current - target) <= 0.1f)
            current = target;

        storage->SetFloat(keys.current, current);
        storage->SetFloat(keys.target, target);
        ImGui::SetScrollY(current);
    }
}

static ImVec2 calculate_child_size(const ImVec2& avail, const ImVec2& preferred, float margin)
{
    ImVec2 requested = avail;
    if (preferred.x >= 0.0f)
        requested.x = ImMin(preferred.x, avail.x);
    if (preferred.y >= 0.0f)
        requested.y = ImMin(preferred.y, avail.y);

    ImVec2 size;
    size.x = ImMax(0.0f, requested.x - margin * 2.0f);
    size.y = ImMax(0.0f, requested.y - margin * 2.0f);
    return size;
}

bool c_widgets::begin_padded_child(const char* str_id, ImGuiChildFlags child_flags, ImGuiWindowFlags window_flags, ImVec2 preferred_size, bool use_margin, bool use_gradient, bool use_accent, bool use_clip, bool block_parent_drag, bool use_accent_background, bool remove_accent_top_padding, bool allow_scroll)
{
    ImVec2 cursor = ImGui::GetCursorPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 parent_padding = ImGui::GetStyle().WindowPadding;

    if (!use_margin)
    {
        avail.x += parent_padding.x;
        avail.y += parent_padding.y;
    }

    float margin = use_margin ? child_margin : 0.0f;
    ImVec2 child_size = calculate_child_size(avail, preferred_size, margin);

    ImVec2 cursor_screen = ImGui::GetCursorScreenPos();
    ImVec2 child_min = ImVec2(cursor_screen.x + margin, cursor_screen.y + margin);
    ImVec2 child_max = ImVec2(child_min.x + child_size.x, child_min.y + child_size.y);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImU32 top_col = ImGui::GetColorU32(c_colors::top_child_background);
    ImU32 bottom_col = ImGui::GetColorU32(c_colors::bottom_child_background);
    auto snap_rect = [](ImRect rect) -> ImRect
    {
        rect.Min.x = IM_ROUND(rect.Min.x);
        rect.Min.y = IM_ROUND(rect.Min.y);
        rect.Max.x = IM_ROUND(rect.Max.x);
        rect.Max.y = IM_ROUND(rect.Max.y);
        return rect;
    };

    ImRect outer_rect(child_min, child_max);
    outer_rect = snap_rect(outer_rect);

    if (use_accent_background)
    {
        draw_list->AddRectFilled(outer_rect.Min, outer_rect.Max, ImGui::GetColorU32(c_colors::top_accent_color),
                                 c_colors::panel_rounding);
    }

    const float inner_offset = 1.0f;
    ImRect inner_rect(
        ImVec2(outer_rect.Min.x + inner_offset, outer_rect.Min.y + inner_offset),
        ImVec2(outer_rect.Max.x - inner_offset, outer_rect.Max.y - inner_offset));

    if (use_gradient)
        c_colors::draw_rounded_gradient_rect(draw_list, outer_rect.Min, outer_rect.Max, top_col, bottom_col,
                                             c_colors::panel_rounding);
    else
        draw_list->AddRectFilled(outer_rect.Min, outer_rect.Max, top_col, c_colors::panel_rounding);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImVec2 padding = use_margin ? ImVec2(8.0f, 8.0f) : ImVec2(0.0f, 0.0f);
    const bool trim_accent_padding = use_accent && remove_accent_top_padding;
    const float bottom_padding_compensation = trim_accent_padding ? padding.y : 0.0f;
    if (trim_accent_padding)
        padding.y = 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);

    c_context ctx{};
    ctx.cursor_start = cursor;
    ctx.child_size = child_size;
    ctx.applied_margin = margin;
    ctx.bottom_padding_compensation = bottom_padding_compensation;
    ctx.use_accent = use_accent;
    ctx.style_color_pushed = true;
    ctx.style_padding_pushed = true;
    ctx.allow_scroll = allow_scroll;
    ctx.child_min = outer_rect.Min;
    ctx.child_max = outer_rect.Max;
    get_child_stack().push_back(ctx);
    c_context& stored_ctx = get_child_stack().back();

    ImGui::SetCursorPos(ImVec2(cursor.x + margin, cursor.y + margin));
    window_flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollWithMouse;
    (void)allow_scroll;
    (void)block_parent_drag;

    bool opened = ImGui::BeginChild(str_id, child_size, child_flags, window_flags);

    if (opened)
        update_smooth_scroll();

    if (opened && use_clip)
    {
        const float border_clip = trim_accent_padding ? 0.0f : 6.0f;
        ImVec2 win_pos = ImGui::GetWindowPos();
        ImVec2 win_size = ImGui::GetWindowSize();

        float clip_top_extra = trim_accent_padding ? 0.0f : 3.0f;
        ImVec2 clip_min = ImVec2(win_pos.x, win_pos.y + border_clip + clip_top_extra);
        ImVec2 clip_max = ImVec2(win_pos.x + win_size.x, win_pos.y + win_size.y - border_clip);

        stored_ctx.clip_pushed = true;
        stored_ctx.clip_min = clip_min;
        stored_ctx.clip_max = clip_max;
    }

    return opened;
}

void c_widgets::end_padded_child()
{
    ImVector<c_context>& stack = get_child_stack();
    IM_ASSERT(stack.Size > 0 && "end_padded_child called without matching begin_padded_child");
    c_context ctx = stack.back();
    stack.pop_back();

    if (ctx.bottom_padding_compensation > 0.0f)
        ImGui::Dummy(ImVec2(0.0f, ctx.bottom_padding_compensation));

    if (ctx.clip_pushed && ctx.allow_scroll)
    {
        const float scroll_max = ImGui::GetScrollMaxY();
        const float scroll_y = ImGui::GetScrollY();
        const float fade_height = 14.0f;
        const float fade_inset_x = 2.0f;
        const float fade_bottom = ctx.child_max.y - 1.0f;
        const float fade_top = ctx.child_min.y + 1.0f;

        if (scroll_max > fade_height && scroll_y < scroll_max - 0.5f)
        {
            ImVec2 fade_min = ImVec2(ctx.child_min.x + fade_inset_x, fade_bottom - fade_height);
            ImVec2 fade_max = ImVec2(ctx.child_max.x - fade_inset_x, fade_bottom);
            fade_min.y = ImMax(fade_min.y, fade_top);

            ImU32 transparent = ImGui::GetColorU32(with_alpha(c_colors::bottom_child_background, 0.0f));
            ImU32 solid = ImGui::GetColorU32(with_alpha(c_colors::bottom_child_background, 0.95f));

            ImDrawList* draw_list = ImGui::GetCurrentWindow()->DrawList;
            draw_list->AddRectFilledMultiColor(
                fade_min,
                fade_max,
                transparent,
                transparent,
                solid,
                solid
            );
        }
        if (scroll_y > 0.5f)
        {
            ImVec2 fade_min = ImVec2(ctx.child_min.x + fade_inset_x, fade_top);
            ImVec2 fade_max = ImVec2(ctx.child_max.x - fade_inset_x, fade_top + fade_height);
            fade_max.y = ImMin(fade_max.y, fade_bottom);

            ImU32 transparent = ImGui::GetColorU32(with_alpha(c_colors::top_child_background, 0.0f));
            ImU32 solid = ImGui::GetColorU32(with_alpha(c_colors::top_child_background, 0.95f));

            ImDrawList* draw_list = ImGui::GetCurrentWindow()->DrawList;
            draw_list->AddRectFilledMultiColor(
                fade_min,
                fade_max,
                solid,
                solid,
                transparent,
                transparent
            );
        }

    }

    {
        ImDrawList* draw_list = ImGui::GetCurrentWindow()->DrawList;
        const float inner_offset = 1.0f;
        const ImRect outer_rect(ctx.child_min, ctx.child_max);
        const ImRect inner_rect(
            ImVec2(ctx.child_min.x + inner_offset, ctx.child_min.y + inner_offset),
            ImVec2(ctx.child_max.x - inner_offset, ctx.child_max.y - inner_offset));

        draw_list->PushClipRect(ctx.child_min, ctx.child_max, false);

        const bool prev_aa = (draw_list->Flags & ImDrawListFlags_AntiAliasedLines) != 0;
        draw_list->Flags &= ~ImDrawListFlags_AntiAliasedLines;
        const float rounding = c_colors::panel_rounding;
        draw_list->AddRect(outer_rect.Min, outer_rect.Max, ImGui::GetColorU32(c_colors::outter_border), rounding, 0,
                           1.0f);
        draw_list->AddRect(inner_rect.Min, inner_rect.Max, ImGui::GetColorU32(c_colors::main_border), rounding, 0,
                           1.0f);
        if (prev_aa)
            draw_list->Flags |= ImDrawListFlags_AntiAliasedLines;

        if (ctx.use_accent)
        {
            const float accent_height = 2.0f;
            ImVec2 accent_min = ImVec2(inner_rect.Min.x, inner_rect.Min.y);
            ImVec2 accent_max = ImVec2(inner_rect.Max.x, accent_min.y + accent_height);
            ImVec2 underline_min = ImVec2(accent_min.x, accent_max.y);
            ImVec2 underline_max = ImVec2(accent_max.x, accent_max.y + 1.0f);

            draw_list->AddRectFilledMultiColor(
                accent_min,
                accent_max,
                ImGui::GetColorU32(c_colors::top_accent_color),
                ImGui::GetColorU32(c_colors::top_accent_color),
                ImGui::GetColorU32(c_colors::bottom_accent_color),
                ImGui::GetColorU32(c_colors::bottom_accent_color));
            draw_list->AddRectFilled(underline_min, underline_max, ImGui::GetColorU32(c_colors::outter_border));
        }

        draw_list->PopClipRect();
    }

    ImGui::EndChild();

    if (ctx.style_padding_pushed)
        ImGui::PopStyleVar();
    if (ctx.style_color_pushed)
        ImGui::PopStyleColor();

    ImVec2 full_size(
        ctx.child_size.x + ctx.applied_margin * 2.0f,
        ctx.child_size.y + ctx.applied_margin * 2.0f
    );

    ImGui::SetCursorPos(ctx.cursor_start);
    ImGui::Dummy(full_size);
}

float c_widgets::padded_child_margin()
{
    return child_margin;
}

void c_widgets::window_drag_handle(const char* id, float height)
{
    if (!id || !*id)
        return;

    if (height <= 0.0f)
        height = ImGui::GetTextLineHeight() + ImGui::GetStyle().WindowPadding.y * 0.5f;

    const ImVec2 pos = ImGui::GetCursorPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPos(pos);
    ImGui::InvisibleButton(id, ImVec2(width, height), ImGuiButtonFlags_MouseButtonLeft);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
        ImGui::SetWindowPos(ImGui::GetWindowPos() + ImGui::GetIO().MouseDelta);
}
