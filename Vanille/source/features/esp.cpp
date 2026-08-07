#include "features/esp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <string>
#include <sstream>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "clipper2/include/clipper.h"
#include "clipper2/include/earcut.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <Windows.h>
#pragma warning(push)
#pragma warning(disable: 4005)
#include <d3d11.h>
#include <d3dcompiler.h>
#pragma warning(pop)
#include <wrl/client.h>

#include "cache/local_player_cache.h"
#include "cache/player_cache.h"
#include "cache/team_utils.h"
#include "cache/dead_body_cache.h"
#include "utils/logger.h"
#include "utils/debug_diag.h"
#include "globals/globals_fixed.h"
#include "gui/resources/fonts.h"
#include "sdk/camera.h"
#include "sdk/engine.h"
#include "sdk/mesh_part.h"
#include "sdk/humanoid.h"
#include "sdk/part.h"
#include "gui/overlay.hpp"
#include "features/aimbot.h"
#include "features/free_aim.h"
#include "features/triggerbot.h"
#include "features/visibility.h"

namespace ImGui
{
    IMGUI_API void SetTextOutlineEnabled(bool enabled);
    IMGUI_API bool IsTextOutlineEnabled();
}

namespace vanille::overlay
{
    void request_avatar_texture(std::uint64_t user_id);
    ImTextureID get_avatar_texture(std::uint64_t user_id, int* out_width, int* out_height);
}

namespace
{
    static float ease_expo(float t, float start, float duration)
    {
        if (duration <= 0.0f)
        {
            return 1.0f;
        }
        const float normalized = (t - start) / duration;
        if (normalized <= 0.0f)
        {
            return 0.0f;
        }
        if (normalized >= 1.0f)
        {
            return 1.0f;
        }
        return 1.0f - std::exp(-5.0f * normalized);
    }

    static ImVec2 round_point(const ImVec2& point)
    {
        return ImVec2(IM_ROUND(point.x), IM_ROUND(point.y));
    }

    static bool get_crosshair_mouse_position(ImVec2& out_position)
    {
        if (!vanille::overlay::g_rbx_window)
        {
            return false;
        }

        POINT screen_point{};
        if (!GetCursorPos(&screen_point))
        {
            return false;
        }

        POINT client_point = screen_point;
        if (!ScreenToClient(vanille::overlay::g_rbx_window, &client_point))
        {
            return false;
        }

        RECT client_rect{};
        if (!GetClientRect(vanille::overlay::g_rbx_window, &client_rect))
        {
            return false;
        }

        if (client_point.x < client_rect.left || client_point.y < client_rect.top || client_point.x >= client_rect.right || client_point.y >= client_rect.bottom)
        {
            return false;
        }

        out_position = ImVec2(static_cast<float>(client_point.x), static_cast<float>(client_point.y));
        return true;
    }

    static float smooth_wave_blend(float value)
    {
        value = std::clamp(value, 0.0f, 1.0f);
        value = value * value * (3.0f - 2.0f * value);
        value = value * value * (3.0f - 2.0f * value);
        return value;
    }

    static ImVec4 get_crosshair_wave_color(const ImVec4& color_a, const ImVec4& color_b, float phase)
    {
        const float primary_wave = 0.5f + 0.5f * std::sin(phase * IM_PI * 2.0f);
        const float secondary_wave = 0.5f + 0.5f * std::sin((phase * 0.5f + 0.35f) * IM_PI * 2.0f);
        const float mixed_wave = std::clamp(primary_wave * 0.82f + secondary_wave * 0.18f, 0.0f, 1.0f);
        const float blend = smooth_wave_blend(mixed_wave);
        ImVec4 out_color = ImLerp(color_a, color_b, blend);
        out_color.w = std::clamp(out_color.w, 0.0f, 1.0f);
        return out_color;
    }

