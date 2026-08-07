#include "features/tests.h"
#include "features/arm_modifier.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Windows.h>
#include <DirectXMath.h>
#include <imgui.h>
#include "clipper2/include/clipper.h"
#include "clipper2/include/earcut.hpp"

#include "cache/local_player_cache.h"
#include "cache/player_cache.h"
#include "globals/globals_fixed.h"
#include "gui/colors/colors_new.h"
#include "gui/overlay.hpp"
#include "memory/memory.h"
#include "sdk/camera.h"
#include "sdk/mesh_part.h"
#include "sdk/offsets.h"
#include "sdk/part.h"

namespace
{
    struct trace_state
    {
        bool active = false;
        rbx::Vector3 start{};
        rbx::Vector3 end{};
        rbx::Vector3 direction{};
        std::string label;
        std::uintptr_t primitive = 0;
        rbx::instance_t instance{};
        bool has_part = false;
        rbx::Vector3 part_center{};
        rbx::Vector3 part_half{};
        DirectX::XMFLOAT3X3 part_rotation{};
    };

    trace_state g_trace{};
    std::mutex g_chat_mutex;

    bool is_printable_ascii(const std::string& text)
    {
        for (unsigned char c : text)
        {
            if (c < 32 || c > 126)
            {
                return false;
            }
        }
        return true;
    }

    std::string sanitize_label(const std::string& text)
    {
        if (text.empty())
        {
            return "Unknown";
        }

        if (is_printable_ascii(text))
        {
            return text;
        }

        std::string out;
        out.reserve(text.size());
        for (unsigned char c : text)
        {
            if (c >= 32 && c <= 126)
            {
                out.push_back(static_cast<char>(c));
            }
        }
        if (out.empty())
        {
            return "Unknown";
        }
        return out;
    }

    std::string get_instance_label(const rbx::instance_t& instance)
    {
        if (!instance.is_valid())
        {
            return {};
        }

        const std::string name = instance.get_name();
        if (!name.empty() && name != "Unknown")
        {
            const std::string clean = sanitize_label(name);
            if (!clean.empty() && clean != "Unknown")
            {
                return clean;
            }
        }

        const std::string class_name = instance.get_class_name();
        if (!class_name.empty() && class_name != "unknown_class")
        {
            const std::string clean = sanitize_label(class_name);
            if (!clean.empty() && clean != "Unknown")
            {
                return clean;
            }
        }

        return {};
    }

    std::string choose_display_name(const cache::player_state& player)
    {
        if (!player.display_name.empty() && is_printable_ascii(player.display_name))
        {
            return player.display_name;
        }
        if (!player.name.empty())
        {
            return player.name;
        }
        return "Player";
    }

    void refresh_chat_instances()
    {
        std::lock_guard<std::mutex> lock(g_chat_mutex);
        if (!globals->text_chat_service.is_valid() && globals->datamodel.is_valid())
        {
            globals->text_chat_service = globals->datamodel.find_first_child("TextChatService");
        }

        if (!globals->chat_input_bar_configuration.is_valid() && globals->text_chat_service.is_valid())
        {
            globals->chat_input_bar_configuration = globals->text_chat_service.find_first_child("ChatInputBarConfiguration");
        }
    }

    bool chat_is_focused()
    {
        refresh_chat_instances();
        if (!globals->chat_input_bar_configuration.is_valid() || !roblox::offsets::chat::is_focused)
        {
            return false;
        }

        const auto addr = globals->chat_input_bar_configuration.get_address() + roblox::offsets::chat::is_focused;
        const std::uint32_t flags = memory->read<std::uint32_t>(addr);
        return (flags & 0x10000u) != 0;
    }

    bool roblox_has_focus()
    {
        const HWND window = vanille::overlay::g_rbx_window;
        return window && ::IsWindow(window) && ::GetForegroundWindow() == window;
    }

