#include "features/target_hud.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <Windows.h>

#include "cache/local_player_cache.h"
#include "cache/player_cache.h"
#include "features/aimbot.h"
#include "features/free_aim.h"
#include "features/triggerbot.h"
#include "globals/globals_fixed.h"
#include "gui/overlay.hpp"
#include "gui/resources/fonts.h"
#include "sdk/camera.h"
#include "sdk/engine.h"

namespace
{
    struct camera_frame_t
    {
        rbx::Matrix view_matrix;
        rbx::Vector2 dimensions;
    };

    bool is_printable_ascii(const std::string& text)
    {
        for (unsigned char c : text)
        {
            if (c < 32 || c > 126)
                return false;
        }
        return true;
    }

    std::string sanitize_name_label(const std::string& display, const std::string& username)
    {
        if (display.empty())
            return username;
        if (!is_printable_ascii(display))
            return username;
        return display;
    }

    std::string ellipsize_text(ImFont* font, float font_size, const std::string& text, float max_width)
    {
        if (text.empty() || max_width <= 0.0f)
            return text;
        if (font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str()).x <= max_width)
            return text;

        std::string out = text;
        while (!out.empty())
        {
            const std::string candidate = out + "...";
            if (font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, candidate.c_str()).x <= max_width)
                return candidate;
            out.pop_back();
        }
        return "...";
    }

    cache::player_state make_dummy_player_state(const cache::dummy_state& dummy)
    {
        cache::player_state state{};
        state.address = dummy.address;
        state.name = !dummy.name.empty() ? dummy.name : "Dummy";
        state.display_name = state.name;
        state.health = dummy.health;
        state.max_health = dummy.max_health;
        state.character = dummy.character;
        state.humanoid = dummy.humanoid;
        state.parts = dummy.parts;
        state.body_effects = dummy.body_effects;
        state.team = static_cast<std::uintptr_t>(-1);
        return state;
    }

    std::optional<std::uintptr_t> resolve_visualengine_address()
    {
        auto address = globals->visualengine.get_address();
        if (address == 0)
        {
            address = rbx::engine->get_visualengine();
            if (address)
                globals->visualengine = rbx::instance_t(address);
        }
        return address ? std::optional<std::uintptr_t>(address) : std::nullopt;
    }

    std::optional<camera_frame_t> read_camera_frame()
    {
        const auto visualengine_address = resolve_visualengine_address();
        if (!visualengine_address)
            return std::nullopt;

        rbx::visualengine_t visualengine(*visualengine_address);
        auto view_matrix = visualengine.get_view_matrix();
        if (!view_matrix)
            return std::nullopt;

        auto dimensions = visualengine.get_dimensions();
        if (!dimensions)
        {
            const ImVec2 display_size = ImGui::GetIO().DisplaySize;
            if (display_size.x <= 0.0f || display_size.y <= 0.0f)
                return std::nullopt;
            dimensions = rbx::Vector2(display_size.x, display_size.y);
        }

        return camera_frame_t{ *view_matrix, *dimensions };
    }

    std::optional<rbx::Vector3> get_part_position(const cache::primitive_part& part)
    {
        if (!part.instance.is_valid())
            return std::nullopt;
        return part.instance.get_position(part.primitive);
    }

    std::optional<ImVec2> project_part_to_screen(const cache::primitive_part& part, const camera_frame_t& frame)
    {
        const auto world_pos = get_part_position(part);
        if (!world_pos)
            return std::nullopt;

        const auto screen = rbx::camera::world_to_screen(*world_pos, frame.view_matrix, frame.dimensions);
        if (!screen)
            return std::nullopt;

        return ImVec2(screen->x, screen->y);
    }

    std::optional<ImVec2> resolve_target_anchor_screen(const cache::player_state& player, const camera_frame_t& frame)
    {
        if (auto head = project_part_to_screen(player.parts.head, frame))
            return head;
        if (auto torso = project_part_to_screen(player.parts.upper_torso, frame))
            return torso;
        return project_part_to_screen(player.parts.humanoid_root_part, frame);
    }

    ImVec2 get_display_size()
    {
        ImVec2 display_size = ImGui::GetIO().DisplaySize;
        if (vanille::overlay::g_rbx_window && ::IsWindow(vanille::overlay::g_rbx_window))
        {
            RECT rc{};
            if (::GetClientRect(vanille::overlay::g_rbx_window, &rc))
            {
                const float w = static_cast<float>(rc.right - rc.left);
                const float h = static_cast<float>(rc.bottom - rc.top);
                if (w > 0.0f && h > 0.0f)
                    display_size = ImVec2(w, h);
            }
        }
        return display_size;
    }

    ImVec2 compute_panel_position(
        const ImVec2& anchor,
        const ImVec2& panel_size,
        int anchor_mode,
        float gap,
        const ImVec2& display_size)
    {
        const int mode = std::clamp(anchor_mode, 0, 7);
        ImVec2 pos{};

        switch (mode)
        {
        case 0:
            pos = ImVec2(anchor.x + gap, anchor.y - panel_size.y * 0.5f);
            break;
        case 1:
            pos = ImVec2(anchor.x - gap - panel_size.x, anchor.y - panel_size.y * 0.5f);
            break;
        case 2:
            pos = ImVec2(anchor.x - panel_size.x * 0.5f, anchor.y - gap - panel_size.y);
            break;
        case 3:
            pos = ImVec2(anchor.x - panel_size.x * 0.5f, anchor.y + gap);
            break;
        case 4:
            pos = ImVec2(anchor.x + gap, anchor.y - gap - panel_size.y);
            break;
        case 5:
            pos = ImVec2(anchor.x - gap - panel_size.x, anchor.y - gap - panel_size.y);
            break;
        case 6:
            pos = ImVec2(anchor.x + gap, anchor.y + gap);
            break;
        default:
            pos = ImVec2(anchor.x - gap - panel_size.x, anchor.y + gap);
            break;
        }

        constexpr float margin = 8.0f;
        pos.x = std::clamp(pos.x, margin, (std::max)(margin, display_size.x - panel_size.x - margin));
        pos.y = std::clamp(pos.y, margin, (std::max)(margin, display_size.y - panel_size.y - margin));
        return ImVec2(std::round(pos.x), std::round(pos.y));
    }

    void draw_fill_bar(
        ImDrawList* draw,
        float fill_ratio,
        const ImVec2& position,
        const ImVec2& size,
        const ImVec4& top_color,
        const ImVec4& bottom_color)
    {
        if (!draw || size.x <= 0.0f || size.y <= 0.0f)
            return;

        const float clamped_ratio = std::clamp(fill_ratio, 0.0f, 1.0f);
        const ImVec2 bar_min(std::round(position.x), std::round(position.y));
        const ImVec2 bar_max(std::round(position.x + size.x), std::round(position.y + size.y));
        const ImVec2 filled_max(std::round(position.x + size.x * clamped_ratio), bar_max.y);

        draw->AddRectFilled(bar_min, bar_max, IM_COL32(42, 42, 42, 210), 1.5f);

        if (clamped_ratio > 0.0f)
        {
            const ImU32 left_color = ImGui::GetColorU32(top_color);
            const ImU32 right_color = ImGui::GetColorU32(bottom_color);
            draw->AddRectFilledMultiColor(bar_min, filled_max, right_color, left_color, left_color, right_color);
        }
    }

    void draw_panel_background(ImDrawList* draw, const ImVec2& pos, const ImVec2& size, float rounding)
    {
        const ImVec2 br(pos.x + size.x, pos.y + size.y);
        const ImU32 panel_col = ImGui::GetColorU32(ImVec4(0.075f, 0.075f, 0.078f, 0.92f));
        const ImU32 border_col = ImGui::GetColorU32(c_colors::main_border);

        draw->AddRectFilled(pos, br, panel_col, rounding);
        draw->AddRect(pos, br, border_col, rounding, 0, 1.0f);
    }

    ImVec4 health_color(float ratio)
    {
        if (ratio >= 0.75f)
            return ImVec4(0.25f, 0.86f, 0.25f, 1.0f);
        if (ratio >= 0.25f)
            return ImVec4(1.0f, 0.60f, 0.15f, 1.0f);
        return ImVec4(0.86f, 0.25f, 0.25f, 1.0f);
    }

    std::optional<cache::player_state> resolve_locked_player(std::uintptr_t locked_address)
    {
        const auto players_snapshot = cache::players_cache ? cache::players_cache->snapshot() : nullptr;
        if (players_snapshot)
        {
            for (const auto& player : *players_snapshot)
            {
                if (player.address == locked_address)
                    return player;
            }
        }

        const auto dummy = cache::players_cache ? cache::players_cache->dummy_snapshot() : nullptr;
        if (dummy && dummy->address == locked_address)
            return make_dummy_player_state(*dummy);

        return std::nullopt;
    }
    std::uintptr_t resolve_locked_player_address()
    {
        if (features->enable_aimbot)
        {
            const std::uintptr_t aimbot_locked = aimbot::get_locked_player();
            if (aimbot_locked != 0)
            {
                return aimbot_locked;
            }
        }

        if (features->enable_free_aim)
        {
            const std::uintptr_t silent_locked = free_aim::get_locked_player();
            if (silent_locked != 0)
            {
                return silent_locked;
            }
        }

        if (features->enable_triggerbot)
        {
            const std::uintptr_t trigger_locked = triggerbot::get_locked_player();
            if (trigger_locked != 0)
            {
                return trigger_locked;
            }
        }

        return 0;
    }
}