    static void draw_crosshair_segment(
        ImDrawList* draw_list,
        const ImVec2& segment_start,
        const ImVec2& segment_end,
        float line_width,
        ImU32 outline_color,
        const ImVec4& fill_color,
        const ImVec4& animated_color_a,
        const ImVec4& animated_color_b,
        bool animated_fill,
        float wave_time)
    {
        if (!draw_list || line_width <= 0.0f)
        {
            return;
        }

        const ImVec2 rounded_start = round_point(segment_start);
        const ImVec2 rounded_end = round_point(segment_end);
        const ImVec2 delta(rounded_end.x - rounded_start.x, rounded_end.y - rounded_start.y);
        const float length_sq = delta.x * delta.x + delta.y * delta.y;
        if (!std::isfinite(length_sq) || length_sq <= 1e-6f)
        {
            return;
        }

        const float length = std::sqrt(length_sq);
        const float inv_length = 1.0f / length;
        const ImVec2 direction(delta.x * inv_length, delta.y * inv_length);
        const ImVec2 normal(-direction.y, direction.x);

        auto draw_segment_quad = [&](const ImVec2& quad_start, const ImVec2& quad_end, float half_width, float extend, ImU32 color)
            {
                const ImVec2 ext(direction.x * extend, direction.y * extend);
                const ImVec2 nrm(normal.x * half_width, normal.y * half_width);

                ImVec2 points[4] = {
                    ImVec2(quad_start.x - ext.x + nrm.x, quad_start.y - ext.y + nrm.y),
                    ImVec2(quad_end.x + ext.x + nrm.x, quad_end.y + ext.y + nrm.y),
                    ImVec2(quad_end.x + ext.x - nrm.x, quad_end.y + ext.y - nrm.y),
                    ImVec2(quad_start.x - ext.x - nrm.x, quad_start.y - ext.y - nrm.y)
                };
                draw_list->AddConvexPolyFilled(points, 4, color);
            };

        const float half_fill_width = line_width * 0.5f;
        const float half_outline_width = half_fill_width + 1.0f;
        draw_segment_quad(rounded_start, rounded_end, half_outline_width, 1.0f, outline_color);

        if (!animated_fill)
        {
            draw_segment_quad(rounded_start, rounded_end, half_fill_width, 0.0f, ImGui::GetColorU32(fill_color));
            return;
        }

        auto draw_segment_quad_gradient = [&](const ImVec2& quad_start, const ImVec2& quad_end, float half_width, float extend)
        {
            const ImVec2 ext(direction.x * extend, direction.y * extend);
            const ImVec2 nrm(normal.x * half_width, normal.y * half_width);
            const ImVec2 p0(quad_start.x - ext.x + nrm.x, quad_start.y - ext.y + nrm.y);
            const ImVec2 p1(quad_end.x + ext.x + nrm.x, quad_end.y + ext.y + nrm.y);
            const ImVec2 p2(quad_end.x + ext.x - nrm.x, quad_end.y + ext.y - nrm.y);
            const ImVec2 p3(quad_start.x - ext.x - nrm.x, quad_start.y - ext.y - nrm.y);

            const float wave_center = 0.5f + 0.18f * std::sin(wave_time * IM_PI * 2.0f);
            const float wave_fade = 0.17f;
            const float diagonal_skew = 0.22f;

            auto sample_color = [&](const ImVec2& point) -> ImU32
                {
                    const ImVec2 rel(point.x - rounded_start.x, point.y - rounded_start.y);
                    const float along = (rel.x * direction.x + rel.y * direction.y) * inv_length;
                    float across = 0.0f;
                    if (half_width > 1e-4f)
                    {
                        across = (rel.x * normal.x + rel.y * normal.y) / half_width;
                    }

                    const float gradient_pos = along + across * diagonal_skew;
                    const float low = wave_center - wave_fade;
                    const float high = wave_center + wave_fade;
                    const float blend = smooth_wave_blend((gradient_pos - low) / (high - low));
                    return ImGui::GetColorU32(ImLerp(animated_color_a, animated_color_b, blend));
                };

            const ImU32 c0 = sample_color(p0);
            const ImU32 c1 = sample_color(p1);
            const ImU32 c2 = sample_color(p2);
            const ImU32 c3 = sample_color(p3);

            const ImVec2 uv = draw_list->_Data->TexUvWhitePixel;
            draw_list->PrimReserve(6, 4);
            const ImDrawIdx idx = static_cast<ImDrawIdx>(draw_list->_VtxCurrentIdx);
            draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 0));
            draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 1));
            draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 2));
            draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 0));
            draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 2));
            draw_list->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 3));
            draw_list->PrimWriteVtx(p0, uv, c0);
            draw_list->PrimWriteVtx(p1, uv, c1);
            draw_list->PrimWriteVtx(p2, uv, c2);
            draw_list->PrimWriteVtx(p3, uv, c3);
        };

        draw_segment_quad_gradient(rounded_start, rounded_end, half_fill_width, 0.0f);
    }

    static bool crosshair_has_smoothed_position = false;
    static ImVec2 crosshair_smoothed_position = ImVec2(0.0f, 0.0f);
    static float crosshair_gap_scale = 1.0f;

    static void draw_outlined_rectangle(const ImVec2& position, const ImVec2& size, const ImU32 color, float rounding = 0.0f)
    {
        const ImVec2 rounded_position(std::round(position.x), std::round(position.y));
        const ImVec2 rounded_size(std::round(size.x), std::round(size.y));

        auto draw = ImGui::GetBackgroundDrawList();
        ImVec2 rect_max = ImVec2(rounded_position.x + rounded_size.x, rounded_position.y + rounded_size.y);
        ImRect rectangle(rounded_position, rect_max);

        const float max_rounding = (std::min)(rounded_size.x, rounded_size.y) / 2.0f;
        rounding = (std::min)(rounding, max_rounding);

        const int outline_alpha = static_cast<int>(((color >> 24) & 0xFF) * 0.5f);
        draw->AddRect(rectangle.Min, rectangle.Max, IM_COL32(15, 15, 15, outline_alpha), rounding);
        draw->AddRect(ImVec2(rectangle.Min.x - 2.0f, rectangle.Min.y - 2.0f), ImVec2(rectangle.Max.x + 2.0f, rectangle.Max.y + 2.0f), IM_COL32(15, 15, 15, outline_alpha), rounding);
        draw->AddRect(ImVec2(rectangle.Min.x - 1.0f, rectangle.Min.y - 1.0f), ImVec2(rectangle.Max.x + 1.0f, rectangle.Max.y + 1.0f), color, rounding);
    }

    static bool is_valid_box(const ImVec2& size, float min_size = 2.0f, float max_size = 5000.0f)
    {
        return std::isfinite(size.x) && std::isfinite(size.y) && size.x >= min_size && size.y >= min_size && size.x <= max_size && size.y <= max_size;
    }

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

    static void draw_corner_box(const ImVec2& position, const ImVec2& size, const ImU32 color)
    {
        auto draw = ImGui::GetBackgroundDrawList();
        if (!draw || !is_valid_box(size, 4.0f))
        {
            return;
        }

        const float min_x = std::round(position.x);
        const float min_y = std::round(position.y);
        const float max_x = std::round(position.x + size.x);
        const float max_y = std::round(position.y + size.y);

        const float width = max_x - min_x;
        const float height = max_y - min_y;
        if (width <= 0.0f || height <= 0.0f)
        {
            return;
        }

        const float stroke = 1.0f;
        const float border = 1.0f;
        const float accent = 1.0f;
        const float min_side = (std::min)(width, height);
        const float len = std::clamp(min_side * 0.30f, stroke * 2.0f, min_side);

        const int outline_alpha = static_cast<int>(((color >> 24) & 0xFF) * 1.0f);
        const ImU32 accent_color = IM_COL32(0, 0, 0, outline_alpha);
        const ImU32 border_color = accent_color;

        auto draw_horizontal = [&](float x, float y, bool inside_down)
            {
                const float white_y0 = inside_down ? y : y - stroke;
                const float white_y1 = white_y0 + stroke;

                const float top_y0 = white_y0 - border;
                const float top_y1 = white_y0;
                const float bot_y0 = white_y1;
                const float bot_y1 = white_y1 + border;

                const float left_x0 = x - border;
                const float left_x1 = x;
                const float right_x0 = x + stroke;
                const float right_x1 = x + stroke + border;

                const float body_x0 = x;
                const float body_x1 = x + len;
                const float cap_x0 = body_x1;
                const float cap_x1 = body_x1 + accent;

                draw->AddRectFilled(ImVec2(body_x0 - border, top_y0), ImVec2(body_x1 + border, top_y1), border_color);
                draw->AddRectFilled(ImVec2(body_x0 - border, bot_y0), ImVec2(body_x1 + border, bot_y1), border_color);
                draw->AddRectFilled(ImVec2(left_x0, white_y0), ImVec2(left_x1, white_y1), border_color);
                draw->AddRectFilled(ImVec2(right_x0, white_y0), ImVec2(right_x1, white_y1), border_color);

                draw->AddRectFilled(ImVec2(cap_x0, white_y0), ImVec2(cap_x1, white_y1), accent_color);
                ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(body_x0, white_y0), ImVec2(body_x1, white_y1), color);
            };

        auto draw_vertical = [&](float x, float y, bool inside_right)
            {
                const float white_x0 = inside_right ? x : x - stroke;
                const float white_x1 = white_x0 + stroke;

                const float left_x0 = white_x0 - border;
                const float left_x1 = white_x0;
                const float right_x0 = white_x1;
                const float right_x1 = white_x1 + border;

                const float body_y0 = y;
                const float body_y1 = y + len;
                const float cap_y0 = body_y1;
                const float cap_y1 = body_y1 + accent;

                const float top_y0 = body_y0 - border;
                const float top_y1 = body_y0;
                const float bot_y0 = body_y1;
                const float bot_y1 = body_y1 + border;

                draw->AddRectFilled(ImVec2(left_x0, body_y0 - border), ImVec2(left_x1, body_y1 + border), border_color);
                draw->AddRectFilled(ImVec2(right_x0, body_y0 - border), ImVec2(right_x1, body_y1 + border), border_color);
                draw->AddRectFilled(ImVec2(white_x0, top_y0), ImVec2(white_x1, top_y1), border_color);
                draw->AddRectFilled(ImVec2(white_x0, bot_y0), ImVec2(white_x1, bot_y1), border_color);

                draw->AddRectFilled(ImVec2(white_x0, cap_y0), ImVec2(white_x1, cap_y0 + accent), accent_color);

                ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(white_x0, body_y0), ImVec2(white_x1, body_y1), color);
            };

        draw_horizontal(min_x, min_y, true);
        draw_vertical(min_x, min_y, true);

        draw_horizontal(max_x - len - accent, min_y, true);
        draw_vertical(max_x, min_y, false);

        draw_horizontal(min_x, max_y, false);
        draw_vertical(min_x, max_y - len - accent, true);

        draw_horizontal(max_x - len - accent, max_y, false);
        draw_vertical(max_x, max_y - len - accent, false);
        draw->AddRectFilled(ImVec2(max_x - border, max_y - border), ImVec2(max_x, max_y), border_color);
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(max_x - stroke, max_y - stroke), ImVec2(max_x, max_y), color);
        draw->AddRectFilled(ImVec2(max_x, max_y - border), ImVec2(max_x + border, max_y + 1.0f), border_color);

        draw->AddRectFilled(ImVec2(min_x - border, min_y - border), ImVec2(min_x, min_y), border_color);
    }

    static ImVec4 adjust_alpha(const ImVec4& color, float factor)
    {
        ImVec4 adjusted = color;
        adjusted.w = std::clamp(color.w * factor, 0.0f, 1.0f);
        return adjusted;
    }

    static ImU32 adjust_alpha(ImU32 color, float factor)
    {
        ImVec4 color_vec = ImGui::ColorConvertU32ToFloat4(color);
        color_vec = adjust_alpha(color_vec, factor);
        return ImGui::ColorConvertFloat4ToU32(color_vec);
    }

    static ImVec4 apply_hit_flash(ImVec4 color, float factor)
    {
        const float t = std::clamp(factor, 0.0f, 1.0f);
        color.w = std::clamp(color.w + 0.5f * t, 0.0f, 1.0f);
        return color;
    }

    static void draw_segmented_line(ImDrawList* draw, const ImVec2& start, const ImVec2& end, ImU32 color, float thickness, float segment_length, float gap_length, float phase = 0.0f)
    {
        if (!draw || thickness <= 0.0f)
        {
            return;
        }

        const ImVec2 delta(end.x - start.x, end.y - start.y);
        const float length_sq = delta.x * delta.x + delta.y * delta.y;
        if (!std::isfinite(length_sq) || length_sq <= 1e-6f)
        {
            return;
        }

        const float length = std::sqrt(length_sq);
        const float inv_len = 1.0f / length;
        const ImVec2 dir(delta.x * inv_len, delta.y * inv_len);

        segment_length = std::clamp(segment_length, 0.1f, 1000.0f);
        gap_length = std::clamp(gap_length, 0.0f, 1000.0f);
        const float cycle = segment_length + gap_length;
        if (cycle <= 1e-6f)
        {
            draw->AddLine(start, end, color, thickness);
            return;
        }

        float offset = std::fmod(phase, cycle);
        if (offset < 0.0f)
        {
            offset += cycle;
        }

        for (float dist = -offset; dist < length; dist += cycle)
        {
            const float seg_start = (std::max)(0.0f, dist);
            const float seg_end = (std::min)(length, dist + segment_length);
            if (seg_end <= seg_start)
            {
                continue;
            }

            const ImVec2 p0(start.x + dir.x * seg_start, start.y + dir.y * seg_start);
            const ImVec2 p1(start.x + dir.x * seg_end, start.y + dir.y * seg_end);
            draw->AddLine(p0, p1, color, thickness);
        }
    }

    static void draw_styled_target_snapline(ImDrawList* draw, const ImVec2& start, const ImVec2& end, ImU32 color, float thickness, int style)
    {
        if (!draw || thickness <= 0.0f)
        {
            return;
        }

        style = std::clamp(style, 0, 2);
        if (style == 0)
        {
            draw->AddLine(start, end, color, thickness);
            return;
        }

        if (style == 1)
        {
            draw_segmented_line(draw, start, end, color, thickness, 2.0f, 5.0f, 0.0f);
            return;
        }

        const float phase = static_cast<float>(ImGui::GetTime()) * 180.0f;
        draw_segmented_line(draw, start, end, color, thickness, 14.0f, 8.0f, phase);
    }

    static void draw_target_snapline(ImDrawList* draw, const ImVec2& start, const ImVec2& end, float alpha_factor)
    {
        if (!draw)
        {
            return;
        }

        const int style = std::clamp(features->target_snapline_style, 0, 2);
        const float width = std::clamp(features->target_snapline_width, 0.5f, 8.0f);
        const ImVec4 line_color = adjust_alpha(features->target_snapline_color, alpha_factor);
        if (line_color.w <= 0.001f)
        {
            return;
        }

        if (features->target_snapline_outline)
        {
            const ImVec4 outline_color = adjust_alpha(features->target_snapline_outline_color, alpha_factor);
            if (outline_color.w > 0.001f)
            {
                draw_styled_target_snapline(draw, start, end, ImGui::GetColorU32(outline_color), width + 2.0f, style);
            }
        }

        draw_styled_target_snapline(draw, start, end, ImGui::GetColorU32(line_color), width, style);
    }

    static float lock_death_time = -1.0f;
    static std::uintptr_t last_locked_address = 0;
    static bool last_locked_alive = false;

    static void trigger_lock_death_image()
    {
        lock_death_time = 0.0f;
    }

    static float advance_lock_death_image()
    {
        if (lock_death_time < 0.0f)
        {
            return 0.0f;
        }

        lock_death_time += ImGui::GetIO().DeltaTime;

        constexpr float lifetime = 0.3f;
        constexpr float fade_in = 0.05f;
        constexpr float fade_out = 0.12f;

        if (lock_death_time >= lifetime)
        {
            lock_death_time = -1.0f;
            return 0.0f;
        }

        float alpha = 1.0f;
        if (lock_death_time < fade_in)
        {
            alpha = lock_death_time / fade_in;
        }
        else if (lock_death_time > lifetime - fade_out)
        {
            alpha = (lifetime - lock_death_time) / fade_out;
        }

        return std::clamp(alpha, 0.0f, 1.0f);
    }

    static void draw_lock_death_image(float alpha)
    {
        if (alpha <= 0.0f)
        {
            return;
        }

        alpha = std::clamp(alpha * 0.8f, 0.0f, 1.0f);

        ImTextureID tex = c_textures::death_image_custom ? c_textures::death_image_custom : c_textures::death_image;
        if (tex == 0)
        {
            return;
        }

        const ImVec2 tex_size = c_textures::death_image_custom ? c_textures::death_image_custom_size : c_textures::death_image_size;
        if (tex_size.x <= 0.0f || tex_size.y <= 0.0f)
        {
            return;
        }

        ImVec2 target_size = ImGui::GetIO().DisplaySize;
        if (vanille::overlay::g_rbx_window)
        {
            RECT rc{};
            if (::GetClientRect(vanille::overlay::g_rbx_window, &rc))
            {
                target_size = ImVec2(static_cast<float>(rc.right - rc.left), static_cast<float>(rc.bottom - rc.top));
            }
        }
        if (target_size.x <= 0.0f || target_size.y <= 0.0f)
        {
            return;
        }

        const ImVec2 pos(0.0f, 0.0f);

        if (ImDrawList* draw = ImGui::GetForegroundDrawList())
        {
            draw->AddImage(
                tex,
                pos,
                ImVec2(pos.x + target_size.x, pos.y + target_size.y),
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f),
                ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha))
            );
        }
    }

    struct hit_flash_state
    {
        float time = 0.0f;
    };

    static std::unordered_map<std::uintptr_t, hit_flash_state> hit_flash_map;

    static float advance_hit_flash(std::uintptr_t player_address)
    {
        auto it = hit_flash_map.find(player_address);
        if (it == hit_flash_map.end())
        {
            return 0.0f;
        }

        hit_flash_state& state = it->second;
        const float delta_time = ImGui::GetIO().DeltaTime;
        state.time += delta_time;

        constexpr float lifetime = 0.30f;
        constexpr float fade_in = 0.05f;
        constexpr float fade_out = 0.08f;

        if (state.time >= lifetime)
        {
            hit_flash_map.erase(it);
            return 0.0f;
        }

        float alpha = 1.0f;
        if (state.time < fade_in)
        {
            alpha = state.time / fade_in;
        }
        else if (state.time > lifetime - fade_out)
        {
            alpha = (lifetime - state.time) / fade_out;
        }

        return std::clamp(alpha, 0.0f, 1.0f);
    }

    static ImVec4 apply_visibility_tint(const ImVec4& color, const visibility::visibility_result& vis, bool enabled)
    {
        if (!enabled)
        {
            return color;
        }

        if (vis.visible)
        {
            return color;
        }

        ImVec4 red = features->occluded_color;
        ImVec4 out{};
        out.x = red.x;
        out.y = red.y;
        out.z = red.z;
        out.w = color.w;
        return out;
    }

    static ImVec4 dormant_tint_base()
    {
        if (features->override_dormant_color)
        {
            return features->dormant_color;
        }
        return ImVec4(1.0f, 0.35f, 0.8f, 1.0f);
    }

    static ImVec4 apply_dormant_tint(const ImVec4& color, bool is_dormant)
    {
        if (!is_dormant)
        {
            return color;
        }
        ImVec4 tinted = dormant_tint_base();
        tinted.w = color.w;
        return tinted;
    }

    static ImU32 apply_dormant_tint(ImU32 color, bool is_dormant)
    {
        if (!is_dormant)
        {
            return color;
        }
        ImVec4 base = ImGui::ColorConvertU32ToFloat4(color);
        base = apply_dormant_tint(base, true);
        return ImGui::ColorConvertFloat4ToU32(base);
    }

    static ImVec4 apply_host_tint(ImVec4 color, bool is_host, const ImVec4& host_color)
    {
        if (!is_host)
        {
            return color;
        }
        ImVec4 out = host_color;
        out.w = color.w;
        return out;
    }

    static bool should_outline(ImFont* font)
    {
        return !(font == c_fonts::verdana_regular || font == c_fonts::verdana_bold);
    }

    static void add_text(ImDrawList* draw, ImFont* font, float font_size, const ImVec2& pos, ImU32 color, const std::string& text, bool draw_outline = false)
    {
        if (!draw || text.empty())
        {
            return;
        }

        struct outline_toggle
        {
            bool previous = ImGui::IsTextOutlineEnabled();
            outline_toggle(bool enable)
            {
                if (enable != previous)
                {
                    ImGui::SetTextOutlineEnabled(enable);
                }
            }
            ~outline_toggle()
            {
                ImGui::SetTextOutlineEnabled(previous);
            }
        } toggle(draw_outline);

        draw->AddText(font, font_size, pos, color, text.c_str());
    }


    void draw_mesh_hull(const Clipper2Lib::PathD& hull, ImU32 fill_color, ImU32 outline_color)
    {
        if (hull.size() < 3)
        {
            return;
        }

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw)
        {
            return;
        }

        std::vector<std::vector<std::array<double, 2>>> polygon(1);
        polygon[0].reserve(hull.size());
        for (const auto& p : hull)
        {
            polygon[0].push_back({ p.x, p.y });
        }

        const auto indices = mapbox::earcut<uint32_t>(polygon);
        draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            const auto& a = polygon[0][indices[i]];
            const auto& b = polygon[0][indices[i + 1]];
            const auto& c = polygon[0][indices[i + 2]];
            draw->AddTriangleFilled(
                ImVec2(static_cast<float>(a[0]), static_cast<float>(a[1])),
                ImVec2(static_cast<float>(b[0]), static_cast<float>(b[1])),
                ImVec2(static_cast<float>(c[0]), static_cast<float>(c[1])),
                fill_color);
        }
        draw->Flags |= ImDrawListFlags_AntiAliasedFill;

        if (hull.size() >= 2)
        {
            std::vector<ImVec2> pts;
            pts.reserve(hull.size());
            for (const auto& p : hull)
            {
                pts.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
            }
            draw->AddPolyline(pts.data(), static_cast<int>(pts.size()), outline_color, true, 1.6f);
        }
    }

    static void draw_health_bar(float max_health, float current_health, const ImVec2& position, const ImVec2& size, float alpha_factor, float bar_width = 2.0f, ImU32 override_color = 0)
    {
        if (max_health <= 0.0f || current_health <= 0.0f)
        {
            return;
        }

        auto draw = ImGui::GetBackgroundDrawList();
        if (!draw)
        {
            return;
        }

        const int padding_top = 1;
        const int padding_bottom = 1;
        const float clamped_health = std::clamp(current_health, 0.0f, max_health);
        const float fill_ratio = clamped_health / max_health;
        const float full_height = std::round(size.y) + padding_top + padding_bottom;
        const float bar_height = std::round(full_height * fill_ratio);
        const float bar_x = std::round(position.x + size.x - bar_width);
        const float bar_top = std::round(position.y - static_cast<float>(padding_top));
        const float bar_bottom = bar_top + full_height;

        ImVec2 bar_background_top = ImVec2(bar_x, bar_top);
        ImVec2 bar_background_bottom = ImVec2(bar_x + bar_width, bar_bottom);
        ImVec2 bar_foreground_top = ImVec2(bar_x, bar_bottom - bar_height);
        ImVec2 bar_foreground_bottom = bar_background_bottom;

        const ImU32 border_color = adjust_alpha(IM_COL32(0, 0, 0, 128), alpha_factor);
        const ImU32 background_color = adjust_alpha(IM_COL32(75, 75, 75, 255), alpha_factor);

        draw->AddRectFilled(ImVec2(bar_background_top.x - 1.0f, bar_background_top.y - 1.0f), ImVec2(bar_background_bottom.x + 1.0f, bar_background_bottom.y + 1.0f), border_color);
        draw->AddRectFilled(bar_background_top, bar_background_bottom, background_color);

        const float ratio = (max_health > 0.0f) ? (clamped_health / max_health) : 0.0f;
        ImVec4 top_color_vec;
        ImVec4 bottom_color_vec;
        const int color_mode = std::clamp(features->healthbar_color_mode, 0, 1);
        if (override_color != 0)
        {
            const ImVec4 override_vec = ImGui::ColorConvertU32ToFloat4(override_color);
            top_color_vec = bottom_color_vec = override_vec;
        }
        else if (color_mode == 0)
        {
            top_color_vec = features->healthbar_top_color;
            bottom_color_vec = features->healthbar_bottom_color;
        }
        else if (ratio >= 0.75f)
        {
            top_color_vec = ImVec4(0.25f, 0.86f, 0.25f, 1.0f);
            bottom_color_vec = ImVec4(0.075f, 0.65f, 0.075f, 1.0f);
        }
        else if (ratio >= 0.25f)
        {
            top_color_vec = ImVec4(1.0f, 0.60f, 0.15f, 1.0f);
            bottom_color_vec = ImVec4(0.80f, 0.35f, 0.05f, 1.0f);
        }
        else
        {
            top_color_vec = ImVec4(0.86f, 0.25f, 0.25f, 1.0f);
            bottom_color_vec = ImVec4(0.60f, 0.10f, 0.10f, 1.0f);
        }

        const ImVec4 high_color = adjust_alpha(top_color_vec, alpha_factor);
        const ImVec4 low_color = adjust_alpha(bottom_color_vec, alpha_factor);

        const ImU32 top_color = ImGui::GetColorU32(high_color);
        const ImU32 bottom_color = ImGui::GetColorU32(low_color);

        draw->AddRectFilledMultiColor(bar_foreground_top, bar_foreground_bottom, top_color, top_color, bottom_color, bottom_color);
    }

    static void draw_armor_bar(float max_armor, float current_armor, const ImVec2& position, const ImVec2& size, float alpha_factor, ImU32 override_color = 0)
    {
        if (max_armor <= 0.0f || current_armor <= 0.0f)
        {
            return;
        }

        auto draw = ImGui::GetBackgroundDrawList();
        if (!draw)
        {
            return;
        }

        const ImVec2 rounded_position(std::round(position.x), std::round(position.y));
        const ImVec2 rounded_size(std::round(size.x), std::round(size.y));
        if (rounded_size.x <= 0.0f || rounded_size.y <= 0.0f)
        {
            return;
        }

        const float clamped_armor = std::clamp(current_armor, 0.0f, max_armor);
        const float fill_ratio = clamped_armor / max_armor;
        const float bar_height = rounded_size.y;
        const float bar_width = rounded_size.x;
        const float filled_width = std::clamp(std::round(bar_width * fill_ratio), 0.0f, bar_width);

        ImVec2 bar_min = rounded_position;
        ImVec2 bar_max = ImVec2(rounded_position.x + bar_width, rounded_position.y + bar_height);
        ImVec2 filled_max = ImVec2(rounded_position.x + filled_width, rounded_position.y + bar_height);

        const ImU32 border_color = adjust_alpha(IM_COL32(0, 0, 0, 128), alpha_factor);
        const ImU32 background_color = adjust_alpha(IM_COL32(60, 60, 60, 200), alpha_factor);

        draw->AddRectFilled(ImVec2(bar_min.x - 1.0f, bar_min.y - 1.0f), ImVec2(bar_max.x + 1.0f, bar_max.y + 1.0f), border_color, 0.0f);
        draw->AddRectFilled(bar_min, bar_max, background_color, 0.0f);

        ImVec4 top_color_vec = ImVec4(0.35f, 0.75f, 1.0f, 1.0f);
        ImVec4 bottom_color_vec = ImVec4(0.12f, 0.32f, 0.65f, 1.0f);

        if (override_color != 0)
        {
            const ImVec4 override_vec = ImGui::ColorConvertU32ToFloat4(override_color);
            top_color_vec = bottom_color_vec = override_vec;
        }

        const ImVec4 high_color = adjust_alpha(top_color_vec, alpha_factor);
        const ImVec4 low_color = adjust_alpha(bottom_color_vec, alpha_factor);

        const ImU32 left_color = ImGui::GetColorU32(high_color);
        const ImU32 right_color = ImGui::GetColorU32(low_color);

        draw->AddRectFilledMultiColor(bar_min, filled_max, right_color, left_color, left_color, right_color);
    }

    static std::unordered_map<std::uintptr_t, float> displayed_health_map;
    static std::unordered_map<std::uintptr_t, float> last_health_map;
    static std::unordered_map<std::uintptr_t, float> displayed_armor_map;
    static std::unordered_map<std::uintptr_t, float> last_armor_map;
    static std::unordered_map<std::uintptr_t, float> last_jump_time_map;
    static std::uint64_t highlight_frame_id = 0;

    struct highlight_hull_cache_entry
    {
        rbx::Vector3 size{};
        rbx::mesh_part::transform transform{};
        rbx::Matrix view_matrix{};
        rbx::Vector2 dimensions{};
        Clipper2Lib::PathD hull{};
        std::uint64_t last_used_frame = 0;
        bool has_hull = false;
        bool valid = false;
    };

    static std::unordered_map<std::uintptr_t, highlight_hull_cache_entry> part_hull_cache;
    static std::unordered_map<std::uintptr_t, highlight_hull_cache_entry> mesh_hull_cache;
    struct mesh_sample_cache_entry
    {
        std::vector<rbx::Vector3> samples;
        rbx::Vector3 local_bounds_min{};
        rbx::Vector3 local_bounds_max{};
        std::uint64_t last_used_frame = 0;
        bool has_local_bounds = false;
        bool valid = false;
    };

    static std::unordered_map<std::uint64_t, mesh_sample_cache_entry> mesh_sample_cache;
    struct mesh_wireframe_edge
    {
        rbx::Vector3 a{};
        rbx::Vector3 b{};
    };
    struct mesh_wireframe_cache_entry
    {
        std::vector<mesh_wireframe_edge> edges;
        rbx::Vector3 local_bounds_min{};
        rbx::Vector3 local_bounds_max{};
        std::uint64_t last_used_frame = 0;
        bool has_local_bounds = false;
        bool valid = false;
    };
    struct projected_wireframe_segment
    {
        ImVec2 a{};
        ImVec2 b{};
    };
    static std::unordered_map<std::uint64_t, mesh_wireframe_cache_entry> mesh_wireframe_cache;
    using SimpleVector3 = DirectX::SimpleMath::Vector3;
    using SimpleMatrix = DirectX::SimpleMath::Matrix;
    struct mesh_material_cache_entry
    {
        std::vector<SimpleVector3> vertices;
        std::vector<std::uint32_t> indices;
        rbx::Vector3 local_bounds_min{};
        rbx::Vector3 local_bounds_max{};
        std::uint64_t last_used_frame = 0;
        bool has_local_bounds = false;
        bool valid = false;
    };
    struct metallic_mesh_draw_command
    {
        std::vector<SimpleVector3> world_vertices;
        std::vector<std::uint32_t> indices;
        ImVec4 color{};
        int material_mode = 0;
    };
    struct metallic_shader_constants
    {
        float view_proj[16]{};
        float color[4]{};
        float camera_pos[3]{};
        float time = 0.0f;
        float material_mode = 0.0f;
        float _padding[3]{};
    };
    struct metallic_shader_pipeline
    {
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> index_buffer;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_texture;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_stencil_view;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_state;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;
        UINT depth_width = 0;
        UINT depth_height = 0;
        std::size_t vertex_capacity = 0;
        std::size_t index_capacity = 0;
        bool initialized = false;
    };
    static constexpr int k_mesh_material_flat = 0;
    static constexpr int k_mesh_material_metallic = 1;
    static constexpr int k_mesh_material_monochrome = 2;
    static constexpr int k_mesh_material_acrylic = 3;
    static constexpr int k_mesh_material_glowing = 4;
    static std::unordered_map<std::uint64_t, mesh_material_cache_entry> mesh_material_cache;
    static std::vector<metallic_mesh_draw_command> metallic_mesh_draw_queue;
    static SimpleMatrix metallic_mesh_view_proj{};
    static SimpleVector3 metallic_mesh_camera_pos{};
    static bool has_metallic_mesh_view_proj = false;
    static std::uint64_t cached_r15_head_mesh_asset_id = 0;
    static metallic_shader_pipeline metallic_pipeline;

    struct health_delta_event
    {
        float value = 0.0f;
        float time = 0.0f;
    };

    static std::unordered_map<std::uintptr_t, health_delta_event> health_delta_map;
    static std::unordered_map<std::uintptr_t, health_delta_event> armor_delta_map;
    struct hit_log_entry
    {
        std::uintptr_t player_address = 0;
        std::string player_name;
        int total_damage = 0;
        int total_hits = 0;
        float age = 0.0f;
    };
    static std::deque<hit_log_entry> hit_log_entries;
    struct distance_anim_state
    {
        float current_y = 0.0f;
        float target_y = 0.0f;
        float start_y = 0.0f;
        float timer = 0.0f;
    };
    static std::unordered_map<std::uintptr_t, distance_anim_state> distance_anim_map;
    static std::unordered_set<std::uintptr_t> previous_addresses;
    static std::unordered_map<std::uint64_t, std::uintptr_t> previous_user_addresses;
    static std::unordered_map<std::uint64_t, std::uintptr_t> previous_user_characters;
    static std::unordered_map<std::string, std::uintptr_t> previous_name_characters;
    static std::unordered_map<std::uintptr_t, cache::player_state> last_player_snapshot;
    static std::unordered_map<std::uintptr_t, std::uintptr_t> last_character_seen;
    enum class player_relation
    {
        neutral,
        friendly,
        enemy
    };
    constexpr std::int64_t lostfront_place_id = 102871156420149;

    static player_relation get_manual_relation(const cache::player_state& player)
    {
        const int status_override = vanille::overlay::get_player_status(player.user_id, player.name);
        if (status_override == 1)
        {
            return player_relation::enemy;
        }
        if (status_override == 2)
        {
            return player_relation::friendly;
        }
        return player_relation::neutral;
    }

    static bool is_friendly_by_team(const cache::player_state& local, const cache::player_state& other)
    {
        return cache::team_utils::is_teammate(local, other);
    }

    static bool is_friendly_by_team(const cache::local_player_state& local, const cache::player_state& other)
    {
        return cache::team_utils::is_teammate(local, other);
    }

    static player_relation determine_relation(const cache::player_state& local, const cache::player_state& other)
    {
        if (local.address == 0 || other.address == 0 || other.address == local.address)
        {
            return player_relation::neutral;
        }

        return get_manual_relation(other);
    }

    static player_relation determine_relation(const cache::local_player_state& local, const cache::player_state& other)
    {
        if (local.address == 0 || other.address == 0 || other.address == local.address)
        {
            return player_relation::neutral;
        }

        return get_manual_relation(other);
    }

    static ImVec4 apply_relation_color(player_relation relation, ImVec4 base)
    {
        switch (relation)
        {
        case player_relation::enemy:
            base.x = 1.0f;
            base.y = 0.2f;
            base.z = 0.2f;
            return base;
        case player_relation::friendly:
            base.x = 0.2f;
            base.y = 1.0f;
            base.z = 0.2f;
            return base;
        default:
            return base;
        }
    }

    struct fade_state
    {
        float alpha = 0.0f;
        float start_alpha = 0.0f;
        float target_alpha = 1.0f;
        float timer = 0.0f;
        std::uintptr_t last_character = 0;
        bool pending_refade = false;
        std::uintptr_t pending_character = 0;
        bool initialized = false;
    };

    static std::unordered_map<std::uintptr_t, fade_state> fade_map;

    struct body_fade_state
    {
        float alpha = 0.0f;
        float start_alpha = 0.0f;
        float target_alpha = 1.0f;
        float timer = 0.0f;
        bool initialized = false;
    };

    static std::unordered_map<std::uintptr_t, body_fade_state> dead_body_fade_map;
    struct dead_body_render_snapshot
    {
        std::vector<Clipper2Lib::PathD> hulls;
        std::optional<ImRect> torso_bounds;
    };

    static std::unordered_map<std::uintptr_t, dead_body_render_snapshot> last_dead_body_snapshot;

    static void prune_inactive_entries(const std::vector<std::uintptr_t>& active_addresses)
    {
        std::unordered_set<std::uintptr_t> active_set(active_addresses.begin(), active_addresses.end());

        auto prune_map = [&](auto& map)
            {
                for (auto it = map.begin(); it != map.end();)
                {
                    if (!active_set.contains(it->first))
                    {
                        it = map.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            };

        prune_map(fade_map);
        prune_map(distance_anim_map);
        prune_map(last_health_map);
        prune_map(last_armor_map);
        prune_map(health_delta_map);
        prune_map(armor_delta_map);
        prune_map(hit_flash_map);
        prune_map(last_character_seen);
        prune_map(last_jump_time_map);
    }

    static void prune_dead_body_entries(const std::vector<std::uintptr_t>& active_addresses)
    {
        std::unordered_set<std::uintptr_t> active_set(active_addresses.begin(), active_addresses.end());

        for (auto it = dead_body_fade_map.begin(); it != dead_body_fade_map.end();)
        {
            if (!active_set.contains(it->first))
            {
                it = dead_body_fade_map.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = last_dead_body_snapshot.begin(); it != last_dead_body_snapshot.end();)
        {
            if (!active_set.contains(it->first))
            {
                it = last_dead_body_snapshot.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    static void reset_player_caches(std::uintptr_t address)
    {
        fade_map.erase(address);
        distance_anim_map.erase(address);
        displayed_health_map.erase(address);
        displayed_armor_map.erase(address);
        last_health_map.erase(address);
        last_armor_map.erase(address);
        health_delta_map.erase(address);
        armor_delta_map.erase(address);
        hit_flash_map.erase(address);
        last_character_seen.erase(address);
        last_jump_time_map.erase(address);
    }

    static void log_player_events(const std::vector<cache::player_state>& players)
    {
        std::unordered_set<std::uintptr_t> current_addresses;
        std::unordered_map<std::uint64_t, std::uintptr_t> current_user_addresses;
        std::unordered_map<std::uint64_t, std::uintptr_t> current_user_characters;
        std::unordered_map<std::string, std::uintptr_t> current_name_characters;

        for (const auto& player : players)
        {
            if (player.address == 0)
            {
                continue;
            }

            current_addresses.insert(player.address);
            if (player.user_id != 0)
            {
                current_user_addresses[player.user_id] = player.address;
                current_user_characters[player.user_id] = player.character.get_address();
            }
            else if (!player.name.empty())
            {
                current_name_characters[player.name] = player.character.get_address();
            }

            if (!previous_addresses.contains(player.address))
            {
                const std::string& name = player.name.empty() ? player.display_name : player.name;
                logger_core::log_info("player joined: {} (0x{:X})", name.empty() ? "unknown" : name, player.address);
            }

            if (player.user_id != 0)
            {
                auto it = previous_user_addresses.find(player.user_id);
                if (it != previous_user_addresses.end() && it->second != player.address)
                {
                    const std::string& name = player.name.empty() ? player.display_name : player.name;
                    logger_core::log_info("player respawned: {} address changed 0x{:X} -> 0x{:X}", name.empty() ? "unknown" : name, it->second, player.address);
                }

                auto it_char = previous_user_characters.find(player.user_id);
                const auto current_char = player.character.get_address();
                if (current_char != 0 && it_char != previous_user_characters.end() && it_char->second != current_char)
                {
                    const std::string& name = player.name.empty() ? player.display_name : player.name;
                    logger_core::log_info("player respawned: {} character changed 0x{:X} -> 0x{:X}", name.empty() ? "unknown" : name, it_char->second, current_char);
                }
            }
            else if (!player.name.empty())
            {
                auto it_char = previous_name_characters.find(player.name);
                const auto current_char = player.character.get_address();
                if (current_char != 0 && it_char != previous_name_characters.end() && it_char->second != current_char)
                {
                    logger_core::log_info("player respawned: {} character changed 0x{:X} -> 0x{:X}", player.name, it_char->second, current_char);
                }
            }
        }

        for (const auto& addr : previous_addresses)
        {
            if (!current_addresses.contains(addr))
            {
                logger_core::log_info("player left: 0x{:X}", addr);
            }
        }

        previous_addresses = std::move(current_addresses);
        previous_user_addresses = std::move(current_user_addresses);
        previous_user_characters = std::move(current_user_characters);
        previous_name_characters = std::move(current_name_characters);
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

    static void push_hit_log_event(std::uintptr_t player_address, const std::string& player_name, int damage)
    {
        if (player_address == 0 || damage <= 0)
        {
            return;
        }

        constexpr float merge_window = 2.0f;
        auto it = std::find_if(hit_log_entries.begin(), hit_log_entries.end(), [&](const hit_log_entry& entry)
            {
                return entry.player_address == player_address && entry.age <= merge_window;
            });

        if (it != hit_log_entries.end())
        {
            it->total_damage += damage;
            it->total_hits += 1;
            it->age = 0.0f;
            if (!player_name.empty())
            {
                it->player_name = player_name;
            }

            if (it != hit_log_entries.begin())
            {
                hit_log_entry updated = std::move(*it);
                hit_log_entries.erase(it);
                hit_log_entries.push_front(std::move(updated));
            }
        }
        else
        {
            hit_log_entry entry{};
            entry.player_address = player_address;
            entry.player_name = player_name;
            entry.total_damage = damage;
            entry.total_hits = 1;
            hit_log_entries.push_front(std::move(entry));
        }

        constexpr std::size_t max_entries = 6;
        while (hit_log_entries.size() > max_entries)
        {
            hit_log_entries.pop_back();
        }
    }

    static void render_hit_logs()
    {
        if (!features->enable_hit_logs)
        {
            hit_log_entries.clear();
            return;
        }

        if (hit_log_entries.empty())
        {
            return;
        }

        ImDrawList* draw = ImGui::GetForegroundDrawList();
        if (!draw)
        {
            return;
        }

        ImFont* font = c_fonts::proggy_tiny;
        if (!font)
        {
            return;
        }

        const ImVec2 display_size = ImGui::GetIO().DisplaySize;
        if (display_size.x <= 0.0f || display_size.y <= 0.0f)
        {
            return;
        }

        ImVec2 anchor_size = display_size;
        if (vanille::overlay::g_rbx_window && ::IsWindow(vanille::overlay::g_rbx_window))
        {
            RECT rc{};
            if (::GetClientRect(vanille::overlay::g_rbx_window, &rc))
            {
                const float w = static_cast<float>(rc.right - rc.left);
                const float h = static_cast<float>(rc.bottom - rc.top);
                if (w > 0.0f && h > 0.0f)
                {
                    anchor_size = ImVec2(w, h);
                }
            }
        }

        const float font_size = font->LegacySize;
        const float delta_time = ImGui::GetIO().DeltaTime;
        constexpr float lifetime = 5.5f;
        constexpr float fade_in = 0.12f;
        constexpr float fade_out = 0.4f;

        struct prepared_log_line
        {
            std::string text;
            ImVec2 size{};
            float alpha = 1.0f;
        };

        std::vector<prepared_log_line> lines;
        lines.reserve(hit_log_entries.size());

        for (auto it = hit_log_entries.begin(); it != hit_log_entries.end();)
        {
            it->age += delta_time;
            if (it->age >= lifetime)
            {
                it = hit_log_entries.erase(it);
                continue;
            }

            float alpha = 1.0f;
            if (it->age < fade_in)
            {
                alpha = it->age / fade_in;
            }
            else if (it->age > lifetime - fade_out)
            {
                alpha = (lifetime - it->age) / fade_out;
            }
            alpha = std::clamp(alpha, 0.0f, 1.0f);
            if (alpha <= 0.001f)
            {
                ++it;
                continue;
            }

            const std::string name = it->player_name.empty() ? std::string("unknown") : it->player_name;
            const char* hit_word = (it->total_hits == 1) ? "hit" : "hits";
            std::ostringstream text_stream;
            text_stream << "Damage Given to \"" << name << "\" - " << it->total_damage << " in " << it->total_hits << " " << hit_word;
            prepared_log_line line{};
            line.text = text_stream.str();
            line.size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, line.text.c_str());
            line.alpha = alpha;
            lines.push_back(std::move(line));
            ++it;
        }

        if (lines.empty())
        {
            return;
        }

        const int position = std::clamp(features->hit_log_position, 0, 4);
        constexpr float margin_x = 20.0f;
        constexpr float margin_y = 20.0f;
        constexpr float line_spacing = 1.0f;

        auto draw_line = [&](const prepared_log_line& line, float x, float y)
            {
                const ImVec4 shadow_color = ImVec4(0.0f, 0.0f, 0.0f, std::clamp(line.alpha * 0.8f, 0.0f, 1.0f));
                const ImVec4 text_color = ImVec4(1.0f, 1.0f, 1.0f, line.alpha);
                draw->AddText(font, font_size, ImVec2(x + 1.0f, y + 1.0f), ImGui::GetColorU32(shadow_color), line.text.c_str());
                draw->AddText(font, font_size, ImVec2(x, y), ImGui::GetColorU32(text_color), line.text.c_str());
            };

        if (position == 0 || position == 1 || position == 4)
        {
            float y = margin_y;
            if (position == 4)
            {
                y = anchor_size.y * 0.5f + 30.0f;
            }
            for (const auto& line : lines)
            {
                float x = margin_x;
                if (position == 1)
                {
                    x = display_size.x - margin_x - line.size.x;
                }
                else if (position == 4)
                {
                    x = (anchor_size.x - line.size.x) * 0.5f;
                }

                x = std::clamp(x, 0.0f, (std::max)(0.0f, display_size.x - line.size.x));
                y = std::clamp(y, 0.0f, (std::max)(0.0f, display_size.y - line.size.y));
                draw_line(line, x, y);
                y += line.size.y + line_spacing;
            }
            return;
        }

        float y = display_size.y - margin_y;
        for (const auto& line : lines)
        {
            y -= line.size.y;

            float x = margin_x;
            if (position == 3)
            {
                x = display_size.x - margin_x - line.size.x;
            }

            x = std::clamp(x, 0.0f, (std::max)(0.0f, display_size.x - line.size.x));
            y = std::clamp(y, 0.0f, (std::max)(0.0f, display_size.y - line.size.y));
            draw_line(line, x, y);
            y -= line_spacing;
        }
    }

    static float get_smoothed_health(std::uintptr_t player_address, float target_health)
    {
        const float clamped_target = (std::max)(0.0f, target_health);
        const float delta_time = ImGui::GetIO().DeltaTime;
        const float response_speed = 8.0f;

        auto it = displayed_health_map.find(player_address);
        if (it == displayed_health_map.end())
        {
            displayed_health_map[player_address] = clamped_target;
            return clamped_target;
        }
        float& displayed = it->second;

        const float t = 1.0f - std::exp(-response_speed * delta_time);
        displayed += (clamped_target - displayed) * t;

        return displayed;
    }

    static float register_health_delta(std::uintptr_t player_address, float current_health, bool track_delta_text, bool track_hit_flash)
    {
        float previous = 0.0f;
        bool has_previous = false;
        if (auto it = last_health_map.find(player_address); it != last_health_map.end())
        {
            previous = it->second;
            has_previous = true;
        }

        if (!track_delta_text)
        {
            health_delta_map.erase(player_address);
        }

        float delta = 0.0f;
        if (has_previous)
        {
            delta = current_health - previous;
            if (track_delta_text && std::fabs(delta) >= 1.0f)
            {
                health_delta_map[player_address] = { delta, 0.0f };
            }
            if (track_hit_flash && delta <= -1.0f)
            {
                hit_flash_map[player_address] = { 0.0f };
            }
        }

        last_health_map[player_address] = current_health;
        return delta;
    }

    static float get_smoothed_armor(std::uintptr_t player_address, float target_armor)
    {
        const float clamped_target = (std::max)(0.0f, target_armor);
        const float delta_time = ImGui::GetIO().DeltaTime;
        const float response_speed = 8.0f;

        auto it = displayed_armor_map.find(player_address);
        if (it == displayed_armor_map.end())
        {
            displayed_armor_map[player_address] = clamped_target;
            return clamped_target;
        }
        float& displayed = it->second;

        const float t = 1.0f - std::exp(-response_speed * delta_time);
        displayed += (clamped_target - displayed) * t;
        return displayed;
    }

    static void register_armor_delta(std::uintptr_t player_address, float current_armor)
    {
        if (current_armor <= 0.0f)
        {
            armor_delta_map.erase(player_address);
            displayed_armor_map[player_address] = 0.0f;
            last_armor_map[player_address] = 0.0f;
            return;
        }

        float previous = 0.0f;
        if (auto it = last_armor_map.find(player_address); it != last_armor_map.end())
        {
            previous = it->second;
        }
        else if (auto it_disp = displayed_armor_map.find(player_address); it_disp != displayed_armor_map.end())
        {
            previous = it_disp->second;
        }

        const float delta = current_armor - previous;
        if (std::fabs(delta) >= 0.25f)
        {
            armor_delta_map[player_address] = { delta, 0.0f };
        }

        last_armor_map[player_address] = current_armor;
    }

    static void draw_health_delta_text(std::uintptr_t player_address, const ImVec2& bar_position, const ImVec2& bar_size, float displayed_health, float max_health, float alpha_factor = 1.0f, bool dormant = false)
    {
        auto it = health_delta_map.find(player_address);
        if (it == health_delta_map.end())
        {
            return;
        }

        health_delta_event& evt = it->second;
        const float delta_time = ImGui::GetIO().DeltaTime;
        evt.time += delta_time;

        constexpr float lifetime = 2.0f;
        constexpr float fade_in = 0.2f;
        constexpr float fade_out = 0.2f;

        if (evt.time >= lifetime)
        {
            health_delta_map.erase(it);
            return;
        }

        float alpha = 1.0f;
        if (evt.time < fade_in)
        {
            alpha = evt.time / fade_in;
        }
        else if (evt.time > lifetime - fade_out)
        {
            alpha = (lifetime - evt.time) / fade_out;
        }

        const int delta_value = static_cast<int>(std::round(evt.value));
        if (delta_value == 0)
        {
            return;
        }

        const std::string text = (delta_value > 0 ? "+" : "") + std::to_string(delta_value);
        ImFont* font = c_fonts::smallest_pixel ? c_fonts::smallest_pixel : ImGui::GetFont();
        const float font_size = 10.0f;
        const ImVec2 text_size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str()) : ImVec2(0, 0);

        const float padding = 2.0f;

        const int padding_top = 1;
        const int padding_bottom = 1;
        const float clamped_health = std::clamp(displayed_health, 0.0f, (std::max)(max_health, 1.0f));
        const float fill_ratio = (max_health > 0.0f) ? (clamped_health / max_health) : 0.0f;
        const float full_height = std::round(bar_size.y) + padding_top + padding_bottom;
        const float bar_top = std::round(bar_position.y - static_cast<float>(padding_top));
        const float bar_bottom = bar_top + full_height;
        const float fill_top = std::round(bar_bottom - full_height * fill_ratio);

        float text_y = fill_top - text_size.y * 0.5f;
        text_y = std::clamp(text_y, bar_top, bar_bottom - text_size.y);

        ImVec2 text_pos(bar_position.x - padding - text_size.x, text_y);

        ImVec4 color = (delta_value > 0)
            ? ImVec4(0.2f, 0.8f, 0.3f, 1.0f)
            : ImVec4(0.8f, 0.2f, 0.2f, 1.0f);
        color = apply_dormant_tint(color, dormant);
        color = adjust_alpha(color, alpha_factor * alpha);

        const ImU32 text_color = ImGui::GetColorU32(color);
        const ImU32 shadow_color = adjust_alpha(IM_COL32(0, 0, 0, 200), alpha_factor * alpha);

        auto draw = ImGui::GetBackgroundDrawList();
        const bool outline = (font == c_fonts::smallest_pixel);
        ::add_text(draw, font, font_size, text_pos, text_color, text, outline);
    }

    static void draw_armor_delta_text(std::uintptr_t player_address, const ImVec2& bar_position, const ImVec2& bar_size, float displayed_armor, float max_armor, float alpha_factor = 1.0f, bool dormant = false)
    {
        auto it = armor_delta_map.find(player_address);
        if (it == armor_delta_map.end())
        {
            return;
        }

        health_delta_event& evt = it->second;
        const float delta_time = ImGui::GetIO().DeltaTime;
        evt.time += delta_time;

        constexpr float lifetime = 2.0f;
        constexpr float fade_in = 0.2f;
        constexpr float fade_out = 0.2f;

        if (evt.time >= lifetime)
        {
            armor_delta_map.erase(it);
            return;
        }

        float alpha = 1.0f;
        if (evt.time < fade_in)
        {
            alpha = evt.time / fade_in;
        }
        else if (evt.time > lifetime - fade_out)
        {
            alpha = (lifetime - evt.time) / fade_out;
        }

        const int delta_value = static_cast<int>(std::round(evt.value));
        if (delta_value == 0)
        {
            return;
        }

        const std::string text = (delta_value > 0 ? "+" : "") + std::to_string(delta_value);
        ImFont* font = c_fonts::smallest_pixel ? c_fonts::smallest_pixel : ImGui::GetFont();
        const float font_size = 10.0f;
        const ImVec2 text_size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str()) : ImVec2(0, 0);

        const float padding = 2.0f;

        const float clamped_armor = std::clamp(displayed_armor, 0.0f, (std::max)(max_armor, 1.0f));
        const float fill_ratio = (max_armor > 0.0f) ? (clamped_armor / max_armor) : 0.0f;

        const float bar_left = bar_position.x;
        const float bar_right = bar_position.x + bar_size.x;
        const float fill_x = bar_left + bar_size.x * fill_ratio;

        float text_x = fill_x - text_size.x * 0.5f;
        text_x = std::clamp(text_x, bar_left, bar_right - text_size.x);

        const float text_y = bar_position.y + bar_size.y + 2.0f;
        ImVec2 text_pos(text_x, text_y);

        ImVec4 color = ImVec4(0.35f, 0.75f, 1.0f, 1.0f);
        color = adjust_alpha(color, alpha_factor * alpha);

        const ImU32 text_color = ImGui::GetColorU32(color);
        const ImU32 shadow_color = adjust_alpha(IM_COL32(0, 0, 0, 200), alpha_factor * alpha);

        auto draw = ImGui::GetBackgroundDrawList();
        ::add_text(draw, font, font_size, text_pos, text_color, text, true);
    }

    static void prune_health_entries(const std::vector<std::uintptr_t>& active_addresses)
    {
        for (auto it = displayed_health_map.begin(); it != displayed_health_map.end();)
        {
            const bool still_active = std::find(active_addresses.begin(), active_addresses.end(), it->first) != active_addresses.end();
            if (!still_active)
            {
                it = displayed_health_map.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = last_health_map.begin(); it != last_health_map.end();)
        {
            const bool still_active = std::find(active_addresses.begin(), active_addresses.end(), it->first) != active_addresses.end();
            if (!still_active)
            {
                it = last_health_map.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = health_delta_map.begin(); it != health_delta_map.end();)
        {
            const bool still_active = std::find(active_addresses.begin(), active_addresses.end(), it->first) != active_addresses.end();
            if (!still_active)
            {
                it = health_delta_map.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = displayed_armor_map.begin(); it != displayed_armor_map.end();)
        {
            const bool still_active = std::find(active_addresses.begin(), active_addresses.end(), it->first) != active_addresses.end();
            if (!still_active)
            {
                it = displayed_armor_map.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = last_health_map.begin(); it != last_health_map.end();)
        {
            const bool still_active = std::find(active_addresses.begin(), active_addresses.end(), it->first) != active_addresses.end();
            if (!still_active)
            {
                it = last_health_map.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = last_armor_map.begin(); it != last_armor_map.end();)
        {
            const bool still_active = std::find(active_addresses.begin(), active_addresses.end(), it->first) != active_addresses.end();
            if (!still_active)
            {
                it = last_armor_map.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = health_delta_map.begin(); it != health_delta_map.end();)
        {
            const bool still_active = std::find(active_addresses.begin(), active_addresses.end(), it->first) != active_addresses.end();
            if (!still_active)
            {
                it = health_delta_map.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = armor_delta_map.begin(); it != armor_delta_map.end();)
        {
            const bool still_active = std::find(active_addresses.begin(), active_addresses.end(), it->first) != active_addresses.end();
            if (!still_active)
            {
                it = armor_delta_map.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = distance_anim_map.begin(); it != distance_anim_map.end();)
        {
            const bool still_active = std::find(active_addresses.begin(), active_addresses.end(), it->first) != active_addresses.end();
            if (!still_active)
            {
                it = distance_anim_map.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = fade_map.begin(); it != fade_map.end();)
        {
            const bool still_active = std::find(active_addresses.begin(), active_addresses.end(), it->first) != active_addresses.end();
            if (!still_active)
            {
                it = fade_map.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = last_character_seen.begin(); it != last_character_seen.end();)
        {
            const bool still_active = std::find(active_addresses.begin(), active_addresses.end(), it->first) != active_addresses.end();
            if (!still_active)
            {
                it = last_character_seen.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    struct camera_frame_t
    {
        rbx::Matrix view_matrix;
        rbx::Vector2 dimensions;
    };

    std::optional<std::uintptr_t> resolve_visualengine_address()
    {
        auto address = globals->visualengine.get_address();
        if (address == 0)
        {
            address = rbx::engine->get_visualengine();
            if (address)
            {
                globals->visualengine = rbx::instance_t(address);
            }
        }

        return address ? std::optional<std::uintptr_t>(address) : std::nullopt;
    }

    std::optional<camera_frame_t> read_camera_frame()
    {
        const auto visualengine_address = resolve_visualengine_address();
        if (!visualengine_address)
        {
            return std::nullopt;
        }

        rbx::visualengine_t visualengine(*visualengine_address);
        auto view_matrix = visualengine.get_view_matrix();
        if (!view_matrix)
        {
            return std::nullopt;
        }

        auto dimensions = visualengine.get_dimensions();
        if (!dimensions)
        {
            const ImVec2 display_size = ImGui::GetIO().DisplaySize;
            if (display_size.x <= 0.0f || display_size.y <= 0.0f)
            {
                return std::nullopt;
            }
            dimensions = rbx::Vector2(display_size.x, display_size.y);
        }

        return camera_frame_t{ *view_matrix, *dimensions };
    }

    std::optional<rbx::Vector3> get_part_position(const cache::primitive_part& part)
    {
        if (!part.instance.is_valid())
        {
            return std::nullopt;
        }

        return part.instance.get_position(part.primitive);
    }

    std::optional<ImVec2> project_part_to_screen(const cache::primitive_part& part, const camera_frame_t& frame)
    {
        const auto world_pos = get_part_position(part);
        if (!world_pos)
        {
            return std::nullopt;
        }

        const auto screen = rbx::camera::world_to_screen(*world_pos, frame.view_matrix, frame.dimensions);
        if (!screen)
        {
            return std::nullopt;
        }

        return ImVec2(std::round(screen->x), std::round(screen->y));
    }

    std::optional<ImVec2> project_part_to_screen_offset(const cache::primitive_part& part, const camera_frame_t& frame, const rbx::Vector3& world_offset)
    {
        const auto world_pos = get_part_position(part);
        if (!world_pos)
        {
            return std::nullopt;
        }

        const auto screen = rbx::camera::world_to_screen(*world_pos + world_offset, frame.view_matrix, frame.dimensions);
        if (!screen)
        {
            return std::nullopt;
        }

        return ImVec2(std::round(screen->x), std::round(screen->y));
    }

    std::optional<ImVec2> project_part_to_screen_local_offset(const cache::primitive_part& part, const camera_frame_t& frame, const rbx::Vector3& local_offset)
    {
        const auto world_pos = get_part_position(part);
        if (!world_pos)
        {
            return std::nullopt;
        }

        rbx::Vector3 world_offset = local_offset;
        
        if (part.primitive && roblox::offsets::base_part::cframe_rotation)
        {
            struct rot_t
            {
                float m[3][3];
            };
            
            const rot_t rot = memory->read<rot_t>(part.primitive + roblox::offsets::base_part::cframe_rotation);
            
            world_offset.x = rot.m[0][0] * local_offset.x + rot.m[0][1] * local_offset.y + rot.m[0][2] * local_offset.z;
            world_offset.y = rot.m[1][0] * local_offset.x + rot.m[1][1] * local_offset.y + rot.m[1][2] * local_offset.z;
            world_offset.z = rot.m[2][0] * local_offset.x + rot.m[2][1] * local_offset.y + rot.m[2][2] * local_offset.z;
        }

        const auto screen = rbx::camera::world_to_screen(*world_pos + world_offset, frame.view_matrix, frame.dimensions);
        if (!screen)
        {
            return std::nullopt;
        }

        return ImVec2(std::round(screen->x), std::round(screen->y));
    }

    std::optional<std::pair<ImVec2, bool>> project_unclamped(const rbx::Vector3& world, const camera_frame_t& frame)
    {
        DirectX::SimpleMath::Vector4 pos(world.x, world.y, world.z, 1.0f);
        DirectX::SimpleMath::Vector4 clip = DirectX::XMVector4Transform(pos, frame.view_matrix);
        if (clip.w == 0.0f)
            return std::nullopt;
        bool behind = clip.w < 0.0f;
        if (behind)
        {
            clip.x = -clip.x;
            clip.y = -clip.y;
            clip.z = -clip.z;
            clip.w = -clip.w;
        }
        float inv_w = 1.0f / clip.w;
        float ndc_x = clip.x * inv_w;
        float ndc_y = clip.y * inv_w;
        float screen_x = (ndc_x * 0.5f + 0.5f) * frame.dimensions.x;
        float screen_y = (-ndc_y * 0.5f + 0.5f) * frame.dimensions.y;
        return std::make_pair(ImVec2(screen_x, screen_y), behind);
    }

    bool is_finite_vec3(const rbx::Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool project_segment_unclamped(
        const rbx::Vector3& start,
        const rbx::Vector3& end,
        const camera_frame_t& frame,
        ImVec2& out_start,
        ImVec2& out_end)
    {
        if (frame.dimensions.x <= 0.0f || frame.dimensions.y <= 0.0f)
        {
            return false;
        }

        rbx::Vector4 clip_start = rbx::camera::transform(start, frame.view_matrix);
        rbx::Vector4 clip_end = rbx::camera::transform(end, frame.view_matrix);

        constexpr float k_clip_epsilon = 0.01f;
        if (clip_start.w <= k_clip_epsilon && clip_end.w <= k_clip_epsilon)
        {
            return false;
        }

        if (clip_start.w <= k_clip_epsilon || clip_end.w <= k_clip_epsilon)
        {
            const rbx::Vector4 delta = clip_end - clip_start;
            const float denom = delta.w;
            if (std::fabs(denom) > 1e-6f)
            {
                const float t = (k_clip_epsilon - clip_start.w) / denom;
                const float clamped_t = std::clamp(t, 0.0f, 1.0f);
                if (clip_start.w <= k_clip_epsilon)
                {
                    clip_start = clip_start + delta * clamped_t;
                }
                else
                {
                    clip_end = clip_start + delta * clamped_t;
                }
            }
        }

        auto to_screen = [&](const rbx::Vector4& clip) -> ImVec2
            {
                const float inv_w = 1.0f / clip.w;
                const float ndc_x = clip.x * inv_w;
                const float ndc_y = clip.y * inv_w;
                const float screen_x = (ndc_x * 0.5f + 0.5f) * frame.dimensions.x;
                const float screen_y = (-ndc_y * 0.5f + 0.5f) * frame.dimensions.y;
                return ImVec2(screen_x, screen_y);
            };

        out_start = to_screen(clip_start);
        out_end = to_screen(clip_end);
        return true;
    }

    std::optional<rbx::Vector2> get_cursor_client_position(const rbx::Vector2& dims)
    {
        if (dims.x <= 0.0f || dims.y <= 0.0f)
        {
            return std::nullopt;
        }

        const HWND window = vanille::overlay::g_rbx_window;
        if (!window || !::IsWindow(window))
        {
            return std::nullopt;
        }

        POINT cursor{};
        if (!::GetCursorPos(&cursor))
        {
            return std::nullopt;
        }

        POINT client = cursor;
        if (!::ScreenToClient(window, &client))
        {
            return std::nullopt;
        }

        RECT client_rect{};
        if (!::GetClientRect(window, &client_rect))
        {
            return std::nullopt;
        }

        if (client_rect.right <= client_rect.left || client_rect.bottom <= client_rect.top)
        {
            return std::nullopt;
        }

        if (client.x < client_rect.left || client.x > client_rect.right ||
            client.y < client_rect.top || client.y > client_rect.bottom)
        {
            return std::nullopt;
        }

        const float x = std::clamp(static_cast<float>(client.x), 0.0f, dims.x);
        const float y = std::clamp(static_cast<float>(client.y), 0.0f, dims.y);
        return rbx::Vector2{ x, y };
    }

    bool screen_to_world_ray(const camera_frame_t& frame, const rbx::Vector2& screen, rbx::Vector3& out_dir)
    {
        if (frame.dimensions.x <= 0.0f || frame.dimensions.y <= 0.0f)
        {
            return false;
        }

        rbx::Matrix inv = frame.view_matrix;
        const float det = inv.Determinant();
        if (!std::isfinite(det) || std::fabs(det) < 1e-6f)
        {
            return false;
        }

        inv.Invert();

        const float ndc_x = (screen.x / frame.dimensions.x) * 2.0f - 1.0f;
        const float ndc_y = 1.0f - (screen.y / frame.dimensions.y) * 2.0f;

        rbx::Vector4 clip_near(ndc_x, ndc_y, 0.0f, 1.0f);
        rbx::Vector4 clip_far(ndc_x, ndc_y, 1.0f, 1.0f);

        rbx::Vector4 world_near = DirectX::XMVector4Transform(clip_near, inv);
        rbx::Vector4 world_far = DirectX::XMVector4Transform(clip_far, inv);

        if (std::fabs(world_near.w) < 1e-6f || std::fabs(world_far.w) < 1e-6f)
        {
            return false;
        }

        const float inv_w_near = 1.0f / world_near.w;
        const float inv_w_far = 1.0f / world_far.w;
        world_near.x *= inv_w_near;
        world_near.y *= inv_w_near;
        world_near.z *= inv_w_near;
        world_far.x *= inv_w_far;
        world_far.y *= inv_w_far;
        world_far.z *= inv_w_far;

        rbx::Vector3 dir(world_far.x - world_near.x, world_far.y - world_near.y, world_far.z - world_near.z);
        if (dir.LengthSquared() < 1e-6f)
        {
            return false;
        }
        dir.Normalize();
        out_dir = dir;
        return true;
    }

    std::optional<rbx::Vector3> resolve_camera_position(const cache::local_player_state& local, const camera_frame_t& frame)
    {
        if (local.camera.is_valid())
        {
            const auto pos = local.camera.get_camera_position();
            if (is_finite_vec3(pos))
            {
                return pos;
            }
        }

        rbx::Matrix view = frame.view_matrix;
        const float det = view.Determinant();
        if (!std::isfinite(det) || std::fabs(det) < 1e-6f)
        {
            return std::nullopt;
        }

        view.Invert();
        rbx::Vector3 pos(view._41, view._42, view._43);
        if (!is_finite_vec3(pos))
        {
            return std::nullopt;
        }
        return pos;
    }

    bool name_starts_with_bracket(const std::string& name)
    {
        return !name.empty() && name.front() == '[';
    }

    std::optional<rbx::Vector3> try_tool_handle_position(const rbx::instance_t& tool)
    {
        if (!tool.is_valid())
        {
            return std::nullopt;
        }

        if (tool.get_class_name() != "Tool")
        {
            return std::nullopt;
        }

        const std::string tool_name = tool.get_name();
        if (!name_starts_with_bracket(tool_name))
        {
            return std::nullopt;
        }

        const auto handle = tool.find_first_child("Handle");
        if (!handle.is_valid())
        {
            return std::nullopt;
        }

        if (handle.get_class_name() != "Part")
        {
            return std::nullopt;
        }

        const auto pos = handle.get_position();
        if (!pos || !is_finite_vec3(*pos))
        {
            return std::nullopt;
        }

        return pos;
    }

    std::optional<rbx::Vector3> resolve_local_gun_position(const cache::local_player_state& local)
    {
        if (auto pos = try_tool_handle_position(local.equipped_tool))
        {
            return pos;
        }

        if (auto pos = try_tool_handle_position(local.revolver))
        {
            return pos;
        }

        if (local.character.is_valid())
        {
            const auto children = local.character.get_children();
            for (const auto& child : children)
            {
                if (auto pos = try_tool_handle_position(child))
                {
                    return pos;
                }
            }
        }

        return std::nullopt;
    }

    rbx::instance_t resolve_local_tool_instance(const cache::local_player_state& local)
    {
        if (local.equipped_tool.is_valid())
        {
            return local.equipped_tool;
        }

        if (local.revolver.is_valid())
        {
            return local.revolver;
        }

        if (local.character.is_valid())
        {
            const auto children = local.character.get_children();
            for (const auto& child : children)
            {
                if (!child.is_valid())
                {
                    continue;
                }
                if (child.get_class_name() == "Tool")
                {
                    return child;
                }
            }
        }

        return {};
    }

    void push_hit_tracer(const rbx::Vector3& start, const rbx::Vector3& end);

    static std::unordered_map<std::uintptr_t, double> grenade_spawn_times;
    static std::unordered_map<std::uintptr_t, double> grenade_last_seen_times;
    static std::unordered_map<std::uintptr_t, double> grenade_local_hold_start_times;
    static std::deque<std::pair<double, double>> grenade_pending_cook_starts;
    static std::vector<rbx::instance_t> grenade_misc_candidates_cache;
    static std::uintptr_t grenade_misc_cached_root_address = 0;
    static double grenade_misc_last_scan_time = -1000.0;
    static double grenade_last_hold_scan_time = -1000.0;

    void clear_grenade_tracking_state()
    {
        grenade_spawn_times.clear();
        grenade_last_seen_times.clear();
        grenade_local_hold_start_times.clear();
        grenade_pending_cook_starts.clear();
        grenade_misc_candidates_cache.clear();
        grenade_misc_cached_root_address = 0;
        grenade_misc_last_scan_time = -1000.0;
        grenade_last_hold_scan_time = -1000.0;
    }

    std::optional<rbx::Vector3> resolve_ignore_local_root_position()
    {
        static double next_retry_time = 0.0;
        constexpr double k_retry_interval = 1.0;

        const auto cached_pf_model = globals->pf_local_player_model;
        if (cached_pf_model.is_valid())
        {
            const auto humanoid_root_part = cached_pf_model.find_first_child("HumanoidRootPart");
            if (humanoid_root_part.is_valid())
            {
                const auto primitive = rbx::part::get_primitive(humanoid_root_part);
                const auto position = humanoid_root_part.get_position(primitive);
                if (position && is_finite_vec3(*position))
                {
                    return position;
                }
            }
        }

        const double now = ImGui::GetTime();
        if (now < next_retry_time)
        {
            return std::nullopt;
        }

        const auto workspace = globals->workspace;
        if (!workspace.is_valid())
        {
            next_retry_time = now + k_retry_interval;
            return std::nullopt;
        }

        const auto ignore = workspace.find_first_child("Ignore");
        if (!ignore.is_valid())
        {
            next_retry_time = now + k_retry_interval;
            return std::nullopt;
        }

        const auto local_model = ignore.find_first_child_by_class("Model");
        if (!local_model.is_valid())
        {
            next_retry_time = now + k_retry_interval;
            return std::nullopt;
        }

        const auto humanoid_root_part = local_model.find_first_child("HumanoidRootPart");
        if (!humanoid_root_part.is_valid())
        {
            next_retry_time = now + k_retry_interval;
            return std::nullopt;
        }

        globals->pf_local_player_model = local_model;
        next_retry_time = now;

        const auto primitive = rbx::part::get_primitive(humanoid_root_part);
        const auto position = humanoid_root_part.get_position(primitive);
        if (!position || !is_finite_vec3(*position))
        {
            next_retry_time = now + k_retry_interval;
            return std::nullopt;
        }

        return position;
    }

    void update_local_grenade_hold_state(const cache::local_player_state& local, double now)
    {
        std::unordered_set<std::uintptr_t> held_trigger_addresses;
        held_trigger_addresses.reserve(16);

        auto collect_triggers_from_root = [&](const rbx::instance_t& root)
        {
            if (!root.is_valid())
            {
                return;
            }

            std::vector<rbx::instance_t> nodes = root.get_descendants();
            nodes.emplace_back(root);
            for (const auto& node : nodes)
            {
                if (!node.is_valid())
                {
                    continue;
                }

                const std::string class_name = node.get_class_name();
                if (class_name != "Part" && class_name != "MeshPart")
                {
                    continue;
                }

                if (node.get_name() != "Trigger")
                {
                    continue;
                }

                const std::uintptr_t address = node.get_address();
                if (address != 0)
                {
                    held_trigger_addresses.insert(address);
                }
            }
        };

        if (local.equipped_tool.is_valid())
        {
            collect_triggers_from_root(local.equipped_tool);
        }
        else
        {
            const auto tool = resolve_local_tool_instance(local);
            collect_triggers_from_root(tool);
        }

        if (held_trigger_addresses.empty() && local.character.is_valid())
        {
            collect_triggers_from_root(local.character);
        }

        for (const std::uintptr_t address : held_trigger_addresses)
        {
            grenade_local_hold_start_times.emplace(address, now);
        }

        for (auto it = grenade_local_hold_start_times.begin(); it != grenade_local_hold_start_times.end();)
        {
            if (!held_trigger_addresses.contains(it->first))
            {
                grenade_pending_cook_starts.emplace_back(it->second, now);
                it = grenade_local_hold_start_times.erase(it);
            }
            else
            {
                ++it;
            }
        }

        constexpr double pending_ttl = 2.0;
        while (!grenade_pending_cook_starts.empty() && (now - grenade_pending_cook_starts.front().second) > pending_ttl)
        {
            grenade_pending_cook_starts.pop_front();
        }
    }

    std::optional<double> consume_pending_cook_start(double now)
    {
        constexpr double k_max_assign_delay = 1.25;
        while (!grenade_pending_cook_starts.empty())
        {
            const auto [cook_start_time, release_time] = grenade_pending_cook_starts.front();
            const double pending_age = now - release_time;
            if (pending_age < 0.0)
            {
                return std::nullopt;
            }

            grenade_pending_cook_starts.pop_front();
            if (pending_age <= k_max_assign_delay)
            {
                return cook_start_time;
            }
        }

        return std::nullopt;
    }

    void render_grenade_indicator(const camera_frame_t& frame, const cache::local_player_state& local)
    {
        const double now = ImGui::GetTime();

        if (!features->enable_esp || !features->enable_grenade_indicator)
        {
            clear_grenade_tracking_state();
            return;
        }

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw)
        {
            return;
        }

        const auto workspace = globals->workspace;
        if (!workspace.is_valid())
        {
            clear_grenade_tracking_state();
            return;
        }

        const auto ignore = workspace.find_first_child("Ignore");
        if (!ignore.is_valid())
        {
            clear_grenade_tracking_state();
            return;
        }

        const auto misc_root = ignore.find_first_child("Misc");
        if (!misc_root.is_valid())
        {
            clear_grenade_tracking_state();
            return;
        }

        std::optional<rbx::Vector3> local_root_pos = get_part_position(local.parts.humanoid_root_part);
        if (!local_root_pos)
        {
            local_root_pos = resolve_ignore_local_root_position();
        }

        const bool has_local_root = local_root_pos && is_finite_vec3(*local_root_pos);
        if (!has_local_root)
        {
            clear_grenade_tracking_state();
            return;
        }

        constexpr double hold_scan_interval = 0.12;
        if ((now - grenade_last_hold_scan_time) >= hold_scan_interval)
        {
            update_local_grenade_hold_state(local, now);
            grenade_last_hold_scan_time = now;
        }

        constexpr double misc_scan_interval = 0.12;
        constexpr std::size_t max_misc_candidates = 2500;
        const std::uintptr_t misc_root_address = misc_root.get_address();
        const bool should_rescan_misc = grenade_misc_candidates_cache.empty()
            || grenade_misc_cached_root_address != misc_root_address
            || (now - grenade_misc_last_scan_time) >= misc_scan_interval;
        if (should_rescan_misc)
        {
            grenade_misc_candidates_cache = misc_root.get_descendants();
            grenade_misc_candidates_cache.emplace_back(misc_root);
            if (grenade_misc_candidates_cache.size() > max_misc_candidates)
            {
                grenade_misc_candidates_cache.resize(max_misc_candidates);
            }
            grenade_misc_cached_root_address = misc_root_address;
            grenade_misc_last_scan_time = now;
        }
        const auto& candidates = grenade_misc_candidates_cache;

        std::unordered_set<std::uintptr_t> active_addresses;
        active_addresses.reserve(candidates.size());

        const float grenade_fuse_seconds = std::clamp(features->grenade_indicator_fuse_time, 0.5f, 15.0f);
        constexpr float grenade_indicator_scale = 1.2f;
        const float base_radius = std::clamp(features->grenade_indicator_radius, 6.0f, 28.0f) * grenade_indicator_scale;
        const ImVec4 safe_color = features->grenade_indicator_safe_color;
        const ImVec4 danger_color = features->grenade_indicator_danger_color;
        const bool draw_timer = features->grenade_indicator_show_timer;

        for (const auto& candidate : candidates)
        {
            if (!candidate.is_valid())
            {
                continue;
            }

            const std::string class_name = candidate.get_class_name();
            if (class_name != "Part" && class_name != "MeshPart")
            {
                continue;
            }

            const std::uintptr_t address = candidate.get_address();
            if (address == 0)
            {
                continue;
            }

            const bool tracked_instance = grenade_spawn_times.contains(address);
            const bool is_trigger_name = (candidate.get_name() == "Trigger");
            if (!is_trigger_name && !tracked_instance)
            {
                continue;
            }

            if (!active_addresses.insert(address).second)
            {
                continue;
            }

            if (!is_trigger_name && tracked_instance)
            {
            }

            auto [spawn_it, inserted] = grenade_spawn_times.emplace(address, now);
            if (inserted)
            {
                if (const auto cook_start_time = consume_pending_cook_start(now))
                {
                    spawn_it->second = *cook_start_time;
                }
            }
            if (!inserted && spawn_it->second <= 0.0)
            {
                spawn_it->second = now;
            }

            grenade_last_seen_times[address] = now;

            const float elapsed = static_cast<float>(now - spawn_it->second);
            const float remaining = grenade_fuse_seconds - elapsed;
            if (remaining <= 0.0f)
            {
                continue;
            }

            const auto primitive = rbx::part::get_primitive(candidate);
            const auto grenade_world = candidate.get_position(primitive);
            if (!grenade_world || !is_finite_vec3(*grenade_world))
            {
                continue;
            }

            float distance_to_local = 0.0f;
            if (local_root_pos && is_finite_vec3(*local_root_pos))
            {
                const rbx::Vector3 delta = *grenade_world - *local_root_pos;
                const float distance = delta.Length();
                if (std::isfinite(distance) && distance >= 0.0f)
                {
                    distance_to_local = distance;
                }
            }

            float world_offset_y = 1.6f;
            if (const auto size = rbx::part::get_size(primitive))
            {
                world_offset_y += std::clamp(size->y * 0.5f, 0.0f, 3.5f);
            }

            const rbx::Vector3 marker_world = *grenade_world + rbx::Vector3(0.0f, world_offset_y, 0.0f);
            const auto marker_screen = rbx::camera::world_to_screen(marker_world, frame.view_matrix, frame.dimensions);
            if (!marker_screen)
            {
                continue;
            }

            const ImVec2 center(IM_ROUND(marker_screen->x), IM_ROUND(marker_screen->y));
            const float radius = std::clamp(base_radius - distance_to_local * 0.015f, 7.2f, base_radius);
            const float fuse_ratio = std::clamp(remaining / grenade_fuse_seconds, 0.0f, 1.0f);
            const ImVec4 ring_color = ImLerp(danger_color, safe_color, fuse_ratio);
            ImVec4 fill_color = ring_color;
            fill_color.w = std::clamp(ring_color.w * 0.23f, 0.0f, 1.0f);

            draw->AddCircleFilled(center, radius, ImGui::GetColorU32(fill_color), 36);
            if (c_textures::grenade_icon && c_textures::grenade_icon_size.x > 0.0f && c_textures::grenade_icon_size.y > 0.0f)
            {
                const float icon_max_extent = radius * 1.15f;
                const float icon_aspect = c_textures::grenade_icon_size.x / c_textures::grenade_icon_size.y;
                ImVec2 icon_size(icon_max_extent, icon_max_extent);
                if (icon_aspect > 1.0f)
                {
                    icon_size.y /= icon_aspect;
                }
                else if (icon_aspect > 0.0f)
                {
                    icon_size.x *= icon_aspect;
                }

                const ImVec2 icon_min(center.x - icon_size.x * 0.5f, center.y - icon_size.y * 0.5f);
                const ImVec2 icon_max(icon_min.x + icon_size.x, icon_min.y + icon_size.y);
                draw->AddImage(c_textures::grenade_icon, icon_min, icon_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 245));
            }

            if (!draw_timer)
            {
                continue;
            }

            const float timer_radius = radius + std::clamp(radius * 0.42f, 3.0f, 8.0f);
            const float timer_thickness = std::clamp(radius * 0.32f, 3.0f, 7.5f);

            ImVec4 timer_track_color = ring_color;
            timer_track_color.w = std::clamp(ring_color.w * 0.22f, 0.0f, 1.0f);
            draw->AddCircle(center, timer_radius, ImGui::GetColorU32(timer_track_color), 48, timer_thickness);

            if (fuse_ratio > 0.0f)
            {
                ImVec4 timer_fill_color = ring_color;
                timer_fill_color.w = std::clamp(ring_color.w * 1.05f, 0.0f, 1.0f);

                const float start_angle = -IM_PI * 0.5f;
                const float end_angle = start_angle + fuse_ratio * IM_PI * 2.0f;
                draw->PathClear();
                draw->PathArcTo(center, timer_radius, start_angle, end_angle, 64);
                draw->PathStroke(ImGui::GetColorU32(timer_fill_color), false, timer_thickness);
            }
        }

        constexpr double missing_grace_seconds = 0.75;
        const double lifetime_grace_seconds = static_cast<double>(grenade_fuse_seconds) + 1.0;
        for (auto it = grenade_spawn_times.begin(); it != grenade_spawn_times.end();)
        {
            const std::uintptr_t address = it->first;
            const auto seen_it = grenade_last_seen_times.find(address);
            const bool stale_missing = (seen_it == grenade_last_seen_times.end()) || ((now - seen_it->second) > missing_grace_seconds);
            const bool stale_lifetime = (now - it->second) > lifetime_grace_seconds;
            if (stale_missing || stale_lifetime)
            {
                if (seen_it != grenade_last_seen_times.end())
                {
                    grenade_last_seen_times.erase(seen_it);
                }
                it = grenade_spawn_times.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = grenade_last_seen_times.begin(); it != grenade_last_seen_times.end();)
        {
            if (!grenade_spawn_times.contains(it->first))
            {
                it = grenade_last_seen_times.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void draw_tool_part_labels(const cache::local_player_state& local, const camera_frame_t& frame)
    {
        constexpr std::int64_t pf_place_id = 292439477;
        if (globals->game_id != pf_place_id || !features->enable_hit_trace)
        {
            return;
        }

        const auto camera_pos = resolve_camera_position(local, frame);
        if (!camera_pos || !is_finite_vec3(*camera_pos))
        {
            return;
        }

        static double last_trace_time = -1.0;
        constexpr double k_trace_min_interval = 0.05;

        struct part_label_entry
        {
            cache::primitive_part part;
            rbx::Vector3 position{};
            float distance_sq = 0.0f;
        };

        auto consider_part = [&](const rbx::instance_t& child, std::optional<part_label_entry>& best_part)
        {
            if (!child.is_valid())
            {
                return;
            }

            const std::string class_name = child.get_class_name();
            if (class_name != "Part" && class_name != "MeshPart")
            {
                return;
            }

            const auto primitive = rbx::part::get_primitive(child);
            if (!primitive)
            {
                return;
            }

            const auto size = rbx::part::get_size(primitive);
            if (!size)
            {
                return;
            }

            const auto pos = child.get_position(primitive);
            if (!pos || !is_finite_vec3(*pos))
            {
                return;
            }

            const rbx::Vector3 delta = *pos - *camera_pos;
            const float dist_sq = delta.LengthSquared();
            if (!std::isfinite(dist_sq))
            {
                return;
            }

            if (!best_part || dist_sq > best_part->distance_sq)
            {
                cache::primitive_part entry;
                entry.instance = child;
                entry.primitive = primitive;
                entry.size = *size;
                best_part = part_label_entry{ entry, *pos, dist_sq };
            }
        };

        auto collect_from_root = [&](const rbx::instance_t& root, std::optional<part_label_entry>& best_part)
        {
            if (!root.is_valid())
            {
                return;
            }

            auto descendants = root.get_descendants();
            if (descendants.empty())
            {
                return;
            }

            for (const auto& child : descendants)
            {
                consider_part(child, best_part);
            }
        };

        std::optional<part_label_entry> best_part;
        if (local.camera.is_valid())
        {
            collect_from_root(local.camera, best_part);
        }

        if (!best_part)
        {
            const auto tool = resolve_local_tool_instance(local);
            collect_from_root(tool, best_part);
        }

        if (!best_part)
        {
            return;
        }

        const double now = ImGui::GetTime();
        const bool left_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool left_pressed = (GetAsyncKeyState(VK_LBUTTON) & 1) != 0;
        if (left_down)
        {
            if (left_pressed || last_trace_time < 0.0 || (now - last_trace_time) >= k_trace_min_interval)
            {
                push_hit_tracer(*camera_pos, best_part->position);
                last_trace_time = now;
            }
        }
    }

    std::optional<rbx::Vector3> resolve_camera_forward(const cache::local_player_state& local)
    {
        if (!local.camera.is_valid())
        {
            return std::nullopt;
        }

        const auto rot = local.camera.get_rotation();
        rbx::Vector3 forward(-rot._13, -rot._23, -rot._33);
        const float len_sq = forward.LengthSquared();
        if (!std::isfinite(len_sq) || len_sq < 1e-6f)
        {
            return std::nullopt;
        }
        forward.Normalize();
        return forward;
    }

    std::optional<rbx::Vector3> resolve_camera_right(const cache::local_player_state& local)
    {
        if (!local.camera.is_valid())
        {
            return std::nullopt;
        }

        const auto rot = local.camera.get_rotation();
        rbx::Vector3 right(-rot._11, rot._21, -rot._31);
        const float len_sq = right.LengthSquared();
        if (!std::isfinite(len_sq) || len_sq < 1e-6f)
        {
            return std::nullopt;
        }
        right.Normalize();
        return right;
    }

    std::optional<rbx::Vector3> resolve_camera_up(const cache::local_player_state& local)
    {
        if (!local.camera.is_valid())
        {
            return std::nullopt;
        }

        const auto rot = local.camera.get_rotation();
        rbx::Vector3 up(rot._12, rot._22, rot._32);
        const float len_sq = up.LengthSquared();
        if (!std::isfinite(len_sq) || len_sq < 1e-6f)
        {
            return std::nullopt;
        }
        up.Normalize();
        return up;
    }

    struct hit_tracer_entry
    {
        rbx::Vector3 start{};
        rbx::Vector3 end{};
        float age = 0.0f;
    };

    static std::vector<hit_tracer_entry> hit_tracers;

    void push_hit_tracer(const rbx::Vector3& start, const rbx::Vector3& end)
    {
        if (!is_finite_vec3(start) || !is_finite_vec3(end))
        {
            return;
        }

        hit_tracers.push_back({ start, end, 0.0f });
        constexpr std::size_t k_max_tracers = 64;
        if (hit_tracers.size() > k_max_tracers)
        {
            hit_tracers.erase(hit_tracers.begin(), hit_tracers.begin() + (hit_tracers.size() - k_max_tracers));
        }
    }

    void render_hit_tracers(const camera_frame_t& frame)
    {
        if (!features->enable_hit_trace)
        {
            hit_tracers.clear();
            return;
        }

        if (hit_tracers.empty())
        {
            return;
        }

        const float lifespan = std::clamp(features->hit_trace_lifespan, 0.02f, 10.0f);
        if (lifespan <= 0.0f)
        {
            hit_tracers.clear();
            return;
        }

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw)
        {
            return;
        }

        const float delta_time = ImGui::GetIO().DeltaTime;
        const float width = std::clamp(features->hit_trace_width, 0.1f, 12.0f);
        const bool draw_outline = features->hit_trace_outline;
        const ImVec4 base_color = features->hit_trace_color;
        const int effect = std::clamp(features->hit_trace_effect, 0, 3);

        for (auto it = hit_tracers.begin(); it != hit_tracers.end();)
        {
            it->age += delta_time;
            if (it->age >= lifespan)
            {
                it = hit_tracers.erase(it);
                continue;
            }

            const float alpha = std::clamp(1.0f - (it->age / lifespan), 0.0f, 1.0f);
            ImVec4 line_vec = base_color;
            line_vec.w = std::clamp(line_vec.w * alpha, 0.0f, 1.0f);

            ImVec2 screen_start{};
            ImVec2 screen_end{};
            if (project_segment_unclamped(it->start, it->end, frame, screen_start, screen_end))
            {
                if (effect == 1)
                {
                    if (draw_outline)
                    {
                        ImVec4 outline_vec(0.0f, 0.0f, 0.0f, line_vec.w);
                        draw->AddLine(screen_start, screen_end, ImGui::GetColorU32(outline_vec), width + 2.0f);
                    }
                    ImVec4 glow_vec = line_vec;
                    glow_vec.w = std::clamp(glow_vec.w * 0.35f, 0.0f, 1.0f);
                    draw->AddLine(screen_start, screen_end, ImGui::GetColorU32(glow_vec), width * 3.2f);
                    draw->AddLine(screen_start, screen_end, ImGui::GetColorU32(glow_vec), width * 2.0f);
                    draw->AddLine(screen_start, screen_end, ImGui::GetColorU32(line_vec), width);
                }
                else if (effect == 2)
                {
                    const float pulse = 0.5f + 0.5f * std::sin(it->age * 10.0f);
                    const float pulse_width = width * (0.7f + 0.6f * pulse);
                    if (draw_outline)
                    {
                        ImVec4 outline_vec(0.0f, 0.0f, 0.0f, line_vec.w);
                        draw->AddLine(screen_start, screen_end, ImGui::GetColorU32(outline_vec), pulse_width + 2.0f);
                    }
                    draw->AddLine(screen_start, screen_end, ImGui::GetColorU32(line_vec), pulse_width);
                    ImVec4 ring_vec = line_vec;
                    ring_vec.w = std::clamp(ring_vec.w * 0.7f, 0.0f, 1.0f);
                    const float ring_radius = (std::max)(pulse_width * 1.5f, 2.0f) + pulse * 4.0f;
                    draw->AddCircle(screen_end, ring_radius, ImGui::GetColorU32(ring_vec), 24, 2.0f);
                }
                else if (effect == 3)
                {
                    ImVec2 delta(screen_end.x - screen_start.x, screen_end.y - screen_start.y);
                    const float len_sq = delta.x * delta.x + delta.y * delta.y;
                    if (len_sq > 1.0f)
                    {
                        const float len = std::sqrt(len_sq);
                        ImVec2 dir(delta.x / len, delta.y / len);
                        ImVec2 perp(-dir.y, dir.x);
                        const float jitter = std::clamp(width * 2.5f, 2.0f, 12.0f);
                        const int segments = 6;
                        for (int s = 0; s < segments; ++s)
                        {
                            const float t0 = static_cast<float>(s) / static_cast<float>(segments);
                            const float t1 = static_cast<float>(s + 1) / static_cast<float>(segments);
                            const float offset0 = std::sin(it->age * 22.0f + static_cast<float>(s) * 2.1f) * jitter;
                            const float offset1 = std::sin(it->age * 22.0f + static_cast<float>(s + 1) * 2.1f) * jitter;
                            ImVec2 p0(screen_start.x + delta.x * t0 + perp.x * offset0, screen_start.y + delta.y * t0 + perp.y * offset0);
                            ImVec2 p1(screen_start.x + delta.x * t1 + perp.x * offset1, screen_start.y + delta.y * t1 + perp.y * offset1);
                            if (draw_outline)
                            {
                                ImVec4 outline_vec(0.0f, 0.0f, 0.0f, line_vec.w);
                                draw->AddLine(p0, p1, ImGui::GetColorU32(outline_vec), width + 2.0f);
                            }
                            draw->AddLine(p0, p1, ImGui::GetColorU32(line_vec), width);
                        }
                    }
                    else
                    {
                        if (draw_outline)
                        {
                            ImVec4 outline_vec(0.0f, 0.0f, 0.0f, line_vec.w);
                            draw->AddLine(screen_start, screen_end, ImGui::GetColorU32(outline_vec), width + 2.0f);
                        }
                        draw->AddLine(screen_start, screen_end, ImGui::GetColorU32(line_vec), width);
                    }
                }
                else
                {
                    if (draw_outline)
                    {
                        ImVec4 outline_vec(0.0f, 0.0f, 0.0f, line_vec.w);
                        draw->AddLine(screen_start, screen_end, ImGui::GetColorU32(outline_vec), width + 2.0f);
                    }
                    draw->AddLine(screen_start, screen_end, ImGui::GetColorU32(line_vec), width);
                }

                if (features->hit_trace_cross_enabled)
                {
                    const float cross_size = std::clamp(features->hit_trace_cross_size, 2.0f, 40.0f);
                    const float cross_thickness = std::clamp(features->hit_trace_cross_thickness, 0.5f, 10.0f);

                    ImVec4 cross_vec = features->hit_trace_cross_color;
                    cross_vec.w = std::clamp(cross_vec.w * alpha, 0.0f, 1.0f);

                    if (draw_outline)
                    {
                        ImVec4 outline_vec(0.0f, 0.0f, 0.0f, cross_vec.w);
                        const ImU32 outline_col = ImGui::GetColorU32(outline_vec);
                        draw->AddLine(ImVec2(screen_end.x - cross_size, screen_end.y), ImVec2(screen_end.x + cross_size, screen_end.y), outline_col, cross_thickness + 2.0f);
                        draw->AddLine(ImVec2(screen_end.x, screen_end.y - cross_size), ImVec2(screen_end.x, screen_end.y + cross_size), outline_col, cross_thickness + 2.0f);
                    }

                    const ImU32 cross_col = ImGui::GetColorU32(cross_vec);
                    draw->AddLine(ImVec2(screen_end.x - cross_size, screen_end.y), ImVec2(screen_end.x + cross_size, screen_end.y), cross_col, cross_thickness);
                    draw->AddLine(ImVec2(screen_end.x, screen_end.y - cross_size), ImVec2(screen_end.x, screen_end.y + cross_size), cross_col, cross_thickness);
                }
            }

            ++it;
        }
    }

    float cross_2d(const ImVec2& a, const ImVec2& b, const ImVec2& c)
    {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    }

    std::vector<ImVec2> convex_hull(std::vector<ImVec2> points)
    {
        if (points.size() < 3)
        {
            return points;
        }

        std::sort(points.begin(), points.end(), [](const ImVec2& lhs, const ImVec2& rhs)
            {
                if (lhs.x == rhs.x)
                {
                    return lhs.y < rhs.y;
                }
                return lhs.x < rhs.x;
            });

        std::vector<ImVec2> hull;
        hull.reserve(points.size() * 2);

        for (const auto& pt : points)
        {
            while (hull.size() >= 2 && cross_2d(hull[hull.size() - 2], hull.back(), pt) <= 0.0f)
            {
                hull.pop_back();
            }
            hull.push_back(pt);
        }

        size_t lower_size = hull.size();
        for (int i = static_cast<int>(points.size()) - 2; i >= 0; --i)
        {
            const auto& pt = points[static_cast<size_t>(i)];
            while (hull.size() > lower_size && cross_2d(hull[hull.size() - 2], hull.back(), pt) <= 0.0f)
            {
                hull.pop_back();
            }
            hull.push_back(pt);
        }

        if (!hull.empty())
        {
            hull.pop_back();
        }

        return hull;
    }

    rbx::Vector3 rotate_point(const rbx::mesh_part::transform& tr, const rbx::Vector3& local)
    {
        if (!tr.has_rotation)
        {
            return local;
        }

        rbx::Vector3 out{};
        out.x = tr.rotation[0][0] * local.x + tr.rotation[0][1] * local.y + tr.rotation[0][2] * local.z;
        out.y = tr.rotation[1][0] * local.x + tr.rotation[1][1] * local.y + tr.rotation[1][2] * local.z;
        out.z = tr.rotation[2][0] * local.x + tr.rotation[2][1] * local.y + tr.rotation[2][2] * local.z;
        return out;
    }

    template <typename VertexT>
    bool compute_mesh_bounds_from_vertices(const std::vector<VertexT>& vertices, rbx::Vector3& out_min, rbx::Vector3& out_max)
    {
        bool has_bounds = false;
        for (const auto& vertex : vertices)
        {
            const float x = vertex.x;
            const float y = vertex.y;
            const float z = vertex.z;
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            {
                continue;
            }

            if (!has_bounds)
            {
                out_min = rbx::Vector3(x, y, z);
                out_max = out_min;
                has_bounds = true;
                continue;
            }

            out_min.x = (std::min)(out_min.x, x);
            out_min.y = (std::min)(out_min.y, y);
            out_min.z = (std::min)(out_min.z, z);
            out_max.x = (std::max)(out_max.x, x);
            out_max.y = (std::max)(out_max.y, y);
            out_max.z = (std::max)(out_max.z, z);
        }
        return has_bounds;
    }

    bool compute_mesh_bounds_from_raw_vertices(const std::vector<float>& raw_vertices, rbx::Vector3& out_min, rbx::Vector3& out_max)
    {
        bool has_bounds = false;
        for (std::size_t i = 0; i + 2 < raw_vertices.size(); i += 3)
        {
            const float x = raw_vertices[i];
            const float y = raw_vertices[i + 1u];
            const float z = raw_vertices[i + 2u];
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            {
                continue;
            }

            if (!has_bounds)
            {
                out_min = rbx::Vector3(x, y, z);
                out_max = out_min;
                has_bounds = true;
                continue;
            }

            out_min.x = (std::min)(out_min.x, x);
            out_min.y = (std::min)(out_min.y, y);
            out_min.z = (std::min)(out_min.z, z);
            out_max.x = (std::max)(out_max.x, x);
            out_max.y = (std::max)(out_max.y, y);
            out_max.z = (std::max)(out_max.z, z);
        }
        return has_bounds;
    }

    struct mesh_vertex_fit
    {
        rbx::Vector3 center{};
        rbx::Vector3 scale{ 1.0f, 1.0f, 1.0f };
        bool valid = false;
    };

    mesh_vertex_fit build_mesh_vertex_fit(const rbx::Vector3& target_size, const rbx::Vector3& local_min, const rbx::Vector3& local_max)
    {
        mesh_vertex_fit fit{};
        const float extent_x = local_max.x - local_min.x;
        const float extent_y = local_max.y - local_min.y;
        const float extent_z = local_max.z - local_min.z;
        if (!std::isfinite(extent_x) || !std::isfinite(extent_y) || !std::isfinite(extent_z))
        {
            return fit;
        }

        const float abs_size_x = std::isfinite(target_size.x) ? std::fabs(target_size.x) : 0.0f;
        const float abs_size_y = std::isfinite(target_size.y) ? std::fabs(target_size.y) : 0.0f;
        const float abs_size_z = std::isfinite(target_size.z) ? std::fabs(target_size.z) : 0.0f;
        constexpr float k_extent_eps = 1e-4f;
        if (abs_size_x <= k_extent_eps && abs_size_y <= k_extent_eps && abs_size_z <= k_extent_eps)
        {
            return fit;
        }

        fit.center = rbx::Vector3(
            (local_min.x + local_max.x) * 0.5f,
            (local_min.y + local_max.y) * 0.5f,
            (local_min.z + local_max.z) * 0.5f);

        auto compute_axis_scale = [&](float extent, float target) -> float
            {
                if (!std::isfinite(extent) || !std::isfinite(target) || extent <= k_extent_eps || target <= k_extent_eps)
                {
                    return 1.0f;
                }
                const float ratio = target / extent;
                if (!std::isfinite(ratio) || ratio <= k_extent_eps)
                {
                    return 1.0f;
                }
                return std::clamp(ratio, 0.01f, 100.0f);
            };

        fit.scale = rbx::Vector3(
            compute_axis_scale(extent_x, abs_size_x),
            compute_axis_scale(extent_y, abs_size_y),
            compute_axis_scale(extent_z, abs_size_z));

        fit.valid = std::isfinite(fit.scale.x) && std::isfinite(fit.scale.y) && std::isfinite(fit.scale.z) &&
            std::isfinite(fit.center.x) && std::isfinite(fit.center.y) && std::isfinite(fit.center.z);
        return fit;
    }

    rbx::Vector3 apply_mesh_vertex_fit(const rbx::Vector3& local, const mesh_vertex_fit& fit)
    {
        if (!fit.valid)
        {
            return local;
        }

        return rbx::Vector3(
            (local.x - fit.center.x) * fit.scale.x,
            (local.y - fit.center.y) * fit.scale.y,
            (local.z - fit.center.z) * fit.scale.z);
    }

    std::vector<const cache::primitive_part*> collect_parts(const cache::character_parts& parts);
    std::optional<Clipper2Lib::PathD> project_part_hull(const cache::primitive_part& part, const camera_frame_t& frame);

    std::uintptr_t hull_cache_key(const cache::primitive_part& part)
    {
        return part.primitive != 0 ? part.primitive : part.instance.get_address();
    }

    bool same_vector3(const rbx::Vector3& a, const rbx::Vector3& b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    bool same_vector2(const rbx::Vector2& a, const rbx::Vector2& b)
    {
        return a.x == b.x && a.y == b.y;
    }

    bool same_matrix(const rbx::Matrix& a, const rbx::Matrix& b)
    {
        return std::memcmp(&a, &b, sizeof(rbx::Matrix)) == 0;
    }

    bool same_transform(const rbx::mesh_part::transform& a, const rbx::mesh_part::transform& b)
    {
        if (!same_vector3(a.position, b.position))
        {
            return false;
        }
        if (a.has_rotation != b.has_rotation)
        {
            return false;
        }
        if (!a.has_rotation)
        {
            return true;
        }
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                if (a.rotation[row][col] != b.rotation[row][col])
                {
                    return false;
                }
            }
        }
        return true;
    }

    void prune_hull_cache(std::unordered_map<std::uintptr_t, highlight_hull_cache_entry>& cache, std::uint64_t frame_id, std::uint64_t max_age)
    {
        for (auto it = cache.begin(); it != cache.end();)
        {
            if (frame_id - it->second.last_used_frame > max_age)
            {
                it = cache.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void prune_mesh_sample_cache(std::unordered_map<std::uint64_t, mesh_sample_cache_entry>& cache, std::uint64_t frame_id, std::uint64_t max_age)
    {
        for (auto it = cache.begin(); it != cache.end();)
        {
            if (frame_id - it->second.last_used_frame > max_age)
            {
                it = cache.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void prune_mesh_wireframe_cache(std::unordered_map<std::uint64_t, mesh_wireframe_cache_entry>& cache, std::uint64_t frame_id, std::uint64_t max_age)
    {
        for (auto it = cache.begin(); it != cache.end();)
        {
            if (frame_id - it->second.last_used_frame > max_age)
            {
                it = cache.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void prune_mesh_material_cache(std::unordered_map<std::uint64_t, mesh_material_cache_entry>& cache, std::uint64_t frame_id, std::uint64_t max_age)
    {
        for (auto it = cache.begin(); it != cache.end();)
        {
            if (frame_id - it->second.last_used_frame > max_age)
            {
                it = cache.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void begin_metallic_mesh_frame()
    {
        metallic_mesh_draw_queue.clear();
        has_metallic_mesh_view_proj = false;
    }

    const mesh_material_cache_entry* get_mesh_material_geometry(std::uint64_t mesh_id)
    {
        constexpr std::uint64_t k_material_mesh_retry_cooldown_frames = 120;

        auto it = mesh_material_cache.find(mesh_id);
        if (it != mesh_material_cache.end())
        {
            if (it->second.valid)
            {
                it->second.last_used_frame = highlight_frame_id;
                return &it->second;
            }

            if (highlight_frame_id >= it->second.last_used_frame &&
                (highlight_frame_id - it->second.last_used_frame) < k_material_mesh_retry_cooldown_frames)
            {
                return nullptr;
            }
        }

        rbx::mesh_parse::mesh_data mesh;
        if (!cache::get_mesh_data(mesh_id, mesh))
        {
            mesh_material_cache_entry failed{};
            failed.last_used_frame = highlight_frame_id;
            failed.valid = false;
            mesh_material_cache.insert_or_assign(mesh_id, std::move(failed));
            return nullptr;
        }

        const std::size_t vertex_count = mesh.vertices.size() / 3u;
        if (vertex_count < 3 || mesh.indices.size() < 3)
        {
            return nullptr;
        }

        mesh_material_cache_entry entry{};
        entry.vertices.reserve(vertex_count);
        for (std::size_t i = 0; i + 2 < mesh.vertices.size(); i += 3)
        {
            entry.vertices.emplace_back(mesh.vertices[i], mesh.vertices[i + 1u], mesh.vertices[i + 2u]);
        }
        entry.has_local_bounds = compute_mesh_bounds_from_raw_vertices(mesh.vertices, entry.local_bounds_min, entry.local_bounds_max);

        entry.indices.reserve(mesh.indices.size());
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            const std::uint32_t a = mesh.indices[i];
            const std::uint32_t b = mesh.indices[i + 1u];
            const std::uint32_t c = mesh.indices[i + 2u];
            if (a >= vertex_count || b >= vertex_count || c >= vertex_count)
            {
                continue;
            }
            entry.indices.push_back(a);
            entry.indices.push_back(b);
            entry.indices.push_back(c);
        }

        entry.last_used_frame = highlight_frame_id;
        entry.valid = !entry.vertices.empty() && !entry.indices.empty();
        auto [new_it, _] = mesh_material_cache.insert_or_assign(mesh_id, std::move(entry));
        if (!new_it->second.valid)
        {
            return nullptr;
        }
        return &new_it->second;
    }

    const mesh_material_cache_entry* get_head_sphere_fallback_geometry()
    {
        static mesh_material_cache_entry sphere{};
        static bool initialized = false;
        if (initialized)
        {
            return sphere.valid ? &sphere : nullptr;
        }
        initialized = true;

        constexpr int k_slices = 18;
        constexpr int k_stacks = 14;
        constexpr float k_pi = 3.14159265358979323846f;
        constexpr float k_two_pi = k_pi * 2.0f;

        sphere.vertices.reserve(static_cast<std::size_t>(k_stacks + 1) * static_cast<std::size_t>(k_slices + 1));
        sphere.indices.reserve(static_cast<std::size_t>(k_stacks) * static_cast<std::size_t>(k_slices) * 6u);

        for (int stack = 0; stack <= k_stacks; ++stack)
        {
            const float v = static_cast<float>(stack) / static_cast<float>(k_stacks);
            const float phi = v * k_pi;
            const float y = std::cos(phi);
            const float r = std::sin(phi);

            for (int slice = 0; slice <= k_slices; ++slice)
            {
                const float u = static_cast<float>(slice) / static_cast<float>(k_slices);
                const float theta = u * k_two_pi;
                const float x = r * std::cos(theta);
                const float z = r * std::sin(theta);
                sphere.vertices.emplace_back(x, y, z);
            }
        }

        for (int stack = 0; stack < k_stacks; ++stack)
        {
            for (int slice = 0; slice < k_slices; ++slice)
            {
                const std::uint32_t a = static_cast<std::uint32_t>(stack * (k_slices + 1) + slice);
                const std::uint32_t b = static_cast<std::uint32_t>((stack + 1) * (k_slices + 1) + slice);
                const std::uint32_t c = a + 1u;
                const std::uint32_t d = b + 1u;

                sphere.indices.insert(sphere.indices.end(), { a, b, c, c, b, d });
            }
        }

        sphere.has_local_bounds = compute_mesh_bounds_from_vertices(sphere.vertices, sphere.local_bounds_min, sphere.local_bounds_max);
        sphere.valid = !sphere.vertices.empty() && !sphere.indices.empty();
        return sphere.valid ? &sphere : nullptr;
    }

    const mesh_material_cache_entry* get_r15_head_fallback_geometry(std::uint64_t& out_mesh_id)
    {
        out_mesh_id = 0;

        std::array<std::uint64_t, 3> candidates{
            cached_r15_head_mesh_asset_id,
            7430070993ull, // Common modern Roblox R15 head mesh asset.
            0ull
        };

        static std::unordered_set<std::uint64_t> ensure_attempted_ids;
        std::unordered_set<std::uint64_t> tried_ids;
        for (const std::uint64_t id : candidates)
        {
            if (id == 0 || !tried_ids.insert(id).second)
            {
                continue;
            }

            if (const auto* geometry = get_mesh_material_geometry(id))
            {
                out_mesh_id = id;
                return geometry;
            }

            if (!ensure_attempted_ids.insert(id).second)
            {
                continue;
            }

            rbx::mesh_parse::mesh_data downloaded{};
            if (!cache::ensure_mesh_data(id, downloaded))
            {
                continue;
            }

            if (const auto* geometry = get_mesh_material_geometry(id))
            {
                out_mesh_id = id;
                return geometry;
            }
        }

        return nullptr;
    }

    bool queue_metallic_mesh_draw(
        const cache::primitive_part& part,
        const camera_frame_t& frame,
        const SimpleVector3& camera_position,
        const ImVec4& color,
        int material_mode)
    {
        if (!part.instance.is_valid())
        {
            return false;
        }

        const auto transform = rbx::mesh_part::get_transform(part.instance, part.primitive);
        if (!transform)
        {
            return false;
        }

        const auto mesh_id = rbx::mesh_part::get_mesh_asset_id(part.instance);
        const mesh_material_cache_entry* geometry = nullptr;
        bool using_box_fallback = false;
        bool using_head_r15_fallback = false;
        bool using_head_sphere_fallback = false;
        std::uint64_t head_r15_fallback_mesh_id = 0;
        std::array<SimpleVector3, 8> box_vertices{};
        std::array<std::uint32_t, 36> box_indices{
            0u, 1u, 2u, 0u, 2u, 3u, // front
            4u, 6u, 5u, 4u, 7u, 6u, // back
            0u, 4u, 5u, 0u, 5u, 1u, // left
            3u, 2u, 6u, 3u, 6u, 7u, // right
            1u, 5u, 6u, 1u, 6u, 2u, // top
            0u, 3u, 7u, 0u, 7u, 4u  // bottom
        };

        if (mesh_id)
        {
            geometry = get_mesh_material_geometry(*mesh_id);
            if (!geometry || geometry->vertices.empty() || geometry->indices.empty())
            {
                geometry = nullptr;
            }
        }

        const float sx = std::isfinite(part.size.x) ? std::fabs(part.size.x) : 0.0f;
        const float sy = std::isfinite(part.size.y) ? std::fabs(part.size.y) : 0.0f;
        const float sz = std::isfinite(part.size.z) ? std::fabs(part.size.z) : 0.0f;
        const float hx = (std::max)(sx * 0.5f, 0.01f);
        const float hy = (std::max)(sy * 0.5f, 0.01f);
        const float hz = (std::max)(sz * 0.5f, 0.01f);
        float head_scale_x = hx;
        float head_scale_y = hy;
        float head_scale_z = hz;
        rbx::Vector3 head_local_offset(0.0f, 0.0f, 0.0f);

        if (!geometry)
        {
            const std::string part_name = part.instance.get_name();
            const bool looks_like_head = (part_name == "Head");
            const bool has_special_mesh_child = looks_like_head && part.instance.find_first_child_by_class("SpecialMesh").is_valid();
            if (has_special_mesh_child)
            {
                // Roblox SpecialMesh MeshType.Head often has no MeshId.
                // Prefer a real R15 head mesh fallback (cached/known id) before primitive fallback.
                if (const auto* r15_head = get_r15_head_fallback_geometry(head_r15_fallback_mesh_id))
                {
                    constexpr float k_head_r15_offset_factor = -0.08f;
                    head_scale_x = (std::max)(0.04f, hx * 1.05f);
                    head_scale_y = (std::max)(0.04f, hy * 1.00f);
                    head_scale_z = (std::max)(0.04f, hz * 1.05f);
                    head_local_offset = rbx::Vector3(0.0f, head_scale_y * k_head_r15_offset_factor, 0.0f);
                    geometry = r15_head;
                    using_head_r15_fallback = true;
                }
                else if (const auto* head_sphere = get_head_sphere_fallback_geometry())
                {
                    constexpr float k_head_sphere_offset_factor = -0.18f;
                    head_scale_x = (std::max)(0.04f, hx * 1.05f);
                    head_scale_y = (std::max)(0.04f, hy * 1.00f);
                    head_scale_z = (std::max)(0.04f, hz * 1.05f);
                    head_local_offset = rbx::Vector3(0.0f, head_scale_y * k_head_sphere_offset_factor, 0.0f);
                    geometry = head_sphere;
                    using_head_sphere_fallback = true;
                }
            }

            if (!geometry)
            {
                box_vertices = {
                    SimpleVector3(-hx, -hy, -hz),
                    SimpleVector3(-hx,  hy, -hz),
                    SimpleVector3( hx,  hy, -hz),
                    SimpleVector3( hx, -hy, -hz),
                    SimpleVector3(-hx, -hy,  hz),
                    SimpleVector3(-hx,  hy,  hz),
                    SimpleVector3( hx,  hy,  hz),
                    SimpleVector3( hx, -hy,  hz)
                };
                using_box_fallback = true;
            }
        }
        else
        {
            const std::string part_name = part.instance.get_name();
            const std::string part_class = part.instance.get_class_name();
            if (part_class == "MeshPart" && part_name == "Head")
            {
                cached_r15_head_mesh_asset_id = *mesh_id;
            }
        }

        metallic_mesh_draw_command command{};
        command.color = color;
        command.material_mode = material_mode;
        mesh_vertex_fit mesh_fit{};
        if (!using_box_fallback && !using_head_r15_fallback && !using_head_sphere_fallback &&
            geometry && geometry->has_local_bounds)
        {
            mesh_fit = build_mesh_vertex_fit(part.size, geometry->local_bounds_min, geometry->local_bounds_max);
        }
        if (using_box_fallback)
        {
            command.indices.assign(box_indices.begin(), box_indices.end());
            command.world_vertices.reserve(box_vertices.size());
            for (const auto& local : box_vertices)
            {
                const rbx::Vector3 local_rbx(local.x, local.y, local.z);
                const rbx::Vector3 world = transform->position + rotate_point(*transform, local_rbx);
                command.world_vertices.emplace_back(world.x, world.y, world.z);
            }
        }
        else if (using_head_r15_fallback)
        {
            float min_x = FLT_MAX, min_y = FLT_MAX, min_z = FLT_MAX;
            float max_x = -FLT_MAX, max_y = -FLT_MAX, max_z = -FLT_MAX;
            for (const auto& local : geometry->vertices)
            {
                min_x = (std::min)(min_x, local.x);
                min_y = (std::min)(min_y, local.y);
                min_z = (std::min)(min_z, local.z);
                max_x = (std::max)(max_x, local.x);
                max_y = (std::max)(max_y, local.y);
                max_z = (std::max)(max_z, local.z);
            }

            const float extent_x = (std::max)(max_x - min_x, 0.001f);
            const float extent_y = (std::max)(max_y - min_y, 0.001f);
            const float extent_z = (std::max)(max_z - min_z, 0.001f);
            const float center_x = (min_x + max_x) * 0.5f;
            const float center_y = (min_y + max_y) * 0.5f;
            const float center_z = (min_z + max_z) * 0.5f;
            const float target_x = head_scale_x * 2.0f;
            const float target_y = head_scale_y * 2.0f;
            const float target_z = head_scale_z * 2.0f;
            const float uniform_scale = (std::max)(
                0.001f,
                (std::min)(target_x / extent_x, (std::min)(target_y / extent_y, target_z / extent_z)));

            command.indices = geometry->indices;
            command.world_vertices.reserve(geometry->vertices.size());
            for (const auto& local : geometry->vertices)
            {
                const rbx::Vector3 local_rbx(
                    (local.x - center_x) * uniform_scale + head_local_offset.x,
                    (local.y - center_y) * uniform_scale + head_local_offset.y,
                    (local.z - center_z) * uniform_scale + head_local_offset.z);
                const rbx::Vector3 world = transform->position + rotate_point(*transform, local_rbx);
                command.world_vertices.emplace_back(world.x, world.y, world.z);
            }
        }
        else if (using_head_sphere_fallback)
        {
            command.indices = geometry->indices;
            command.world_vertices.reserve(geometry->vertices.size());
            for (const auto& local : geometry->vertices)
            {
                const rbx::Vector3 local_rbx(
                    local.x * head_scale_x + head_local_offset.x,
                    local.y * head_scale_y + head_local_offset.y,
                    local.z * head_scale_z + head_local_offset.z);
                const rbx::Vector3 world = transform->position + rotate_point(*transform, local_rbx);
                command.world_vertices.emplace_back(world.x, world.y, world.z);
            }
        }
        else
        {
            command.indices = geometry->indices;
            command.world_vertices.reserve(geometry->vertices.size());
            for (const auto& local : geometry->vertices)
            {
                const auto fitted = apply_mesh_vertex_fit(local, mesh_fit);
                const rbx::Vector3 local_rbx(fitted.x, fitted.y, fitted.z);
                const rbx::Vector3 world = transform->position + rotate_point(*transform, local_rbx);
                command.world_vertices.emplace_back(world.x, world.y, world.z);
            }
        }

        if (command.world_vertices.empty() || command.indices.empty())
        {
            return false;
        }

        metallic_mesh_draw_queue.push_back(std::move(command));
        metallic_mesh_view_proj = frame.view_matrix;
        metallic_mesh_camera_pos = camera_position;
        has_metallic_mesh_view_proj = true;
        return true;
    }

    bool ensure_metallic_vertex_buffer(ID3D11Device* device, std::size_t vertex_count)
    {
        if (!device || vertex_count == 0)
        {
            return false;
        }

        if (metallic_pipeline.vertex_buffer && metallic_pipeline.vertex_capacity >= vertex_count)
        {
            return true;
        }

        const std::size_t next_capacity = (std::max)(
            vertex_count,
            (std::max)(std::size_t(2048), metallic_pipeline.vertex_capacity > 0 ? metallic_pipeline.vertex_capacity * 2 : std::size_t(0)));

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(next_capacity * sizeof(SimpleVector3));
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &buffer)))
        {
            return false;
        }

        metallic_pipeline.vertex_buffer = std::move(buffer);
        metallic_pipeline.vertex_capacity = next_capacity;
        return true;
    }

    bool ensure_metallic_index_buffer(ID3D11Device* device, std::size_t index_count)
    {
        if (!device || index_count == 0)
        {
            return false;
        }

        if (metallic_pipeline.index_buffer && metallic_pipeline.index_capacity >= index_count)
        {
            return true;
        }

        const std::size_t next_capacity = (std::max)(
            index_count,
            (std::max)(std::size_t(6144), metallic_pipeline.index_capacity > 0 ? metallic_pipeline.index_capacity * 2 : std::size_t(0)));

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(next_capacity * sizeof(std::uint32_t));
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &buffer)))
        {
            return false;
        }

        metallic_pipeline.index_buffer = std::move(buffer);
        metallic_pipeline.index_capacity = next_capacity;
        return true;
    }

    bool ensure_metallic_depth_buffer(ID3D11Device* device, UINT width, UINT height)
    {
        if (!device || width == 0 || height == 0)
        {
            return false;
        }

        if (metallic_pipeline.depth_texture &&
            metallic_pipeline.depth_stencil_view &&
            metallic_pipeline.depth_width == width &&
            metallic_pipeline.depth_height == height)
        {
            return true;
        }

        metallic_pipeline.depth_texture.Reset();
        metallic_pipeline.depth_stencil_view.Reset();

        D3D11_TEXTURE2D_DESC depth_desc{};
        depth_desc.Width = width;
        depth_desc.Height = height;
        depth_desc.MipLevels = 1;
        depth_desc.ArraySize = 1;
        depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count = 1;
        depth_desc.Usage = D3D11_USAGE_DEFAULT;
        depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_texture;
        if (FAILED(device->CreateTexture2D(&depth_desc, nullptr, &depth_texture)))
        {
            return false;
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = depth_desc.Format;
        dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dsv_desc.Texture2D.MipSlice = 0;

        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_view;
        if (FAILED(device->CreateDepthStencilView(depth_texture.Get(), &dsv_desc, &depth_view)))
        {
            return false;
        }

        metallic_pipeline.depth_texture = std::move(depth_texture);
        metallic_pipeline.depth_stencil_view = std::move(depth_view);
        metallic_pipeline.depth_width = width;
        metallic_pipeline.depth_height = height;
        return true;
    }

    bool initialize_metallic_pipeline(ID3D11Device* device)
    {
        if (!device)
        {
            return false;
        }
        if (metallic_pipeline.initialized)
        {
            return true;
        }

        static constexpr const char* k_metallic_shader_source = R"(
cbuffer MetallicConstants : register(b0)
{
    row_major float4x4 ViewProj;
    float4 BaseColor;
    float3 CameraPos;
    float Time;
    float MaterialMode;
    float3 Padding;
};

struct VS_INPUT
{
    float3 pos : POSITION;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float3 world_pos : TEXCOORD0;
};

PS_INPUT VS_Main(VS_INPUT input)
{
    PS_INPUT output;
    output.world_pos = input.pos;
    float4 world = float4(input.pos, 1.0f);
    output.pos = float4(
        dot(world, float4(ViewProj[0][0], ViewProj[0][1], ViewProj[0][2], ViewProj[0][3])),
        dot(world, float4(ViewProj[1][0], ViewProj[1][1], ViewProj[1][2], ViewProj[1][3])),
        dot(world, float4(ViewProj[2][0], ViewProj[2][1], ViewProj[2][2], ViewProj[2][3])),
        dot(world, float4(ViewProj[3][0], ViewProj[3][1], ViewProj[3][2], ViewProj[3][3]))
    );
    return output;
}

float4 PS_Main(PS_INPUT input) : SV_TARGET
{
    float3 p = input.world_pos;
    float3 view_dir = normalize(CameraPos - p);
    float3 dx = ddx(p);
    float3 dy = ddy(p);
    // Derivative normals can be flipped depending on winding/projection orientation.
    // Force them to face the camera so front/back lighting is stable.
    float3 normal = normalize(cross(dy, dx));
    if (dot(normal, view_dir) < 0.0f)
    {
        normal = -normal;
    }

    float3 light_dir = normalize(float3(-0.35f, 0.82f, 0.45f));
    float3 half_vec = normalize(light_dir + view_dir);
    float ndotl = saturate(dot(normal, light_dir));
    float ndotv = saturate(dot(normal, view_dir));
    float vdoth = saturate(dot(view_dir, half_vec));
    float rim = 1.0f - ndotv;
    float rim_pow = rim * rim;
    rim_pow *= rim_pow;

    float3 reflect_dir = reflect(-view_dir, normal);
    float env_factor = saturate(reflect_dir.y * 0.5f + 0.5f);
    float horizon = saturate(1.0f - abs(reflect_dir.y));

    float3 env_sky = float3(0.22f, 0.30f, 0.40f);
    float3 env_ground = float3(0.03f, 0.04f, 0.05f);
    float3 env = lerp(env_ground, env_sky, env_factor);
    env += float3(0.08f, 0.09f, 0.11f) * horizon;

    float specular_wide = pow(saturate(dot(normal, half_vec)), 28.0f);
    float specular_tight = pow(saturate(dot(normal, half_vec)), 180.0f);
    float3 dielectric_f0 = float3(0.04f, 0.04f, 0.04f);
    float3 dielectric_fresnel = dielectric_f0 + (1.0f - dielectric_f0) * pow(1.0f - vdoth, 5.0f);

    float3 final_color = BaseColor.rgb;
    float alpha = BaseColor.a;
    int mode = (int)(MaterialMode + 0.5f);

    if (mode == 2)
    {
        // Rec. 709 luminance coefficients.
        float mono = dot(BaseColor.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        float3 mono_base = mono.xxx;
        float3 mono_f0 = float3(0.06f, 0.06f, 0.06f);
        float3 mono_fresnel = mono_f0 + (1.0f - mono_f0) * pow(1.0f - vdoth, 5.0f);
        final_color = mono_base * (0.20f + ndotl * 0.30f);
        final_color += env * 0.70f;
        final_color += mono_fresnel * specular_wide * 0.65f;
        final_color += mono_fresnel * specular_tight * 1.10f;
        final_color += rim_pow * 0.22f;
        alpha = BaseColor.a * (0.84f + specular_tight * 0.08f + rim_pow * 0.08f);
    }
    else if (mode == 3)
    {
        // Acrylic is treated as a dielectric surface with a strong grazing-angle response.
        float3 acrylic_f0 = float3(0.04f, 0.04f, 0.04f);
        float3 acrylic_fresnel = acrylic_f0 + (1.0f - acrylic_f0) * pow(1.0f - vdoth, 5.0f);
        float3 tint = lerp(BaseColor.rgb * 0.55f, BaseColor.rgb, 0.65f);
        final_color = tint * (0.08f + ndotl * 0.18f);
        final_color += env * 1.05f;
        final_color += acrylic_fresnel * specular_wide * 0.75f;
        final_color += acrylic_fresnel * specular_tight * 0.85f;
        final_color += rim_pow * 0.38f;
        alpha = BaseColor.a * (0.55f + rim * 0.25f + specular_wide * 0.12f);
    }
    else if (mode == 4)
    {
        float glow_core = pow(saturate(ndotl), 0.6f);
        float glow_rim = pow(rim, 1.25f);
        float3 emission = BaseColor.rgb * (0.75f + glow_core * 1.10f + glow_rim * 0.65f);
        final_color = emission;
        final_color += env * 0.20f;
        final_color += dielectric_fresnel * specular_tight * 0.22f;
        alpha = BaseColor.a * (0.92f + glow_rim * 0.06f);
    }
    else
    {
        float3 metallic_f0 = saturate(lerp(float3(0.08f, 0.08f, 0.08f), BaseColor.rgb, 0.98f));
        float3 metallic_fresnel = metallic_f0 + (1.0f - metallic_f0) * pow(1.0f - vdoth, 5.0f);
        float3 metal_tint = saturate(lerp(BaseColor.rgb, float3(1.0f, 1.0f, 1.0f), 0.18f));
        float3 spec_tint = saturate(lerp(metal_tint, float3(1.0f, 1.0f, 1.0f), 0.25f));
        float tight_glint = pow(saturate(dot(normal, half_vec)), 420.0f);
        float3 base = BaseColor.rgb * (0.06f + ndotl * 0.14f);
        final_color = base;
        final_color += env * metal_tint * 1.35f;
        final_color += metallic_fresnel * specular_wide * 1.25f * spec_tint;
        final_color += metallic_fresnel * specular_tight * 2.15f * spec_tint;
        final_color += metallic_fresnel * tight_glint * 0.85f * spec_tint;
        final_color += rim_pow * metal_tint * 0.34f;
        alpha = BaseColor.a;
    }

    alpha = 1.0f;
    return float4(saturate(final_color), saturate(alpha));
}
)";

        UINT compile_flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compile_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        Microsoft::WRL::ComPtr<ID3DBlob> vs_blob;
        Microsoft::WRL::ComPtr<ID3DBlob> ps_blob;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;

        if (FAILED(D3DCompile(k_metallic_shader_source, std::strlen(k_metallic_shader_source), nullptr, nullptr, nullptr, "VS_Main", "vs_4_0", compile_flags, 0, &vs_blob, &errors)))
        {
            return false;
        }

        if (FAILED(D3DCompile(k_metallic_shader_source, std::strlen(k_metallic_shader_source), nullptr, nullptr, nullptr, "PS_Main", "ps_4_0", compile_flags, 0, &ps_blob, &errors)))
        {
            return false;
        }

        if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &metallic_pipeline.vertex_shader)))
        {
            return false;
        }

        if (FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &metallic_pipeline.pixel_shader)))
        {
            return false;
        }

        constexpr D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}
        };

        if (FAILED(device->CreateInputLayout(layout, 1, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &metallic_pipeline.input_layout)))
        {
            return false;
        }

        D3D11_BUFFER_DESC cb_desc{};
        cb_desc.ByteWidth = sizeof(metallic_shader_constants);
        cb_desc.Usage = D3D11_USAGE_DYNAMIC;
        cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cb_desc, nullptr, &metallic_pipeline.constant_buffer)))
        {
            return false;
        }

        D3D11_RASTERIZER_DESC rs_desc{};
        rs_desc.FillMode = D3D11_FILL_SOLID;
        rs_desc.CullMode = D3D11_CULL_NONE;
        rs_desc.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&rs_desc, &metallic_pipeline.rasterizer_state)))
        {
            return false;
        }

        D3D11_DEPTH_STENCIL_DESC depth_desc{};
        depth_desc.DepthEnable = TRUE;
        depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_desc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        depth_desc.StencilEnable = FALSE;
        if (FAILED(device->CreateDepthStencilState(&depth_desc, &metallic_pipeline.depth_state)))
        {
            return false;
        }

        D3D11_BLEND_DESC blend_desc{};
        blend_desc.RenderTarget[0].BlendEnable = TRUE;
        blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&blend_desc, &metallic_pipeline.blend_state)))
        {
            return false;
        }

        metallic_pipeline.initialized = true;
        return true;
    }

    void write_matrix_row_major(const SimpleMatrix& matrix, float out[16])
    {
        out[0] = matrix._11; out[1] = matrix._12; out[2] = matrix._13; out[3] = matrix._14;
        out[4] = matrix._21; out[5] = matrix._22; out[6] = matrix._23; out[7] = matrix._24;
        out[8] = matrix._31; out[9] = matrix._32; out[10] = matrix._33; out[11] = matrix._34;
        out[12] = matrix._41; out[13] = matrix._42; out[14] = matrix._43; out[15] = matrix._44;
    }

    bool update_metallic_constants(ID3D11DeviceContext* context, const ImVec4& color, int material_mode)
    {
        if (!context || !metallic_pipeline.constant_buffer)
        {
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(metallic_pipeline.constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            return false;
        }

        auto* constants = static_cast<metallic_shader_constants*>(mapped.pData);
        write_matrix_row_major(metallic_mesh_view_proj, constants->view_proj);
        constants->color[0] = color.x;
        constants->color[1] = color.y;
        constants->color[2] = color.z;
        constants->color[3] = color.w;
        constants->camera_pos[0] = metallic_mesh_camera_pos.x;
        constants->camera_pos[1] = metallic_mesh_camera_pos.y;
        constants->camera_pos[2] = metallic_mesh_camera_pos.z;
        constants->time = static_cast<float>(GetTickCount64() * 0.001);
        constants->material_mode = static_cast<float>(material_mode);

        context->Unmap(metallic_pipeline.constant_buffer.Get(), 0);
        return true;
    }


    struct desync_highlight_state
    {
        bool has_snapshot = false;
        std::vector<std::vector<rbx::Vector3>> world_hulls;
        ImVec4 fill_color{};
        ImVec4 outline_color{};
    };

    static desync_highlight_state g_desync_highlight;

    void clear_desync_highlight_snapshot()
    {
        g_desync_highlight.has_snapshot = false;
        g_desync_highlight.world_hulls.clear();
    }

    std::optional<std::vector<rbx::Vector3>> build_world_corners(const cache::primitive_part& part)
    {
        const auto transform = rbx::mesh_part::get_transform(part.instance, part.primitive);
        if (!transform)
        {
            return std::nullopt;
        }

        const rbx::Vector3 size = part.size;
        if (!(std::isfinite(size.x) && std::isfinite(size.y) && std::isfinite(size.z)) ||
            size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f)
        {
            return std::nullopt;
        }

        const rbx::Vector3 half = size * 0.5f;
        const rbx::Vector3 base_pos = transform->position;
        std::array<rbx::Vector3, 8> corners = {
            rbx::Vector3(-half.x, -half.y, -half.z),
            rbx::Vector3(half.x, -half.y, -half.z),
            rbx::Vector3(-half.x,  half.y, -half.z),
            rbx::Vector3(half.x,  half.y, -half.z),
            rbx::Vector3(-half.x, -half.y,  half.z),
            rbx::Vector3(half.x, -half.y,  half.z),
            rbx::Vector3(-half.x,  half.y,  half.z),
            rbx::Vector3(half.x,  half.y,  half.z)
        };

        std::vector<rbx::Vector3> world_corners;
        world_corners.reserve(corners.size());
        for (const auto& local : corners)
        {
            world_corners.emplace_back(base_pos + rotate_point(*transform, local));
        }

        return world_corners;
    }

    bool capture_desync_highlight_snapshot(const cache::local_player_state& local)
    {
        clear_desync_highlight_snapshot();

        const auto parts = collect_parts(local.parts);
        g_desync_highlight.world_hulls.reserve(parts.size());
        for (const auto* part : parts)
        {
            if (!part)
            {
                continue;
            }
            if (auto corners = build_world_corners(*part))
            {
                g_desync_highlight.world_hulls.push_back(std::move(*corners));
            }
        }

        if (g_desync_highlight.world_hulls.empty())
        {
            return false;
        }

        g_desync_highlight.fill_color = features->highlight_fill_color;
        g_desync_highlight.outline_color = features->highlight_outline_color;
        g_desync_highlight.has_snapshot = true;
        return true;
    }

    std::vector<Clipper2Lib::PathD> project_world_hulls(
        const std::vector<std::vector<rbx::Vector3>>& world_hulls,
        const camera_frame_t& frame)
    {
        std::vector<Clipper2Lib::PathD> projected;
        projected.reserve(world_hulls.size());

        for (const auto& hull : world_hulls)
        {
            std::vector<ImVec2> points;
            points.reserve(hull.size());
            for (const auto& pt : hull)
            {
                if (const auto screen = rbx::camera::world_to_screen(pt, frame.view_matrix, frame.dimensions))
                {
                    points.emplace_back(screen->x, screen->y);
                }
            }

            if (points.size() < 3)
            {
                continue;
            }

            auto hull_2d = convex_hull(std::move(points));
            if (hull_2d.size() < 3)
            {
                continue;
            }

            Clipper2Lib::PathD path;
            path.reserve(hull_2d.size());
            for (const auto& p : hull_2d)
            {
                path.emplace_back(p.x, p.y);
            }
            projected.push_back(std::move(path));
        }

        return projected;
    }

    std::vector<Clipper2Lib::PathD> project_desync_hulls(const camera_frame_t& frame)
    {
        return project_world_hulls(g_desync_highlight.world_hulls, frame);
    }

    void build_dead_body_snapshot(const cache::dead_body_state& body, const camera_frame_t& frame, dead_body_render_snapshot& out)
    {
        out.hulls.clear();
        out.torso_bounds.reset();

        const cache::primitive_part* torso_part = &body.parts.torso;
        const cache::primitive_part* part_list[] = {
            &body.parts.head,
            torso_part,
            &body.parts.left_arm,
            &body.parts.right_arm,
            &body.parts.left_leg,
            &body.parts.right_leg
        };

        out.hulls.reserve(sizeof(part_list) / sizeof(part_list[0]));

        for (const auto* part : part_list)
        {
            if (!part || !part->instance.is_valid())
            {
                continue;
            }

            auto hull = project_part_hull(*part, frame);
            if (!hull)
            {
                continue;
            }

            if (part == torso_part)
            {
                float min_x = (std::numeric_limits<float>::max)();
                float min_y = (std::numeric_limits<float>::max)();
                float max_x = -(std::numeric_limits<float>::max)();
                float max_y = -(std::numeric_limits<float>::max)();
                for (const auto& pt : *hull)
                {
                    const float x = static_cast<float>(pt.x);
                    const float y = static_cast<float>(pt.y);
                    min_x = (std::min)(min_x, x);
                    min_y = (std::min)(min_y, y);
                    max_x = (std::max)(max_x, x);
                    max_y = (std::max)(max_y, y);
                }
                if (min_x < max_x && min_y < max_y)
                {
                    min_x = std::clamp(min_x, 0.0f, frame.dimensions.x);
                    min_y = std::clamp(min_y, 0.0f, frame.dimensions.y);
                    max_x = std::clamp(max_x, 0.0f, frame.dimensions.x);
                    max_y = std::clamp(max_y, 0.0f, frame.dimensions.y);
                    if (min_x < max_x && min_y < max_y)
                    {
                        out.torso_bounds = ImRect(ImVec2(min_x, min_y), ImVec2(max_x, max_y));
                    }
                }
            }

            out.hulls.push_back(std::move(*hull));
        }
    }

    std::optional<Clipper2Lib::PathD> project_part_hull(const cache::primitive_part& part, const camera_frame_t& frame)
    {
        const auto transform = rbx::mesh_part::get_transform(part.instance, part.primitive);
        if (!transform)
        {
            return std::nullopt;
        }

        const rbx::Vector3 size = part.size;
        const std::uintptr_t cache_key = hull_cache_key(part);
        if (cache_key != 0)
        {
            auto it = part_hull_cache.find(cache_key);
            if (it != part_hull_cache.end())
            {
                auto& entry = it->second;
                if (entry.valid &&
                    same_vector3(entry.size, size) &&
                    same_transform(entry.transform, *transform) &&
                    same_matrix(entry.view_matrix, frame.view_matrix) &&
                    same_vector2(entry.dimensions, frame.dimensions))
                {
                    entry.last_used_frame = highlight_frame_id;
                    if (entry.has_hull)
                    {
                        return entry.hull;
                    }
                    return std::nullopt;
                }
            }
        }

        if (!(std::isfinite(size.x) && std::isfinite(size.y) && std::isfinite(size.z)) ||
            size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f)
        {
            return std::nullopt;
        }

        const rbx::Vector3 half = size * 0.5f;
        const rbx::Vector3 base_pos = transform->position;
        std::array<rbx::Vector3, 8> corners = {
            rbx::Vector3(-half.x, -half.y, -half.z),
            rbx::Vector3(half.x, -half.y, -half.z),
            rbx::Vector3(-half.x,  half.y, -half.z),
            rbx::Vector3(half.x,  half.y, -half.z),
            rbx::Vector3(-half.x, -half.y,  half.z),
            rbx::Vector3(half.x, -half.y,  half.z),
            rbx::Vector3(-half.x,  half.y,  half.z),
            rbx::Vector3(half.x,  half.y,  half.z)
        };

        std::vector<ImVec2> projected;
        projected.reserve(corners.size());
        for (const auto& local : corners)
        {
            const rbx::Vector3 world = base_pos + rotate_point(*transform, local);
            const auto screen = rbx::camera::world_to_screen(world, frame.view_matrix, frame.dimensions);
            if (!screen)
            {
                continue;
            }
            projected.emplace_back(screen->x, screen->y);
        }

        if (projected.size() < 3)
        {
            return std::nullopt;
        }

        auto hull = convex_hull(projected);
        if (hull.size() < 3)
        {
            return std::nullopt;
        }

        Clipper2Lib::PathD path;
        path.reserve(hull.size());
        for (const auto& pt : hull)
        {
            path.emplace_back(pt.x, pt.y);
        }

        if (cache_key != 0)
        {
            auto& entry = part_hull_cache[cache_key];
            entry.size = size;
            entry.transform = *transform;
            entry.view_matrix = frame.view_matrix;
            entry.dimensions = frame.dimensions;
            entry.hull = path;
            entry.last_used_frame = highlight_frame_id;
            entry.has_hull = true;
            entry.valid = true;
        }
        return path;
    }

    std::optional<Clipper2Lib::PathD> project_mesh_hull(const cache::primitive_part& part, const camera_frame_t& frame, std::size_t max_samples)
    {
        constexpr std::size_t k_mesh_sample_cache_points = 400;

        if (!part.instance.is_valid())
        {
            return std::nullopt;
        }

        const std::uintptr_t cache_key = hull_cache_key(part);
        const auto cached_transform = rbx::mesh_part::get_transform(part.instance, part.primitive);
        if (!cached_transform)
        {
            return std::nullopt;
        }

        if (cache_key != 0)
        {
            auto it = mesh_hull_cache.find(cache_key);
            if (it != mesh_hull_cache.end())
            {
                auto& entry = it->second;
                if (entry.valid &&
                    same_vector3(entry.size, part.size) &&
                    same_transform(entry.transform, *cached_transform) &&
                    same_matrix(entry.view_matrix, frame.view_matrix) &&
                    same_vector2(entry.dimensions, frame.dimensions))
                {
                    entry.last_used_frame = highlight_frame_id;
                    if (entry.has_hull)
                    {
                        return entry.hull;
                    }
                    return std::nullopt;
                }
            }
        }

        auto mesh_id = rbx::mesh_part::get_mesh_asset_id(part.instance);
        if (!mesh_id)
        {
            return std::nullopt;
        }

        std::vector<ImVec2> projected;
        const std::vector<rbx::Vector3>* samples = nullptr;
        const mesh_sample_cache_entry* sample_entry = nullptr;
        {
            auto it = mesh_sample_cache.find(*mesh_id);
            if (it != mesh_sample_cache.end() && it->second.valid)
            {
                it->second.last_used_frame = highlight_frame_id;
                samples = &it->second.samples;
                sample_entry = &it->second;
            }
            else
            {
                rbx::mesh_parse::mesh_data mesh;
                if (!cache::get_mesh_data(*mesh_id, mesh))
                {
                    return std::nullopt;
                }
                if (mesh.vertices.size() < 3)
                {
                    return std::nullopt;
                }

                mesh_sample_cache_entry entry{};
                const std::size_t total_vertices = mesh.vertices.size() / 3;
                const std::size_t mesh_stride = std::max<std::size_t>(1, (total_vertices + k_mesh_sample_cache_points - 1) / k_mesh_sample_cache_points);
                entry.samples.reserve(std::min<std::size_t>(total_vertices, k_mesh_sample_cache_points));
                for (size_t i = 0; i + 2 < mesh.vertices.size(); i += mesh_stride * 3)
                {
                    entry.samples.emplace_back(mesh.vertices[i], mesh.vertices[i + 1], mesh.vertices[i + 2]);
                }
                entry.has_local_bounds = compute_mesh_bounds_from_raw_vertices(mesh.vertices, entry.local_bounds_min, entry.local_bounds_max);
                entry.last_used_frame = highlight_frame_id;
                entry.valid = !entry.samples.empty();
                auto [new_it, _] = mesh_sample_cache.insert_or_assign(*mesh_id, std::move(entry));
                samples = &new_it->second.samples;
                sample_entry = &new_it->second;
            }
        }

        if (!samples || samples->size() < 3)
        {
            return std::nullopt;
        }

        mesh_vertex_fit mesh_fit{};
        if (sample_entry && sample_entry->has_local_bounds)
        {
            mesh_fit = build_mesh_vertex_fit(part.size, sample_entry->local_bounds_min, sample_entry->local_bounds_max);
        }

        if (max_samples < 3)
        {
            max_samples = 3;
        }
        const std::size_t sample_count = samples->size();
        const std::size_t stride = (max_samples >= sample_count)
            ? 1
            : (sample_count + max_samples - 1) / max_samples;

        projected.reserve((sample_count + stride - 1) / stride);
        for (std::size_t i = 0; i < sample_count; i += stride)
        {
            const rbx::Vector3 local = apply_mesh_vertex_fit((*samples)[i], mesh_fit);
            rbx::Vector3 world = cached_transform->position + rotate_point(*cached_transform, local);
            if (const auto screen = rbx::camera::world_to_screen(world, frame.view_matrix, frame.dimensions))
            {
                projected.emplace_back(screen->x, screen->y);
            }
        }

        if (projected.size() < 3)
        {
            return std::nullopt;
        }

        auto hull = convex_hull(std::move(projected));
        if (hull.size() < 3)
        {
            return std::nullopt;
        }

        Clipper2Lib::PathD path;
        path.reserve(hull.size());
        for (const auto& pt : hull)
        {
            path.emplace_back(pt.x, pt.y);
        }

        if (cache_key != 0)
        {
            auto& entry = mesh_hull_cache[cache_key];
            entry.size = part.size;
            entry.transform = *cached_transform;
            entry.view_matrix = frame.view_matrix;
            entry.dimensions = frame.dimensions;
            entry.hull = path;
            entry.last_used_frame = highlight_frame_id;
            entry.has_hull = true;
            entry.valid = true;
        }
        return path;
    }

    std::uint64_t build_wireframe_edge_key(std::uint32_t a, std::uint32_t b)
    {
        if (a > b)
        {
            std::swap(a, b);
        }
        return (static_cast<std::uint64_t>(a) << 32) | static_cast<std::uint64_t>(b);
    }

    const mesh_wireframe_cache_entry* get_mesh_wireframe_edges(std::uint64_t mesh_id)
    {
        auto it = mesh_wireframe_cache.find(mesh_id);
        if (it != mesh_wireframe_cache.end() && it->second.valid)
        {
            it->second.last_used_frame = highlight_frame_id;
            return &it->second;
        }

        rbx::mesh_parse::mesh_data mesh;
        if (!cache::get_mesh_data(mesh_id, mesh))
        {
            return nullptr;
        }

        const std::size_t vertex_count = mesh.vertices.size() / 3u;
        if (vertex_count == 0 || mesh.indices.size() < 3)
        {
            return nullptr;
        }

        constexpr std::size_t k_wireframe_reduce_threshold = 2000;
        const bool should_reduce = (vertex_count > k_wireframe_reduce_threshold) || (mesh.indices.size() > k_wireframe_reduce_threshold);

        mesh_wireframe_cache_entry entry{};
        entry.last_used_frame = highlight_frame_id;
        entry.valid = false;
        entry.has_local_bounds = compute_mesh_bounds_from_raw_vertices(mesh.vertices, entry.local_bounds_min, entry.local_bounds_max);

        std::unordered_set<std::uint64_t> edge_keys;
        edge_keys.reserve(mesh.indices.size());
        entry.edges.reserve(mesh.indices.size());

        auto add_edge = [&](std::uint32_t ia, std::uint32_t ib)
            {
                if (ia >= vertex_count || ib >= vertex_count || ia == ib)
                {
                    return;
                }

                const std::uint64_t key = build_wireframe_edge_key(ia, ib);
                if (!edge_keys.insert(key).second)
                {
                    return;
                }

                const std::size_t a_base = static_cast<std::size_t>(ia) * 3u;
                const std::size_t b_base = static_cast<std::size_t>(ib) * 3u;
                entry.edges.push_back(mesh_wireframe_edge{
                    rbx::Vector3(mesh.vertices[a_base], mesh.vertices[a_base + 1u], mesh.vertices[a_base + 2u]),
                    rbx::Vector3(mesh.vertices[b_base], mesh.vertices[b_base + 1u], mesh.vertices[b_base + 2u])
                    });
            };

        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            if (should_reduce)
            {
                const std::size_t triangle_index = i / 3u;
                if ((triangle_index % 4u) == 0u)
                {
                    continue;
                }
            }

            const std::uint32_t a = mesh.indices[i];
            const std::uint32_t b = mesh.indices[i + 1];
            const std::uint32_t c = mesh.indices[i + 2];
            add_edge(a, b);
            add_edge(b, c);
            add_edge(c, a);
        }

        entry.valid = !entry.edges.empty();
        auto [new_it, _] = mesh_wireframe_cache.insert_or_assign(mesh_id, std::move(entry));
        if (!new_it->second.valid)
        {
            return nullptr;
        }

        return &new_it->second;
    }

    bool project_mesh_wireframe_edges(
        const cache::primitive_part& part,
        const camera_frame_t& frame,
        std::vector<projected_wireframe_segment>& out_segments)
    {
        if (!part.instance.is_valid())
        {
            return false;
        }

        const auto transform = rbx::mesh_part::get_transform(part.instance, part.primitive);
        if (!transform)
        {
            return false;
        }

        const auto mesh_id = rbx::mesh_part::get_mesh_asset_id(part.instance);
        if (!mesh_id)
        {
            return false;
        }

        const auto* wireframe = get_mesh_wireframe_edges(*mesh_id);
        if (!wireframe || wireframe->edges.empty())
        {
            return false;
        }

        mesh_vertex_fit mesh_fit{};
        if (wireframe->has_local_bounds)
        {
            mesh_fit = build_mesh_vertex_fit(part.size, wireframe->local_bounds_min, wireframe->local_bounds_max);
        }

        const std::size_t start_count = out_segments.size();
        out_segments.reserve(start_count + wireframe->edges.size());
        for (const auto& edge : wireframe->edges)
        {
            const rbx::Vector3 local_a = apply_mesh_vertex_fit(edge.a, mesh_fit);
            const rbx::Vector3 local_b = apply_mesh_vertex_fit(edge.b, mesh_fit);
            const rbx::Vector3 world_a = transform->position + rotate_point(*transform, local_a);
            const rbx::Vector3 world_b = transform->position + rotate_point(*transform, local_b);
            const auto screen_a = rbx::camera::world_to_screen(world_a, frame.view_matrix, frame.dimensions);
            const auto screen_b = rbx::camera::world_to_screen(world_b, frame.view_matrix, frame.dimensions);
            if (!screen_a || !screen_b)
            {
                continue;
            }

            out_segments.push_back(projected_wireframe_segment{
                ImVec2(screen_a->x, screen_a->y),
                ImVec2(screen_b->x, screen_b->y)
                });
        }

        return out_segments.size() > start_count;
    }

    void draw_wireframe_segments(const std::vector<projected_wireframe_segment>& segments, ImU32 color)
    {
        if (segments.empty())
        {
            return;
        }

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw)
        {
            return;
        }

        const ImDrawListFlags saved_flags = draw->Flags;
        draw->Flags = saved_flags & ~ImDrawListFlags_AntiAliasedLines;
        for (const auto& segment : segments)
        {
            draw->AddLine(segment.a, segment.b, color, 1.0f);
        }
        draw->Flags = saved_flags;
    }

    rbx::Vector3 rotate_occluder_local(const ::visibility::debug_occluder_primitive& primitive, const rbx::Vector3& local)
    {
        return rbx::Vector3(
            primitive.rotation[0] * local.x + primitive.rotation[3] * local.y + primitive.rotation[6] * local.z,
            primitive.rotation[1] * local.x + primitive.rotation[4] * local.y + primitive.rotation[7] * local.z,
            primitive.rotation[2] * local.x + primitive.rotation[5] * local.y + primitive.rotation[8] * local.z
        );
    }

    bool project_occluder_wireframe(
        const ::visibility::debug_occluder_primitive& primitive,
        const camera_frame_t& frame,
        std::vector<projected_wireframe_segment>& out_segments)
    {
        const rbx::Vector3 half = primitive.half_size;
        if (!std::isfinite(half.x) || !std::isfinite(half.y) || !std::isfinite(half.z) ||
            half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f)
        {
            return false;
        }

        std::array<rbx::Vector3, 8> world_corners{};
        std::array<rbx::Vector3, 8> local_corners = {
            rbx::Vector3(half.x, half.y, half.z),
            rbx::Vector3(half.x, half.y, -half.z),
            rbx::Vector3(half.x, -half.y, half.z),
            rbx::Vector3(half.x, -half.y, -half.z),
            rbx::Vector3(-half.x, half.y, half.z),
            rbx::Vector3(-half.x, half.y, -half.z),
            rbx::Vector3(-half.x, -half.y, half.z),
            rbx::Vector3(-half.x, -half.y, -half.z)
        };

        for (std::size_t i = 0; i < local_corners.size(); ++i)
        {
            world_corners[i] = primitive.position + rotate_occluder_local(primitive, local_corners[i]);
        }

        constexpr std::array<std::pair<std::uint8_t, std::uint8_t>, 12> edges = {
            std::pair<std::uint8_t, std::uint8_t>(0, 1),
            std::pair<std::uint8_t, std::uint8_t>(0, 2),
            std::pair<std::uint8_t, std::uint8_t>(0, 4),
            std::pair<std::uint8_t, std::uint8_t>(1, 3),
            std::pair<std::uint8_t, std::uint8_t>(1, 5),
            std::pair<std::uint8_t, std::uint8_t>(2, 3),
            std::pair<std::uint8_t, std::uint8_t>(2, 6),
            std::pair<std::uint8_t, std::uint8_t>(3, 7),
            std::pair<std::uint8_t, std::uint8_t>(4, 5),
            std::pair<std::uint8_t, std::uint8_t>(4, 6),
            std::pair<std::uint8_t, std::uint8_t>(5, 7),
            std::pair<std::uint8_t, std::uint8_t>(6, 7)
        };

        const std::size_t start_count = out_segments.size();
        for (const auto& edge : edges)
        {
            ImVec2 a{};
            ImVec2 b{};
            if (!project_segment_unclamped(world_corners[edge.first], world_corners[edge.second], frame, a, b))
            {
                continue;
            }
            out_segments.push_back(projected_wireframe_segment{ a, b });
        }

        return out_segments.size() > start_count;
    }

    void draw_visibility_debug_primitives(const camera_frame_t& frame, const cache::local_player_state& local)
    {
        if (!features->show_visibility_debug_primitives)
        {
            return;
        }

        static std::vector<::visibility::debug_occluder_primitive> primitives;
        static std::vector<projected_wireframe_segment> segments;
        static double last_refresh_time = 0.0;
        const double now = ImGui::GetTime();

        const float fps = g_overlay_fps.load(std::memory_order_relaxed);
        const std::size_t max_debug_primitives =
            (fps >= 120.0f) ? 12000 :
            (fps >= 90.0f) ? 8000 :
            (fps >= 70.0f) ? 5500 :
            (fps >= 50.0f) ? 3500 : 2000;
        const float max_draw_distance =
            (fps >= 120.0f) ? 1000.0f :
            (fps >= 90.0f) ? 850.0f :
            (fps >= 70.0f) ? 700.0f :
            (fps >= 50.0f) ? 575.0f : 475.0f;
        const double refresh_interval =
            (fps >= 90.0f) ? 0.12 :
            (fps >= 60.0f) ? 0.18 : 0.25;

        if (primitives.empty() || (now - last_refresh_time) >= refresh_interval)
        {
            ::visibility::get_debug_occluder_primitives(primitives, max_debug_primitives);
            last_refresh_time = now;
        }
        if (primitives.empty())
        {
            return;
        }

        const auto camera_pos = resolve_camera_position(local, frame);
        const float max_distance_sq = max_draw_distance * max_draw_distance;

        segments.clear();
        const std::size_t max_segments = max_debug_primitives * 12;
        segments.reserve((std::min)(max_segments, primitives.size() * 12));
        for (const auto& primitive : primitives)
        {
            if (camera_pos)
            {
                const rbx::Vector3 to_primitive = primitive.position - *camera_pos;
                if (to_primitive.LengthSquared() > max_distance_sq)
                {
                    continue;
                }
            }

            project_occluder_wireframe(primitive, frame, segments);
            if (segments.size() >= max_segments)
            {
                break;
            }
        }

        if (segments.empty())
        {
            return;
        }

        const ImU32 color = IM_COL32(0, 225, 255, 235);
        draw_wireframe_segments(segments, color);
    }

    Clipper2Lib::Paths64 scale_to_int(const Clipper2Lib::PathsD& paths, double scale = 100.0)
    {
        Clipper2Lib::Paths64 out;
        out.reserve(paths.size());
        for (const auto& path : paths)
        {
            Clipper2Lib::Path64 p;
            p.reserve(path.size());
            for (const auto& pt : path)
            {
                p.emplace_back(
                    static_cast<int64_t>(std::llround(pt.x * scale)),
                    static_cast<int64_t>(std::llround(pt.y * scale))
                );
            }
            out.push_back(std::move(p));
        }
        return out;
    }

    Clipper2Lib::PathsD scale_to_double(const Clipper2Lib::Paths64& paths, double scale = 100.0)
    {
        Clipper2Lib::PathsD out;
        out.reserve(paths.size());
        for (const auto& path : paths)
        {
            Clipper2Lib::PathD p;
            p.reserve(path.size());
            for (const auto& pt : path)
            {
                p.emplace_back(
                    static_cast<double>(pt.x) / scale,
                    static_cast<double>(pt.y) / scale
                );
            }
            out.push_back(std::move(p));
        }
        return out;
    }

    static void draw_filled_path(ImDrawList* draw, const Clipper2Lib::PathD& poly, ImU32 color)
    {
        if (!draw || poly.size() < 3)
        {
            return;
        }

        std::vector<std::vector<std::array<double, 2>>> polygon(1);
        polygon[0].reserve(poly.size());
        for (const auto& p : poly)
        {
            polygon[0].push_back({ p.x, p.y });
        }

        const auto indices = mapbox::earcut<uint32_t>(polygon);
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            const auto& a = polygon[0][indices[i]];
            const auto& b = polygon[0][indices[i + 1]];
            const auto& c = polygon[0][indices[i + 2]];
            draw->AddTriangleFilled(
                ImVec2(static_cast<float>(a[0]), static_cast<float>(a[1])),
                ImVec2(static_cast<float>(b[0]), static_cast<float>(b[1])),
                ImVec2(static_cast<float>(c[0]), static_cast<float>(c[1])),
                color);
        }
    }

    struct highlight_draw_path
    {
        std::vector<ImVec2> points;
        float span = 0.0f;
    };

    static bool build_highlight_path(const Clipper2Lib::PathD& path, highlight_draw_path& out)
    {
        if (path.size() < 2)
        {
            return false;
        }

        out.points.clear();
        out.points.reserve(path.size());
        float minx = (std::numeric_limits<float>::max)();
        float miny = (std::numeric_limits<float>::max)();
        float maxx = -(std::numeric_limits<float>::max)();
        float maxy = -(std::numeric_limits<float>::max)();
        for (const auto& p : path)
        {
            ImVec2 pt(static_cast<float>(p.x), static_cast<float>(p.y));
            out.points.push_back(pt);
            minx = (std::min)(minx, pt.x);
            miny = (std::min)(miny, pt.y);
            maxx = (std::max)(maxx, pt.x);
            maxy = (std::max)(maxy, pt.y);
        }

        out.span = (std::max)(maxx - minx, maxy - miny);
        return out.points.size() >= 2;
    }

    void draw_highlight_hulls(
        const std::vector<Clipper2Lib::PathD>& hulls,
        ImU32 fill_color,
        ImU32 outline_color,
        bool outline_only = false)
    {
        if (hulls.empty())
        {
            return;
        }

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw)
        {
            return;
        }

        constexpr double scale = 100.0;
        Clipper2Lib::Paths64 hulls_i = scale_to_int(hulls, scale);

        Clipper2Lib::Paths64 unified = Clipper2Lib::Union(hulls_i, Clipper2Lib::FillRule::NonZero);
        unified = Clipper2Lib::SimplifyPaths(unified, 2.0, true);
        Clipper2Lib::PathsD unified_d = scale_to_double(unified, scale);
        if (unified_d.empty())
        {
            return;
        }

        std::vector<highlight_draw_path> draw_paths;
        draw_paths.reserve(unified_d.size());
        for (const auto& path : unified_d)
        {
            highlight_draw_path prepared{};
            if (!build_highlight_path(path, prepared))
            {
                continue;
            }
            draw_paths.push_back(std::move(prepared));
        }
        if (draw_paths.empty())
        {
            return;
        }

        const ImDrawListFlags saved_flags = draw->Flags;
        const bool has_fill = ((fill_color >> IM_COL32_A_SHIFT) & 0xFF) > 0;
        const bool has_outline = ((outline_color >> IM_COL32_A_SHIFT) & 0xFF) > 0;

        if (!outline_only && has_fill)
        {
            draw->Flags = saved_flags & ~ImDrawListFlags_AntiAliasedFill;
            for (const auto& poly : unified_d)
            {
                draw_filled_path(draw, poly, fill_color);
            }
        }

        if (has_outline)
        {
            draw->Flags = saved_flags & ~ImDrawListFlags_AntiAliasedFill & ~ImDrawListFlags_AntiAliasedLines;
            for (const auto& path : draw_paths)
            {
                const float thickness = std::clamp(path.span / 100.0f, 1.5f, 2.5f);
                draw->AddPolyline(path.points.data(), static_cast<int>(path.points.size()), outline_color, true, thickness);
            }
        }

        draw->Flags = saved_flags;
    }

    void add_skeleton_segment(ImDrawList* draw, const std::optional<ImVec2>& from, const std::optional<ImVec2>& to, ImU32 color, float thickness)
    {
        if (!draw || !from || !to)
        {
            return;
        }

        draw->AddLine(*from, *to, color, thickness);
    }

    void draw_skeleton(const cache::player_state& player, const camera_frame_t& frame, float alpha_factor, player_relation relation, bool highlight, bool dormant, bool host, const visibility::visibility_result& visibility_state, bool visibility_enabled)
    {
        if (alpha_factor <= 0.0f)
        {
            return;
        }

        auto draw = ImGui::GetBackgroundDrawList();
        if (!draw)
        {
            return;
        }

        const ImVec4 accent = c_colors::top_accent_color;
        ImVec4 base_color = highlight ? accent : features->skeleton_color;
        ImVec4 base_outline = highlight ? accent : features->skeleton_outline_color;
        base_color = apply_relation_color(relation, base_color);
        base_color = apply_host_tint(base_color, host, features->host_color);
        base_outline = apply_host_tint(base_outline, host, features->host_color);
        base_color = apply_visibility_tint(base_color, visibility_state, visibility_enabled);
        base_outline = apply_visibility_tint(base_outline, visibility_state, visibility_enabled);
        ImVec4 color_vec = adjust_alpha(base_color, alpha_factor);
        ImVec4 outline_vec = adjust_alpha(base_outline, alpha_factor);
        color_vec = apply_dormant_tint(color_vec, dormant);
        const ImU32 color = ImGui::GetColorU32(color_vec);
        const ImU32 outline_color = ImGui::GetColorU32(outline_vec);
        constexpr float thickness = 1.0f;
        constexpr float outline_extra = 2.0f;

        const auto& parts = player.parts;

        auto connect = [&](const std::optional<ImVec2>& a, const std::optional<ImVec2>& b)
            {
                if (features->enable_skeleton_outline)
                {
                    add_skeleton_segment(draw, a, b, outline_color, thickness + outline_extra);
                }
                add_skeleton_segment(draw, a, b, color, thickness);
            };

        if (parts.is_r15)
        {
            const auto head = project_part_to_screen(parts.head, frame);
            const float upper_torso_offset = parts.upper_torso.size.y * 0.1f;
            const auto upper_torso = project_part_to_screen_offset(parts.upper_torso, frame, rbx::Vector3(0.0f, upper_torso_offset, 0.0f));
            const auto lower_torso = project_part_to_screen(parts.lower_torso, frame);

            const auto left_upper_arm = project_part_to_screen(parts.left_upper_arm, frame);
            const auto left_lower_arm = project_part_to_screen(parts.left_lower_arm, frame);
            const auto left_hand = project_part_to_screen(parts.left_hand, frame);
            const auto right_upper_arm = project_part_to_screen(parts.right_upper_arm, frame);
            const auto right_lower_arm = project_part_to_screen(parts.right_lower_arm, frame);
            const auto right_hand = project_part_to_screen(parts.right_hand, frame);

            const auto left_upper_leg = project_part_to_screen(parts.left_upper_leg, frame);
            const auto left_lower_leg = project_part_to_screen(parts.left_lower_leg, frame);
            const auto left_foot = project_part_to_screen(parts.left_foot, frame);
            const auto right_upper_leg = project_part_to_screen(parts.right_upper_leg, frame);
            const auto right_lower_leg = project_part_to_screen(parts.right_lower_leg, frame);
            const auto right_foot = project_part_to_screen(parts.right_foot, frame);

            connect(head, upper_torso);
            connect(upper_torso, lower_torso);

            connect(upper_torso, left_upper_arm);
            connect(left_upper_arm, left_lower_arm);
            connect(left_lower_arm, left_hand);

            connect(upper_torso, right_upper_arm);
            connect(right_upper_arm, right_lower_arm);
            connect(right_lower_arm, right_hand);

            connect(lower_torso, left_upper_leg);
            connect(left_upper_leg, left_lower_leg);
            connect(left_lower_leg, left_foot);

            connect(lower_torso, right_upper_leg);
            connect(right_upper_leg, right_lower_leg);
            connect(right_lower_leg, right_foot);
        }
        else
        {
            const auto head = project_part_to_screen(parts.head, frame);
            
            const float torso_top_offset = parts.torso.size.y * 0.275f;
            const auto torso_top = project_part_to_screen_offset(parts.torso, frame, rbx::Vector3(0.0f, torso_top_offset, 0.0f));
            const float torso_bottom_offset = -parts.torso.size.y * 0.4f;
            const auto torso_bottom = project_part_to_screen_offset(parts.torso, frame, rbx::Vector3(0.0f, torso_bottom_offset, 0.0f));

            const float arm_offset = parts.left_arm.size.y * 0.30f;
            const auto left_arm_top = project_part_to_screen_local_offset(parts.left_arm, frame, rbx::Vector3(0.0f, arm_offset, 0.0f));
            const auto right_arm_top = project_part_to_screen_local_offset(parts.right_arm, frame, rbx::Vector3(0.0f, arm_offset, 0.0f));
            const auto left_arm_bottom = project_part_to_screen_local_offset(parts.left_arm, frame, rbx::Vector3(0.0f, -arm_offset, 0.0f));
            const auto right_arm_bottom = project_part_to_screen_local_offset(parts.right_arm, frame, rbx::Vector3(0.0f, -arm_offset, 0.0f));

            const float leg_offset = parts.left_leg.size.y * 0.35f;
            const auto left_leg_top = project_part_to_screen_local_offset(parts.left_leg, frame, rbx::Vector3(0.0f, leg_offset, 0.0f));
            const auto right_leg_top = project_part_to_screen_local_offset(parts.right_leg, frame, rbx::Vector3(0.0f, leg_offset, 0.0f));
            const auto left_leg_bottom = project_part_to_screen_local_offset(parts.left_leg, frame, rbx::Vector3(0.0f, -leg_offset, 0.0f));
            const auto right_leg_bottom = project_part_to_screen_local_offset(parts.right_leg, frame, rbx::Vector3(0.0f, -leg_offset, 0.0f));

            connect(head, torso_top);
            connect(torso_top, torso_bottom);

            connect(torso_top, left_arm_top);
            connect(torso_top, right_arm_top);
            
            connect(left_arm_top, left_arm_bottom);
            connect(right_arm_top, right_arm_bottom);

            connect(torso_bottom, left_leg_top);
            connect(torso_bottom, right_leg_top);
            
            connect(left_leg_top, left_leg_bottom);
            connect(right_leg_top, right_leg_bottom);
        }

    }

    std::vector<const cache::primitive_part*> collect_parts(const cache::character_parts& parts)
    {
        if (parts.is_r15)
        {
            return {
                &parts.head,
                &parts.upper_torso,
                &parts.lower_torso,
                &parts.left_upper_arm,
                &parts.left_lower_arm,
                &parts.left_hand,
                &parts.right_upper_arm,
                &parts.right_lower_arm,
                &parts.right_hand,
                &parts.left_upper_leg,
                &parts.left_lower_leg,
                &parts.left_foot,
                &parts.right_upper_leg,
                &parts.right_lower_leg,
                &parts.right_foot
            };
        }

        return {
            &parts.head,
            &parts.torso,
            &parts.left_arm,
            &parts.right_arm,
            &parts.left_leg,
            &parts.right_leg
        };
    }

    static float dot_vec3(const rbx::Vector3& a, const rbx::Vector3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static rbx::Vector3 normalize_or(const rbx::Vector3& value, const rbx::Vector3& fallback)
    {
        rbx::Vector3 out = value;
        const float len_sq = out.LengthSquared();
        if (!std::isfinite(len_sq) || len_sq < 1e-6f)
        {
            return fallback;
        }
        out.Normalize();
        return out;
    }

    struct part_basis_t
    {
        rbx::Vector3 position{};
        rbx::Vector3 axis_x{ 1.0f, 0.0f, 0.0f };
        rbx::Vector3 axis_y{ 0.0f, 1.0f, 0.0f };
        rbx::Vector3 axis_z{ 0.0f, 0.0f, 1.0f };
        rbx::Vector3 half_extents{};
    };

    static bool build_part_basis(const cache::primitive_part& part, part_basis_t& out)
    {
        const auto pos = get_part_position(part);
        if (!pos || !is_finite_vec3(*pos))
        {
            return false;
        }

        out.position = *pos;

        struct rot_t
        {
            float m[3][3];
        };

        const bool has_rotation = part.primitive != 0 && roblox::offsets::base_part::cframe_rotation != 0;
        if (has_rotation)
        {
            const rot_t rot = memory->read<rot_t>(part.primitive + roblox::offsets::base_part::cframe_rotation);
            out.axis_x = rbx::Vector3(rot.m[0][0], rot.m[1][0], rot.m[2][0]);
            out.axis_y = rbx::Vector3(rot.m[0][1], rot.m[1][1], rot.m[2][1]);
            out.axis_z = rbx::Vector3(rot.m[0][2], rot.m[1][2], rot.m[2][2]);
        }

        out.axis_x = normalize_or(out.axis_x, rbx::Vector3(1.0f, 0.0f, 0.0f));
        out.axis_y = normalize_or(out.axis_y, rbx::Vector3(0.0f, 1.0f, 0.0f));
        out.axis_z = normalize_or(out.axis_z, rbx::Vector3(0.0f, 0.0f, 1.0f));

        const rbx::Vector3 size = part.size;
        out.half_extents.x = (std::isfinite(size.x) && size.x > 0.0f) ? (size.x * 0.5f) : 0.0f;
        out.half_extents.y = (std::isfinite(size.y) && size.y > 0.0f) ? (size.y * 0.5f) : 0.0f;
        out.half_extents.z = (std::isfinite(size.z) && size.z > 0.0f) ? (size.z * 0.5f) : 0.0f;

        return true;
    }

    static float clamp_to_extent(float value, float half_extent)
    {
        if (!std::isfinite(half_extent) || half_extent <= 0.0f)
        {
            return 0.0f;
        }
        return std::clamp(value, -half_extent, half_extent);
    }

    static bool closest_point_on_part(const cache::primitive_part& part, const rbx::Vector3& point, rbx::Vector3& out_point, float& out_dist_sq)
    {
        part_basis_t basis{};
        if (!build_part_basis(part, basis))
        {
            return false;
        }

        const rbx::Vector3 to_point = point - basis.position;
        const float dx = dot_vec3(to_point, basis.axis_x);
        const float dy = dot_vec3(to_point, basis.axis_y);
        const float dz = dot_vec3(to_point, basis.axis_z);

        const float cx = clamp_to_extent(dx, basis.half_extents.x);
        const float cy = clamp_to_extent(dy, basis.half_extents.y);
        const float cz = clamp_to_extent(dz, basis.half_extents.z);

        out_point = basis.position + basis.axis_x * cx + basis.axis_y * cy + basis.axis_z * cz;
        if (!is_finite_vec3(out_point))
        {
            return false;
        }

        const rbx::Vector3 delta = out_point - point;
        const float dist_sq = delta.LengthSquared();
        if (!std::isfinite(dist_sq))
        {
            return false;
        }

        out_dist_sq = dist_sq;
        return true;
    }

    static std::optional<rbx::Vector3> find_closest_point_on_character(const cache::character_parts& parts, const rbx::Vector3& point)
    {
        float best_dist_sq = (std::numeric_limits<float>::max)();
        std::optional<rbx::Vector3> best_point;

        auto try_part = [&](const cache::primitive_part& part)
            {
                rbx::Vector3 candidate{};
                float dist_sq = 0.0f;
                if (closest_point_on_part(part, point, candidate, dist_sq))
                {
                    if (dist_sq < best_dist_sq)
                    {
                        best_dist_sq = dist_sq;
                        best_point = candidate;
                    }
                }
            };

        const auto list = collect_parts(parts);
        for (const auto* part : list)
        {
            if (part)
            {
                try_part(*part);
            }
        }

        try_part(parts.humanoid_root_part);
        return best_point;
    }

    std::optional<ImRect> compute_bounding_box(const cache::player_state& player, const camera_frame_t& frame)
    {
        const auto parts = collect_parts(player.parts);

        const float max_val = (std::numeric_limits<float>::max)();
        float min_x = max_val;
        float min_y = max_val;
        float max_x = -max_val;
        float max_y = -max_val;
        bool has_point = false;

        struct rot_t
        {
            float m[3][3];
        };

        for (const auto* part : parts)
        {
            if (!part)
            {
                continue;
            }

            const auto pos = get_part_position(*part);
            if (!pos)
            {
                continue;
            }

            const rbx::Vector3 size = part->size;
            const bool has_size = std::isfinite(size.x) && std::isfinite(size.y) && std::isfinite(size.z) && size.x > 0.0f && size.y > 0.0f && size.z > 0.0f;
            const bool has_rotation = part->primitive != 0 && roblox::offsets::base_part::cframe_rotation != 0;
            rot_t rot{};
            if (has_rotation)
            {
                rot = memory->read<rot_t>(part->primitive + roblox::offsets::base_part::cframe_rotation);
            }

            if (has_size)
            {
                const rbx::Vector3 half = size * 0.5f;
                const rbx::Vector3 basis_x = has_rotation ? rbx::Vector3(rot.m[0][0], rot.m[1][0], rot.m[2][0]) : rbx::Vector3(1.0f, 0.0f, 0.0f);
                const rbx::Vector3 basis_y = has_rotation ? rbx::Vector3(rot.m[0][1], rot.m[1][1], rot.m[2][1]) : rbx::Vector3(0.0f, 1.0f, 0.0f);
                const rbx::Vector3 basis_z = has_rotation ? rbx::Vector3(rot.m[0][2], rot.m[1][2], rot.m[2][2]) : rbx::Vector3(0.0f, 0.0f, 1.0f);

                for (int xi : { -1, 1 })
                {
                    for (int yi : { -1, 1 })
                    {
                        for (int zi : { -1, 1 })
                        {
                            rbx::Vector3 world = *pos;
                            world += basis_x * (half.x * static_cast<float>(xi));
                            world += basis_y * (half.y * static_cast<float>(yi));
                            world += basis_z * (half.z * static_cast<float>(zi));
                            const auto screen = rbx::camera::world_to_screen(world, frame.view_matrix, frame.dimensions);
                            if (!screen)
                            {
                                continue;
                            }

                            has_point = true;
                            min_x = (std::min)(min_x, screen->x);
                            min_y = (std::min)(min_y, screen->y);
                            max_x = (std::max)(max_x, screen->x);
                            max_y = (std::max)(max_y, screen->y);
                        }
                    }
                }
            }
            else
            {
                const auto screen = rbx::camera::world_to_screen(*pos, frame.view_matrix, frame.dimensions);
                if (!screen)
                {
                    continue;
                }

                has_point = true;
                min_x = (std::min)(min_x, screen->x);
                min_y = (std::min)(min_y, screen->y);
                max_x = (std::max)(max_x, screen->x);
                max_y = (std::max)(max_y, screen->y);
            }
        }

        if (!has_point)
        {
            return std::nullopt;
        }

        min_x = std::clamp(min_x, 0.0f, frame.dimensions.x);
        min_y = std::clamp(min_y, 0.0f, frame.dimensions.y);
        max_x = std::clamp(max_x, 0.0f, frame.dimensions.x);
        max_y = std::clamp(max_y, 0.0f, frame.dimensions.y);

        constexpr float k_padding = 2.0f;
        min_x = (std::max)(0.0f, min_x - k_padding);
        min_y = (std::max)(0.0f, min_y - k_padding);
        max_x = (std::min)(frame.dimensions.x, max_x + k_padding);
        max_y = (std::min)(frame.dimensions.y, max_y + k_padding);

        if (min_x >= max_x || min_y >= max_y)
        {
            return std::nullopt;
        }

        return ImRect(ImVec2(min_x, min_y), ImVec2(max_x, max_y));
    }

    static float advance_fade(std::uintptr_t player_address, std::uintptr_t character_address, bool is_present, bool force_refade)
    {
        const float fade_duration = features->fade_dormant ? (std::max)(0.0f, features->dormant_fade_time) : 1.25f;
        auto& state = fade_map[player_address];

        if (!state.initialized)
        {
            state.initialized = true;
            state.alpha = is_present ? 0.0f : 0.0f;
            state.start_alpha = state.alpha;
            state.target_alpha = is_present ? 1.0f : 0.0f;
            state.timer = 0.0f;
            state.pending_refade = false;
            state.pending_character = 0;
            state.last_character = 0;
        }

        auto start_fade_in = [&](std::uintptr_t new_character)
            {
                state.pending_refade = false;
                state.pending_character = 0;
                state.alpha = 0.0f;
                state.start_alpha = 0.0f;
                state.target_alpha = 1.0f;
                state.timer = 0.0f;
                if (new_character != 0)
                {
                    state.last_character = new_character;
                }
            };

        if (force_refade)
        {
            start_fade_in(character_address);
        }
        else if (character_address != 0 && state.last_character != 0 && character_address != state.last_character)
        {
            start_fade_in(character_address);
        }
        else if (!is_present && state.target_alpha != 0.0f)
        {
            state.start_alpha = state.alpha;
            state.target_alpha = 0.0f;
            state.timer = 0.0f;
        }
        else if (is_present && state.target_alpha == 0.0f && !state.pending_refade)
        {
            state.start_alpha = state.alpha;
            state.target_alpha = 1.0f;
            state.timer = 0.0f;
        }

        if (character_address != 0)
        {
            state.last_character = character_address;
        }

        state.timer += ImGui::GetIO().DeltaTime;
        const float progress = ease_expo(state.timer, 0.0f, fade_duration);
        const float clamped_progress = std::clamp(progress, 0.0f, 1.0f);
        state.alpha = state.start_alpha + (state.target_alpha - state.start_alpha) * clamped_progress;

        if (clamped_progress >= 1.0f)
        {
            state.alpha = state.target_alpha;
            state.start_alpha = state.target_alpha;
            state.timer = fade_duration;

            if (state.pending_refade && state.alpha <= 0.0f)
            {
                state.pending_refade = false;
                state.last_character = state.pending_character != 0 ? state.pending_character : state.last_character;
                state.start_alpha = 0.0f;
                state.target_alpha = 1.0f;
                state.timer = 0.0f;
            }
        }

        return std::clamp(state.alpha, 0.0f, 1.0f);
    }

    static float advance_dead_body_fade(std::uintptr_t body_address, bool is_present)
    {
        const float fade_duration = features->fade_dormant ? (std::max)(0.0f, features->dormant_fade_time) : 0.35f;
        auto& state = dead_body_fade_map[body_address];

        if (!state.initialized)
        {
            state.initialized = true;
            state.alpha = 0.0f;
            state.start_alpha = 0.0f;
            state.target_alpha = is_present ? 1.0f : 0.0f;
            state.timer = 0.0f;
        }

        if (!is_present && state.target_alpha != 0.0f)
        {
            state.start_alpha = state.alpha;
            state.target_alpha = 0.0f;
            state.timer = 0.0f;
        }
        else if (is_present && state.target_alpha == 0.0f)
        {
            state.start_alpha = state.alpha;
            state.target_alpha = 1.0f;
            state.timer = 0.0f;
        }

        state.timer += ImGui::GetIO().DeltaTime;
        const float progress = ease_expo(state.timer, 0.0f, fade_duration);
        const float clamped_progress = std::clamp(progress, 0.0f, 1.0f);
        state.alpha = state.start_alpha + (state.target_alpha - state.start_alpha) * clamped_progress;

        if (clamped_progress >= 1.0f)
        {
            state.alpha = state.target_alpha;
            state.start_alpha = state.target_alpha;
            state.timer = fade_duration;
        }

        return std::clamp(state.alpha, 0.0f, 1.0f);
    }
}

namespace
{
    struct radar_map_part
    {
        rbx::Vector3 position{};
        std::array<rbx::Vector2, 4> footprint_offsets{};
        float half_height = 0.0f;
        float footprint_radius = 0.0f;
        ImU32 color = 0;
    };

    static std::vector<radar_map_part> g_radar_map_parts;
    static double g_radar_map_last_refresh = 0.0;
    static rbx::Vector3 g_radar_map_anchor{};
    static bool g_radar_map_has_anchor = false;

    constexpr float k_radar_map_anchor_move = 60.0f;
    constexpr std::size_t k_radar_map_max_parts = 2500;
    constexpr float k_radar_map_min_extent = 4.0f;
    constexpr double k_radar_map_min_refresh_interval = 0.5;

    static bool should_skip_radar_root(const rbx::instance_t& root)
    {
        const std::string name = root.get_name();
        if (name == "Players" || name == "Camera" || name == "Ignore" || name == "Terrain")
        {
            return true;
        }
        return false;
    }

    static std::optional<ImU32> read_part_color(const rbx::instance_t& part)
    {
        if (!part.is_valid() || !roblox::offsets::base_part::color3)
        {
            return std::nullopt;
        }

        std::uint32_t packed = memory->read<std::uint32_t>(part.get_address() + roblox::offsets::base_part::color3);
        packed &= 0x00FFFFFFu;
        const std::uint8_t r = static_cast<std::uint8_t>(packed & 0xFF);
        const std::uint8_t g = static_cast<std::uint8_t>((packed >> 8) & 0xFF);
        const std::uint8_t b = static_cast<std::uint8_t>((packed >> 16) & 0xFF);
        return IM_COL32(r, g, b, 255);
    }

    static void refresh_radar_map(const rbx::Vector3& origin, float range)
    {
        (void)range;

        g_radar_map_parts.clear();
        g_radar_map_parts.reserve(512);

        const auto workspace = globals->workspace;
        if (!workspace.is_valid())
        {
            return;
        }

        auto roots = workspace.get_children();
        if (roots.empty())
        {
            return;
        }

        auto try_add_part = [&](const rbx::instance_t& part)
            {
                if (!part.is_valid())
                {
                    return;
                }
                const std::string cls = part.get_class_name();
                if (cls != "Part" && cls != "MeshPart" && cls != "UnionOperation" && cls != "CornerWedgePart"
                    && cls != "WedgePart" && cls != "TrussPart" && cls != "Seat" && cls != "VehicleSeat"
                    && cls != "SpawnLocation")
                {
                    return;
                }

                const std::uintptr_t primitive = rbx::part::get_primitive(part);
                if (!primitive)
                {
                    return;
                }

                if (const auto transparency = rbx::part::get_transparency(part, primitive))
                {
                    if (*transparency > 0.0f)
                    {
                        return;
                    }
                }

                const auto size_opt = rbx::part::get_size(primitive);
                if (!size_opt)
                {
                    return;
                }

                const rbx::Vector3 size = *size_opt;
                const float max_extent = (std::max)(size.x, size.z);
                if (!std::isfinite(max_extent) || max_extent < k_radar_map_min_extent)
                {
                    return;
                }

                const auto pos_opt = part.get_position(primitive);
                if (!pos_opt)
                {
                    return;
                }

                const rbx::Vector3 pos = *pos_opt;
                const float half_x = size.x * 0.5f;
                const float half_z = size.z * 0.5f;
                std::array<rbx::Vector2, 4> footprint_offsets = {
                    rbx::Vector2(-half_x, -half_z),
                    rbx::Vector2(half_x, -half_z),
                    rbx::Vector2(half_x, half_z),
                    rbx::Vector2(-half_x, half_z)
                };

                if (roblox::offsets::base_part::cframe_rotation)
                {
                    struct rot_t
                    {
                        float m[3][3];
                    };

                    const rot_t rot = memory->read<rot_t>(primitive + roblox::offsets::base_part::cframe_rotation);
                    bool finite = true;
                    for (const auto& row : rot.m)
                    {
                        for (float value : row)
                        {
                            if (!std::isfinite(value))
                            {
                                finite = false;
                                break;
                            }
                        }
                        if (!finite)
                        {
                            break;
                        }
                    }

                    if (finite)
                    {
                        const auto rotate_local_xz = [&](float lx, float lz) -> rbx::Vector2
                        {
                            // Project local X/Z extents into world X/Z using the part basis.
                            const float wx = rot.m[0][0] * lx + rot.m[0][2] * lz;
                            const float wz = rot.m[2][0] * lx + rot.m[2][2] * lz;
                            return rbx::Vector2(wx, wz);
                        };

                        footprint_offsets = {
                            rotate_local_xz(-half_x, -half_z),
                            rotate_local_xz(half_x, -half_z),
                            rotate_local_xz(half_x, half_z),
                            rotate_local_xz(-half_x, half_z)
                        };
                    }
                }

                float footprint_radius = 0.0f;
                for (const auto& offset : footprint_offsets)
                {
                    const float radius = std::sqrt(offset.x * offset.x + offset.y * offset.y);
                    if (std::isfinite(radius))
                    {
                        footprint_radius = (std::max)(footprint_radius, radius);
                    }
                }

                ImU32 color = IM_COL32(180, 180, 180, 255);
                if (const auto color_opt = read_part_color(part))
                {
                    color = *color_opt;
                }

                g_radar_map_parts.push_back({
                    pos,
                    footprint_offsets,
                    size.y * 0.5f,
                    footprint_radius,
                    color
                    });
            };

        for (const auto& root : roots)
        {
            if (!root.is_valid())
            {
                continue;
            }
            if (should_skip_radar_root(root))
            {
                continue;
            }

            try_add_part(root);

            auto descendants = root.get_descendants();
            for (const auto& node : descendants)
            {
                try_add_part(node);
                if (g_radar_map_parts.size() >= k_radar_map_max_parts)
                {
                    break;
                }
            }

            if (g_radar_map_parts.size() >= k_radar_map_max_parts)
            {
                break;
            }
        }

        g_radar_map_last_refresh = ImGui::GetTime();
        g_radar_map_anchor = origin;
        g_radar_map_has_anchor = true;
    }
}

namespace esp
{
    void render_esp()
    {
        debug_diag::esp_frame_report diag{};
        diag.esp_enabled = features->enable_esp;
        diag.bbox_enabled = features->enable_bounding_box;
        diag.name_enabled = features->enable_name_esp;
        diag.skeleton_enabled = features->enable_skeleton;
        diag.highlight_enabled = features->enable_highlight;
        diag.datamodel = globals ? globals->datamodel.get_address() : 0;
        diag.players_service = globals ? globals->players.get_address() : 0;
        diag.workspace = globals ? globals->workspace.get_address() : 0;
        diag.visualengine = globals ? globals->visualengine.get_address() : 0;

        const auto publish_diag = [&](const char* phase)
        {
            diag.phase = phase;
            debug_diag::report_esp_frame(diag);
        };

        struct outline_guard
        {
            bool previous = ImGui::IsTextOutlineEnabled();
            outline_guard() { ImGui::SetTextOutlineEnabled(false); }
            ~outline_guard() { ImGui::SetTextOutlineEnabled(previous); }
        } guard;

        struct hit_log_draw_guard
        {
            ~hit_log_draw_guard() { render_hit_logs(); }
        } hit_log_guard;
        (void)hit_log_guard;

        ++highlight_frame_id;
        begin_metallic_mesh_frame();

        if (!features->enable_death_overlay)
        {
            lock_death_time = -1.0f;
        }

        float lock_death_alpha = features->enable_death_overlay ? advance_lock_death_image() : 0.0f;
        struct lock_death_draw_guard
        {
            float* alpha = nullptr;
            ~lock_death_draw_guard()
            {
                if (alpha && *alpha > 0.0f)
                {
                    draw_lock_death_image(*alpha);
                }
            }
        } lock_death_draw{ &lock_death_alpha };

        auto draw = ImGui::GetBackgroundDrawList();
        diag.draw_list_ok = draw != nullptr;
        if (!draw)
        {
            publish_diag("no_draw_list");
            return;
        }

        const bool draw_desync_marker = features->desync_marker_active;
        const bool draw_visibility_debug_primitives_flag = features->show_visibility_debug_primitives;
        const bool esp_enabled = features->enable_esp;
        diag.esp_enabled = esp_enabled;
        if (!draw_desync_marker)
        {
            clear_desync_highlight_snapshot();
        }

        const auto local = cache::localplayer->snapshot();
        const bool needs_lock_death_update = features->enable_aimbot || features->enable_free_aim || features->enable_triggerbot || last_locked_address != 0;
        if (!esp_enabled && !draw_desync_marker && !draw_visibility_debug_primitives_flag && !needs_lock_death_update)
        {
            clear_grenade_tracking_state();
            publish_diag("esp_disabled");
            return;
        }

        if (draw_visibility_debug_primitives_flag && !esp_enabled && !draw_desync_marker && !needs_lock_death_update)
        {
            const auto camera_frame = read_camera_frame();
            if (camera_frame)
            {
                draw_visibility_debug_primitives(*camera_frame, local);
            }
            clear_grenade_tracking_state();
            return;
        }

        const auto players_snapshot = cache::players_cache->snapshot();
        const auto dummy = cache::players_cache->dummy_snapshot();
        const auto dead_bodies_snapshot = cache::dead_bodies_cache->snapshot();
        cache::player_state dummy_player{};
        bool has_dummy = false;
        if (dummy && dummy->address != 0)
        {
            dummy_player = make_dummy_player_state(*dummy);
            has_dummy = true;
        }

        auto for_each_player = [&](auto&& fn) -> bool
            {
                if (players_snapshot)
                {
                    for (const auto& player : *players_snapshot)
                    {
                        if (!fn(player))
                        {
                            return false;
                        }
                    }
                }
                if (has_dummy)
                {
                    if (!fn(dummy_player))
                    {
                        return false;
                    }
                }
                return true;
            };

        const std::size_t player_count = (players_snapshot ? players_snapshot->size() : 0u) + (has_dummy ? 1u : 0u);
        diag.cached_players = player_count;
        const std::size_t dead_body_count = dead_bodies_snapshot ? dead_bodies_snapshot->size() : 0u;
        const int highlight_mode = std::clamp(features->highlight_mode, 0, 2);
        const int highlight_mesh_material = std::clamp(features->highlight_mesh_material, 0, 4);

        const std::uintptr_t aimbot_locked = features->enable_aimbot ? aimbot::get_locked_player() : 0;
        const std::uintptr_t free_locked = features->enable_free_aim ? free_aim::get_locked_player() : 0;
        const std::uintptr_t trigger_locked = features->enable_triggerbot ? triggerbot::get_locked_player() : 0;
        const std::uintptr_t unified_locked = aimbot_locked ? aimbot_locked : (free_locked ? free_locked : trigger_locked);
        std::optional<ImVec2> target_snapline_head_screen;
        float target_snapline_alpha = 1.0f;

        if (needs_lock_death_update && player_count > 0)
        {
            bool locked_found = false;
            bool locked_alive = false;
            bool last_found = false;
            bool last_alive_now = false;

            for_each_player([&](const cache::player_state& player)
                {
                    if (player.address == unified_locked)
                    {
                        locked_found = true;
                        locked_alive = (player.health > 0.0f) && !player.body_effects.knocked;
                    }
                    if (player.address == last_locked_address)
                    {
                        last_found = true;
                        last_alive_now = (player.health > 0.0f) && !player.body_effects.knocked;
                    }
                    if ((unified_locked == 0 || locked_found) && (last_locked_address == 0 || last_found))
                    {
                        return false;
                    }
                    return true;
                });

            if (last_locked_address != 0 && last_locked_alive)
            {
                bool found_now = false;
                bool alive_now = false;
                if (last_locked_address == unified_locked)
                {
                    found_now = locked_found;
                    alive_now = locked_alive;
                }
                else
                {
                    found_now = last_found;
                    alive_now = last_alive_now;
                }

                if (features->enable_death_overlay && (!found_now || !alive_now))
                {
                    trigger_lock_death_image();
                    lock_death_alpha = 0.0f;
                }
            }

            last_locked_address = unified_locked;
            last_locked_alive = locked_found && locked_alive;
        }

        const bool wants_dead_body_highlight = features->enable_highlight;
        const bool has_dead_bodies = wants_dead_body_highlight && (dead_body_count > 0 || !last_dead_body_snapshot.empty());
        constexpr std::int64_t pf_place_id = 292439477;
        const bool is_pf_place = (globals->game_id == pf_place_id);
        const bool wants_pf_tool_tracer = is_pf_place && features->enable_hit_trace;
        const bool wants_grenade_indicator = esp_enabled && features->enable_grenade_indicator;
        if (player_count == 0 &&
            last_player_snapshot.empty() &&
            !has_dead_bodies &&
            !draw_desync_marker &&
            !draw_visibility_debug_primitives_flag &&
            !wants_pf_tool_tracer &&
            !wants_grenade_indicator)
        {
            clear_grenade_tracking_state();
            publish_diag("no_players");
            return;
        }

        const auto camera_frame = read_camera_frame();
        diag.camera_ok = camera_frame.has_value();
        if (camera_frame)
        {
            diag.viewport_w = camera_frame->dimensions.x;
            diag.viewport_h = camera_frame->dimensions.y;
        }
        if (!camera_frame)
        {
            publish_diag("no_camera");
            return;
        }

        draw_visibility_debug_primitives(*camera_frame, local);
        render_grenade_indicator(*camera_frame, local);

        if (local.address == 0)
        {
            publish_diag("no_local_player");
            return;
        }

        diag.local_ok = true;

        if (draw_desync_marker && !g_desync_highlight.has_snapshot)
        {
            capture_desync_highlight_snapshot(local);
        }

        draw_tool_part_labels(local, *camera_frame);

        const auto local_root_pos = get_part_position(local.parts.humanoid_root_part);
        auto local_head_pos = get_part_position(local.parts.head);
        if (!local_head_pos)
        {
            local_head_pos = local_root_pos;
        }
        auto gun_pos = resolve_local_gun_position(local);
        std::optional<rbx::Vector3> tracer_start = gun_pos ? gun_pos : local_head_pos;
        const auto local_character = local.character.get_address();
        const auto camera_pos = resolve_camera_position(local, *camera_frame);
        const auto camera_right = resolve_camera_right(local);
        const auto camera_up = resolve_camera_up(local);
        const bool can_draw_offscreen = features->enable_offscreen_arrows && features->aimbot_fov_radius > 0.0f && camera_pos && camera_right && camera_up;
        float offscreen_radius = 0.0f;
        float offscreen_size = 0.0f;
        float offscreen_arrow_length = 0.0f;
        float offscreen_arrow_half_width = 0.0f;
        ImVec2 offscreen_center(camera_frame->dimensions.x * 0.5f, camera_frame->dimensions.y * 0.5f);
        if (can_draw_offscreen)
        {
            if (features->aimbot_fov_mode == 1)
            {
                POINT pt;
                if (GetCursorPos(&pt) && vanille::overlay::g_overlay_window)
                {
                    POINT client = pt;
                    if (ScreenToClient(vanille::overlay::g_overlay_window, &client))
                    {
                        offscreen_center = ImVec2(static_cast<float>(client.x), static_cast<float>(client.y));
                    }
                }
            }

            offscreen_size = std::clamp(features->offscreen_arrow_size, 6.0f, 36.0f);
            offscreen_arrow_length = offscreen_size * 1.2f;
            offscreen_arrow_half_width = offscreen_size * 0.55f;
            const float max_radius_x = (std::min)(offscreen_center.x, camera_frame->dimensions.x - offscreen_center.x);
            const float max_radius_y = (std::min)(offscreen_center.y, camera_frame->dimensions.y - offscreen_center.y);
            float max_radius = (std::min)(max_radius_x, max_radius_y);
            max_radius = (std::max)(0.0f, max_radius - offscreen_size * 1.35f);
            const float padding = std::clamp(offscreen_size * 0.85f, 6.0f, 24.0f);
            const float desired_radius = features->aimbot_fov_radius + padding + offscreen_arrow_length;
            if (max_radius >= 20.0f)
            {
                offscreen_radius = std::clamp(desired_radius, 20.0f, max_radius);
            }
            else
            {
                offscreen_radius = 0.0f;
            }
        }

        if (draw_desync_marker)
        {
            const auto projected = g_desync_highlight.has_snapshot ? project_desync_hulls(*camera_frame) : std::vector<Clipper2Lib::PathD>{};
            if (!projected.empty())
            {
                const ImU32 fill = ImGui::GetColorU32(g_desync_highlight.fill_color);
                const ImU32 outline = ImGui::GetColorU32(g_desync_highlight.outline_color);
                draw_highlight_hulls(projected, fill, outline);

                float min_x = (std::numeric_limits<float>::max)();
                float min_y = (std::numeric_limits<float>::max)();
                float max_x = -(std::numeric_limits<float>::max)();
                float max_y = -(std::numeric_limits<float>::max)();
                for (const auto& path : projected)
                {
                    for (const auto& pt : path)
                    {
                        min_x = (std::min)(min_x, static_cast<float>(pt.x));
                        min_y = (std::min)(min_y, static_cast<float>(pt.y));
                        max_x = (std::max)(max_x, static_cast<float>(pt.x));
                        max_y = (std::max)(max_y, static_cast<float>(pt.y));
                    }
                }

                if (min_x < max_x && min_y < max_y)
                {
                    ImDrawList* label_draw = ImGui::GetBackgroundDrawList();
                    ImFont* font = c_fonts::verdana_bold ? c_fonts::verdana_bold : ImGui::GetFont();
                    const float font_size = font ? font->LegacySize : ImGui::GetFontSize();
                    const std::string label = "Client";
                    const float text_y = min_y - 6.0f - font_size;
                    const float text_x = (min_x + max_x) * 0.5f;
                    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label.c_str());
                    ImVec2 pos(text_x - text_size.x * 0.5f, text_y);
                    ImVec4 text_vec = ImGui::ColorConvertU32ToFloat4(outline);
                    ImVec4 shadow_vec = ImVec4(0.0f, 0.0f, 0.0f, 0.6f);
                    if (label_draw && font)
                    {
                        label_draw->AddText(font, font_size, ImVec2(pos.x + 1.0f, pos.y + 1.0f), ImGui::GetColorU32(shadow_vec), label.c_str());
                        ::add_text(label_draw, font, font_size, pos, ImGui::GetColorU32(text_vec), label, false);
                    }
                }
            }
            else
            {
                const auto fallback = rbx::camera::world_to_screen(features->desync_marker_position, camera_frame->view_matrix, camera_frame->dimensions);
                if (fallback)
                {
                    const ImVec2 pos(fallback->x, fallback->y);
                    const float radius = 18.0f;
                    ImU32 fill = ImGui::GetColorU32(ImVec4(c_colors::top_accent_color.x, c_colors::top_accent_color.y, c_colors::top_accent_color.z, 0.18f));
                    ImU32 outline = ImGui::GetColorU32(ImVec4(c_colors::top_accent_color.x, c_colors::top_accent_color.y, c_colors::top_accent_color.z, 0.9f));
                    draw->AddCircleFilled(pos, radius, fill, 48);
                    draw->AddCircle(pos, radius, outline, 48, 2.0f);
                    draw->AddLine(ImVec2(pos.x - radius * 0.6f, pos.y), ImVec2(pos.x + radius * 0.6f, pos.y), outline, 1.5f);
                    draw->AddLine(ImVec2(pos.x, pos.y - radius * 0.6f), ImVec2(pos.x, pos.y + radius * 0.6f), outline, 1.5f);

                    ImFont* font = c_fonts::verdana_bold ? c_fonts::verdana_bold : ImGui::GetFont();
                    const float font_size = font ? font->LegacySize : ImGui::GetFontSize();
                    const std::string label = "Client";
                    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label.c_str());
                    ImVec2 text_pos(pos.x - text_size.x * 0.5f, pos.y - radius - font_size - 4.0f);
                    ImVec4 text_vec = ImGui::ColorConvertU32ToFloat4(outline);
                    ImVec4 shadow_vec = ImVec4(0.0f, 0.0f, 0.0f, 0.6f);
                    if (font)
                    {
                        draw->AddText(font, font_size, ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), ImGui::GetColorU32(shadow_vec), label.c_str());
                        ::add_text(draw, font, font_size, text_pos, ImGui::GetColorU32(text_vec), label, false);
                    }
                }
            }
        }

        std::vector<std::uintptr_t> active_player_addresses;
        active_player_addresses.reserve(player_count + last_player_snapshot.size());

        std::unordered_set<std::uintptr_t> current_addresses;
        current_addresses.reserve(player_count);

        std::unordered_set<std::uint64_t> current_user_ids;

        for_each_player([&](const cache::player_state& player)
            {
                if (player.user_id != 0)
                {
                    current_user_ids.insert(player.user_id);
                    auto it_prev = previous_user_addresses.find(player.user_id);
                    if (it_prev != previous_user_addresses.end() && it_prev->second != player.address)
                    {
                        reset_player_caches(it_prev->second);
                        last_player_snapshot.erase(it_prev->second);
                    }
                    previous_user_addresses[player.user_id] = player.address;
                }
                return true;
            });

        auto render_player = [&](const cache::player_state& player, bool is_leaving)
            {
                if (player.address == local.address)
                {
                    return;
                }

                player_relation relation = determine_relation(local, player);

                if (features->esp_enemy_only && relation != player_relation::enemy)
                {
                    return;
                }

                if (features->team_check && is_friendly_by_team(local, player))
                {
                    return;
                }

                if (!is_leaving)
                {
                    current_addresses.insert(player.address);
                }

                const auto player_character = player.character.get_address();
                bool character_changed = false;
                {
                    auto it_last_char = last_character_seen.find(player.address);
                    if (it_last_char != last_character_seen.end() && player_character != 0 && it_last_char->second != player_character)
                    {
                        character_changed = true;
                        reset_player_caches(player.address);
                    }
                }

                const bool is_alive = player.health > 0.0f;
                const bool is_dead = !is_alive;
                const bool is_knocked = player.body_effects.knocked;
                const bool is_present = !is_leaving && !is_dead;
                const bool is_dormant = features->draw_dormant && (is_leaving || is_dead || is_knocked);
                const bool is_host = vanille::overlay::is_host(player.user_id, player.name);
                const ImVec4 host_color = features->host_color;

                const float fade_alpha = advance_fade(player.address, player_character, is_present, character_changed);
                if (!(is_leaving && fade_alpha <= 0.001f))
                {
                    active_player_addresses.push_back(player.address);
                    ++diag.rendered_players;
                }

                if (is_leaving && fade_alpha <= 0.001f)
                {
                    last_player_snapshot[player.address] = player;
                    last_character_seen[player.address] = player_character;
                    return;
                }

                std::optional<ImRect> bounds;
                if (features->enable_bounding_box || features->enable_name_esp || features->enable_healthbar || features->enable_armor_bar || features->enable_distance || features->enable_body_status || features->enable_target_snapline)
                {
                    bounds = compute_bounding_box(player, *camera_frame);
                    if (bounds)
                    {
                        ++diag.players_with_bounds;
                    }
                }

                float distance_to_local = -1.0f;
                if (local_root_pos)
                {
                    if (const auto target_root_pos = get_part_position(player.parts.humanoid_root_part))
                    {
                        distance_to_local = (*target_root_pos - *local_root_pos).Length();
                    }
                }

                float distance_alpha = 1.0f;
                bool too_far = false;
                if (distance_to_local > 0.0f && features->max_distance > 0.0f)
                {
                    const float max_dist = features->max_distance;
                    const float start_fade = max_dist * 0.7f;
                    if (distance_to_local >= max_dist)
                    {
                        distance_alpha = 0.0f;
                        too_far = true;
                    }
                    else if (distance_to_local > start_fade)
                    {
                        const float t = (distance_to_local - start_fade) / (max_dist - start_fade);
                        distance_alpha = std::clamp(1.0f - t, 0.0f, 1.0f);
                    }
                }
                float alpha_factor = distance_alpha * fade_alpha;
                if (player.body_effects.knocked)
                {
                    alpha_factor *= 0.375f;
                }

                const bool locked_target = (unified_locked != 0 && unified_locked == player.address);
                const bool hit_flash_enabled = features->enable_highlight && features->enable_hit_flash;
                const bool hit_trace_enabled = features->enable_hit_trace && !is_pf_place;
                const bool hit_log_enabled = features->enable_hit_logs;
                float health_delta = 0.0f;
                if (!is_leaving && (features->enable_healthbar || hit_flash_enabled || hit_trace_enabled || hit_log_enabled))
                {
                    health_delta = register_health_delta(player.address, player.health, features->enable_healthbar, hit_flash_enabled);
                }

                if (hit_log_enabled && locked_target && health_delta <= -1.0f)
                {
                    const int damage = static_cast<int>(std::lround(-health_delta));
                    if (damage > 0)
                    {
                        int name_mode = std::clamp(features->name_esp_mode, 0, 1);
                        std::string resolved_name = (name_mode == 1)
                            ? sanitize_name_label(player.display_name, player.name)
                            : player.name;
                        if (resolved_name.empty())
                        {
                            resolved_name = "unknown";
                        }
                        push_hit_log_event(player.address, resolved_name, damage);
                    }
                }

                float hit_flash_factor = 0.0f;
                if (hit_flash_enabled)
                {
                    hit_flash_factor = advance_hit_flash(player.address);
                }
                else
                {
                    hit_flash_map.erase(player.address);
                }

                if (hit_trace_enabled && locked_target && health_delta <= -1.0f && tracer_start)
                {
                    const rbx::Vector3 origin = *tracer_start;
                    auto target_point = find_closest_point_on_character(player.parts, origin);

                    if (!target_point)
                    {
                        auto fallback = get_part_position(player.parts.head);
                        if (!fallback)
                        {
                            fallback = get_part_position(player.parts.humanoid_root_part);
                        }
                        if (fallback && is_finite_vec3(*fallback))
                        {
                            target_point = *fallback;
                        }
                    }

                    if (target_point)
                    {
                        const float max_dist = std::clamp(features->aim_trace_max_distance, 50.0f, 10000.0f);
                        rbx::Vector3 to_target = *target_point - origin;
                        const float dist = to_target.Length();
                        if (std::isfinite(dist) && dist > max_dist && dist > 1e-3f)
                        {
                            to_target.Normalize();
                            push_hit_tracer(origin, origin + to_target * max_dist);
                        }
                        else
                        {
                            push_hit_tracer(origin, *target_point);
                        }
                    }
                }

                if (alpha_factor <= 0.0f)
                {
                    last_player_snapshot[player.address] = player;
                    last_character_seen[player.address] = player_character;
                    return;
                }

                ImVec4 accent_color = is_host ? host_color : c_colors::top_accent_color;
                accent_color.w = 1.0f;
                ImVec4 highlight_accent = accent_color;
                highlight_accent.x = std::clamp(highlight_accent.x + 0.25f, 0.0f, 1.0f);
                highlight_accent.y = std::clamp(highlight_accent.y + 0.25f, 0.0f, 1.0f);
                highlight_accent.z = std::clamp(highlight_accent.z + 0.25f, 0.0f, 1.0f);
                highlight_accent.w = std::clamp(highlight_accent.w * 0.85f + 0.15f, 0.0f, 1.0f);

                const bool visibility_enabled = visibility::can_run_visibility_check(features->enable_visibility_check);
                visibility::visibility_result visibility_state{};
                if (visibility_enabled)
                {
                    visibility_state = visibility::is_player_visible(player, local, camera_frame->view_matrix);
                }
                else
                {
                    visibility_state.visible = true;
                    visibility_state.coverage = 1.0f;
                }

                if (too_far && (!visibility_enabled || !visibility_state.visible))
                {
                    last_player_snapshot[player.address] = player;
                    last_character_seen[player.address] = player_character;
                    return;
                }

                std::optional<rbx::Vector3> offscreen_target;
                if (const auto root = get_part_position(player.parts.humanoid_root_part))
                {
                    offscreen_target = *root;
                }
                else if (const auto head = get_part_position(player.parts.head))
                {
                    offscreen_target = *head;
                }
                std::optional<rbx::Vector2> on_screen;
                if (offscreen_target)
                {
                    on_screen = rbx::camera::world_to_screen(*offscreen_target, camera_frame->view_matrix, camera_frame->dimensions);
                }
                const bool player_on_screen = on_screen.has_value();

                if (can_draw_offscreen)
                {
                    if (offscreen_target && is_finite_vec3(*offscreen_target))
                    {
                        const rbx::Vector4 clip = rbx::camera::transform(*offscreen_target, camera_frame->view_matrix);
                        const bool offscreen_candidate = !player_on_screen && std::isfinite(clip.w);

                        rbx::Vector3 to_target = *offscreen_target - *camera_pos;
                        const float dist_sq = to_target.LengthSquared();
                        if (offscreen_candidate && std::isfinite(dist_sq) && dist_sq > 1e-4f)
                        {
                            const bool behind = clip.w <= 0.0f;
                            if (behind && offscreen_radius > 0.0f)
                            {
                                const float right_dot = to_target.x * camera_right->x + to_target.y * camera_right->y + to_target.z * camera_right->z;
                                const float up_dot = to_target.x * camera_up->x + to_target.y * camera_up->y + to_target.z * camera_up->z;
                                ImVec2 dir(-right_dot, -up_dot);
                                const float dir_len_sq = dir.x * dir.x + dir.y * dir.y;
                                if (dir_len_sq > 1e-6f)
                                {
                                    const float inv_len = 1.0f / std::sqrt(dir_len_sq);
                                    dir.x *= inv_len;
                                    dir.y *= inv_len;
                                }
                                else
                                {
                                    dir = ImVec2(0.0f, 1.0f);
                                }

                                const ImVec2 tip_raw(offscreen_center.x + dir.x * offscreen_radius, offscreen_center.y + dir.y * offscreen_radius);
                                const ImVec2 base_raw(tip_raw.x - dir.x * offscreen_arrow_length, tip_raw.y - dir.y * offscreen_arrow_length);
                                const ImVec2 perp(-dir.y, dir.x);
                                const ImVec2 left_raw(base_raw.x + perp.x * offscreen_arrow_half_width, base_raw.y + perp.y * offscreen_arrow_half_width);
                                const ImVec2 right_raw(base_raw.x - perp.x * offscreen_arrow_half_width, base_raw.y - perp.y * offscreen_arrow_half_width);
                                const ImVec2 tip(IM_ROUND(tip_raw.x), IM_ROUND(tip_raw.y));
                                const ImVec2 base(IM_ROUND(base_raw.x), IM_ROUND(base_raw.y));
                                const ImVec2 left(IM_ROUND(left_raw.x), IM_ROUND(left_raw.y));
                                const ImVec2 right(IM_ROUND(right_raw.x), IM_ROUND(right_raw.y));

                                ImVec4 arrow_base = features->offscreen_arrow_color;
                                arrow_base = apply_relation_color(relation, arrow_base);
                                arrow_base = apply_host_tint(arrow_base, is_host, host_color);
                                arrow_base = apply_visibility_tint(arrow_base, visibility_state, visibility_enabled);
                                ImVec4 arrow_color = adjust_alpha(arrow_base, alpha_factor);
                                arrow_color = apply_dormant_tint(arrow_color, is_dormant);
                                const ImU32 fill_col = ImGui::GetColorU32(arrow_color);
                                ImVec4 outline_vec = ImVec4(0.0f, 0.0f, 0.0f, arrow_color.w);
                                const ImU32 outline_col = ImGui::GetColorU32(outline_vec);
                                const float outline_thickness = 1.0f;

                                const ImDrawListFlags prev_flags = draw->Flags;
                                draw->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;

                                draw->AddTriangleFilled(tip, left, right, fill_col);
                                draw->AddTriangle(tip, left, right, outline_col, outline_thickness);

                                const float tri_min_x = (std::min)(tip.x, (std::min)(left.x, right.x));
                                const float tri_max_x = (std::max)(tip.x, (std::max)(left.x, right.x));
                                const float tri_min_y = (std::min)(tip.y, (std::min)(left.y, right.y));
                                const float tri_max_y = (std::max)(tip.y, (std::max)(left.y, right.y));

                                if (features->offscreen_arrow_healthbar && player.max_health > 0.0f && player.health > 0.0f)
                                {
                                    const float bar_height = (std::max)(tri_max_y - tri_min_y, offscreen_size * 1.4f);
                                    const float bar_width = 2.0f;
                                    const float bar_x = tri_min_x - bar_width - 4.0f;
                                    const ImVec2 bar_pos(bar_x, tri_min_y);
                                    const ImVec2 bar_size(bar_width, bar_height);
                                    draw_health_bar(player.max_health, player.health, bar_pos, bar_size, alpha_factor, bar_width);
                                }

                                std::string name_label;
                                std::string distance_label;
                                if (features->offscreen_arrow_name)
                                {
                                    const int name_mode = std::clamp(features->name_esp_mode, 0, 1);
                                    const std::string display = name_mode == 1 ? player.display_name : player.name;
                                    name_label = sanitize_name_label(display, player.name);
                                }
                                if (features->offscreen_arrow_distance && distance_to_local > 0.0f)
                                {
                                    std::ostringstream ss;
                                    ss << std::fixed << std::setprecision(0) << distance_to_local;
                                    distance_label = ss.str();
                                }

                                if (!name_label.empty() || !distance_label.empty())
                                {
                                    ImFont* font = nullptr;
                                    switch (features->name_esp_font)
                                    {
                                    case 3:
                                        font = c_fonts::smallest_pixel ? c_fonts::smallest_pixel : nullptr;
                                        break;
                                    case 4:
                                        font = c_fonts::proggy_clean ? c_fonts::proggy_clean : nullptr;
                                        break;
                                    case 2:
                                        font = c_fonts::verdana_bold ? c_fonts::verdana_bold : nullptr;
                                        break;
                                    case 1:
                                        font = c_fonts::verdana_regular ? c_fonts::verdana_regular : nullptr;
                                        break;
                                    default:
                                        font = c_fonts::tahoma ? c_fonts::tahoma : nullptr;
                                        break;
                                    }
                                    if (!font)
                                    {
                                        font = ImGui::GetFont();
                                    }
                                    if (font)
                                    {
                                        const float font_size = font->LegacySize;
                                        ImVec4 text_base = arrow_color;
                                        ImU32 text_col = ImGui::GetColorU32(text_base);
                                        ImVec4 shadow_vec = ImVec4(0.0f, 0.0f, 0.0f, std::clamp(text_base.w * 0.6f, 0.0f, 1.0f));
                                        const bool outline_text = (font == c_fonts::smallest_pixel || font == c_fonts::proggy_clean);

                                        if (!name_label.empty())
                                        {
                                            const ImVec2 text_size = font->CalcTextSizeA(font_size, (std::numeric_limits<float>::max)(), 0.0f, name_label.c_str());
                                            ImVec2 text_pos(((tri_min_x + tri_max_x) * 0.5f) - text_size.x * 0.5f, tri_min_y - text_size.y - 2.0f);
                                            text_pos.x = std::clamp(text_pos.x, 0.0f, camera_frame->dimensions.x - text_size.x);
                                            text_pos.y = std::clamp(text_pos.y, 0.0f, camera_frame->dimensions.y - text_size.y);
                                            if (!outline_text)
                                            {
                                                ::add_text(draw, font, font_size, ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), ImGui::GetColorU32(shadow_vec), name_label, false);
                                            }
                                            ::add_text(draw, font, font_size, text_pos, text_col, name_label, outline_text);
                                        }

                                        if (!distance_label.empty())
                                        {
                                            const ImVec2 text_size = font->CalcTextSizeA(font_size, (std::numeric_limits<float>::max)(), 0.0f, distance_label.c_str());
                                            ImVec2 text_pos(((tri_min_x + tri_max_x) * 0.5f) - text_size.x * 0.5f, tri_max_y + 2.0f);
                                            text_pos.x = std::clamp(text_pos.x, 0.0f, camera_frame->dimensions.x - text_size.x);
                                            text_pos.y = std::clamp(text_pos.y, 0.0f, camera_frame->dimensions.y - text_size.y);
                                            const bool distance_outline = outline_text;
                                            if (!distance_outline)
                                            {
                                                ::add_text(draw, font, font_size, ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), ImGui::GetColorU32(shadow_vec), distance_label, false);
                                            }
                                            ::add_text(draw, font, font_size, text_pos, text_col, distance_label, distance_outline);
                                        }
                                    }
                                }

                                draw->Flags = prev_flags;
                            }
                        }
                    }
                }

                if (!player_on_screen)
                {
                    if (!is_leaving)
                    {
                        last_player_snapshot[player.address] = player;
                        last_character_seen[player.address] = player_character;
                    }
                    return;
                }

                if (features->enable_target_snapline && locked_target && !is_leaving)
                {
                    auto head_screen = project_part_to_screen(player.parts.head, *camera_frame);
                    if (!head_screen && bounds)
                    {
                        head_screen = ImVec2((bounds->Min.x + bounds->Max.x) * 0.5f, bounds->Min.y);
                    }
                    if (head_screen)
                    {
                        target_snapline_head_screen = *head_screen;
                        target_snapline_alpha = alpha_factor;
                    }
                }

                if (features->enable_highlight)
                {
                    std::vector<Clipper2Lib::PathD> hulls;
                    std::vector<Clipper2Lib::PathD> wireframe_fallback_hulls;
                    std::vector<projected_wireframe_segment> wireframe_segments;
                    std::vector<Clipper2Lib::PathD> metallic_outline_hulls;
                    const auto parts = collect_parts(player.parts);
                    hulls.reserve(parts.size());
                    wireframe_fallback_hulls.reserve(parts.size());
                    metallic_outline_hulls.reserve(parts.size());
                    float mesh_distance_cutoff = 600.0f;
                    if (features->max_distance > 0.0f)
                    {
                        mesh_distance_cutoff = (std::min)(mesh_distance_cutoff, features->max_distance);
                    }
                    mesh_distance_cutoff = (std::min)(mesh_distance_cutoff, 500.0f);
                    const bool mesh_distance_ok = (distance_to_local <= 0.0f) || (distance_to_local <= mesh_distance_cutoff);
                    const bool allow_mesh_hulls = mesh_distance_ok;
                    const bool is_r6_character = !player.parts.is_r15;
                    const bool mesh_mode = (highlight_mode == 1);
                    const bool mesh_preferred = (mesh_mode && allow_mesh_hulls);
                    const bool wireframe_mode = (highlight_mode == 2);
                    const bool mesh_allowed = allow_mesh_hulls;
                    const bool shader_mesh_mode = mesh_mode && mesh_allowed && (highlight_mesh_material != k_mesh_material_flat);
                    bool queued_shader_mesh = false;
                    std::size_t mesh_sample_limit = 400;
                    if (allow_mesh_hulls && distance_to_local > 0.0f)
                    {
                        const float t = std::clamp(distance_to_local / mesh_distance_cutoff, 0.0f, 1.0f);
                        const float lerp = 400.0f + (80.0f - 400.0f) * t;
                        mesh_sample_limit = static_cast<std::size_t>(std::lround(std::clamp(lerp, 80.0f, 400.0f)));
                    }

                    ImVec4 base_fill = locked_target ? ImVec4(accent_color.x, accent_color.y, accent_color.z, 0.25f) : features->highlight_fill_color;
                    base_fill = apply_relation_color(relation, base_fill);
                    base_fill = apply_host_tint(base_fill, is_host, host_color);
                    base_fill = apply_visibility_tint(base_fill, visibility_state, visibility_enabled);
                    ImVec4 fill = base_fill;
                    fill = apply_dormant_tint(fill, is_dormant);
                    if (is_dormant)
                    {
                        fill.w = 0.8f;
                    }
                    if (hit_flash_factor > 0.0f)
                    {
                        fill = apply_hit_flash(fill, hit_flash_factor);
                    }
                    fill = adjust_alpha(fill, alpha_factor);

                    ImVec4 base_outline = locked_target ? highlight_accent : features->highlight_outline_color;
                    if (locked_target && mesh_mode && highlight_mesh_material != k_mesh_material_flat)
                    {
                        base_outline.w = 0.0f;
                    }
                    base_outline = apply_relation_color(relation, base_outline);
                    base_outline = apply_host_tint(base_outline, is_host, host_color);
                    base_outline = apply_visibility_tint(base_outline, visibility_state, visibility_enabled);
                    ImVec4 outline = adjust_alpha(base_outline, alpha_factor);
                    outline = apply_dormant_tint(outline, is_dormant);
                    if (wireframe_mode && hit_flash_factor > 0.0f)
                    {
                        outline = apply_hit_flash(outline, hit_flash_factor);
                    }

                    const ImU32 fill_u32 = ImGui::GetColorU32(fill);
                    const ImU32 outline_u32 = ImGui::GetColorU32(outline);
                    const rbx::Vector3 camera_world = local.camera.get_camera_position();
                    const SimpleVector3 metallic_camera_position(camera_world.x, camera_world.y, camera_world.z);

                    for (const auto* part : parts)
                    {
                        if (!part)
                        {
                            continue;
                        }

                        const std::string part_name = part->instance.is_valid() ? part->instance.get_name() : std::string{};
                        const bool is_head_part = (part_name == "Head");
                        const bool allow_mesh_for_part = mesh_allowed && (!is_r6_character || is_head_part);
                        const bool mesh_preferred_for_part = mesh_preferred && allow_mesh_for_part;
                        const bool shader_mesh_for_part = shader_mesh_mode;

                        if (wireframe_mode)
                        {
                            bool has_mesh_edges = false;
                            if (allow_mesh_for_part)
                            {
                                has_mesh_edges = project_mesh_wireframe_edges(*part, *camera_frame, wireframe_segments);
                            }
                            if (!has_mesh_edges)
                            {
                                if (auto fallback_hull = project_part_hull(*part, *camera_frame))
                                {
                                    wireframe_fallback_hulls.push_back(std::move(*fallback_hull));
                                }
                            }
                            continue;
                        }

                        if (shader_mesh_for_part)
                        {
                            ImVec4 material_fill = fill;
                            material_fill.w = 1.0f;
                            if (queue_metallic_mesh_draw(*part, *camera_frame, metallic_camera_position, material_fill, highlight_mesh_material))
                            {
                                queued_shader_mesh = true;
                                if (auto outline_hull = project_mesh_hull(*part, *camera_frame, mesh_sample_limit))
                                {
                                    metallic_outline_hulls.push_back(std::move(*outline_hull));
                                }
                                else if (auto fallback_hull = project_part_hull(*part, *camera_frame))
                                {
                                    metallic_outline_hulls.push_back(std::move(*fallback_hull));
                                }
                                continue;
                            }
                        }

                        std::optional<Clipper2Lib::PathD> hull;
                        if (mesh_preferred_for_part)
                        {
                            hull = project_mesh_hull(*part, *camera_frame, mesh_sample_limit);
                            if (!hull)
                            {
                                hull = project_part_hull(*part, *camera_frame);
                            }
                        }
                        else
                        {
                            hull = project_part_hull(*part, *camera_frame);
                            if (!hull && allow_mesh_for_part)
                            {
                                hull = project_mesh_hull(*part, *camera_frame, mesh_sample_limit);
                            }
                        }

                        if (hull)
                        {
                            hulls.push_back(std::move(*hull));
                        }
                    }

                    if (wireframe_mode)
                    {
                        if (!wireframe_segments.empty() || !wireframe_fallback_hulls.empty())
                        {
                            draw_wireframe_segments(wireframe_segments, outline_u32);
                            if (!wireframe_fallback_hulls.empty())
                            {
                                draw_highlight_hulls(wireframe_fallback_hulls, 0, outline_u32, true);
                            }
                        }
                    }
                    else
                    {
                        if (queued_shader_mesh && !metallic_outline_hulls.empty())
                        {
                            draw_highlight_hulls(metallic_outline_hulls, 0, outline_u32, true);
                        }
                        if (!hulls.empty())
                        {
                            if (queued_shader_mesh && shader_mesh_mode)
                            {
                                draw_highlight_hulls(hulls, 0, outline_u32, true);
                            }
                            else
                            {
                                draw_highlight_hulls(hulls, fill_u32, outline_u32);
                            }
                        }
                    }
                }

                if (features->enable_skeleton)
                {
                    draw_skeleton(player, *camera_frame, alpha_factor, relation, locked_target, is_dormant, is_host, visibility_state, visibility_enabled);
                }

                if (features->enable_bounding_box && bounds)
                {
                    const ImVec2 pos = bounds->Min;
                    ImVec2 size(bounds->Max.x - bounds->Min.x, bounds->Max.y - bounds->Min.y);
                    const int box_style = std::clamp(features->bounding_box_style, 0, 1);

                    if (box_style == 1 && features->enable_healthbar)
                    {
                        constexpr float health_padding = 1.0f;
                        size.y += health_padding * 2.0f;
                    }

                    if (is_valid_box(size))
                    {
                        ImVec4 box_base = locked_target ? accent_color : features->bounding_box_color;
                        box_base = apply_relation_color(relation, box_base);
                        box_base = apply_host_tint(box_base, is_host, host_color);
                        box_base = apply_visibility_tint(box_base, visibility_state, visibility_enabled);
                        ImVec4 box_col = adjust_alpha(box_base, alpha_factor);
                        box_col = apply_dormant_tint(box_col, is_dormant);
                        if (box_style == 1)
                        {
                            const ImVec2 corner_pos(pos.x, pos.y - (features->enable_healthbar ? 1.0f : 0.0f));
                            draw_corner_box(corner_pos, size, ImGui::GetColorU32(box_col));
                        }
                        else
                        {
                            draw_outlined_rectangle(pos, size, ImGui::GetColorU32(box_col));
                        }
                    }
                }

                bool armor_drawn = false;
                ImVec2 armor_bar_position{};
                ImVec2 armor_bar_size{};

                if (features->enable_healthbar && bounds)
                {
                    if (player.max_health > 0.0f && player.health >= 0.0f)
                    {
                        constexpr float bar_width = 2.0f;
                        constexpr float bar_padding = 4.0f;
                        const ImVec2 bar_position(bounds->Min.x - bar_padding - bar_width, bounds->Min.y);
                        const ImVec2 bar_size(bar_width, bounds->Max.y - bounds->Min.y);
                        const float displayed_health = get_smoothed_health(player.address, player.health);
                        ImU32 health_override = 0;
                        if (is_dormant)
                        {
                            ImVec4 dc = adjust_alpha(dormant_tint_base(), alpha_factor);
                            health_override = ImGui::GetColorU32(dc);
                        }
                        else if (visibility_enabled && !visibility_state.visible)
                        {
                            ImVec4 red = features->occluded_color;
                            red = adjust_alpha(red, alpha_factor);
                            health_override = ImGui::GetColorU32(red);
                        }
                        draw_health_bar(player.max_health, displayed_health, bar_position, bar_size, alpha_factor, bar_width, health_override);
                        draw_health_delta_text(player.address, bar_position, bar_size, displayed_health, player.max_health, alpha_factor, is_dormant);
                    }
                }

                if (features->enable_armor_bar && bounds)
                {
                    const float armor_value = static_cast<float>(player.body_effects.armor);
                    register_armor_delta(player.address, armor_value);
                    const bool has_armor = armor_value > 0.0f;

                    constexpr float bar_height = 2.0f;
                    constexpr float bar_padding_y = 4.0f;
                    const float box_width = std::round(bounds->Max.x - bounds->Min.x);
                    const float bar_width = (std::max)(0.0f, box_width + 2.0f);
                    const float bar_x = std::round(bounds->Min.x - 1.0f);
                    const float bar_y = std::round(bounds->Max.y) + bar_padding_y;
                    armor_bar_position = ImVec2(bar_x, bar_y);
                    armor_bar_size = ImVec2(bar_width, bar_height);
                    const float displayed_armor = has_armor ? get_smoothed_armor(player.address, armor_value) : 0.0f;
                    constexpr float armor_max = 100.0f;
                    if (has_armor)
                    {
                        ImU32 armor_override = 0;
                        if (is_dormant)
                        {
                            ImVec4 dc = adjust_alpha(dormant_tint_base(), alpha_factor);
                            armor_override = ImGui::GetColorU32(dc);
                        }
                        else if (visibility_enabled && !visibility_state.visible)
                        {
                            ImVec4 red = features->occluded_color;
                            red = adjust_alpha(red, alpha_factor);
                            armor_override = ImGui::GetColorU32(red);
                        }
                        draw_armor_bar(armor_max, displayed_armor, armor_bar_position, armor_bar_size, alpha_factor, armor_override);
                        ::draw_armor_delta_text(player.address, armor_bar_position, armor_bar_size, displayed_armor, armor_max, alpha_factor, is_dormant);
                        armor_drawn = true;
                    }
                }

                if (features->enable_body_status && bounds)
                {
                    std::vector<std::string> status_labels;
                    status_labels.reserve(6);

                    if (features->show_status_movement)
                    {
                        const float now = static_cast<float>(ImGui::GetTime());
                        constexpr float jump_linger = 0.5f;

                        bool jump_active = false;
                        if (player.humanoid.is_valid())
                        {
                            if (const auto live_jump = rbx::humanoid::get_jump(player.humanoid))
                            {
                                if (*live_jump)
                                {
                                    jump_active = true;
                                    last_jump_time_map[player.address] = now;
                                }
                            }
                        }
                        if (player.movement.has_jump_state && player.movement.jumping)
                        {
                            jump_active = true;
                            last_jump_time_map[player.address] = now;
                        }
                        else
                        {
                            auto it = last_jump_time_map.find(player.address);
                            if (it != last_jump_time_map.end() && (now - it->second) <= jump_linger)
                            {
                                jump_active = true;
                            }
                        }

                        const bool movement_known = player.movement.has_move_direction || player.movement.has_jump_state || jump_active;
                        if (movement_known)
                        {
                            const bool moving = player.movement.has_move_direction && player.movement.moving;
                            status_labels.emplace_back(moving ? "Moving" : "Idle");
                            if (jump_active)
                            {
                                status_labels.emplace_back("Jumping");
                            }
                        }
                    }

                    if (features->show_status_reload && player.body_effects.reload)
                    {
                        status_labels.emplace_back("Reloading");
                    }
                    if (features->show_status_grabbed && player.body_effects.grabbed)
                    {
                        status_labels.emplace_back("Grabbed");
                    }
                    if (features->show_status_gun_firing && player.body_effects.gun_firing)
                    {
                        status_labels.emplace_back("Shooting");
                    }
                    if (features->show_status_knocked && player.body_effects.knocked)
                    {
                        status_labels.emplace_back("KO");
                    }

                    if (!status_labels.empty())
                    {
                        ImFont* font = c_fonts::smallest_pixel ? c_fonts::smallest_pixel : ImGui::GetFont();
                        const float font_size = 10.0f;
                        ImDrawList* draw = ImGui::GetBackgroundDrawList();
                        const float padding_x = 4.0f;
                        const float text_x = bounds->Max.x + padding_x;

                        float text_y = bounds->Min.y;
                        for (const auto& label : status_labels)
                        {
                            const ImVec2 text_size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label.c_str()) : ImVec2(0, 0);
                            ImVec2 pos(text_x, text_y - 3.0f);
                            ImVec4 status_base = locked_target ? accent_color : features->status_color;
                            status_base = apply_relation_color(relation, status_base);
                            status_base = apply_host_tint(status_base, is_host, host_color);
                            status_base = apply_visibility_tint(status_base, visibility_state, visibility_enabled);
                            ImVec4 status_color = adjust_alpha(status_base, alpha_factor);
                            status_color = apply_dormant_tint(status_color, is_dormant);
                            const bool outline = (font == c_fonts::smallest_pixel);
                            ::add_text(draw, font, font_size, pos, ImGui::GetColorU32(status_color), label, outline);
                            text_y += text_size.y;
                        }
                    }
                }

                if (features->enable_distance && bounds && local_root_pos)
                {
                    const auto target_root_pos = get_part_position(player.parts.humanoid_root_part);
                    if (target_root_pos)
                    {
                        const float distance = (*target_root_pos - *local_root_pos).Length();

                        std::ostringstream ss;
                        ss << std::fixed << std::setprecision(0) << distance;
                        const std::string text = ss.str();

                        ImFont* font = c_fonts::smallest_pixel ? c_fonts::smallest_pixel : ImGui::GetFont();
                        const float font_size = 10.0f;
                        const ImVec2 text_size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str()) : ImVec2(0, 0);

                        const bool armor_delta_active = armor_delta_map.find(player.address) != armor_delta_map.end();
                        float base_y = armor_drawn ? (armor_bar_position.y + armor_bar_size.y - 1.0f) : bounds->Max.y;
                        if (armor_drawn && armor_delta_active)
                        {
                            const ImVec2 delta_size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "+0") : ImVec2(0, 0);
                            const float min_under_delta = armor_bar_position.y + armor_bar_size.y + delta_size.y + 1.0f;
                            base_y = (std::max)(base_y, min_under_delta);
                        }
                        float text_x = bounds->Min.x + ((bounds->Max.x - bounds->Min.x) - text_size.x) * 0.5f;
                        text_x = std::clamp(text_x, 0.0f, camera_frame->dimensions.x - text_size.x);
                        float target_y = std::clamp(base_y, 0.0f, camera_frame->dimensions.y - text_size.y);

                        auto& anim = distance_anim_map[player.address];
                        const float duration = 0.15f;
                        if (!armor_delta_active)
                        {
                            anim.current_y = target_y;
                            anim.target_y = target_y;
                            anim.start_y = target_y;
                            anim.timer = duration;
                        }
                        else
                        {
                            if (anim.current_y == 0.0f && anim.target_y == 0.0f && anim.timer == 0.0f)
                            {
                                anim.current_y = target_y;
                                anim.target_y = target_y;
                                anim.start_y = target_y;
                            }

                            if (std::fabs(anim.target_y - target_y) > 0.5f)
                            {
                                anim.start_y = anim.current_y;
                                anim.target_y = target_y;
                                anim.timer = 0.0f;
                            }

                            anim.timer += ImGui::GetIO().DeltaTime;
                            float progress = ease_expo(anim.timer, 0.0f, duration);
                            anim.current_y = anim.start_y + (anim.target_y - anim.start_y) * progress;
                            if (progress >= 1.0f)
                            {
                                anim.current_y = anim.target_y;
                                anim.start_y = anim.target_y;
                            }
                        }

                        float text_y = std::clamp(anim.current_y + 1.0f, 0.0f, camera_frame->dimensions.y - text_size.y);

                        ImDrawList* draw = ImGui::GetBackgroundDrawList();
                        ImVec4 distance_base = locked_target ? accent_color : features->distance_color;
                        distance_base = apply_relation_color(relation, distance_base);
                        distance_base = apply_host_tint(distance_base, is_host, host_color);
                        distance_base = apply_visibility_tint(distance_base, visibility_state, visibility_enabled);
                        ImVec4 distance_color = adjust_alpha(distance_base, alpha_factor);
                        distance_color = apply_dormant_tint(distance_color, is_dormant);
                        const ImU32 text_color = ImGui::GetColorU32(distance_color);
                        const bool outline = (font == c_fonts::smallest_pixel);
                        ::add_text(draw, font, font_size, ImVec2(text_x, text_y), text_color, text, outline);
                    }
                }

                if (features->enable_name_esp && bounds)
                {
                    int mode = features->name_esp_mode;
                    if (mode < 0 || mode > 1)
                    {
                        mode = 0;
                    }

                    std::string label = (mode == 1) ? sanitize_name_label(player.display_name, player.name) : player.name;
                    if (label.empty())
                    {
                        return;
                    }

                    ImFont* name_font = nullptr;
                    switch (features->name_esp_font)
                    {
                    case 3:
                        name_font = c_fonts::smallest_pixel ? c_fonts::smallest_pixel : nullptr;
                        break;
                    case 4:
                        name_font = c_fonts::proggy_clean ? c_fonts::proggy_clean : nullptr;
                        break;
                    case 2:
                        name_font = c_fonts::verdana_bold ? c_fonts::verdana_bold : nullptr;
                        break;
                    case 1:
                        name_font = c_fonts::verdana_regular ? c_fonts::verdana_regular : nullptr;
                        break;
                    default:
                        name_font = c_fonts::tahoma ? c_fonts::tahoma : nullptr;
                        break;
                    }
                    if (!name_font)
                    {
                        name_font = ImGui::GetFont();
                    }
                    if (!name_font)
                    {
                        return;
                    }
                    const float name_font_size = name_font->LegacySize;
                    const float box_width = bounds->Max.x - bounds->Min.x;
                    const ImVec2 text_size = name_font->CalcTextSizeA(name_font_size, (std::numeric_limits<float>::max)(), 0.0f, label.c_str());

                    float text_x = bounds->Min.x + (box_width - text_size.x) * 0.5f;
                    float text_y = bounds->Min.y - text_size.y - 2.0f;
                    text_x = std::clamp(text_x, 0.0f, camera_frame->dimensions.x - text_size.x);
                    text_y = (std::max)(0.0f, text_y);

                    ImVec2 text_pos(text_x, text_y);
                    ImVec4 name_base = locked_target ? accent_color : features->name_esp_color;
                    name_base = apply_relation_color(relation, name_base);
                    name_base = apply_host_tint(name_base, is_host, host_color);
                    name_base = apply_visibility_tint(name_base, visibility_state, visibility_enabled);
                    ImVec4 name_color_vec = adjust_alpha(name_base, alpha_factor);
                    name_color_vec = apply_dormant_tint(name_color_vec, is_dormant);
                    ImVec4 shadow_vec4 = adjust_alpha(ImVec4(c_colors::black.x, c_colors::black.y, c_colors::black.z, 0.5f), alpha_factor);
                    const ImU32 text_color = ImGui::GetColorU32(name_color_vec);
                    const ImU32 shadow_color = ImGui::GetColorU32(shadow_vec4);
                    const bool draw_outline = should_outline(name_font);
                    const bool draw_shadow = !draw_outline;
                    if (draw_shadow)
                    {
                        ::add_text(draw, name_font, name_font_size, ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), shadow_color, label, false);
                    }
                    ::add_text(draw, name_font, name_font_size, text_pos, text_color, label, draw_outline);
                }

                if (!is_leaving)
                {
                    last_player_snapshot[player.address] = player;
                    last_character_seen[player.address] = player_character;
                }
            };

        for_each_player([&](const cache::player_state& player)
            {
                render_player(player, false);
                return true;
            });

        for (auto it = last_player_snapshot.begin(); it != last_player_snapshot.end(); ++it)
        {
            if (!current_addresses.contains(it->first))
            {
                render_player(it->second, true);
            }
        }

        if (features->enable_target_snapline && target_snapline_head_screen)
        {
            if (const auto cursor = get_cursor_client_position(camera_frame->dimensions))
            {
                draw_target_snapline(draw, ImVec2(cursor->x, cursor->y), *target_snapline_head_screen, target_snapline_alpha);
            }
        }

        if (!esp_enabled)
        {
            return;
        }

        for (auto it = last_player_snapshot.begin(); it != last_player_snapshot.end();)
        {
            const bool still_active = std::find(active_player_addresses.begin(), active_player_addresses.end(), it->first) != active_player_addresses.end();
            if (!still_active)
            {
                it = last_player_snapshot.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (wants_dead_body_highlight)
        {
            std::vector<std::uintptr_t> active_dead_body_addresses;
            active_dead_body_addresses.reserve(dead_body_count + last_dead_body_snapshot.size());

            std::unordered_set<std::uintptr_t> current_dead_body_addresses;
            current_dead_body_addresses.reserve(dead_body_count);

            auto draw_dead_body_snapshot = [&](const dead_body_render_snapshot& snapshot, float fade_alpha)
                {
                    if (snapshot.hulls.empty())
                    {
                        return;
                    }

                    ImVec4 base_fill = features->highlight_fill_color;
                    ImVec4 base_outline = features->highlight_outline_color;
                    if (features->override_dormant_color)
                    {
                        base_fill = apply_dormant_tint(base_fill, true);
                        base_outline = apply_dormant_tint(base_outline, true);
                    }

                    ImVec4 fill = adjust_alpha(base_fill, fade_alpha);
                    ImVec4 outline = adjust_alpha(base_outline, fade_alpha);
                    draw_highlight_hulls(snapshot.hulls, ImGui::GetColorU32(fill), ImGui::GetColorU32(outline));

                    if (snapshot.torso_bounds)
                    {
                        ImFont* font = c_fonts::verdana_bold ? c_fonts::verdana_bold : ImGui::GetFont();
                        if (font)
                        {
                            const char* label = "Dead";
                            const float font_size = font->LegacySize;
                            const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label);
                            const ImRect& bounds = *snapshot.torso_bounds;
                            const ImVec2 center((bounds.Min.x + bounds.Max.x) * 0.5f, (bounds.Min.y + bounds.Max.y) * 0.5f);
                            ImVec2 pos(center.x - text_size.x * 0.5f, center.y - text_size.y * 0.5f);
                            pos.x = std::clamp(pos.x, 0.0f, camera_frame->dimensions.x - text_size.x);
                            pos.y = std::clamp(pos.y, 0.0f, camera_frame->dimensions.y - text_size.y);

                            ImVec4 text_vec = base_outline;
                            text_vec.w = std::clamp(text_vec.w * fade_alpha, 0.0f, 1.0f);
                            ImVec4 shadow_vec(0.0f, 0.0f, 0.0f, std::clamp(text_vec.w * 0.7f, 0.0f, 1.0f));

                            const ImU32 text_color = ImGui::GetColorU32(text_vec);
                            const ImU32 shadow_color = ImGui::GetColorU32(shadow_vec);
                            draw->AddText(font, font_size, ImVec2(pos.x + 1.0f, pos.y + 1.0f), shadow_color, label);
                            ::add_text(draw, font, font_size, pos, text_color, label, false);
                        }
                    }
                };

            auto render_dead_body = [&](const cache::dead_body_state& body)
                {
                    if (body.address == 0)
                    {
                        return;
                    }

                    current_dead_body_addresses.insert(body.address);

                    const bool is_present = !body.expired;
                    const float fade_alpha = advance_dead_body_fade(body.address, is_present);
                    if (!is_present && fade_alpha <= 0.001f)
                    {
                        return;
                    }

                    dead_body_render_snapshot snapshot{};
                    build_dead_body_snapshot(body, *camera_frame, snapshot);
                    if (snapshot.hulls.empty())
                    {
                        return;
                    }

                    active_dead_body_addresses.push_back(body.address);
                    draw_dead_body_snapshot(snapshot, fade_alpha);
                    last_dead_body_snapshot[body.address] = std::move(snapshot);
                };

            if (dead_bodies_snapshot)
            {
                for (const auto& body : *dead_bodies_snapshot)
                {
                    render_dead_body(body);
                }
            }

            for (const auto& entry : last_dead_body_snapshot)
            {
                if (!current_dead_body_addresses.contains(entry.first))
                {
                    const float fade_alpha = advance_dead_body_fade(entry.first, false);
                    if (fade_alpha <= 0.001f)
                    {
                        continue;
                    }

                    active_dead_body_addresses.push_back(entry.first);
                    draw_dead_body_snapshot(entry.second, fade_alpha);
                }
            }

            prune_dead_body_entries(active_dead_body_addresses);
        }
        else
        {
            dead_body_fade_map.clear();
            last_dead_body_snapshot.clear();
        }

        prune_health_entries(active_player_addresses);
        ::prune_inactive_entries(active_player_addresses);

        render_hit_tracers(*camera_frame);

        if (features->debug_overlay)
        {
            std::vector<cache::player_state> debug_players;
            debug_players.reserve(player_count);
            for_each_player([&](const cache::player_state& player)
                {
                    debug_players.push_back(player);
                    return true;
                });
            log_player_events(debug_players);
        }

        for (auto it = previous_user_addresses.begin(); it != previous_user_addresses.end();)
        {
            if (!current_user_ids.contains(it->first))
            {
                reset_player_caches(it->second);
                last_player_snapshot.erase(it->second);
                it = previous_user_addresses.erase(it);
            }
            else
            {
                ++it;
            }
        }

        constexpr std::uint64_t k_hull_cache_max_age = 120;
        if ((highlight_frame_id % k_hull_cache_max_age) == 0)
        {
            prune_hull_cache(part_hull_cache, highlight_frame_id, k_hull_cache_max_age);
            prune_hull_cache(mesh_hull_cache, highlight_frame_id, k_hull_cache_max_age);
            prune_mesh_sample_cache(mesh_sample_cache, highlight_frame_id, k_hull_cache_max_age);
            prune_mesh_wireframe_cache(mesh_wireframe_cache, highlight_frame_id, k_hull_cache_max_age);
            prune_mesh_material_cache(mesh_material_cache, highlight_frame_id, k_hull_cache_max_age);
        }

        if (draw)
        {
            diag.draw_primitives = static_cast<std::size_t>(draw->VtxBuffer.Size);
        }
        publish_diag(esp_enabled ? "rendering" : "aux_only");
    }

    static void draw_outlined_rectangle_on(ImDrawList* draw, const ImVec2& position, const ImVec2& size, const ImU32 color, float rounding = 0.0f)
    {
        if (!draw)
        {
            return;
        }

        const ImVec2 rounded_position(std::round(position.x), std::round(position.y));
        const ImVec2 rounded_size(std::round(size.x), std::round(size.y));

        ImVec2 rect_max = ImVec2(rounded_position.x + rounded_size.x, rounded_position.y + rounded_size.y);
        ImRect rectangle(rounded_position, rect_max);

        const float max_rounding = (std::min)(rounded_size.x, rounded_size.y) / 2.0f;
        rounding = (std::min)(rounding, max_rounding);

        const int outline_alpha = static_cast<int>(((color >> 24) & 0xFF) * 0.5f);
        draw->AddRect(rectangle.Min, rectangle.Max, IM_COL32(15, 15, 15, outline_alpha), rounding);
        draw->AddRect(ImVec2(rectangle.Min.x - 2.0f, rectangle.Min.y - 2.0f), ImVec2(rectangle.Max.x + 2.0f, rectangle.Max.y + 2.0f), IM_COL32(15, 15, 15, outline_alpha), rounding);
        draw->AddRect(ImVec2(rectangle.Min.x - 1.0f, rectangle.Min.y - 1.0f), ImVec2(rectangle.Max.x + 1.0f, rectangle.Max.y + 1.0f), color, rounding);
    }

    static void draw_corner_box_on(ImDrawList* draw, const ImVec2& position, const ImVec2& size, const ImU32 color)
    {
        if (!draw || !is_valid_box(size, 4.0f))
        {
            return;
        }

        const float min_x = std::round(position.x);
        const float min_y = std::round(position.y);
        const float max_x = std::round(position.x + size.x);
        const float max_y = std::round(position.y + size.y);

        const float width = max_x - min_x;
        const float height = max_y - min_y;
        if (width <= 0.0f || height <= 0.0f)
        {
            return;
        }

        const float stroke = 1.0f;
        const float border = 1.0f;
        const float accent = 1.0f;
        const float min_side = (std::min)(width, height);
        const float len = std::clamp(min_side * 0.30f, stroke * 2.0f, min_side);

        const int outline_alpha = static_cast<int>(((color >> 24) & 0xFF) * 1.0f);
        const ImU32 accent_color = IM_COL32(0, 0, 0, outline_alpha);
        const ImU32 border_color = accent_color;

        auto draw_horizontal = [&](float x, float y, bool inside_down)
            {
                const float white_y0 = inside_down ? y : y - stroke;
                const float white_y1 = white_y0 + stroke;

                const float top_y0 = white_y0 - border;
                const float top_y1 = white_y0;
                const float bot_y0 = white_y1;
                const float bot_y1 = white_y1 + border;

                const float left_x0 = x - border;
                const float left_x1 = x;
                const float right_x0 = x + stroke;
                const float right_x1 = x + stroke + border;

                const float body_x0 = x;
                const float body_x1 = x + len;
                const float cap_x0 = body_x1;
                const float cap_x1 = body_x1 + accent;

                draw->AddRectFilled(ImVec2(body_x0 - border, top_y0), ImVec2(body_x1 + border, top_y1), border_color);
                draw->AddRectFilled(ImVec2(body_x0 - border, bot_y0), ImVec2(body_x1 + border, bot_y1), border_color);
                draw->AddRectFilled(ImVec2(left_x0, white_y0), ImVec2(left_x1, white_y1), border_color);
                draw->AddRectFilled(ImVec2(right_x0, white_y0), ImVec2(right_x1, white_y1), border_color);

                draw->AddRectFilled(ImVec2(cap_x0, white_y0), ImVec2(cap_x1, white_y1), accent_color);
                draw->AddRectFilled(ImVec2(body_x0, white_y0), ImVec2(body_x1, white_y1), color);
            };

        auto draw_vertical = [&](float x, float y, bool inside_right)
            {
                const float white_x0 = inside_right ? x : x - stroke;
                const float white_x1 = white_x0 + stroke;

                const float left_x0 = white_x0 - border;
                const float left_x1 = white_x0;
                const float right_x0 = white_x1;
                const float right_x1 = white_x1 + border;

                const float body_y0 = y;
                const float body_y1 = y + len;
                const float cap_y0 = body_y1;
                const float cap_y1 = body_y1 + accent;

                const float top_y0 = body_y0 - border;
                const float top_y1 = body_y0;
                const float bot_y0 = body_y1;
                const float bot_y1 = body_y1 + border;

                draw->AddRectFilled(ImVec2(left_x0, body_y0 - border), ImVec2(left_x1, body_y1 + border), border_color);
                draw->AddRectFilled(ImVec2(right_x0, body_y0 - border), ImVec2(right_x1, body_y1 + border), border_color);
                draw->AddRectFilled(ImVec2(white_x0, top_y0), ImVec2(white_x1, top_y1), border_color);
                draw->AddRectFilled(ImVec2(white_x0, bot_y0), ImVec2(white_x1, bot_y1), border_color);

                draw->AddRectFilled(ImVec2(white_x0, cap_y0), ImVec2(white_x1, cap_y0 + accent), accent_color);

                draw->AddRectFilled(ImVec2(white_x0, body_y0), ImVec2(white_x1, body_y1), color);
            };

        draw_horizontal(min_x, min_y, true);
        draw_vertical(min_x, min_y, true);

        draw_horizontal(max_x - len - accent, min_y, true);
        draw_vertical(max_x, min_y, false);

        draw_horizontal(min_x, max_y, false);
        draw_vertical(min_x, max_y - len - accent, true);

        draw_horizontal(max_x - len - accent, max_y, false);
        draw_vertical(max_x, max_y - len - accent, false);
        draw->AddRectFilled(ImVec2(max_x - border, max_y - border), ImVec2(max_x, max_y), border_color);
        draw->AddRectFilled(ImVec2(max_x - stroke, max_y - stroke), ImVec2(max_x, max_y), color);
        draw->AddRectFilled(ImVec2(max_x, max_y - border), ImVec2(max_x + border, max_y + 1.0f), border_color);

        draw->AddRectFilled(ImVec2(min_x - border, min_y - border), ImVec2(min_x, min_y), border_color);
    }

    static void draw_health_bar_on(ImDrawList* draw, float max_health, float current_health, const ImVec2& position, const ImVec2& size, float alpha_factor, float bar_width = 2.0f, ImU32 override_color = 0)
    {
        if (!draw || max_health <= 0.0f || current_health <= 0.0f)
        {
            return;
        }

        const int padding_top = 1;
        const int padding_bottom = 1;
        const float clamped_health = std::clamp(current_health, 0.0f, max_health);
        const float fill_ratio = clamped_health / max_health;
        const float full_height = std::round(size.y) + padding_top + padding_bottom;
        const float bar_height = std::round(full_height * fill_ratio);
        const float bar_x = std::round(position.x + size.x - bar_width);
        const float bar_top = std::round(position.y - static_cast<float>(padding_top));
        const float bar_bottom = bar_top + full_height;

        ImVec2 bar_background_top = ImVec2(bar_x, bar_top);
        ImVec2 bar_background_bottom = ImVec2(bar_x + bar_width, bar_bottom);
        ImVec2 bar_foreground_top = ImVec2(bar_x, bar_bottom - bar_height);
        ImVec2 bar_foreground_bottom = bar_background_bottom;

        const ImU32 border_color = adjust_alpha(IM_COL32(0, 0, 0, 128), alpha_factor);
        const ImU32 background_color = adjust_alpha(IM_COL32(75, 75, 75, 255), alpha_factor);

        draw->AddRectFilled(ImVec2(bar_background_top.x - 1.0f, bar_background_top.y - 1.0f), ImVec2(bar_background_bottom.x + 1.0f, bar_background_bottom.y + 1.0f), border_color);
        draw->AddRectFilled(bar_background_top, bar_background_bottom, background_color);

        const float ratio = (max_health > 0.0f) ? (clamped_health / max_health) : 0.0f;
        ImVec4 top_color_vec;
        ImVec4 bottom_color_vec;
        const int color_mode = std::clamp(features->healthbar_color_mode, 0, 1);
        if (override_color != 0)
        {
            const ImVec4 override_vec = ImGui::ColorConvertU32ToFloat4(override_color);
            top_color_vec = bottom_color_vec = override_vec;
        }
        else if (color_mode == 0)
        {
            top_color_vec = features->healthbar_top_color;
            bottom_color_vec = features->healthbar_bottom_color;
        }
        else if (ratio >= 0.75f)
        {
            top_color_vec = ImVec4(0.25f, 0.86f, 0.25f, 1.0f);
            bottom_color_vec = ImVec4(0.075f, 0.65f, 0.075f, 1.0f);
        }
        else if (ratio >= 0.25f)
        {
            top_color_vec = ImVec4(1.0f, 0.60f, 0.15f, 1.0f);
            bottom_color_vec = ImVec4(0.80f, 0.35f, 0.05f, 1.0f);
        }
        else
        {
            top_color_vec = ImVec4(0.86f, 0.25f, 0.25f, 1.0f);
            bottom_color_vec = ImVec4(0.60f, 0.10f, 0.10f, 1.0f);
        }

        const ImVec4 high_color = adjust_alpha(top_color_vec, alpha_factor);
        const ImVec4 low_color = adjust_alpha(bottom_color_vec, alpha_factor);

        const ImU32 top_color = ImGui::GetColorU32(high_color);
        const ImU32 bottom_color = ImGui::GetColorU32(low_color);

        draw->AddRectFilledMultiColor(bar_foreground_top, bar_foreground_bottom, top_color, top_color, bottom_color, bottom_color);
    }

    static void draw_armor_bar_on(ImDrawList* draw, float max_armor, float current_armor, const ImVec2& position, const ImVec2& size, float alpha_factor, ImU32 override_color = 0)
    {
        if (!draw || max_armor <= 0.0f || current_armor <= 0.0f)
        {
            return;
        }

        const ImVec2 rounded_position(std::round(position.x), std::round(position.y));
        const ImVec2 rounded_size(std::round(size.x), std::round(size.y));
        if (rounded_size.x <= 0.0f || rounded_size.y <= 0.0f)
        {
            return;
        }

        const float clamped_armor = std::clamp(current_armor, 0.0f, max_armor);
        const float fill_ratio = clamped_armor / max_armor;
        const float bar_height = rounded_size.y;
        const float bar_width = rounded_size.x;
        const float filled_width = std::clamp(std::round(bar_width * fill_ratio), 0.0f, bar_width);

        ImVec2 bar_min = rounded_position;
        ImVec2 bar_max = ImVec2(rounded_position.x + bar_width, rounded_position.y + bar_height);
        ImVec2 filled_max = ImVec2(rounded_position.x + filled_width, rounded_position.y + bar_height);

        const ImU32 border_color = adjust_alpha(IM_COL32(0, 0, 0, 128), alpha_factor);
        const ImU32 background_color = adjust_alpha(IM_COL32(60, 60, 60, 200), alpha_factor);

        draw->AddRectFilled(ImVec2(bar_min.x - 1.0f, bar_min.y - 1.0f), ImVec2(bar_max.x + 1.0f, bar_max.y + 1.0f), border_color, 0.0f);
        draw->AddRectFilled(bar_min, bar_max, background_color, 0.0f);

        ImVec4 top_color_vec = ImVec4(0.35f, 0.75f, 1.0f, 1.0f);
        ImVec4 bottom_color_vec = ImVec4(0.12f, 0.32f, 0.65f, 1.0f);

        if (override_color != 0)
        {
            const ImVec4 override_vec = ImGui::ColorConvertU32ToFloat4(override_color);
            top_color_vec = bottom_color_vec = override_vec;
        }

        const ImVec4 high_color = adjust_alpha(top_color_vec, alpha_factor);
        const ImVec4 low_color = adjust_alpha(bottom_color_vec, alpha_factor);

        const ImU32 left_color = ImGui::GetColorU32(high_color);
        const ImU32 right_color = ImGui::GetColorU32(low_color);

        draw->AddRectFilledMultiColor(bar_min, filled_max, right_color, left_color, left_color, right_color);
    }

    void render_esp_preview(ImDrawList* draw, const esp_preview_render_info& info)
    {
        if (!features->enable_esp)
        {
            return;
        }

        if (!draw)
        {
            return;
        }

        draw->PushClipRect(info.clip_min, info.clip_max, true);

        ImRect bounds = info.bounds;
        if (bounds.Min.x >= bounds.Max.x || bounds.Min.y >= bounds.Max.y)
        {
            draw->PopClipRect();
            return;
        }

        const float alpha_factor = 1.0f;
        const bool visibility_enabled = false;
        const visibility::visibility_result visibility_state{};
        const player_relation relation = player_relation::neutral;
        const bool is_dormant = false;
        const bool locked_target = false;
        const bool is_host = info.is_host;
        ImVec4 host_color = features->host_color;
        ImVec4 accent_color = is_host ? host_color : c_colors::top_accent_color;
        ImVec4 highlight_accent = accent_color;
        highlight_accent.x = std::clamp(highlight_accent.x + 0.25f, 0.0f, 1.0f);
        highlight_accent.y = std::clamp(highlight_accent.y + 0.25f, 0.0f, 1.0f);
        highlight_accent.z = std::clamp(highlight_accent.z + 0.25f, 0.0f, 1.0f);
        highlight_accent.w = std::clamp(highlight_accent.w * 0.85f + 0.15f, 0.0f, 1.0f);

        auto build_hull = [&](std::vector<ImVec2> points) -> std::vector<ImVec2>
            {
                std::vector<ImVec2> pts = points;
                pts.erase(std::remove_if(pts.begin(), pts.end(), [&](const ImVec2& p)
                    {
                        return p.x < info.clip_min.x || p.x > info.clip_max.x || p.y < info.clip_min.y || p.y > info.clip_max.y;
                    }), pts.end());
                std::sort(pts.begin(), pts.end(), [](const ImVec2& a, const ImVec2& b)
                    {
                        return a.x < b.x || (a.x == b.x && a.y < b.y);
                    });
                pts.erase(std::unique(pts.begin(), pts.end(), [](const ImVec2& a, const ImVec2& b)
                    {
                        return a.x == b.x && a.y == b.y;
                    }), pts.end());
                if (pts.size() < 3)
                    return {};

                auto cross = [](const ImVec2& o, const ImVec2& a, const ImVec2& b)
                    {
                        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
                    };

                std::vector<ImVec2> hull;
                hull.reserve(pts.size() * 2);
                for (const auto& p : pts)
                {
                    while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull[hull.size() - 1], p) <= 0.0f)
                        hull.pop_back();
                    hull.push_back(p);
                }
                size_t lower_size = hull.size();
                for (int i = static_cast<int>(pts.size()) - 2; i >= 0; --i)
                {
                    const auto& p = pts[i];
                    while (hull.size() > lower_size && cross(hull[hull.size() - 2], hull[hull.size() - 1], p) <= 0.0f)
                        hull.pop_back();
                    hull.push_back(p);
                }
                if (!hull.empty())
                    hull.pop_back();
                return hull;
            };

        if (features->enable_highlight)
        {
            std::vector<Clipper2Lib::PathD> highlight_paths;
            const bool mesh_mode = true;

            auto add_path = [&](const std::vector<ImVec2>& pts)
                {
                    if (pts.size() < 3)
                        return;
                    Clipper2Lib::PathD path;
                    path.reserve(pts.size());
                    for (const auto& p : pts)
                        path.emplace_back(p.x, p.y);
                    highlight_paths.push_back(std::move(path));
                };

            if (mesh_mode)
            {
                if (!info.subset_hulls.empty())
                {
                    for (const auto& subset : info.subset_hulls)
                    {
                        add_path(build_hull(subset));
                    }
                }
                if (highlight_paths.empty())
                {
                    add_path(build_hull(info.projected_points));
                }
            }

            if (highlight_paths.empty())
            {
                Clipper2Lib::PathD rect;
                rect.emplace_back(bounds.Min.x, bounds.Min.y);
                rect.emplace_back(bounds.Max.x, bounds.Min.y);
                rect.emplace_back(bounds.Max.x, bounds.Max.y);
                rect.emplace_back(bounds.Min.x, bounds.Max.y);
                highlight_paths.push_back(std::move(rect));
            }

            ImVec4 base_fill = locked_target ? ImVec4(accent_color.x, accent_color.y, accent_color.z, 0.25f) : features->highlight_fill_color;
            base_fill = apply_relation_color(relation, base_fill);
            base_fill = apply_host_tint(base_fill, is_host, host_color);
            base_fill = apply_visibility_tint(base_fill, visibility_state, visibility_enabled);
            ImVec4 fill = adjust_alpha(base_fill, alpha_factor);
            fill = apply_dormant_tint(fill, is_dormant);

            ImVec4 base_outline = locked_target ? highlight_accent : features->highlight_outline_color;
            const bool mesh_mode_selected = std::clamp(features->highlight_mode, 0, 2) == 1;
            const bool non_flat_material = std::clamp(features->highlight_mesh_material, 0, 4) != k_mesh_material_flat;
            if (locked_target && mesh_mode_selected && non_flat_material)
            {
                base_outline.w = 0.0f;
            }
            base_outline = apply_relation_color(relation, base_outline);
            base_outline = apply_host_tint(base_outline, is_host, host_color);
            base_outline = apply_visibility_tint(base_outline, visibility_state, visibility_enabled);
            ImVec4 outline = adjust_alpha(base_outline, alpha_factor);
            outline = apply_dormant_tint(outline, is_dormant);

            ImU32 fill_col = ImGui::GetColorU32(fill);
            ImU32 outline_col = ImGui::GetColorU32(outline);

            constexpr double scale = 100.0;
            Clipper2Lib::Paths64 paths_i = scale_to_int(highlight_paths, scale);
            Clipper2Lib::Paths64 unified_i = Clipper2Lib::Union(paths_i, Clipper2Lib::FillRule::NonZero);
            unified_i = Clipper2Lib::SimplifyPaths(unified_i, 2.0, true);
            Clipper2Lib::PathsD unified = scale_to_double(unified_i, scale);

            draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            for (const auto& poly : unified)
            {
                if (poly.size() < 3)
                    continue;
                std::vector<std::vector<std::array<double, 2>>> polygon(1);
                polygon[0].reserve(poly.size());
                for (const auto& p : poly)
                    polygon[0].push_back({ p.x, p.y });
                const auto indices = mapbox::earcut<uint32_t>(polygon);
                for (size_t i = 0; i + 2 < indices.size(); i += 3)
                {
                    const auto& a = polygon[0][indices[i]];
                    const auto& b = polygon[0][indices[i + 1]];
                    const auto& c = polygon[0][indices[i + 2]];
                    draw->AddTriangleFilled(
                        ImVec2(static_cast<float>(a[0]), static_cast<float>(a[1])),
                        ImVec2(static_cast<float>(b[0]), static_cast<float>(b[1])),
                        ImVec2(static_cast<float>(c[0]), static_cast<float>(c[1])),
                        fill_col);
                }
            }
            draw->Flags |= ImDrawListFlags_AntiAliasedFill;

            ImDrawListFlags saved_flags = draw->Flags;
            draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            for (const auto& poly : unified)
            {
                if (poly.size() < 2)
                    continue;
                std::vector<ImVec2> pts;
                pts.reserve(poly.size());
                float minx = (std::numeric_limits<float>::max)();
                float miny = (std::numeric_limits<float>::max)();
                float maxx = -(std::numeric_limits<float>::max)();
                float maxy = -(std::numeric_limits<float>::max)();
                for (const auto& p : poly)
                {
                    ImVec2 pt(static_cast<float>(p.x), static_cast<float>(p.y));
                    pts.push_back(pt);
                    minx = (std::min)(minx, pt.x);
                    miny = (std::min)(miny, pt.y);
                    maxx = (std::max)(maxx, pt.x);
                    maxy = (std::max)(maxy, pt.y);
                }
                const float span = (std::max)(maxx - minx, maxy - miny);
                const float thickness = std::clamp(span / 90.0f, 1.0f, 1.5f);
                draw->AddPolyline(pts.data(), static_cast<int>(pts.size()), outline_col, true, thickness);
            }
            draw->Flags = saved_flags;
        }

        if (false)
        {
            const float width = bounds.Max.x - bounds.Min.x;
            const float height = bounds.Max.y - bounds.Min.y;
            ImVec2 center(bounds.Min.x + width * 0.5f, bounds.Min.y + height * 0.5f);
            float head_y = bounds.Min.y + height * 0.08f;
            float neck_y = bounds.Min.y + height * 0.18f;
            float chest_y = bounds.Min.y + height * 0.32f;
            float pelvis_y = bounds.Min.y + height * 0.55f;
            float leg_y = bounds.Max.y;
            float arm_span = width * 0.45f;
            float leg_span = width * 0.22f;

            ImVec2 head(center.x, head_y);
            ImVec2 neck(center.x, neck_y);
            ImVec2 chest(center.x, chest_y);
            ImVec2 pelvis(center.x, pelvis_y);
            ImVec2 hand_left(center.x - arm_span, chest_y);
            ImVec2 hand_right(center.x + arm_span, chest_y);
            ImVec2 foot_left(center.x - leg_span, leg_y);
            ImVec2 foot_right(center.x + leg_span, leg_y);

            ImVec4 base_color = locked_target ? accent_color : features->skeleton_color;
            ImVec4 base_outline = locked_target ? highlight_accent : features->skeleton_outline_color;
            base_color = apply_host_tint(base_color, is_host, host_color);
            base_outline = apply_host_tint(base_outline, is_host, host_color);
            base_color = apply_visibility_tint(base_color, visibility_state, visibility_enabled);
            base_outline = apply_visibility_tint(base_outline, visibility_state, visibility_enabled);
            ImVec4 color_vec = adjust_alpha(base_color, alpha_factor);
            ImVec4 outline_vec = adjust_alpha(base_outline, alpha_factor);
            color_vec = apply_dormant_tint(color_vec, is_dormant);
            outline_vec = apply_dormant_tint(outline_vec, is_dormant);
            const ImU32 color = ImGui::GetColorU32(color_vec);
            const ImU32 outline_color = ImGui::GetColorU32(outline_vec);
            constexpr float thickness = 1.0f;
            constexpr float outline_extra = 2.0f;

            auto link = [&](const ImVec2& a, const ImVec2& b)
                {
                    std::optional<ImVec2> oa = a;
                    std::optional<ImVec2> ob = b;
                    if (features->enable_skeleton_outline)
                    {
                        add_skeleton_segment(draw, oa, ob, outline_color, thickness + outline_extra);
                    }
                    add_skeleton_segment(draw, oa, ob, color, thickness);
                };

            link(head, center);
            link(center, pelvis);
            link(center, hand_left);
            link(center, hand_right);
            link(pelvis, foot_left);
            link(pelvis, foot_right);
        }

        bool armor_drawn = false;
        ImVec2 armor_bar_position{};
        ImVec2 armor_bar_size{};

        if (features->enable_bounding_box)
        {
            ImVec2 size(bounds.Max.x - bounds.Min.x, bounds.Max.y - bounds.Min.y);
            if (is_valid_box(size))
            {
                const int box_style = std::clamp(features->bounding_box_style, 0, 1);
                ImVec4 box_base = locked_target ? accent_color : features->bounding_box_color;
                box_base = apply_relation_color(relation, box_base);
                box_base = apply_host_tint(box_base, is_host, host_color);
                box_base = apply_visibility_tint(box_base, visibility_state, visibility_enabled);
                ImVec4 box_col = adjust_alpha(box_base, alpha_factor);
                box_col = apply_dormant_tint(box_col, is_dormant);
                if (box_style == 1)
                {
                    ImVec2 box_pos(bounds.Min.x, bounds.Min.y - (features->enable_healthbar ? 1.0f : 0.0f));
                    ImVec2 box_size(size.x, size.y + (features->enable_healthbar ? 2.0f : 0.0f));
                    draw_corner_box_on(draw, box_pos, box_size, ImGui::GetColorU32(box_col));
                }
                else
                {
                    draw_outlined_rectangle_on(draw, bounds.Min, size, ImGui::GetColorU32(box_col));
                }
            }
        }

        if (features->enable_healthbar)
        {
            float max_health = info.max_health > 0.0f ? info.max_health : 100.0f;
            float clamped_health = std::clamp(info.health, 0.0f, max_health);
            constexpr float bar_width = 2.0f;
            constexpr float bar_padding = 4.0f;
            const ImVec2 bar_position(bounds.Min.x - bar_padding - bar_width, bounds.Min.y);
            const ImVec2 bar_size(bar_width, bounds.Max.y - bounds.Min.y);
            draw_health_bar_on(draw, max_health, clamped_health, bar_position, bar_size, alpha_factor);
        }

        if (features->enable_armor_bar && info.armor > 0)
        {
            constexpr float bar_height = 2.0f;
            constexpr float bar_padding_y = 4.0f;
            const float box_width = bounds.Max.x - bounds.Min.x;
            const float bar_width = box_width + 2.0f;
            const float bar_x = bounds.Min.x - 1.0f;
            const float bar_y = bounds.Max.y + bar_padding_y;
            armor_bar_position = ImVec2(bar_x, bar_y);
            armor_bar_size = ImVec2(bar_width, bar_height);
            constexpr float armor_max = 100.0f;
            float armor_value = static_cast<float>(std::clamp(info.armor, 0, 200));
            draw_armor_bar_on(draw, armor_max, armor_value, armor_bar_position, armor_bar_size, alpha_factor);
            armor_drawn = true;
        }

        if (features->enable_body_status)
        {
            std::vector<std::string> status_labels;
            status_labels.reserve(6);

            if (features->show_status_movement)
            {
                status_labels.emplace_back("Moving");
            }
            if (features->show_status_reload)
            {
                status_labels.emplace_back("Reloading");
            }
            if (features->show_status_grabbed)
            {
                status_labels.emplace_back("Grabbed");
            }
            if (features->show_status_gun_firing)
            {
                status_labels.emplace_back("Shooting");
            }
            if (features->show_status_knocked)
            {
                status_labels.emplace_back("KO");
            }

            if (!status_labels.empty())
            {
                ImFont* font = c_fonts::smallest_pixel ? c_fonts::smallest_pixel : ImGui::GetFont();
                const float font_size = 10.0f;
                const float padding_x = 4.0f;
                float text_x = bounds.Max.x + padding_x;
                float text_y = bounds.Min.y;
                for (const auto& label : status_labels)
                {
                    const ImVec2 text_size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label.c_str()) : ImVec2(0, 0);
                    ImVec4 status_base = features->status_color;
                    status_base = apply_relation_color(relation, status_base);
                    status_base = apply_host_tint(status_base, is_host, host_color);
                    status_base = apply_visibility_tint(status_base, visibility_state, visibility_enabled);
                    ImVec4 status_color = adjust_alpha(status_base, alpha_factor);
                    status_color = apply_dormant_tint(status_color, is_dormant);
                    const bool outline = (font == c_fonts::smallest_pixel);
                    ::add_text(draw, font, font_size, ImVec2(text_x, text_y - 3.0f), ImGui::GetColorU32(status_color), label, outline);
                    text_y += text_size.y;
                }
            }
        }

        if (features->enable_distance)
        {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(0) << info.distance;
            const std::string text = ss.str();
            ImFont* font = c_fonts::smallest_pixel ? c_fonts::smallest_pixel : ImGui::GetFont();
            const float font_size = 10.0f;
            const ImVec2 text_size = font ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str()) : ImVec2(0, 0);
            float base_y = armor_drawn ? (armor_bar_position.y + armor_bar_size.y - 1.0f) : bounds.Max.y;
            float text_x = bounds.Min.x + ((bounds.Max.x - bounds.Min.x) - text_size.x) * 0.5f;
            text_x = std::clamp(text_x, info.clip_min.x, info.clip_max.x - text_size.x);
            float text_y = std::clamp(base_y, info.clip_min.y, info.clip_max.y - text_size.y);

            ImVec4 distance_base = features->distance_color;
            distance_base = apply_relation_color(relation, distance_base);
            distance_base = apply_host_tint(distance_base, is_host, host_color);
            distance_base = apply_visibility_tint(distance_base, visibility_state, visibility_enabled);
            ImVec4 distance_color = adjust_alpha(distance_base, alpha_factor);
            distance_color = apply_dormant_tint(distance_color, is_dormant);
            const ImU32 text_color = ImGui::GetColorU32(distance_color);
            const bool outline = (font == c_fonts::smallest_pixel);
            ::add_text(draw, font, font_size, ImVec2(text_x, text_y), text_color, text, outline);
        }

        if (features->enable_name_esp && !info.name.empty())
        {
            ImFont* name_font = nullptr;
                    switch (features->name_esp_font)
                    {
                    case 3:
                        name_font = c_fonts::smallest_pixel ? c_fonts::smallest_pixel : nullptr;
                        break;
                    case 4:
                        name_font = c_fonts::proggy_clean ? c_fonts::proggy_clean : nullptr;
                        break;
                    case 2:
                        name_font = c_fonts::verdana_bold ? c_fonts::verdana_bold : nullptr;
                        break;
                    case 1:
                name_font = c_fonts::verdana_regular ? c_fonts::verdana_regular : nullptr;
                break;
            default:
                name_font = c_fonts::tahoma ? c_fonts::tahoma : nullptr;
                break;
            }
            if (!name_font)
            {
                name_font = ImGui::GetFont();
            }
            if (name_font)
            {
                const float name_font_size = name_font->LegacySize;
                const ImVec2 text_size = name_font->CalcTextSizeA(name_font_size, (std::numeric_limits<float>::max)(), 0.0f, info.name.c_str());

                float text_x = bounds.Min.x + ((bounds.Max.x - bounds.Min.x) - text_size.x) * 0.5f;
                float text_y = bounds.Min.y - text_size.y - 2.0f;
                text_x = std::clamp(text_x, info.clip_min.x, info.clip_max.x - text_size.x);
                text_y = (std::max)(info.clip_min.y, text_y);

                ImVec2 text_pos(text_x, text_y);
                ImVec4 name_base = locked_target ? accent_color : features->name_esp_color;
                name_base = apply_relation_color(relation, name_base);
                name_base = apply_host_tint(name_base, is_host, host_color);
                name_base = apply_visibility_tint(name_base, visibility_state, visibility_enabled);
                ImVec4 name_color_vec = adjust_alpha(name_base, alpha_factor);
                name_color_vec = apply_dormant_tint(name_color_vec, is_dormant);
                ImVec4 shadow_vec4 = adjust_alpha(ImVec4(c_colors::black.x, c_colors::black.y, c_colors::black.z, 0.5f), alpha_factor);
                const ImU32 text_color = ImGui::GetColorU32(name_color_vec);
                const ImU32 shadow_color = ImGui::GetColorU32(shadow_vec4);
                const bool draw_outline = should_outline(name_font);
                const bool draw_shadow = !draw_outline;
                if (draw_shadow)
                {
                    ::add_text(draw, name_font, name_font_size, ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), shadow_color, info.name, false);
                }
                ::add_text(draw, name_font, name_font_size, text_pos, text_color, info.name, draw_outline);
            }
        }

        draw->PopClipRect();
    }

    void render_highlight_mesh_material_pass(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target_view)
    {
        if (!device || !context || !target_view)
        {
            return;
        }

        if (!features->enable_highlight || std::clamp(features->highlight_mode, 0, 2) != 1)
        {
            return;
        }
        if (std::clamp(features->highlight_mesh_material, 0, 4) == k_mesh_material_flat)
        {
            return;
        }
        if (!has_metallic_mesh_view_proj || metallic_mesh_draw_queue.empty())
        {
            return;
        }
        if (!initialize_metallic_pipeline(device))
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = (std::max)(1.0f, io.DisplaySize.x);
        viewport.Height = (std::max)(1.0f, io.DisplaySize.y);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);

        const UINT depth_width = (std::max)(1u, static_cast<UINT>(viewport.Width));
        const UINT depth_height = (std::max)(1u, static_cast<UINT>(viewport.Height));
        if (!ensure_metallic_depth_buffer(device, depth_width, depth_height))
        {
            return;
        }

        ID3D11RenderTargetView* render_target = target_view;
        context->ClearDepthStencilView(metallic_pipeline.depth_stencil_view.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        context->OMSetRenderTargets(1, &render_target, metallic_pipeline.depth_stencil_view.Get());

        context->IASetInputLayout(metallic_pipeline.input_layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(metallic_pipeline.vertex_shader.Get(), nullptr, 0);
        context->PSSetShader(metallic_pipeline.pixel_shader.Get(), nullptr, 0);
        context->RSSetState(metallic_pipeline.rasterizer_state.Get());
        context->OMSetDepthStencilState(metallic_pipeline.depth_state.Get(), 0);
        constexpr float blend_factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetBlendState(metallic_pipeline.blend_state.Get(), blend_factor, 0xFFFFFFFF);

        constexpr UINT stride = sizeof(SimpleVector3);
        constexpr UINT offset = 0;
        ID3D11Buffer* constant_buffer = metallic_pipeline.constant_buffer.Get();
        context->VSSetConstantBuffers(0, 1, &constant_buffer);
        context->PSSetConstantBuffers(0, 1, &constant_buffer);

        struct batched_draw_range
        {
            UINT index_count = 0;
            UINT start_index = 0;
            INT base_vertex = 0;
            ImVec4 color{};
            int material_mode = 0;
        };

        std::size_t total_vertices = 0;
        std::size_t total_indices = 0;
        for (const auto& draw_command : metallic_mesh_draw_queue)
        {
            if (draw_command.world_vertices.empty() || draw_command.indices.empty())
            {
                continue;
            }
            total_vertices += draw_command.world_vertices.size();
            total_indices += draw_command.indices.size();
        }

        if (total_vertices == 0 || total_indices == 0)
        {
            context->OMSetRenderTargets(1, &render_target, nullptr);
            return;
        }

        if (total_vertices > static_cast<std::size_t>((std::numeric_limits<UINT>::max)()) ||
            total_indices > static_cast<std::size_t>((std::numeric_limits<UINT>::max)()))
        {
            context->OMSetRenderTargets(1, &render_target, nullptr);
            return;
        }

        if (!ensure_metallic_vertex_buffer(device, total_vertices))
        {
            context->OMSetRenderTargets(1, &render_target, nullptr);
            return;
        }
        if (!ensure_metallic_index_buffer(device, total_indices))
        {
            context->OMSetRenderTargets(1, &render_target, nullptr);
            return;
        }

        D3D11_MAPPED_SUBRESOURCE mapped_vertices{};
        if (FAILED(context->Map(metallic_pipeline.vertex_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_vertices)))
        {
            context->OMSetRenderTargets(1, &render_target, nullptr);
            return;
        }

        D3D11_MAPPED_SUBRESOURCE mapped_indices{};
        if (FAILED(context->Map(metallic_pipeline.index_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_indices)))
        {
            context->Unmap(metallic_pipeline.vertex_buffer.Get(), 0);
            context->OMSetRenderTargets(1, &render_target, nullptr);
            return;
        }

        auto* vertex_dst = static_cast<SimpleVector3*>(mapped_vertices.pData);
        auto* index_dst = static_cast<std::uint32_t*>(mapped_indices.pData);
        std::vector<batched_draw_range> ranges;
        ranges.reserve(metallic_mesh_draw_queue.size());

        std::size_t vertex_cursor = 0;
        std::size_t index_cursor = 0;
        for (const auto& draw_command : metallic_mesh_draw_queue)
        {
            if (draw_command.world_vertices.empty() || draw_command.indices.empty())
            {
                continue;
            }

            const std::size_t vertex_count = draw_command.world_vertices.size();
            const std::size_t index_count = draw_command.indices.size();
            std::memcpy(vertex_dst + vertex_cursor, draw_command.world_vertices.data(), vertex_count * sizeof(SimpleVector3));

            const std::uint32_t index_bias = static_cast<std::uint32_t>(vertex_cursor);
            for (std::size_t i = 0; i < index_count; ++i)
            {
                index_dst[index_cursor + i] = draw_command.indices[i] + index_bias;
            }

            ranges.push_back(batched_draw_range{
                static_cast<UINT>(index_count),
                static_cast<UINT>(index_cursor),
                0,
                draw_command.color,
                draw_command.material_mode
                });

            vertex_cursor += vertex_count;
            index_cursor += index_count;
        }

        context->Unmap(metallic_pipeline.vertex_buffer.Get(), 0);
        context->Unmap(metallic_pipeline.index_buffer.Get(), 0);

        if (ranges.empty())
        {
            context->OMSetRenderTargets(1, &render_target, nullptr);
            return;
        }

        ID3D11Buffer* vertex_buffer = metallic_pipeline.vertex_buffer.Get();
        context->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
        context->IASetIndexBuffer(metallic_pipeline.index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);

        ImVec4 last_color{};
        int last_material = -1;
        bool has_last_constants = false;
        for (const auto& range : ranges)
        {
            const bool constants_changed =
                !has_last_constants ||
                range.material_mode != last_material ||
                range.color.x != last_color.x ||
                range.color.y != last_color.y ||
                range.color.z != last_color.z ||
                range.color.w != last_color.w;

            if (constants_changed)
            {
                if (!update_metallic_constants(context, range.color, range.material_mode))
                {
                    continue;
                }
                last_color = range.color;
                last_material = range.material_mode;
                has_last_constants = true;
            }

            context->DrawIndexed(range.index_count, range.start_index, range.base_vertex);
        }

        context->OMSetRenderTargets(1, &render_target, nullptr);
    }

    void render_radar()
    {
        if (!features->enable_radar)
        {
            return;
        }

        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw)
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
        {
            return;
        }

        auto clampf = [](float v, float lo, float hi)
        {
            return v < lo ? lo : (v > hi ? hi : v);
        };

        const float size = clampf(features->radar_size, 80.0f, 400.0f);
        const float radius = size * 0.5f;
        const float alpha = clampf(features->radar_alpha, 0.05f, 1.0f);
        float zoom = clampf(features->radar_zoom, 0.25f, 4.0f);
        const int mode = std::clamp(features->radar_mode, 0, 1);
        const bool is_3d = (mode == 1);
        const int radar_position = std::clamp(features->radar_position, 0, 3);

        const ImVec2 margin(24.0f, 80.0f);
        ImVec2 pos(io.DisplaySize.x - size - margin.x, io.DisplaySize.y - size - margin.y);
        switch (radar_position)
        {
        case 0:
            pos = ImVec2(margin.x, margin.y);
            break;
        case 1:
            pos = ImVec2(io.DisplaySize.x - size - margin.x, margin.y);
            break;
        case 2:
            pos = ImVec2(margin.x, io.DisplaySize.y - size - margin.y);
            break;
        case 3:
        default:
            pos = ImVec2(io.DisplaySize.x - size - margin.x, io.DisplaySize.y - size - margin.y);
            break;
        }
        pos.x = std::clamp(pos.x, 10.0f, io.DisplaySize.x - size - 10.0f);
        pos.y = std::clamp(pos.y, 10.0f, io.DisplaySize.y - size - 10.0f);

        ImRect rect(pos, ImVec2(pos.x + size, pos.y + size));
        ImVec2 center(rect.Min.x + radius, rect.Min.y + radius);

        const ImU32 bg_col = ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.05f, 0.75f * alpha));

        draw->AddRectFilled(rect.Min, rect.Max, bg_col, 0.0f);

        const auto local = cache::localplayer->snapshot();
        if (local.address == 0)
        {
            return;
        }

        const auto local_root_pos = get_part_position(local.parts.humanoid_root_part);
        if (!local_root_pos)
        {
            return;
        }

        if (features->radar_auto_zoom && local.camera.is_valid())
        {
            const rbx::Vector3 cam_pos = local.camera.get_camera_position();
            if (is_finite_vec3(cam_pos))
            {
                const rbx::Vector3 delta = cam_pos - *local_root_pos;
                const float cam_dist = delta.Length();
                if (std::isfinite(cam_dist) && cam_dist > 0.01f)
                {
                    constexpr float reference_dist = 18.0f;
                    const float auto_zoom = reference_dist / cam_dist;
                    zoom = clampf(auto_zoom * zoom, 0.25f, 4.0f);
                }
            }
        }

        float yaw = 0.0f;
        if (const auto forward = resolve_camera_forward(local))
        {
            yaw = std::atan2(forward->x, forward->z);
        }
        const float sin_yaw = std::sin(yaw);
        const float cos_yaw = std::cos(yaw);

        constexpr float base_range = 250.0f;
        const float range = (std::max)(base_range / zoom, 50.0f);
        const float scale = radius / range;

        const auto rotate_to_radar = [&](float x, float z)
        {
            const float rx = x * cos_yaw - z * sin_yaw;
            const float rz = x * sin_yaw + z * cos_yaw;
            return ImVec2(-rx, -rz);
        };

        const float pitch = 0.8f;
        const float cos_pitch = std::cos(pitch);
        const float height_scale = scale * 0.5f;
        const float ellipse_squash = is_3d ? 0.75f : 1.0f;

        auto project_offset = [&](const ImVec2& offset, float height, float* out_scale)
        {
            if (!is_3d)
            {
                if (out_scale)
                {
                    *out_scale = 1.0f;
                }
                return ImVec2(center.x + offset.x * scale, center.y + offset.y * scale);
            }

            if (out_scale)
            {
                *out_scale = 1.0f;
            }
            const float x = offset.x * scale;
            const float y = offset.y * scale * cos_pitch;
            const float h = height * height_scale;
            return ImVec2(center.x + x, center.y + y - h);
        };

        draw->PushClipRect(rect.Min, rect.Max, true);

        const float icon_size = clampf(radius * 0.18f, 12.0f, 24.0f);
        const float local_icon_size = clampf(radius * 0.22f, 14.0f, 28.0f);
        const ImU32 neutral_outline = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.35f * alpha));

        auto draw_avatar_icon = [&](std::uint64_t user_id, const ImVec2& pos, float size, float tint_alpha, ImU32 outline_col, float scale_factor) -> bool
        {
            if (user_id == 0)
            {
                return false;
            }

            vanille::overlay::request_avatar_texture(user_id);

            ImTextureID tex = vanille::overlay::get_avatar_texture(user_id, nullptr, nullptr);
            if (!tex)
            {
                return false;
            }

            const float width = size * scale_factor;
            const float height = size * scale_factor * ellipse_squash;
            const float half_w = width * 0.5f;
            const float half_h = height * 0.5f;
            const ImVec2 pmin(pos.x - half_w, pos.y - half_h);
            const ImVec2 pmax(pos.x + half_w, pos.y + half_h);
            const ImU32 tint = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, tint_alpha));
            const float rounding = (std::min)(width, height) * 0.5f;
            draw->AddImageRounded(ImTextureRef(tex), pmin, pmax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint, rounding);
            (void)outline_col;
            return true;
        };

        auto draw_placeholder_icon = [&](const ImVec2& pos, float size, ImU32 fill_col, ImU32 outline_col, float scale_factor)
        {
            const float width = size * scale_factor;
            const float height = size * scale_factor * ellipse_squash;
            const float half_w = width * 0.5f;
            const float half_h = height * 0.5f;
            const ImVec2 pmin(pos.x - half_w, pos.y - half_h);
            const ImVec2 pmax(pos.x + half_w, pos.y + half_h);
            const float rounding = (std::min)(width, height) * 0.5f;
            draw->AddRectFilled(pmin, pmax, fill_col, rounding);
            (void)outline_col;
        };

        auto tint_map = [](ImU32 color, float rgb_factor, float alpha_factor)
        {
            ImVec4 col = ImGui::ColorConvertU32ToFloat4(color);
            col.x = std::clamp(col.x * rgb_factor, 0.0f, 1.0f);
            col.y = std::clamp(col.y * rgb_factor, 0.0f, 1.0f);
            col.z = std::clamp(col.z * rgb_factor, 0.0f, 1.0f);
            col.w = std::clamp(col.w * alpha_factor, 0.0f, 1.0f);
            return ImGui::ColorConvertFloat4ToU32(col);
        };

        const bool anchor_ready = g_radar_map_has_anchor;
        const float anchor_move_sq = k_radar_map_anchor_move * k_radar_map_anchor_move;
        bool should_refresh_map = !anchor_ready;
        if (anchor_ready)
        {
            const rbx::Vector3 delta = *local_root_pos - g_radar_map_anchor;
            should_refresh_map = (delta.LengthSquared() > anchor_move_sq);
        }
        if (should_refresh_map)
        {
            const double now = ImGui::GetTime();
            if (now - g_radar_map_last_refresh >= k_radar_map_min_refresh_interval)
            {
                refresh_radar_map(*local_root_pos, range);
            }
        }
        const float radar_plane_y = g_radar_map_has_anchor ? g_radar_map_anchor.y : local_root_pos->y;

        struct radar_part_draw
        {
            const radar_map_part* part = nullptr;
            ImVec2 center_offset{};
            float depth = 0.0f;
            bool floor_like = false;
            bool background_like = false;
        };

        std::vector<radar_part_draw> map_draw;
        map_draw.reserve(g_radar_map_parts.size());
        for (const auto& part : g_radar_map_parts)
        {
            const float dx = part.position.x - local_root_pos->x;
            const float dz = part.position.z - local_root_pos->z;
            const ImVec2 center_offset = rotate_to_radar(dx, dz);
            const float dist_sq = center_offset.x * center_offset.x + center_offset.y * center_offset.y;
            const float max_reach = range + part.footprint_radius;
            if (dist_sq > (max_reach * max_reach))
            {
                continue;
            }
            const float depth = -center_offset.y;
            const bool floor_like = part.half_height <= 3.0f && part.footprint_radius >= (range * 0.35f);
            const bool background_like = part.footprint_radius >= (range * 0.45f);
            map_draw.push_back({ &part, center_offset, depth, floor_like, background_like });
        }

        if (is_3d)
        {
            std::sort(map_draw.begin(), map_draw.end(), [](const radar_part_draw& a, const radar_part_draw& b)
                {
                    if (a.background_like != b.background_like)
                    {
                        return a.background_like && !b.background_like;
                    }
                    if (a.floor_like != b.floor_like)
                    {
                        return a.floor_like && !b.floor_like;
                    }
                    if (std::fabs(a.part->footprint_radius - b.part->footprint_radius) > 0.01f)
                    {
                        return a.part->footprint_radius > b.part->footprint_radius;
                    }
                    return a.depth > b.depth;
                });
        }

        for (const auto& entry : map_draw)
        {
            const auto& part = *entry.part;
            const ImVec2 center_offset = entry.center_offset;
            const ImVec2 o0 = rotate_to_radar(part.footprint_offsets[0].x, part.footprint_offsets[0].y);
            const ImVec2 o1 = rotate_to_radar(part.footprint_offsets[1].x, part.footprint_offsets[1].y);
            const ImVec2 o2 = rotate_to_radar(part.footprint_offsets[2].x, part.footprint_offsets[2].y);
            const ImVec2 o3 = rotate_to_radar(part.footprint_offsets[3].x, part.footprint_offsets[3].y);

            const ImVec2 v0(center_offset.x + o0.x, center_offset.y + o0.y);
            const ImVec2 v1(center_offset.x + o1.x, center_offset.y + o1.y);
            const ImVec2 v2(center_offset.x + o2.x, center_offset.y + o2.y);
            const ImVec2 v3(center_offset.x + o3.x, center_offset.y + o3.y);

            float height = 0.0f;
            if (is_3d)
            {
                height = (part.position.y + part.half_height) - radar_plane_y;
                height = std::clamp(height, -range, range);
            }

            const ImVec2 top[4] = {
                project_offset(v0, height, nullptr),
                project_offset(v1, height, nullptr),
                project_offset(v2, height, nullptr),
                project_offset(v3, height, nullptr)
            };

            if (is_3d && height > 0.1f)
            {
                const ImVec2 base[4] = {
                    project_offset(v0, 0.0f, nullptr),
                    project_offset(v1, 0.0f, nullptr),
                    project_offset(v2, 0.0f, nullptr),
                    project_offset(v3, 0.0f, nullptr)
                };
                const ImU32 base_col = tint_map(part.color, 0.7f, 1.0f);
                draw->AddConvexPolyFilled(base, 4, base_col);

                const ImU32 side_col = tint_map(part.color, 0.8f, 1.0f);
                const ImVec2 side0[4] = { base[0], base[1], top[1], top[0] };
                const ImVec2 side1[4] = { base[1], base[2], top[2], top[1] };
                const ImVec2 side2[4] = { base[2], base[3], top[3], top[2] };
                const ImVec2 side3[4] = { base[3], base[0], top[0], top[3] };
                draw->AddConvexPolyFilled(side0, 4, side_col);
                draw->AddConvexPolyFilled(side1, 4, side_col);
                draw->AddConvexPolyFilled(side2, 4, side_col);
                draw->AddConvexPolyFilled(side3, 4, side_col);
            }

            const ImU32 top_col = tint_map(part.color, 1.0f, 1.0f);
            draw->AddConvexPolyFilled(top, 4, top_col);
        }

        const auto players_snapshot = cache::players_cache->snapshot();
        const auto dummy = cache::players_cache->dummy_snapshot();
        cache::player_state dummy_player{};
        bool has_dummy = false;
        if (dummy && dummy->address != 0)
        {
            dummy_player = make_dummy_player_state(*dummy);
            has_dummy = true;
        }

        auto draw_heading_indicator = [&](const ImVec2& icon_pos, float size, float scale_factor, const rbx::Vector3& world_dir, ImU32 color)
        {
            ImVec2 dir = rotate_to_radar(world_dir.x, world_dir.z);
            float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (len < 1e-3f)
            {
                return;
            }
            dir.x /= len;
            dir.y /= len;
            if (is_3d)
            {
                dir.y *= cos_pitch;
                float len2 = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (len2 > 1e-3f)
                {
                    dir.x /= len2;
                    dir.y /= len2;
                }
            }

            const float angle = std::atan2(dir.y, dir.x);
            const float arc_radius = size * 0.7f * scale_factor;
            const float arc_span = 1.25f;
            const float arc_thickness = 1.6f;
            const float a0 = angle - arc_span * 0.5f;
            const float a1 = angle + arc_span * 0.5f;
            draw->PathArcTo(icon_pos, arc_radius, a0, a1, 24);
            draw->PathStroke(color, false, arc_thickness);
        };

        static std::unordered_map<std::uintptr_t, rbx::Vector3> last_heading_dir;

        auto emit_player = [&](const cache::player_state& player)
            {
                if (player.address == 0 || player.address == local.address)
                {
                    return;
                }

                player_relation relation = determine_relation(local, player);
                if (features->esp_enemy_only && relation != player_relation::enemy)
                {
                    return;
                }
                if (features->team_check && is_friendly_by_team(local, player))
                {
                    return;
                }

                const auto target_root = get_part_position(player.parts.humanoid_root_part);
                if (!target_root)
                {
                    return;
                }

                float dx = target_root->x - local_root_pos->x;
                float dz = target_root->z - local_root_pos->z;

                ImVec2 radar_offset = rotate_to_radar(dx, dz);
                float dot_alpha = alpha;
                if (player.body_effects.knocked)
                {
                    dot_alpha *= 0.35f;
                }
                if (player.health <= 0.0f)
                {
                    dot_alpha *= 0.25f;
                }

                ImU32 outline_col = neutral_outline;
                switch (relation)
                {
                case player_relation::enemy:
                    outline_col = ImGui::GetColorU32(ImVec4(0.95f, 0.3f, 0.3f, dot_alpha));
                    break;
                case player_relation::friendly:
                    outline_col = ImGui::GetColorU32(ImVec4(0.3f, 0.85f, 0.3f, dot_alpha));
                    break;
                default:
                    outline_col = ImGui::GetColorU32(ImVec4(0.9f, 0.9f, 0.9f, dot_alpha));
                    break;
                }

                float icon_scale = 1.0f;
                float player_height = 0.0f;
                if (is_3d)
                {
                    player_height = target_root->y - radar_plane_y;
                    player_height = std::clamp(player_height, -range, range);
                }
                const ImVec2 icon_pos = project_offset(radar_offset, player_height, &icon_scale);
                if (!draw_avatar_icon(player.user_id, icon_pos, icon_size, dot_alpha, outline_col, icon_scale))
                {
                    const ImU32 fill_col = ImGui::GetColorU32(ImVec4(0.6f, 0.6f, 0.6f, dot_alpha));
                    draw_placeholder_icon(icon_pos, icon_size, fill_col, outline_col, icon_scale);
                }

                if (is_3d)
                {
                    bool has_heading = false;
                    rbx::Vector3 heading{};

                    struct rot_t
                    {
                        float m[3][3];
                    };

                    const auto& root_part = player.parts.humanoid_root_part;
                    if (root_part.primitive && roblox::offsets::base_part::cframe_rotation)
                    {
                        const rot_t rot = memory->read<rot_t>(root_part.primitive + roblox::offsets::base_part::cframe_rotation);
                        const rbx::Vector3 basis_z(rot.m[0][2], rot.m[1][2], rot.m[2][2]);
                        if (std::isfinite(basis_z.x) && std::isfinite(basis_z.y) && std::isfinite(basis_z.z))
                        {
                            const float mag_sq = basis_z.x * basis_z.x + basis_z.y * basis_z.y + basis_z.z * basis_z.z;
                            if (mag_sq > 1e-3f)
                            {
                                heading = rbx::Vector3(-basis_z.x, -basis_z.y, -basis_z.z);
                                has_heading = true;
                            }
                        }
                    }

                    if (!has_heading && player.movement.has_move_direction)
                    {
                        const float mag_sq =
                            player.movement.move_direction.x * player.movement.move_direction.x +
                            player.movement.move_direction.y * player.movement.move_direction.y +
                            player.movement.move_direction.z * player.movement.move_direction.z;
                        if (mag_sq > 1e-3f)
                        {
                            heading = player.movement.move_direction;
                            has_heading = true;
                        }
                    }

                    if (has_heading)
                    {
                        last_heading_dir[player.address] = heading;
                    }
                    else
                    {
                        auto it = last_heading_dir.find(player.address);
                        if (it != last_heading_dir.end())
                        {
                            heading = it->second;
                            has_heading = true;
                        }
                    }

                    if (has_heading)
                    {
                        draw_heading_indicator(icon_pos, icon_size, icon_scale, heading, outline_col);
                    }
                }
            };

        if (players_snapshot)
        {
            for (const auto& player : *players_snapshot)
            {
                emit_player(player);
            }
        }
        if (has_dummy)
        {
            emit_player(dummy_player);
        }

        const ImU32 self_outline = ImGui::GetColorU32(ImVec4(c_colors::top_accent_color.x, c_colors::top_accent_color.y, c_colors::top_accent_color.z, alpha));
        if (!draw_avatar_icon(local.user_id, center, local_icon_size, alpha, self_outline, 1.0f))
        {
            const ImU32 fill_col = ImGui::GetColorU32(ImVec4(0.7f, 0.7f, 0.7f, alpha));
            draw_placeholder_icon(center, local_icon_size, fill_col, self_outline, 1.0f);
        }

        if (is_3d)
        {
            struct rot_t
            {
                float m[3][3];
            };

            static rbx::Vector3 last_local_heading{};
            static bool has_last_local_heading = false;

            bool has_heading = false;
            rbx::Vector3 heading{};
            const auto& root_part = local.parts.humanoid_root_part;
            if (root_part.primitive && roblox::offsets::base_part::cframe_rotation)
            {
                const rot_t rot = memory->read<rot_t>(root_part.primitive + roblox::offsets::base_part::cframe_rotation);
                const rbx::Vector3 basis_z(rot.m[0][2], rot.m[1][2], rot.m[2][2]);
                if (std::isfinite(basis_z.x) && std::isfinite(basis_z.y) && std::isfinite(basis_z.z))
                {
                    const float mag_sq = basis_z.x * basis_z.x + basis_z.y * basis_z.y + basis_z.z * basis_z.z;
                    if (mag_sq > 1e-3f)
                    {
                        heading = rbx::Vector3(-basis_z.x, -basis_z.y, -basis_z.z);
                        has_heading = true;
                    }
                }
            }

            if (has_heading)
            {
                last_local_heading = heading;
                has_last_local_heading = true;
            }
            else if (has_last_local_heading)
            {
                heading = last_local_heading;
                has_heading = true;
            }

            if (has_heading)
            {
                draw_heading_indicator(center, local_icon_size, 1.0f, heading, self_outline);
            }
        }

        draw->PopClipRect();
    }

    void render_crosshair()
    {
        if (!features->enable_crosshair)
        {
            crosshair_has_smoothed_position = false;
            crosshair_gap_scale = 1.0f;
            return;
        }

        ImDrawList* draw_list = ImGui::GetForegroundDrawList();
        if (!draw_list)
        {
            return;
        }

        ImVec2 target_position{};
        if (!get_crosshair_mouse_position(target_position))
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        if (!crosshair_has_smoothed_position)
        {
            crosshair_smoothed_position = target_position;
            crosshair_has_smoothed_position = true;
        }

        if (features->crosshair_lerp)
        {
            const float lerp_speed = std::clamp(features->crosshair_lerp_speed, 1.0f, 30.0f);
            const float lerp_alpha = ease_expo(io.DeltaTime, 0.0f, 1.0f / (std::max)(lerp_speed, 0.001f));
            crosshair_smoothed_position.x += (target_position.x - crosshair_smoothed_position.x) * lerp_alpha;
            crosshair_smoothed_position.y += (target_position.y - crosshair_smoothed_position.y) * lerp_alpha;
        }
        else
        {
            crosshair_smoothed_position = target_position;
        }

        const bool rmb_down = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        float target_gap_scale = 1.0f;
        if (features->crosshair_ads_gap && rmb_down)
        {
            target_gap_scale = std::clamp(features->crosshair_ads_gap_scale, 0.05f, 1.0f);
        }

        const float gap_lerp_speed = std::clamp(features->crosshair_ads_gap_lerp_speed, 1.0f, 30.0f);
        const float gap_lerp_alpha = ease_expo(io.DeltaTime, 0.0f, 1.0f / (std::max)(gap_lerp_speed, 0.001f));
        crosshair_gap_scale += (target_gap_scale - crosshair_gap_scale) * gap_lerp_alpha;
        crosshair_gap_scale = std::clamp(crosshair_gap_scale, 0.05f, 1.0f);

        const float line_width = std::clamp(features->crosshair_line_width, 0.5f, 8.0f);
        const float gap = std::clamp(features->crosshair_gap, 0.0f, 48.0f) * crosshair_gap_scale;
        const float size = std::clamp(features->crosshair_size, 2.0f, 64.0f);
        const bool animated_fill = features->crosshair_animated_fill;
        const float wave_speed = std::clamp(features->crosshair_gradient_speed, 0.05f, 3.0f);
        const float wave_time = static_cast<float>(ImGui::GetTime()) * wave_speed;
        const ImVec2 center = round_point(crosshair_smoothed_position);
        const ImU32 outline_color = ImGui::GetColorU32(features->crosshair_outline_color);

        float rotation = 0.0f;
        if (features->crosshair_animated_lines)
        {
            const float spin_speed = std::clamp(features->crosshair_spin_speed, 1.0f, 720.0f);
            rotation = static_cast<float>(ImGui::GetTime()) * spin_speed * (IM_PI / 180.0f);
        }

        const ImDrawListFlags previous_flags = draw_list->Flags;
        draw_list->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;

        for (int line_index = 0; line_index < 4; ++line_index)
        {
            const float angle = rotation + static_cast<float>(line_index) * (IM_PI * 0.5f);
            const ImVec2 direction(std::cos(angle), std::sin(angle));
            const ImVec2 line_start(center.x + direction.x * gap, center.y + direction.y * gap);
            const ImVec2 line_end(center.x + direction.x * (gap + size), center.y + direction.y * (gap + size));
            draw_crosshair_segment(
                draw_list,
                line_start,
                line_end,
                line_width,
                outline_color,
                features->crosshair_color,
                features->crosshair_animated_color_a,
                features->crosshair_animated_color_b,
                animated_fill,
                wave_time);
        }

        if (features->crosshair_middle_dot)
        {
            ImVec4 dot_color = features->crosshair_color;
            if (animated_fill)
            {
                dot_color = get_crosshair_wave_color(features->crosshair_animated_color_a, features->crosshair_animated_color_b, wave_time);
            }
            const float middle_dot_size = std::clamp(features->crosshair_middle_dot_size, 1.0f, 12.0f);
            const float half_size = middle_dot_size * 0.5f;
            const ImVec2 fill_min = round_point(ImVec2(center.x - half_size, center.y - half_size));
            const ImVec2 fill_max = round_point(ImVec2(center.x + half_size, center.y + half_size));
            const ImVec2 outline_min(fill_min.x - 1.0f, fill_min.y - 1.0f);
            const ImVec2 outline_max(fill_max.x + 1.0f, fill_max.y + 1.0f);
            draw_list->AddRectFilled(outline_min, outline_max, outline_color, 0.0f);
            draw_list->AddRectFilled(fill_min, fill_max, ImGui::GetColorU32(dot_color), 0.0f);
        }

        draw_list->Flags = previous_flags;
    }

    void render_free_aim_fov()
    {
        if (!features->free_aim_draw_fov || features->free_aim_fov_radius <= 0.0f)
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        if (features->free_aim_fov_mode == 1)
        {
            POINT pt;
            if (GetCursorPos(&pt) && vanille::overlay::g_overlay_window)
            {
                POINT client = pt;
                if (ScreenToClient(vanille::overlay::g_overlay_window, &client))
                {
                    center = ImVec2(static_cast<float>(client.x), static_cast<float>(client.y));
                }
            }
        }

        ImDrawList* draw_list = ImGui::GetForegroundDrawList();
        if (!draw_list)
        {
            return;
        }

        const ImU32 fov_color = ImGui::GetColorU32(ImVec4(c_colors::top_accent_color.x, c_colors::top_accent_color.y, c_colors::top_accent_color.z, 0.35f));
        draw_list->AddCircle(center, features->free_aim_fov_radius, fov_color, 64, 1.5f);
    }

    void render_aimbot_fov()
    {
        if (!features->aimbot_draw_fov || features->aimbot_fov_radius <= 0.0f)
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        if (features->aimbot_fov_mode == 1)
        {
            POINT pt;
            if (GetCursorPos(&pt) && vanille::overlay::g_overlay_window)
            {
                POINT client = pt;
                if (ScreenToClient(vanille::overlay::g_overlay_window, &client))
                {
                    center = ImVec2(static_cast<float>(client.x), static_cast<float>(client.y));
                }
            }
        }

        ImDrawList* draw_list = ImGui::GetForegroundDrawList();
        if (!draw_list)
        {
            return;
        }

        const ImU32 fov_color = ImGui::GetColorU32(ImVec4(c_colors::top_accent_color.x, c_colors::top_accent_color.y, c_colors::top_accent_color.z, 0.35f));
        draw_list->AddCircle(center, features->aimbot_fov_radius, fov_color, 64, 1.5f);
    }

    void render_triggerbot_fov()
    {
        if (!features->triggerbot_draw_fov || features->triggerbot_fov_radius <= 0.0f)
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        if (features->triggerbot_fov_mode == 1)
        {
            POINT pt;
            if (GetCursorPos(&pt) && vanille::overlay::g_overlay_window)
            {
                POINT client = pt;
                if (ScreenToClient(vanille::overlay::g_overlay_window, &client))
                {
                    center = ImVec2(static_cast<float>(client.x), static_cast<float>(client.y));
                }
            }
        }

        ImDrawList* draw_list = ImGui::GetForegroundDrawList();
        if (!draw_list)
        {
            return;
        }

        const ImU32 fov_color = ImGui::GetColorU32(ImVec4(c_colors::top_accent_color.x, c_colors::top_accent_color.y, c_colors::top_accent_color.z, 0.35f));
        draw_list->AddCircle(center, features->triggerbot_fov_radius, fov_color, 64, 1.5f);
    }
}