    bool is_finite_vec(const rbx::Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    rbx::Vector3 get_forward(const DirectX::XMFLOAT3X3& rot)
    {
        rbx::Vector3 forward(-rot._13, -rot._23, -rot._33);
        const float len_sq = forward.LengthSquared();
        if (len_sq < 1e-4f || !std::isfinite(len_sq))
        {
            return { 0.0f, 0.0f, -1.0f };
        }
        forward.Normalize();
        return forward;
    }

    DirectX::XMFLOAT3X3 identity_rotation()
    {
        DirectX::XMFLOAT3X3 rot{};
        rot._11 = rot._22 = rot._33 = 1.0f;
        rot._12 = rot._13 = rot._21 = rot._23 = rot._31 = rot._32 = 0.0f;
        return rot;
    }

    rbx::Vector3 multiply_transpose(const DirectX::XMFLOAT3X3& m, const rbx::Vector3& v)
    {
        return rbx::Vector3(
            m._11 * v.x + m._12 * v.y + m._13 * v.z,
            m._21 * v.x + m._22 * v.y + m._23 * v.z,
            m._31 * v.x + m._32 * v.y + m._33 * v.z
        );
    }

    DirectX::XMFLOAT3X3 read_part_rotation(std::uintptr_t primitive)
    {
        if (!primitive || !roblox::offsets::base_part::cframe_rotation)
        {
            return identity_rotation();
        }

        try
        {
            return memory->read<DirectX::XMFLOAT3X3>(primitive + roblox::offsets::base_part::cframe_rotation);
        }
        catch (...)
        {
            return identity_rotation();
        }
    }

    std::optional<rbx::Vector3> read_primitive_position(std::uintptr_t primitive)
    {
        if (!primitive || !roblox::offsets::base_part::position)
        {
            return std::nullopt;
        }

        const auto pos = memory->read<rbx::Vector3>(primitive + roblox::offsets::base_part::position);
        if (!is_finite_vec(pos))
        {
            return std::nullopt;
        }
        return pos;
    }

    bool is_valid_primitive(std::uintptr_t primitive)
    {
        if (!primitive || !roblox::offsets::base_part::validate)
        {
            return false;
        }

        constexpr std::uint32_t k_validate_value = 0x6;
        try
        {
            const auto validate_raw = memory->read<std::uint32_t>(primitive + roblox::offsets::base_part::validate);
            return validate_raw == k_validate_value
                || (validate_raw & 0xFFu) == k_validate_value
                || ((validate_raw >> 8) & 0xFFu) == k_validate_value
                || ((validate_raw >> 16) & 0xFFu) == k_validate_value
                || ((validate_raw >> 24) & 0xFFu) == k_validate_value;
        }
        catch (...)
        {
            return false;
        }
    }

    bool is_collidable(std::uintptr_t primitive)
    {
        if (!primitive || !roblox::offsets::base_part::can_collide || !roblox::offsets::base_part::can_collide_mask)
        {
            return true;
        }

        try
        {
            const std::uintptr_t addr = primitive + roblox::offsets::base_part::can_collide;
            const std::uint8_t flags = memory->read<std::uint8_t>(addr);
            const std::uint8_t mask = static_cast<std::uint8_t>(roblox::offsets::base_part::can_collide_mask);
            return (flags & mask) != 0;
        }
        catch (...)
        {
            return false;
        }
    }

    bool size_is_reasonable(const rbx::Vector3& size)
    {
        if (!is_finite_vec(size))
        {
            return false;
        }

        constexpr float k_min_size = 0.1f;
        constexpr float k_max_size = 5000.0f;
        if (size.x < k_min_size || size.y < k_min_size || size.z < k_min_size)
        {
            return false;
        }
        if (size.x > k_max_size || size.y > k_max_size || size.z > k_max_size)
        {
            return false;
        }
        return true;
    }

    bool resolve_part_bounds(
        std::uintptr_t primitive,
        const rbx::instance_t& instance,
        rbx::Vector3& out_center,
        rbx::Vector3& out_half,
        DirectX::XMFLOAT3X3& out_rotation)
    {
        if (!primitive)
        {
            return false;
        }

        const auto size_opt = rbx::part::get_size(primitive);
        if (!size_opt || !size_is_reasonable(*size_opt))
        {
            return false;
        }

        std::optional<rbx::Vector3> pos_opt;
        if (instance.is_valid())
        {
            pos_opt = instance.get_position(primitive);
        }
        if (!pos_opt)
        {
            pos_opt = read_primitive_position(primitive);
        }
        if (!pos_opt || !is_finite_vec(*pos_opt))
        {
            return false;
        }

        out_center = *pos_opt;
        out_half = *size_opt * 0.5f;
        out_rotation = read_part_rotation(primitive);
        return true;
    }

    bool ray_intersects_obb(
        const rbx::Vector3& origin,
        const rbx::Vector3& dir,
        const rbx::Vector3& center,
        const rbx::Vector3& half,
        const DirectX::XMFLOAT3X3& rotation,
        float max_distance,
        float& out_t)
    {
        rbx::Vector3 local_origin = multiply_transpose(rotation, origin - center);
        rbx::Vector3 local_dir = multiply_transpose(rotation, dir);

        float tmin = -(std::numeric_limits<float>::max)();
        float tmax = (std::numeric_limits<float>::max)();
        constexpr float epsilon = 1e-6f;

        const float bounds_min[3] = { -half.x, -half.y, -half.z };
        const float bounds_max[3] = { half.x, half.y, half.z };
        const float origin_components[3] = { local_origin.x, local_origin.y, local_origin.z };
        const float dir_components[3] = { local_dir.x, local_dir.y, local_dir.z };

        for (int axis = 0; axis < 3; ++axis)
        {
            const float origin_component = origin_components[axis];
            const float dir_component = dir_components[axis];
            const float min_bound = bounds_min[axis];
            const float max_bound = bounds_max[axis];

            if (std::fabs(dir_component) < epsilon)
            {
                if (origin_component < min_bound || origin_component > max_bound)
                {
                    return false;
                }
                continue;
            }

            float t1 = (min_bound - origin_component) / dir_component;
            float t2 = (max_bound - origin_component) / dir_component;
            if (t1 > t2)
            {
                std::swap(t1, t2);
            }

            tmin = (std::max)(tmin, t1);
            tmax = (std::min)(tmax, t2);
            if (tmin > tmax)
            {
                return false;
            }
        }

        if (tmax < 0.0f)
        {
            return false;
        }

        const float t = tmin >= 0.0f ? tmin : tmax;
        if (t < 0.0f || t > max_distance)
        {
            return false;
        }

        out_t = t;
        return true;
    }

    bool project_segment_unclamped(
        const rbx::Vector3& start,
        const rbx::Vector3& end,
        const rbx::Matrix& view_matrix,
        const rbx::Vector2& dimensions,
        ImVec2& out_start,
        ImVec2& out_end)
    {
        if (dimensions.x <= 0.0f || dimensions.y <= 0.0f)
        {
            return false;
        }

        rbx::Vector4 clip_start = rbx::camera::transform(start, view_matrix);
        rbx::Vector4 clip_end = rbx::camera::transform(end, view_matrix);

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
            const float screen_x = (ndc_x * 0.5f + 0.5f) * dimensions.x;
            const float screen_y = (-ndc_y * 0.5f + 0.5f) * dimensions.y;
            return ImVec2(screen_x, screen_y);
        };

        out_start = to_screen(clip_start);
        out_end = to_screen(clip_end);
        return true;
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

        const size_t lower_size = hull.size();
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

        return rbx::Vector3(
            tr.rotation[0][0] * local.x + tr.rotation[0][1] * local.y + tr.rotation[0][2] * local.z,
            tr.rotation[1][0] * local.x + tr.rotation[1][1] * local.y + tr.rotation[1][2] * local.z,
            tr.rotation[2][0] * local.x + tr.rotation[2][1] * local.y + tr.rotation[2][2] * local.z
        );
    }