void target_hud::render()
{
    if (!features->enable_target_hud)
        return;

    const std::uintptr_t locked_address = resolve_locked_player_address();
    if (locked_address == 0)
        return;

    const auto target_player = resolve_locked_player(locked_address);
    if (!target_player)
        return;

    const auto frame = read_camera_frame();
    if (!frame)
        return;

    const auto anchor_screen = resolve_target_anchor_screen(*target_player, *frame);
    if (!anchor_screen)
        return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (!draw)
        return;

    ImFont* title_font = c_fonts::verdana_bold ? c_fonts::verdana_bold : ImGui::GetFont();
    ImFont* meta_font = c_fonts::smallest_pixel ? c_fonts::smallest_pixel : (c_fonts::verdana_regular ? c_fonts::verdana_regular : title_font);
    if (!title_font || !meta_font)
        return;

    const float title_font_size = title_font->LegacySize - 1.0f;
    const float meta_font_size = meta_font->LegacySize;
    const ImVec2 display_size = get_display_size();
    if (display_size.x <= 0.0f || display_size.y <= 0.0f)
        return;

    const cache::player_state& player = *target_player;
    const std::string display_label = ellipsize_text(title_font, title_font_size, sanitize_name_label(player.display_name, player.name), 108.0f);
    const std::string username = player.name;
    const bool show_username = !username.empty() && display_label != username;

    std::vector<std::string> status_labels;
    if (player.body_effects.knocked)
        status_labels.emplace_back("Knocked");
    if (player.body_effects.reload)
        status_labels.emplace_back("Reloading");
    if (player.body_effects.grabbed)
        status_labels.emplace_back("Grabbed");

    std::optional<float> distance_to_local;
    const auto local = cache::localplayer ? cache::localplayer->snapshot() : cache::local_player_state{};
    if (const auto local_root_pos = get_part_position(local.parts.humanoid_root_part))
    {
        if (const auto target_root_pos = get_part_position(player.parts.humanoid_root_part))
            distance_to_local = (*target_root_pos - *local_root_pos).Length();
    }

    constexpr float panel_width = 156.0f;
    constexpr float padding = 7.0f;
    constexpr float row_gap = 3.0f;
    constexpr float bar_height = 3.0f;
    constexpr float chip_gap = 3.0f;
    constexpr float chip_pad_x = 4.0f;
    constexpr float chip_pad_y = 1.0f;
    constexpr float base_clearance = 22.0f;

    const float max_health = player.max_health > 1.0f ? player.max_health : 1.0f;
    const float current_health = std::clamp(player.health, 0.0f, max_health);
    const float health_ratio = current_health / max_health;
    const int armor_value = std::clamp(player.body_effects.armor, 0, 100);
    const bool show_armor = armor_value > 0;

    std::ostringstream health_text_stream;
    health_text_stream << static_cast<int>(std::lround(current_health)) << "/" << static_cast<int>(std::lround(max_health));
    const std::string health_text = health_text_stream.str();

    std::string distance_text;
    if (distance_to_local)
    {
        std::ostringstream distance_stream;
        distance_stream << std::fixed << std::setprecision(0) << *distance_to_local << "m";
        distance_text = distance_stream.str();
    }

    const ImVec2 display_text_size = title_font->CalcTextSizeA(title_font_size, FLT_MAX, 0.0f, display_label.c_str());
    const ImVec2 username_text_size = show_username
        ? meta_font->CalcTextSizeA(meta_font_size, FLT_MAX, 0.0f, username.c_str())
        : ImVec2(0.0f, 0.0f);
    const ImVec2 health_value_size = meta_font->CalcTextSizeA(meta_font_size, FLT_MAX, 0.0f, health_text.c_str());
    const ImVec2 distance_text_size = !distance_text.empty()
        ? meta_font->CalcTextSizeA(meta_font_size, FLT_MAX, 0.0f, distance_text.c_str())
        : ImVec2(0.0f, 0.0f);

    float chip_row_height = 0.0f;
    if (!status_labels.empty())
    {
        for (const auto& label : status_labels)
        {
            const ImVec2 chip_text_size = meta_font->CalcTextSizeA(meta_font_size, FLT_MAX, 0.0f, label.c_str());
            chip_row_height = (std::max)(chip_row_height, chip_text_size.y + chip_pad_y * 2.0f);
        }
    }

    float content_height = display_text_size.y + row_gap + bar_height;
    if (show_username)
        content_height += row_gap + username_text_size.y;
    if (show_armor)
        content_height += row_gap + bar_height;
    if (!status_labels.empty())
        content_height += row_gap + chip_row_height;

    const float panel_height = padding * 2.0f + content_height;
    const ImVec2 panel_size(panel_width, panel_height);
    const float gap = std::clamp(features->target_hud_offset, 16.0f, 120.0f) + base_clearance;
    const ImVec2 panel_pos = compute_panel_position(
        *anchor_screen,
        panel_size,
        features->target_hud_anchor,
        gap,
        display_size);

    draw_panel_background(draw, panel_pos, panel_size, c_colors::widget_rounding);

    float cursor_y = panel_pos.y + padding;
    const float content_x = panel_pos.x + padding;
    const float content_right = panel_pos.x + panel_width - padding;
    const float content_width = content_right - content_x;

    draw->AddText(title_font, title_font_size, ImVec2(content_x, cursor_y), ImGui::GetColorU32(c_colors::top_accent_color), display_label.c_str());
    if (!distance_text.empty())
    {
        draw->AddText(
            meta_font,
            meta_font_size,
            ImVec2(content_right - distance_text_size.x, cursor_y + (display_text_size.y - distance_text_size.y) * 0.5f),
            ImGui::GetColorU32(c_colors::text_muted),
            distance_text.c_str());
    }
    cursor_y += display_text_size.y;

    if (show_username)
    {
        cursor_y += row_gap;
        draw->AddText(
            meta_font,
            meta_font_size,
            ImVec2(content_x, cursor_y),
            ImGui::GetColorU32(c_colors::text_muted),
            ellipsize_text(meta_font, meta_font_size, username, content_width).c_str());
        cursor_y += username_text_size.y;
    }

    cursor_y += row_gap;
    const ImVec4 hp_color = health_color(health_ratio);
    const ImVec4 hp_color_dark = ImVec4(hp_color.x * 0.55f, hp_color.y * 0.55f, hp_color.z * 0.55f, hp_color.w);
    constexpr float hp_gap = 4.0f;
    const float bar_width = (std::max)(24.0f, content_width - health_value_size.x - hp_gap);
    draw_fill_bar(draw, health_ratio, ImVec2(content_x, cursor_y), ImVec2(bar_width, bar_height), hp_color, hp_color_dark);
    draw->AddText(
        meta_font,
        meta_font_size,
        ImVec2(content_x + bar_width + hp_gap, cursor_y - 1.0f),
        ImGui::GetColorU32(c_colors::white),
        health_text.c_str());
    cursor_y += bar_height;

    if (show_armor)
    {
        cursor_y += row_gap;
        const float armor_ratio = static_cast<float>(armor_value) / 100.0f;
        draw_fill_bar(
            draw,
            armor_ratio,
            ImVec2(content_x, cursor_y),
            ImVec2(content_width, bar_height),
            ImVec4(0.35f, 0.75f, 1.0f, 1.0f),
            ImVec4(0.12f, 0.32f, 0.65f, 1.0f));
        cursor_y += bar_height;
    }

    if (!status_labels.empty())
    {
        cursor_y += row_gap;
        float chip_x = content_x;
        for (const auto& label : status_labels)
        {
            const ImVec2 chip_text_size = meta_font->CalcTextSizeA(meta_font_size, FLT_MAX, 0.0f, label.c_str());
            const float chip_w = chip_text_size.x + chip_pad_x * 2.0f;
            const float chip_h = chip_text_size.y + chip_pad_y * 2.0f;
            const ImVec2 chip_min(chip_x, cursor_y);
            const ImVec2 chip_max(chip_x + chip_w, cursor_y + chip_h);
            draw->AddRectFilled(chip_min, chip_max, ImGui::GetColorU32(c_colors::accent_soft), 2.0f);
            draw->AddText(meta_font, meta_font_size, ImVec2(chip_x + chip_pad_x, cursor_y + chip_pad_y), ImGui::GetColorU32(c_colors::text_muted), label.c_str());
            chip_x += chip_w + chip_gap;
        }
    }
}