    std::optional<Clipper2Lib::PathD> build_hull_path(std::vector<ImVec2> projected)
    {
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
        return path;
    }

    std::optional<Clipper2Lib::PathD> project_box_hull(
        const rbx::Vector3& center,
        const rbx::Vector3& half,
        const DirectX::XMFLOAT3X3& rotation,
        const rbx::Matrix& view_matrix,
        const rbx::Vector2& dimensions)
    {
        if (!is_finite_vec(half) || half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f)
        {
            return std::nullopt;
        }

        const rbx::Vector3 basis_x(rotation._11, rotation._21, rotation._31);
        const rbx::Vector3 basis_y(rotation._12, rotation._22, rotation._32);
        const rbx::Vector3 basis_z(rotation._13, rotation._23, rotation._33);
        const rbx::Vector3 hx = basis_x * half.x;
        const rbx::Vector3 hy = basis_y * half.y;
        const rbx::Vector3 hz = basis_z * half.z;

        const rbx::Vector3 corners[8] = {
            center - hx - hy - hz,
            center + hx - hy - hz,
            center + hx + hy - hz,
            center - hx + hy - hz,
            center - hx - hy + hz,
            center + hx - hy + hz,
            center + hx + hy + hz,
            center - hx + hy + hz
        };

        std::vector<ImVec2> projected;
        projected.reserve(8);
        for (const auto& corner : corners)
        {
            const auto screen = rbx::camera::world_to_screen(corner, view_matrix, dimensions);
            if (!screen)
            {
                continue;
            }
            projected.emplace_back(screen->x, screen->y);
        }

        return build_hull_path(std::move(projected));
    }

    std::optional<Clipper2Lib::PathD> project_part_hull(
        std::uintptr_t primitive,
        const rbx::instance_t& instance,
        const rbx::Matrix& view_matrix,
        const rbx::Vector2& dimensions)
    {
        if (!primitive || !instance.is_valid())
        {
            return std::nullopt;
        }

        const auto transform = rbx::mesh_part::get_transform(instance, primitive);
        if (!transform)
        {
            return std::nullopt;
        }

        const auto size_opt = rbx::part::get_size(primitive);
        if (!size_opt || !size_is_reasonable(*size_opt))
        {
            return std::nullopt;
        }

        const rbx::Vector3 half = *size_opt * 0.5f;
        const std::array<rbx::Vector3, 8> corners = {
            rbx::Vector3(-half.x, -half.y, -half.z),
            rbx::Vector3( half.x, -half.y, -half.z),
            rbx::Vector3(-half.x,  half.y, -half.z),
            rbx::Vector3( half.x,  half.y, -half.z),
            rbx::Vector3(-half.x, -half.y,  half.z),
            rbx::Vector3( half.x, -half.y,  half.z),
            rbx::Vector3(-half.x,  half.y,  half.z),
            rbx::Vector3( half.x,  half.y,  half.z)
        };

        std::vector<ImVec2> projected;
        projected.reserve(corners.size());
        for (const auto& local : corners)
        {
            const rbx::Vector3 world = transform->position + rotate_point(*transform, local);
            const auto screen = rbx::camera::world_to_screen(world, view_matrix, dimensions);
            if (!screen)
            {
                continue;
            }
            projected.emplace_back(screen->x, screen->y);
        }

        return build_hull_path(std::move(projected));
    }

    std::optional<Clipper2Lib::PathD> project_mesh_hull(
        std::uintptr_t primitive,
        const rbx::instance_t& instance,
        const rbx::Matrix& view_matrix,
        const rbx::Vector2& dimensions)
    {
        constexpr std::size_t k_max_project_points = 600;

        if (!instance.is_valid())
        {
            return std::nullopt;
        }

        const auto mesh_id = rbx::mesh_part::get_mesh_asset_id(instance);
        if (!mesh_id)
        {
            return std::nullopt;
        }

        rbx::mesh_parse::mesh_data mesh;
        if (!cache::ensure_mesh_data(*mesh_id, mesh))
        {
            return std::nullopt;
        }
        if (mesh.vertices.size() < 3)
        {
            return std::nullopt;
        }

        const auto transform = rbx::mesh_part::get_transform(instance, primitive);
        if (!transform)
        {
            return std::nullopt;
        }

        std::vector<ImVec2> projected;
        projected.reserve(mesh.vertices.size() / 3);

        const std::size_t total_vertices = mesh.vertices.size() / 3;
        const std::size_t stride = std::max<std::size_t>(1, (total_vertices + k_max_project_points - 1) / k_max_project_points);

        for (size_t i = 0; i + 2 < mesh.vertices.size(); i += stride * 3)
        {
            rbx::Vector3 local{ mesh.vertices[i], mesh.vertices[i + 1], mesh.vertices[i + 2] };
            rbx::Vector3 world = transform->position + rotate_point(*transform, local);
            if (const auto screen = rbx::camera::world_to_screen(world, view_matrix, dimensions))
            {
                projected.emplace_back(screen->x, screen->y);
            }
        }

        return build_hull_path(std::move(projected));
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

    void draw_trace_hulls(ImDrawList* draw, const std::vector<Clipper2Lib::PathD>& hulls, ImU32 fill_color, ImU32 outline_color)
    {
        if (!draw || hulls.empty())
        {
            return;
        }

        constexpr double scale = 100.0;
        Clipper2Lib::Paths64 hulls_i = scale_to_int(hulls, scale);

        Clipper2Lib::Paths64 unified = Clipper2Lib::Union(hulls_i, Clipper2Lib::FillRule::NonZero);
        unified = Clipper2Lib::SimplifyPaths(unified, 2.0, true);
        Clipper2Lib::PathsD unified_d = scale_to_double(unified, scale);

        ImDrawListFlags saved_flags = draw->Flags;
        draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (const auto& poly : unified_d)
        {
            if (poly.size() < 3)
            {
                continue;
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
                    fill_color);
            }
        }
        draw->Flags = saved_flags;

        for (const auto& path : unified_d)
        {
            if (path.size() < 2)
            {
                continue;
            }

            std::vector<ImVec2> pts;
            pts.reserve(path.size());
            float minx = (std::numeric_limits<float>::max)();
            float miny = (std::numeric_limits<float>::max)();
            float maxx = -(std::numeric_limits<float>::max)();
            float maxy = -(std::numeric_limits<float>::max)();
            for (const auto& p : path)
            {
                ImVec2 pt(static_cast<float>(p.x), static_cast<float>(p.y));
                pts.push_back(pt);
                minx = (std::min)(minx, pt.x);
                miny = (std::min)(miny, pt.y);
                maxx = (std::max)(maxx, pt.x);
                maxy = (std::max)(maxy, pt.y);
            }

            const float span = (std::max)(maxx - minx, maxy - miny);
            const float thickness = std::clamp(span / 90.0f, 2.0f, 3.5f);

            ImDrawListFlags outline_flags = draw->Flags;
            outline_flags &= ~ImDrawListFlags_AntiAliasedLines;
            draw->Flags = outline_flags;
            draw->AddPolyline(pts.data(), static_cast<int>(pts.size()), outline_color, true, thickness);
        }

        draw->Flags = saved_flags;
    }

    void draw_trace_part_box(
        ImDrawList* draw,
        const trace_state& trace,
        const rbx::Matrix& view_matrix,
        const rbx::Vector2& dimensions,
        ImU32 fill_color,
        ImU32 outline_color)
    {
        if (!draw)
        {
            return;
        }

        std::optional<Clipper2Lib::PathD> hull;
        if (trace.primitive && trace.instance.is_valid())
        {
            hull = project_mesh_hull(trace.primitive, trace.instance, view_matrix, dimensions);
            if (!hull)
            {
                hull = project_part_hull(trace.primitive, trace.instance, view_matrix, dimensions);
            }
        }

        if (!hull && trace.has_part)
        {
            hull = project_box_hull(trace.part_center, trace.part_half, trace.part_rotation, view_matrix, dimensions);
        }

        if (!hull)
        {
            return;
        }

        std::vector<Clipper2Lib::PathD> hulls;
        hulls.reserve(1);
        hulls.push_back(std::move(*hull));
        draw_trace_hulls(draw, hulls, fill_color, outline_color);
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
                &parts.right_foot,
                &parts.humanoid_root_part
            };
        }

        return {
            &parts.head,
            &parts.torso,
            &parts.left_arm,
            &parts.right_arm,
            &parts.left_leg,
            &parts.right_leg,
            &parts.humanoid_root_part
        };
    }

    void add_primitives_from_parts(const cache::character_parts& parts, std::unordered_set<std::uintptr_t>& out)
    {
        const auto collected = collect_parts(parts);
        for (const auto* part : collected)
        {
            if (part && part->primitive)
            {
                out.insert(part->primitive);
            }
        }
    }

    std::optional<std::uintptr_t> get_workspace_primitives_root()
    {
        if (!globals->workspace.is_valid())
        {
            return std::nullopt;
        }

        if (!roblox::offsets::workspace::primitives_pointer1 || !roblox::offsets::workspace::primitives_pointer2)
        {
            return std::nullopt;
        }

        try
        {
            const auto p1 = memory->read<std::uintptr_t>(globals->workspace.get_address() + roblox::offsets::workspace::primitives_pointer1);
            if (!p1)
            {
                return std::nullopt;
            }

            const auto p2 = memory->read<std::uintptr_t>(p1 + roblox::offsets::workspace::primitives_pointer2);
            if (!p2)
            {
                return std::nullopt;
            }

            return p2;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::vector<rbx::instance_t> get_children_safely(const rbx::instance_t& instance, std::size_t max_children)
    {
        if (!instance.is_valid())
        {
            return {};
        }

        try
        {
            auto children = instance.get_children();
            if (children.size() > max_children)
            {
                children.resize(max_children);
            }
            return children;
        }
        catch (...)
        {
            return {};
        }
    }

    std::unordered_map<std::uintptr_t, rbx::instance_t> build_workspace_part_map(const std::unordered_set<std::uintptr_t>& ignore)
    {
        std::unordered_map<std::uintptr_t, rbx::instance_t> result;
        if (!globals->workspace.is_valid())
        {
            return result;
        }

        constexpr std::size_t k_max_nodes = 20000;
        auto root_children = get_children_safely(globals->workspace, 4096);
        std::vector<rbx::instance_t> stack;
        stack.reserve(root_children.size());
        for (const auto& child : root_children)
        {
            stack.push_back(child);
        }

        std::size_t processed = 0;
        result.reserve(4096);

        while (!stack.empty() && processed < k_max_nodes)
        {
            rbx::instance_t entry = stack.back();
            stack.pop_back();
            ++processed;

            const auto primitive = rbx::part::get_primitive(entry);
            if (primitive && ignore.find(primitive) == ignore.end())
            {
                result.emplace(primitive, entry);
            }

            auto children = get_children_safely(entry, 512);
            for (const auto& child : children)
            {
                if (stack.size() >= k_max_nodes)
                {
                    break;
                }
                stack.push_back(child);
            }
        }

        return result;
    }

    bool consider_primitive(
        std::uintptr_t primitive,
        const rbx::instance_t* instance,
        const std::string& label,
        bool require_collide,
        const rbx::Vector3& origin,
        const rbx::Vector3& dir,
        float max_distance,
        float& best_t,
        std::uintptr_t& best_primitive,
        rbx::instance_t& best_instance,
        std::string& best_label)
    {
        if (!is_valid_primitive(primitive))
        {
            return false;
        }

        if (require_collide && !is_collidable(primitive))
        {
            return false;
        }

        const auto size_opt = rbx::part::get_size(primitive);
        if (!size_opt || !size_is_reasonable(*size_opt))
        {
            return false;
        }

        std::optional<rbx::Vector3> pos_opt;
        if (instance && instance->is_valid())
        {
            pos_opt = instance->get_position(primitive);
        }
        if (!pos_opt)
        {
            pos_opt = read_primitive_position(primitive);
        }
        if (!pos_opt || !is_finite_vec(*pos_opt))
        {
            return false;
        }

        const rbx::Vector3 center = *pos_opt;
        const rbx::Vector3 half = *size_opt * 0.5f;
        const DirectX::XMFLOAT3X3 rotation = read_part_rotation(primitive);

        float t = 0.0f;
        if (!ray_intersects_obb(origin, dir, center, half, rotation, max_distance, t))
        {
            return false;
        }

        if (t < best_t)
        {
            best_t = t;
            best_primitive = primitive;
            if (instance && instance->is_valid())
            {
                best_instance = *instance;
            }
            else
            {
                best_instance = {};
            }
            best_label = label;
            return true;
        }

        return false;
    }

    std::optional<rbx::instance_t> find_instance_by_primitive(std::uintptr_t primitive, std::size_t max_nodes)
    {
        if (!globals->workspace.is_valid() || !primitive)
        {
            return std::nullopt;
        }

        auto root_children = get_children_safely(globals->workspace, 4096);
        std::vector<rbx::instance_t> stack;
        stack.reserve(root_children.size());
        for (const auto& child : root_children)
        {
            stack.push_back(child);
        }

        std::size_t processed = 0;
        while (!stack.empty() && processed < max_nodes)
        {
            rbx::instance_t entry = stack.back();
            stack.pop_back();
            ++processed;

            const auto entry_primitive = rbx::part::get_primitive(entry);
            if (entry_primitive == primitive)
            {
                return entry;
            }

            auto children = get_children_safely(entry, 512);
            for (const auto& child : children)
            {
                if (stack.size() >= max_nodes)
                {
                    break;
                }
                stack.push_back(child);
            }
        }

        return std::nullopt;
    }

    bool capture_trace(trace_state& trace)
    {
        const auto local = cache::localplayer->snapshot();
        if (local.address == 0 || !local.camera.is_valid())
        {
            return false;
        }

        const rbx::Vector3 origin = local.camera.get_camera_position();
        if (!is_finite_vec(origin))
        {
            return false;
        }

        const rbx::Vector3 dir = get_forward(local.camera.get_rotation());
        float max_distance = features->aim_trace_max_distance;
        if (!std::isfinite(max_distance) || max_distance <= 0.0f)
        {
            max_distance = 5000.0f;
        }

        float best_t = max_distance + 1.0f;
        std::uintptr_t best_primitive = 0;
        rbx::instance_t best_instance{};
        std::string best_label;

        std::unordered_set<std::uintptr_t> player_primitives;
        player_primitives.reserve(256);

        add_primitives_from_parts(local.parts, player_primitives);

        auto players_snapshot = cache::players_cache->snapshot();
        if (players_snapshot)
        {
            for (const auto& player : *players_snapshot)
            {
                if (player.address == 0)
                {
                    continue;
                }
                add_primitives_from_parts(player.parts, player_primitives);
            }
        }

        if (players_snapshot)
        {
            for (const auto& player : *players_snapshot)
            {
                if (player.address == 0 || player.address == local.address)
                {
                    continue;
                }

            std::string owner = sanitize_label(choose_display_name(player));
            if (owner.empty() || owner == "Unknown")
            {
                owner = "Player";
            }
                const auto parts = collect_parts(player.parts);
                for (const auto* part : parts)
                {
                    if (!part || !part->primitive || !part->instance.is_valid())
                    {
                        continue;
                    }

                std::string part_name = get_instance_label(part->instance);
                if (part_name.empty() || part_name == "Unknown")
                {
                    part_name = "Part";
                }
                std::string label = "Part: " + part_name + " (Entity: " + owner + ")";
                consider_primitive(part->primitive, &part->instance, label, false, origin, dir, max_distance, best_t, best_primitive, best_instance, best_label);
                }
            }
        }

        const auto workspace_map = build_workspace_part_map(player_primitives);
        const auto primitives_root = get_workspace_primitives_root();
        if (primitives_root)
        {
            constexpr std::size_t k_max_scan = 5024;
            for (std::size_t i = 0; i < k_max_scan; ++i)
            {
                std::uintptr_t primitive = 0;
                try
                {
                    primitive = memory->read<std::uintptr_t>(*primitives_root + i * sizeof(std::uintptr_t));
                }
                catch (...)
                {
                    break;
                }

                if (!primitive)
                {
                    break;
                }
                if (player_primitives.find(primitive) != player_primitives.end())
                {
                    continue;
                }

                const rbx::instance_t* instance = nullptr;
                std::string part_name;
                auto it = workspace_map.find(primitive);
                if (it != workspace_map.end())
                {
                    instance = &it->second;
                    part_name = get_instance_label(it->second);
                }
                if (part_name.empty())
                {
                    part_name = "World";
                }
                std::string label;
                if (!part_name.empty())
                {
                    label = "Part: " + part_name;
                }
                consider_primitive(primitive, instance, label, true, origin, dir, max_distance, best_t, best_primitive, best_instance, best_label);
            }
        }

        trace.active = true;
        trace.start = origin;
        trace.direction = dir;
        trace.primitive = best_primitive;
        trace.instance = {};
        trace.has_part = false;
        trace.part_center = {};
        trace.part_half = {};
        trace.part_rotation = identity_rotation();
        if (best_label.empty() && best_primitive != 0)
        {
            if (!best_instance.is_valid())
            {
                if (auto found = find_instance_by_primitive(best_primitive, 25000))
                {
                    best_instance = *found;
                }
            }

            if (best_instance.is_valid())
            {
                const std::string resolved = get_instance_label(best_instance);
                if (!resolved.empty() && resolved != "Unknown")
                {
                    best_label = "Part: " + resolved;
                }
            }
        }

        if (!best_instance.is_valid() && best_primitive != 0)
        {
            if (auto found = find_instance_by_primitive(best_primitive, 25000))
            {
                best_instance = *found;
            }
        }

        trace.instance = best_instance;

        if (!best_label.empty())
        {
            trace.end = origin + dir * best_t;
            trace.label = best_label;
        }
        else
        {
            trace.end = origin + dir * max_distance;
            trace.label = "Part: None";
        }

        if (best_primitive != 0)
        {
            rbx::Vector3 part_center{};
            rbx::Vector3 part_half{};
            DirectX::XMFLOAT3X3 part_rotation = identity_rotation();
            if (resolve_part_bounds(best_primitive, best_instance, part_center, part_half, part_rotation))
            {
                trace.has_part = true;
                trace.part_center = part_center;
                trace.part_half = part_half;
                trace.part_rotation = part_rotation;
            }
        }

        return true;
    }
}

namespace tests
{
    void render()
    {
        arm_modifier::run();
    }
}
