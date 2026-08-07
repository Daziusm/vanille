#include "features/visibility.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <climits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "globals/globals_fixed.h"
#include "memory/memory.h"
#include "sdk/part.h"
#include "sdk/offsets.h"
#include "sdk/mesh_part.h"

namespace visibility
{
    namespace
    {
        struct occluder_part
        {
            std::uintptr_t primitive = 0;
            rbx::Vector3 position{};
            rbx::Vector3 half_size{};
            DirectX::XMFLOAT3X3 rotation{};
            float radius_sq = 0.0f;
            mutable std::atomic<std::uint64_t> last_query_id{ 0 };

            occluder_part() = default;

            occluder_part(const occluder_part& other)
                : primitive(other.primitive)
                , position(other.position)
                , half_size(other.half_size)
                , rotation(other.rotation)
                , radius_sq(other.radius_sq)
            {
                last_query_id.store(other.last_query_id.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }

            occluder_part& operator=(const occluder_part& other)
            {
                if (this != &other)
                {
                    primitive = other.primitive;
                    position = other.position;
                    half_size = other.half_size;
                    rotation = other.rotation;
                    radius_sq = other.radius_sq;
                    last_query_id.store(other.last_query_id.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
                return *this;
            }

            occluder_part(occluder_part&& other) noexcept
                : primitive(other.primitive)
                , position(other.position)
                , half_size(other.half_size)
                , rotation(other.rotation)
                , radius_sq(other.radius_sq)
            {
                last_query_id.store(other.last_query_id.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }

            occluder_part& operator=(occluder_part&& other) noexcept
            {
                if (this != &other)
                {
                    primitive = other.primitive;
                    position = other.position;
                    half_size = other.half_size;
                    rotation = other.rotation;
                    radius_sq = other.radius_sq;
                    last_query_id.store(other.last_query_id.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
                return *this;
            }
        };

        struct cell_key
        {
            int x = 0;
            int y = 0;
            int z = 0;

            bool operator==(const cell_key& other) const
            {
                return x == other.x && y == other.y && z == other.z;
            }
        };

        struct cell_key_hash
        {
            std::size_t operator()(const cell_key& key) const noexcept
            {
                std::size_t h = 0;
                auto combine = [&h](std::size_t value)
                {
                    h ^= value + 0x9e3779b9 + (h << 6) + (h >> 2);
                };
                combine(std::hash<int>{}(key.x));
                combine(std::hash<int>{}(key.y));
                combine(std::hash<int>{}(key.z));
                return h;
            }
        };

        struct occluder_cache
        {
            std::vector<occluder_part> parts;
            std::vector<occluder_part> debug_parts;
            std::unordered_map<cell_key, std::vector<std::uint32_t>, cell_key_hash> grid;
            float cell_size = 32.0f;
            bool complete = false;
            double build_time = 0.0;
        };

        struct occluder_builder
        {
            std::uintptr_t primitives_base = 0;
            std::size_t cursor = 0;
            std::size_t max_scan = 0;
            bool active = false;
            bool complete = false;
            double last_step = 0.0;
            std::size_t slots_scanned = 0;
            std::size_t null_slots = 0;
            std::size_t ignored_slots = 0;
            std::size_t append_success = 0;
            std::size_t append_fail = 0;
            std::size_t consecutive_null_slots = 0;
            bool found_non_null_slot = false;
            occluder_cache building_cache;
            std::unordered_set<std::uintptr_t> ignore_primitives;

            void reset()
            {
                primitives_base = 0;
                cursor = 0;
                max_scan = 0;
                active = false;
                complete = false;
                last_step = 0.0;
                slots_scanned = 0;
                null_slots = 0;
                ignored_slots = 0;
                append_success = 0;
                append_fail = 0;
                consecutive_null_slots = 0;
                found_non_null_slot = false;
                building_cache = occluder_cache{};
                ignore_primitives.clear();
            }

            void start(
                std::uintptr_t base,
                std::unordered_set<std::uintptr_t>&& ignore,
                float cell_size,
                std::size_t max_scan_count)
            {
                primitives_base = base;
                cursor = 0;
                max_scan = max_scan_count;
                active = base != 0 && max_scan_count > 0;
                complete = !active;
                last_step = 0.0;
                slots_scanned = 0;
                null_slots = 0;
                ignored_slots = 0;
                append_success = 0;
                append_fail = 0;
                consecutive_null_slots = 0;
                found_non_null_slot = false;
                building_cache = occluder_cache{};
                building_cache.cell_size = cell_size;
                building_cache.complete = false;
                building_cache.build_time = 0.0;
                ignore_primitives = std::move(ignore);
                if (active)
                {
                    const std::size_t reserve_count = (std::min)(max_scan_count, static_cast<std::size_t>(131072));
                    if (reserve_count > 0)
                    {
                        building_cache.parts.reserve(reserve_count);
                        building_cache.debug_parts.reserve(reserve_count);
                    }
                }
            }
        };

        double monotonic_time_seconds()
        {
            using clock = std::chrono::steady_clock;
            return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
        }

        rbx::Vector3 multiply_transpose(const DirectX::XMFLOAT3X3& m, const rbx::Vector3& v)
        {
            return rbx::Vector3(
                m._11 * v.x + m._12 * v.y + m._13 * v.z,
                m._21 * v.x + m._22 * v.y + m._23 * v.z,
                m._31 * v.x + m._32 * v.y + m._33 * v.z
            );
        }

        float get_component(const rbx::Vector3& v, int axis)
        {
            switch (axis)
            {
            case 0: return v.x;
            case 1: return v.y;
            default: return v.z;
            }
        }

        DirectX::XMFLOAT3X3 identity_rotation()
        {
            DirectX::XMFLOAT3X3 rot{};
            rot._11 = rot._22 = rot._33 = 1.0f;
            rot._12 = rot._13 = rot._21 = rot._23 = rot._31 = rot._32 = 0.0f;
            return rot;
        }

        std::optional<DirectX::XMFLOAT3X3> read_part_rotation(std::uintptr_t primitive)
        {
            if (!primitive || !roblox::offsets::base_part::cframe_rotation)
            {
                return std::nullopt;
            }

            const auto rot = memory->read<DirectX::XMFLOAT3X3>(primitive + roblox::offsets::base_part::cframe_rotation);
            const float* values = reinterpret_cast<const float*>(&rot);
            for (int i = 0; i < 9; ++i)
            {
                if (!std::isfinite(values[i]))
                {
                    return std::nullopt;
                }
            }
            return rot;
        }

        std::optional<rbx::Vector3> get_primitive_position(std::uintptr_t primitive)
        {
            if (!primitive || !roblox::offsets::base_part::position)
            {
                return std::nullopt;
            }

            const auto pos = memory->read<rbx::Vector3>(primitive + roblox::offsets::base_part::position);
            if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
            {
                return std::nullopt;
            }
            return pos;
        }

        bool has_valid_primitive_signature(std::uintptr_t primitive)
        {
            if (!primitive || !roblox::offsets::base_part::validate)
            {
                return false;
            }

            const auto validate_raw = memory->read<std::uint32_t>(primitive + roblox::offsets::base_part::validate);
            constexpr std::uint32_t k_validate_value = 0x6;

            // Accept both plain 0x6 and packed variants (e.g. 0x00060000) observed in live builds.
            return validate_raw == k_validate_value
                || (validate_raw & 0xFFu) == k_validate_value
                || ((validate_raw >> 8) & 0xFFu) == k_validate_value
                || ((validate_raw >> 16) & 0xFFu) == k_validate_value
                || ((validate_raw >> 24) & 0xFFu) == k_validate_value;
        }

        bool ray_intersects_obb(
            const rbx::Vector3& ray_origin,
            const rbx::Vector3& ray_dir,
            const occluder_part& part,
            float max_distance,
            float* out_hit_distance = nullptr)
        {
            rbx::Vector3 to_part = part.position - ray_origin;
            const float sphere_radius = std::sqrt((std::max)(part.radius_sq, 0.0f));
            const float dist_sq = to_part.LengthSquared();
            const float combined = sphere_radius + max_distance;
            if (dist_sq > combined * combined)
            {
                return false;
            }

            const DirectX::XMFLOAT3X3 rot = part.rotation;
            const rbx::Vector3 local_origin = multiply_transpose(rot, ray_origin - part.position);
            const rbx::Vector3 local_dir = multiply_transpose(rot, ray_dir);
            const rbx::Vector3 half = part.half_size;

            float tmin = -(std::numeric_limits<float>::max)();
            float tmax = (std::numeric_limits<float>::max)();
            constexpr float epsilon = 1e-6f;

            for (int axis = 0; axis < 3; ++axis)
            {
                const float origin_component = get_component(local_origin, axis);
                const float dir_component = get_component(local_dir, axis);
                const float min_bound = -get_component(half, axis);
                const float max_bound = get_component(half, axis);

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

                if (tmin > tmax || tmax < 0.0f || tmin > max_distance)
                {
                    return false;
                }
            }

            const float hit_distance = (tmin >= 0.0f) ? tmin : tmax;
            if (hit_distance < 0.0f || hit_distance > max_distance || tmin > tmax)
            {
                return false;
            }

            if (out_hit_distance)
            {
                *out_hit_distance = hit_distance;
            }
            return true;
        }

    std::optional<rbx::Vector3> get_part_position(const cache::primitive_part& part)
    {
        if (!part.instance.is_valid())
        {
            return std::nullopt;
            }
            return part.instance.get_position(part.primitive);
        }

        std::optional<rbx::Vector3> resolve_camera_position(const cache::local_player_state& local, const rbx::Matrix& view_matrix)
        {
            if (local.camera.is_valid())
            {
                const auto pos = local.camera.get_camera_position();
                if (std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z))
                {
                    return pos;
                }
            }

            rbx::Matrix view = view_matrix;
            const float det = view.Determinant();
            if (std::fabs(det) < 1e-6f)
            {
                return std::nullopt;
            }

            view.Invert();
            const rbx::Vector3 camera_pos(view._41, view._42, view._43);
            if (!std::isfinite(camera_pos.x) || !std::isfinite(camera_pos.y) || !std::isfinite(camera_pos.z))
            {
                return std::nullopt;
            }
            return camera_pos;
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

        std::size_t add_part_corners(const cache::primitive_part& part, std::vector<rbx::Vector3>& out_points, std::size_t max_points)
        {
            if (out_points.size() >= max_points)
            {
                return 0;
            }

            const auto size_opt = rbx::part::get_size(part.primitive);
            const auto transform_opt = rbx::mesh_part::get_transform(part.instance, part.primitive);
            if (!size_opt || !transform_opt)
            {
                return 0;
            }

            const rbx::Vector3 half = *size_opt * 0.5f;
            const auto& tr = *transform_opt;

            const rbx::Vector3 corners[8] = {
                rbx::Vector3( half.x,  half.y,  half.z),
                rbx::Vector3( half.x,  half.y, -half.z),
                rbx::Vector3( half.x, -half.y,  half.z),
                rbx::Vector3( half.x, -half.y, -half.z),
                rbx::Vector3(-half.x,  half.y,  half.z),
                rbx::Vector3(-half.x,  half.y, -half.z),
                rbx::Vector3(-half.x, -half.y,  half.z),
                rbx::Vector3(-half.x, -half.y, -half.z)
            };

            const std::size_t before = out_points.size();
            for (const auto& local : corners)
            {
                if (out_points.size() >= max_points)
                {
                    break;
                }
                out_points.push_back(tr.position + rotate_point(tr, local));
            }

            return out_points.size() - before;
        }

        std::size_t add_part_face_corners(const cache::primitive_part& part, const rbx::Vector3& camera_pos, std::vector<rbx::Vector3>& out_points, std::size_t max_points)
        {
            if (out_points.size() >= max_points)
            {
                return 0;
            }

            rbx::Vector3 size = part.size;
            auto valid_size = [](float v)
            {
                return std::isfinite(v) && v > 0.0f;
            };

            if (!valid_size(size.x) || !valid_size(size.y) || !valid_size(size.z))
            {
                if (const auto size_opt = rbx::part::get_size(part.primitive))
                {
                    size = *size_opt;
                }
            }

            if (!valid_size(size.x) || !valid_size(size.y) || !valid_size(size.z))
            {
                return 0;
            }

            const auto transform_opt = rbx::mesh_part::get_transform(part.instance, part.primitive);
            if (!transform_opt)
            {
                return 0;
            }

            const auto& tr = *transform_opt;
            const rbx::Vector3 half = size * 0.5f;

            rbx::Vector3 axis_x{ 1.0f, 0.0f, 0.0f };
            rbx::Vector3 axis_y{ 0.0f, 1.0f, 0.0f };
            rbx::Vector3 axis_z{ 0.0f, 0.0f, 1.0f };

            if (tr.has_rotation)
            {
                axis_x = rbx::Vector3(tr.rotation[0][0], tr.rotation[1][0], tr.rotation[2][0]);
                axis_y = rbx::Vector3(tr.rotation[0][1], tr.rotation[1][1], tr.rotation[2][1]);
                axis_z = rbx::Vector3(tr.rotation[0][2], tr.rotation[1][2], tr.rotation[2][2]);
            }

            auto normalize_or = [](const rbx::Vector3& value, const rbx::Vector3& fallback)
            {
                const float len_sq = value.LengthSquared();
                if (!std::isfinite(len_sq) || len_sq < 1e-4f)
                {
                    return fallback;
                }
                rbx::Vector3 out = value;
                out.Normalize();
                return out;
            };

            axis_x = normalize_or(axis_x, rbx::Vector3(1.0f, 0.0f, 0.0f));
            axis_y = normalize_or(axis_y, rbx::Vector3(0.0f, 1.0f, 0.0f));
            axis_z = normalize_or(axis_z, rbx::Vector3(0.0f, 0.0f, 1.0f));

            auto dot = [](const rbx::Vector3& a, const rbx::Vector3& b)
            {
                return a.x * b.x + a.y * b.y + a.z * b.z;
            };

            const rbx::Vector3 to_cam = camera_pos - tr.position;
            const float dx = dot(to_cam, axis_x);
            const float dy = dot(to_cam, axis_y);
            const float dz = dot(to_cam, axis_z);
            const float ax = std::fabs(dx);
            const float ay = std::fabs(dy);
            const float az = std::fabs(dz);

            int axis = 0;
            float sign = (dx >= 0.0f) ? 1.0f : -1.0f;
            if (ay > ax && ay >= az)
            {
                axis = 1;
                sign = (dy >= 0.0f) ? 1.0f : -1.0f;
            }
            else if (az > ax && az > ay)
            {
                axis = 2;
                sign = (dz >= 0.0f) ? 1.0f : -1.0f;
            }

            const std::size_t before = out_points.size();
            auto push_corner = [&](float sx, float sy, float sz)
            {
                if (out_points.size() >= max_points)
                {
                    return;
                }
                const rbx::Vector3 world = tr.position
                    + axis_x * (half.x * sx)
                    + axis_y * (half.y * sy)
                    + axis_z * (half.z * sz);
                out_points.push_back(world);
            };

            if (axis == 0)
            {
                push_corner(sign, -1.0f, -1.0f);
                push_corner(sign, -1.0f, 1.0f);
                push_corner(sign, 1.0f, -1.0f);
                push_corner(sign, 1.0f, 1.0f);
            }
            else if (axis == 1)
            {
                push_corner(-1.0f, sign, -1.0f);
                push_corner(-1.0f, sign, 1.0f);
                push_corner(1.0f, sign, -1.0f);
                push_corner(1.0f, sign, 1.0f);
            }
            else
            {
                push_corner(-1.0f, -1.0f, sign);
                push_corner(-1.0f, 1.0f, sign);
                push_corner(1.0f, -1.0f, sign);
                push_corner(1.0f, 1.0f, sign);
            }

            return out_points.size() - before;
        }

        void add_mesh_vertices(const cache::primitive_part& part, std::vector<rbx::Vector3>& out_points)
        {
            if (!part.instance.is_valid())
            {
                return;
            }

            const auto mesh_id = rbx::mesh_part::get_mesh_asset_id(part.instance);
            if (!mesh_id)
            {
                return;
            }

            rbx::mesh_parse::mesh_data mesh;
            if (!cache::get_mesh_data(*mesh_id, mesh))
            {
                return;
            }
            if (mesh.vertices.size() < 3)
            {
                return;
            }

            const auto transform = rbx::mesh_part::get_transform(part.instance, part.primitive);
            if (!transform)
            {
                return;
            }

            constexpr std::size_t k_max_points = 300;
            const std::size_t total_vertices = mesh.vertices.size() / 3;
            if (total_vertices == 0)
            {
                return;
            }

            const std::size_t stride = std::max<std::size_t>(1, (total_vertices + k_max_points - 1) / k_max_points);
            for (std::size_t i = 0; i + 2 < mesh.vertices.size(); i += stride * 3)
            {
                rbx::Vector3 local{ mesh.vertices[i], mesh.vertices[i + 1], mesh.vertices[i + 2] };
                out_points.push_back(transform->position + rotate_point(*transform, local));
            }
        }

        void gather_player_primitives(std::unordered_set<std::uintptr_t>& out_primitives)
        {
            out_primitives.clear();

            const auto players_snapshot = cache::players_cache->snapshot();
            out_primitives.reserve(players_snapshot ? players_snapshot->size() * 6 : 0);
            if (players_snapshot)
            {
                for (const auto& player : *players_snapshot)
                {
                    for (const auto* part : collect_parts(player.parts))
                    {
                        if (part && part->primitive)
                        {
                            out_primitives.insert(part->primitive);
                        }
                    }
                }
            }

            const auto dummy = cache::players_cache->dummy_snapshot();
            if (dummy && dummy->address != 0)
            {
                for (const auto* part : collect_parts(dummy->parts))
                {
                    if (part && part->primitive)
                    {
                        out_primitives.insert(part->primitive);
                    }
                }
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

        std::optional<std::uintptr_t> get_workspace_primitives_root()
        {
            if (!globals->workspace.is_valid())
            {
                return std::nullopt;
            }

            const auto workspace_address = globals->workspace.get_address();
            const auto p1_offset = roblox::offsets::workspace::primitives_pointer1;
            if (!p1_offset)
            {
                return std::nullopt;
            }

            try
            {
                const auto p1_address = workspace_address + p1_offset;
                const auto world = memory->read<std::uintptr_t>(p1_address);
                if (!world)
                {
                    return std::nullopt;
                }

                const auto p2_offset = roblox::offsets::workspace::primitives_pointer2;
                if (p2_offset)
                {
                    const auto p2_address = world + p2_offset;
                    const auto primitives_v2 = memory->read<std::uintptr_t>(p2_address);
                    if (primitives_v2)
                    {
                        return primitives_v2;
                    }
                }

                constexpr std::uintptr_t k_world_primitives_offset = 0x240;
                const auto primitives_v1_address = world + k_world_primitives_offset;
                const auto primitives_v1 = memory->read<std::uintptr_t>(primitives_v1_address);
                if (primitives_v1)
                {
                    return primitives_v1;
                }

                return std::nullopt;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        int cell_coord(float value, float inv_cell)
        {
            if (!std::isfinite(value) || !std::isfinite(inv_cell) || std::fabs(inv_cell) < 1e-6f)
            {
                return 0;
            }

            float scaled = value * inv_cell;
            if (!std::isfinite(scaled))
            {
                return scaled > 0.0f ? (INT_MAX / 4) : (INT_MIN / 4);
            }

            constexpr float k_cell_limit = 1'000'000.0f;
            scaled = std::clamp(scaled, -k_cell_limit, k_cell_limit);
            return static_cast<int>(std::floor(scaled));
        }

        void insert_part_into_grid(occluder_cache& cache, std::uint32_t index, const occluder_part& part)
        {
            if (cache.cell_size <= 0.0f)
            {
                return;
            }

            const float inv_cell = 1.0f / cache.cell_size;
            const DirectX::XMFLOAT3X3& rot = part.rotation;
            const float hx = part.half_size.x;
            const float hy = part.half_size.y;
            const float hz = part.half_size.z;
            rbx::Vector3 extent(
                std::fabs(rot._11) * hx + std::fabs(rot._12) * hy + std::fabs(rot._13) * hz,
                std::fabs(rot._21) * hx + std::fabs(rot._22) * hy + std::fabs(rot._23) * hz,
                std::fabs(rot._31) * hx + std::fabs(rot._32) * hy + std::fabs(rot._33) * hz);

            constexpr float k_grid_padding = 2.0f;
            extent.x += k_grid_padding;
            extent.y += k_grid_padding;
            extent.z += k_grid_padding;

            const rbx::Vector3 min = part.position - extent;
            const rbx::Vector3 max = part.position + extent;

            const int min_x = cell_coord(min.x, inv_cell);
            const int min_y = cell_coord(min.y, inv_cell);
            const int min_z = cell_coord(min.z, inv_cell);
            const int max_x = cell_coord(max.x, inv_cell);
            const int max_y = cell_coord(max.y, inv_cell);
            const int max_z = cell_coord(max.z, inv_cell);

            constexpr int k_max_cells_per_axis = 512;
            if ((max_x - min_x) > k_max_cells_per_axis ||
                (max_y - min_y) > k_max_cells_per_axis ||
                (max_z - min_z) > k_max_cells_per_axis)
            {
                return;
            }

            for (int x = min_x; x <= max_x; ++x)
            {
                for (int y = min_y; y <= max_y; ++y)
                {
                    for (int z = min_z; z <= max_z; ++z)
                    {
                        cache.grid[cell_key{ x, y, z }].push_back(index);
                    }
                }
            }
        }

        bool append_debug_primitive_from_primitive(std::uintptr_t primitive, occluder_cache& cache)
        {
            if (!primitive)
            {
                return false;
            }

            if (!has_valid_primitive_signature(primitive))
            {
                return false;
            }

            const auto size = rbx::part::get_size(primitive);
            const auto position = get_primitive_position(primitive);
            if (!size || !position)
            {
                return false;
            }

            if (!std::isfinite(size->x) || !std::isfinite(size->y) || !std::isfinite(size->z))
            {
                return false;
            }

            constexpr float k_max_size = 500.0f;
            if (size->x <= 0.0f || size->y <= 0.0f || size->z <= 0.0f ||
                size->x > k_max_size || size->y > k_max_size || size->z > k_max_size)
            {
                return false;
            }

            occluder_part part{};
            part.primitive = primitive;
            part.position = *position;
            part.half_size = rbx::Vector3(size->x * 0.5f, size->y * 0.5f, size->z * 0.5f);
            part.rotation = read_part_rotation(primitive).value_or(identity_rotation());
            part.radius_sq = part.half_size.LengthSquared();
            cache.debug_parts.push_back(part);
            return true;
        }

        bool append_occluder_from_primitive(std::uintptr_t primitive, occluder_cache& cache, const rbx::instance_t* part_instance = nullptr)
        {
            if (!primitive)
            {
                return false;
            }

            float transparency_value = 0.0f;
            bool has_transparency = false;
            if (part_instance && part_instance->is_valid())
            {
                if (const auto transparency = rbx::part::get_transparency(*part_instance, primitive))
                {
                    transparency_value = std::clamp(*transparency, 0.0f, 1.0f);
                    has_transparency = true;
                }
            }
            else if (const auto transparency = rbx::part::get_transparency(primitive))
            {
                transparency_value = std::clamp(*transparency, 0.0f, 1.0f);
                has_transparency = true;
            }

            constexpr float k_skip_transparency_threshold = 0.985f;
            if (has_transparency && transparency_value >= k_skip_transparency_threshold)
            {
                return false;
            }

            if (!has_valid_primitive_signature(primitive))
            {
                return false;
            }

            const auto size = rbx::part::get_size(primitive);
            const auto position = get_primitive_position(primitive);
            if (!size || !position)
            {
                return false;
            }

            if (!std::isfinite(size->x) || !std::isfinite(size->y) || !std::isfinite(size->z))
            {
                return false;
            }

            constexpr float k_max_size = 500.0f;
            if (size->x <= 0.0f || size->y <= 0.0f || size->z <= 0.0f ||
                size->x > k_max_size || size->y > k_max_size || size->z > k_max_size)
            {
                return false;
            }

            bool can_collide = true;
            if (roblox::offsets::base_part::can_collide && roblox::offsets::base_part::can_collide_mask)
            {
                const std::uintptr_t addr = primitive + roblox::offsets::base_part::can_collide;
                const std::uint8_t flags = memory->read<std::uint8_t>(addr);
                const std::uint8_t mask = static_cast<std::uint8_t>(roblox::offsets::base_part::can_collide_mask);
                can_collide = (flags & mask) != 0;
            }

            if (!can_collide && has_transparency && transparency_value >= 0.8f)
            {
                return false;
            }

            occluder_part part{};
            part.primitive = primitive;
            part.position = *position;
            part.half_size = rbx::Vector3(size->x * 0.5f, size->y * 0.5f, size->z * 0.5f);
            part.rotation = read_part_rotation(primitive).value_or(identity_rotation());
            part.radius_sq = part.half_size.LengthSquared();

            cache.parts.push_back(part);
            const std::uint32_t index = static_cast<std::uint32_t>(cache.parts.size() - 1);
            insert_part_into_grid(cache, index, cache.parts.back());
            return true;
        }

        void step_occluder_builder(occluder_builder& builder, std::size_t step_count)
        {
            if (!builder.active || builder.complete || builder.primitives_base == 0)
            {
                return;
            }

            constexpr std::size_t k_null_run_limit = 16384;
            std::size_t processed = 0;
            while (processed < step_count && builder.cursor < builder.max_scan)
            {
                std::uintptr_t primitive = 0;
                try
                {
                    primitive = memory->read<std::uintptr_t>(builder.primitives_base + builder.cursor * sizeof(std::uintptr_t));
                }
                catch (...)
                {
                    builder.complete = true;
                    break;
                }

                ++builder.cursor;
                ++processed;
                ++builder.slots_scanned;
                if (!primitive)
                {
                    ++builder.null_slots;
                    ++builder.consecutive_null_slots;
                    if (builder.found_non_null_slot && builder.consecutive_null_slots >= k_null_run_limit)
                    {
                        builder.complete = true;
                        break;
                    }
                    continue;
                }

                builder.found_non_null_slot = true;
                builder.consecutive_null_slots = 0;

                if (builder.ignore_primitives.find(primitive) == builder.ignore_primitives.end())
                {
                    append_debug_primitive_from_primitive(primitive, builder.building_cache);
                    if (append_occluder_from_primitive(primitive, builder.building_cache))
                    {
                        ++builder.append_success;
                    }
                    else
                    {
                        ++builder.append_fail;
                    }
                }
                else
                {
                    ++builder.ignored_slots;
                }
            }

            if (builder.cursor >= builder.max_scan)
            {
                builder.complete = true;
            }

            if (builder.complete)
            {
                builder.active = false;
                builder.building_cache.complete = true;
                builder.building_cache.build_time = monotonic_time_seconds();
            }
        }

        std::atomic<std::shared_ptr<occluder_cache>>& occluder_cache_storage()
        {
            static std::atomic<std::shared_ptr<occluder_cache>> cached_parts{ std::make_shared<occluder_cache>() };
            return cached_parts;
        }

        occluder_builder& occluder_builder_storage()
        {
            static occluder_builder builder;
            return builder;
        }

        std::mutex& occluder_builder_mutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::shared_ptr<occluder_cache> get_occluder_cache()
        {
            constexpr float k_cell_size = 32.0f;
            constexpr std::size_t k_max_scan = 524288;
            constexpr double refresh_interval = 4.0;
            constexpr double step_interval = 0.02;

            auto& cache_ref = occluder_cache_storage();
            auto cache_ptr = cache_ref.load(std::memory_order_acquire);

            const double now = monotonic_time_seconds();
            const bool cache_empty = !cache_ptr || cache_ptr->parts.empty();
            const bool cache_stale = cache_ptr && cache_ptr->complete && (now - cache_ptr->build_time) > refresh_interval;

            const auto primitives_base_opt = get_workspace_primitives_root();
            const std::uintptr_t primitives_base = primitives_base_opt.has_value() ? *primitives_base_opt : 0;

            auto& builder = occluder_builder_storage();

            {
                std::lock_guard<std::mutex> lock(occluder_builder_mutex());

                if (builder.active && builder.primitives_base != primitives_base)
                {
                    builder.reset();
                }

                if ((cache_empty || cache_stale) && !builder.active && primitives_base != 0)
                {
                    std::unordered_set<std::uintptr_t> ignore;
                    gather_player_primitives(ignore);
                    builder.start(primitives_base, std::move(ignore), k_cell_size, k_max_scan);
                }

                if (builder.active)
                {
                    if (builder.last_step <= 0.0 || (now - builder.last_step) >= step_interval)
                    {
                        const float fps = g_overlay_fps.load(std::memory_order_relaxed);
                        const std::size_t scan_chunk =
                            (fps >= 120.0f) ? 2048 :
                            (fps >= 90.0f) ? 1536 :
                            (fps >= 70.0f) ? 1024 :
                            (fps >= 55.0f) ? 768 : 512;
                        builder.last_step = now;
                        step_occluder_builder(builder, scan_chunk);
                    }

                    if (builder.complete)
                    {
                        auto new_cache = std::make_shared<occluder_cache>(std::move(builder.building_cache));
                        cache_ref.store(new_cache, std::memory_order_release);
                        builder.reset();
                    }
                }
            }

            return cache_ref.load(std::memory_order_acquire);
        }

        bool line_of_sight_clear(const rbx::Vector3& from, const rbx::Vector3& to, const occluder_cache& cache, const std::unordered_set<std::uintptr_t>& ignore)
        {
            if (!std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(from.z) ||
                !std::isfinite(to.x) || !std::isfinite(to.y) || !std::isfinite(to.z))
            {
                return true;
            }

            rbx::Vector3 delta = to - from;
            float distance = delta.Length();
            if (!std::isfinite(distance) || distance <= 1e-5f)
            {
                return true;
            }

            delta /= distance;
            constexpr float k_hit_start_tolerance = 0.15f;
            constexpr float k_hit_end_tolerance = 0.55f;

            if (cache.parts.empty())
            {
                return true;
            }

            if (cache.cell_size <= 0.0f || cache.grid.empty())
            {
                for (const auto& part : cache.parts)
                {
                    if (part.primitive && ignore.find(part.primitive) != ignore.end())
                    {
                        continue;
                    }

                    float hit_distance = 0.0f;
                    if (ray_intersects_obb(from, delta, part, distance, &hit_distance))
                    {
                        if (hit_distance <= k_hit_start_tolerance)
                        {
                            continue;
                        }
                        if ((distance - hit_distance) <= k_hit_end_tolerance)
                        {
                            continue;
                        }
                        return false;
                    }
                }

                return true;
            }

            static std::atomic<std::uint64_t> query_counter{ 1 };
            const std::uint64_t query_id = query_counter.fetch_add(1, std::memory_order_relaxed) + 1;

            const float cell_size = cache.cell_size;
            if (!std::isfinite(cell_size) || cell_size <= 1e-6f)
            {
                return true;
            }
            const float inv_cell = 1.0f / cell_size;

            int cell_x = cell_coord(from.x, inv_cell);
            int cell_y = cell_coord(from.y, inv_cell);
            int cell_z = cell_coord(from.z, inv_cell);

            const int end_x = cell_coord(to.x, inv_cell);
            const int end_y = cell_coord(to.y, inv_cell);
            const int end_z = cell_coord(to.z, inv_cell);

            int step_x = 0;
            int step_y = 0;
            int step_z = 0;
            float t_max_x = (std::numeric_limits<float>::infinity)();
            float t_max_y = (std::numeric_limits<float>::infinity)();
            float t_max_z = (std::numeric_limits<float>::infinity)();
            float t_delta_x = (std::numeric_limits<float>::infinity)();
            float t_delta_y = (std::numeric_limits<float>::infinity)();
            float t_delta_z = (std::numeric_limits<float>::infinity)();

            auto setup_axis = [&](float origin, float dir, int cell, int& step, float& t_max, float& t_delta)
            {
                if (std::fabs(dir) < 1e-6f)
                {
                    step = 0;
                    t_max = (std::numeric_limits<float>::infinity)();
                    t_delta = (std::numeric_limits<float>::infinity)();
                    return;
                }

                if (dir > 0.0f)
                {
                    step = 1;
                    const float next_boundary = (static_cast<float>(cell) + 1.0f) * cell_size;
                    t_max = (next_boundary - origin) / dir;
                }
                else
                {
                    step = -1;
                    const float next_boundary = static_cast<float>(cell) * cell_size;
                    t_max = (next_boundary - origin) / dir;
                }

                t_delta = cell_size / std::fabs(dir);
            };

            setup_axis(from.x, delta.x, cell_x, step_x, t_max_x, t_delta_x);
            setup_axis(from.y, delta.y, cell_y, step_y, t_max_y, t_delta_y);
            setup_axis(from.z, delta.z, cell_z, step_z, t_max_z, t_delta_z);

            const int max_steps = 1 + std::abs(end_x - cell_x) + std::abs(end_y - cell_y) + std::abs(end_z - cell_z);
            float t = 0.0f;

            for (int step = 0; step <= max_steps; ++step)
            {
                auto it = cache.grid.find(cell_key{ cell_x, cell_y, cell_z });
                if (it != cache.grid.end())
                {
                    for (std::uint32_t index : it->second)
                    {
                        if (index >= cache.parts.size())
                        {
                            continue;
                        }

                        auto& part = cache.parts[index];
                        if (part.primitive && ignore.find(part.primitive) != ignore.end())
                        {
                            continue;
                        }

                        const std::uint64_t last_id = part.last_query_id.load(std::memory_order_relaxed);
                        if (last_id == query_id)
                        {
                            continue;
                        }
                        part.last_query_id.store(query_id, std::memory_order_relaxed);

                        float hit_distance = 0.0f;
                        if (ray_intersects_obb(from, delta, part, distance, &hit_distance))
                        {
                            if (hit_distance <= k_hit_start_tolerance)
                            {
                                continue;
                            }
                            if ((distance - hit_distance) <= k_hit_end_tolerance)
                            {
                                continue;
                            }
                            return false;
                        }
                    }
                }

                if (cell_x == end_x && cell_y == end_y && cell_z == end_z)
                {
                    break;
                }

                if (t_max_x < t_max_y)
                {
                    if (t_max_x < t_max_z)
                    {
                        cell_x += step_x;
                        t = t_max_x;
                        t_max_x += t_delta_x;
                    }
                    else
                    {
                        cell_z += step_z;
                        t = t_max_z;
                        t_max_z += t_delta_z;
                    }
                }
                else
                {
                    if (t_max_y < t_max_z)
                    {
                        cell_y += step_y;
                        t = t_max_y;
                        t_max_y += t_delta_y;
                    }
                    else
                    {
                        cell_z += step_z;
                        t = t_max_z;
                        t_max_z += t_delta_z;
                    }
                }

                if (t > distance)
                {
                    break;
                }
            }

            return true;
        }

        void add_parts_to_ignore(const cache::character_parts& parts, std::unordered_set<std::uintptr_t>& ignore)
        {
            const auto collected = collect_parts(parts);
            ignore.reserve(ignore.size() + collected.size());
            for (const auto* part : collected)
            {
                if (part && part->primitive)
                {
                    ignore.insert(part->primitive);
                }
            }
        }

        void add_character_instance_primitives_to_ignore(const rbx::instance_t& character, std::unordered_set<std::uintptr_t>& ignore)
        {
            if (!character.is_valid())
            {
                return;
            }

            constexpr std::size_t k_max_nodes = 2048;
            std::vector<rbx::instance_t> stack;
            stack.reserve(256);
            stack.push_back(character);

            std::size_t processed = 0;
            while (!stack.empty() && processed < k_max_nodes)
            {
                rbx::instance_t entry = stack.back();
                stack.pop_back();
                ++processed;

                const auto primitive = rbx::part::get_primitive(entry);
                if (primitive)
                {
                    ignore.insert(primitive);
                }

                auto children = get_children_safely(entry, 256);
                for (const auto& child : children)
                {
                    if (stack.size() >= k_max_nodes)
                    {
                        break;
                    }
                    stack.push_back(child);
                }
            }
        }

        bool is_player_visible_impl(const cache::player_state& player, const cache::local_player_state& local, const rbx::Vector3& camera_pos, const occluder_cache& occluders, float& out_coverage, std::size_t max_tests)
        {
            if (occluders.parts.empty())
            {
                out_coverage = 1.0f;
                return true;
            }

            std::unordered_set<std::uintptr_t> ignore{};
            add_parts_to_ignore(player.parts, ignore);
            add_parts_to_ignore(local.parts, ignore);
            add_character_instance_primitives_to_ignore(player.character, ignore);
            add_character_instance_primitives_to_ignore(local.character, ignore);

            constexpr std::size_t k_max_sample_points = 64;
            std::vector<rbx::Vector3> sample_points;
            sample_points.reserve(k_max_sample_points);

            bool added_head_points = false;
            bool added_root_point = false;
            bool added_torso_point = false;
            if (const auto head = get_part_position(player.parts.head))
            {
                sample_points.push_back(*head);
                added_head_points = true;
            }
            if (sample_points.size() < k_max_sample_points)
            {
                if (add_part_corners(player.parts.head, sample_points, k_max_sample_points) > 0)
                {
                    added_head_points = true;
                }
            }
            if (sample_points.size() < k_max_sample_points)
            {
                if (const auto root = get_part_position(player.parts.humanoid_root_part))
                {
                    sample_points.push_back(*root);
                    added_root_point = true;
                }
            }
            if (sample_points.size() < k_max_sample_points)
            {
                if (player.parts.is_r15)
                {
                    if (const auto torso = get_part_position(player.parts.upper_torso))
                    {
                        sample_points.push_back(*torso);
                        added_torso_point = true;
                    }
                }
                else
                {
                    if (const auto torso = get_part_position(player.parts.torso))
                    {
                        sample_points.push_back(*torso);
                        added_torso_point = true;
                    }
                }
            }

            auto is_limb_part = [&](const cache::primitive_part* part)
            {
                if (!part)
                {
                    return false;
                }

                return part == &player.parts.left_arm ||
                    part == &player.parts.right_arm ||
                    part == &player.parts.left_leg ||
                    part == &player.parts.right_leg ||
                    part == &player.parts.left_upper_arm ||
                    part == &player.parts.left_lower_arm ||
                    part == &player.parts.left_hand ||
                    part == &player.parts.right_upper_arm ||
                    part == &player.parts.right_lower_arm ||
                    part == &player.parts.right_hand ||
                    part == &player.parts.left_upper_leg ||
                    part == &player.parts.left_lower_leg ||
                    part == &player.parts.left_foot ||
                    part == &player.parts.right_upper_leg ||
                    part == &player.parts.right_lower_leg ||
                    part == &player.parts.right_foot;
            };

            for (const auto* part : collect_parts(player.parts))
            {
                if (!part || sample_points.size() >= k_max_sample_points)
                {
                    break;
                }
                if (added_head_points && part == &player.parts.head)
                {
                    continue;
                }
                if (added_root_point && part == &player.parts.humanoid_root_part)
                {
                    continue;
                }
                if (added_torso_point &&
                    (part == &player.parts.torso || part == &player.parts.upper_torso || part == &player.parts.lower_torso))
                {
                    continue;
                }

                if (is_limb_part(part))
                {
                    if (add_part_face_corners(*part, camera_pos, sample_points, k_max_sample_points) > 0)
                    {
                        continue;
                    }
                }

                if (const auto pos = get_part_position(*part))
                {
                    sample_points.push_back(*pos);
                }
            }

            if (sample_points.empty())
            {
                return true;
            }

            // Fast path: if the primary sample (head/upper body) is clear, treat as visible.
            if (line_of_sight_clear(camera_pos, sample_points.front(), occluders, ignore))
            {
                out_coverage = 1.0f;
                return true;
            }

            const std::size_t total_budget = (max_tests > 0)
                ? (std::min)(max_tests, sample_points.size())
                : sample_points.size();
            if (total_budget == 0)
            {
                out_coverage = 0.0f;
                return false;
            }

            int clear_count = 0;
            const int min_clear_samples = (total_budget <= 6) ? 1 : 2;
            int required_clear = static_cast<int>(std::ceil(static_cast<float>(total_budget) * 0.10f));
            required_clear = (std::max)(required_clear, min_clear_samples);
            if (total_budget >= 24)
            {
                required_clear = (std::min)(required_clear, 6);
            }
            required_clear = std::clamp(required_clear, 1, static_cast<int>(total_budget));

            std::size_t tested = 0;
            for (std::size_t i = 0; i < total_budget; ++i)
            {
                ++tested;
                if (line_of_sight_clear(camera_pos, sample_points[i], occluders, ignore))
                {
                    ++clear_count;
                    if (clear_count >= required_clear)
                    {
                        out_coverage = static_cast<float>(clear_count) / static_cast<float>(tested);
                        return true;
                    }
                }

                const std::size_t remaining = total_budget - tested;
                if (clear_count + static_cast<int>(remaining) < required_clear)
                {
                    break;
                }
            }

            out_coverage = static_cast<float>(clear_count) / static_cast<float>((tested > 0) ? tested : total_budget);
            return clear_count >= required_clear;
        }
    }

    bool is_raycast_engine_enabled()
    {
        return features && features->enable_raycast_engine;
    }

    bool is_any_occluded_check_enabled()
    {
        if (!features)
        {
            return false;
        }

        return features->enable_visibility_check
            || features->aimbot_visibility_check
            || features->triggerbot_visibility_check
            || features->free_aim_visibility_check;
    }

    bool should_show_raycast_engine_warning()
    {
        return !is_raycast_engine_enabled() && is_any_occluded_check_enabled();
    }

    bool can_run_visibility_check(bool feature_visibility_check_enabled)
    {
        return feature_visibility_check_enabled && is_raycast_engine_enabled();
    }

    visibility_result is_player_visible(const cache::player_state& player, const cache::local_player_state& local, const rbx::Matrix& view_matrix)
    {
        if (!is_raycast_engine_enabled())
        {
            return visibility_result{};
        }

        const auto camera_pos = resolve_camera_position(local, view_matrix);
        if (!camera_pos)
        {
            return visibility_result{};
        }

        struct smoothed_state
        {
            float smoothed = 1.0f;
            bool visible = true;
            double last_time = 0.0;
            double last_query_time = 0.0;
            int visible_confirmation = 0;
            int occluded_confirmation = 0;
            visibility_result last_result{};
        };
        thread_local std::unordered_map<std::uintptr_t, smoothed_state> smooth_map;

        smoothed_state& state = smooth_map[player.address];
        const double now = monotonic_time_seconds();
        constexpr double k_query_interval = 0.015;
        if (state.last_query_time != 0.0 && (now - state.last_query_time) < k_query_interval)
        {
            return state.last_result;
        }

        const auto occluders = get_occluder_cache();
        if (!occluders)
        {
            return visibility_result{};
        }

        constexpr std::size_t k_visible_max_tests = 64;
        constexpr std::size_t k_occluded_max_tests = 64;
        const bool was_visible = (state.last_time == 0.0) ? true : state.visible;
        const std::size_t max_tests = was_visible ? k_visible_max_tests : k_occluded_max_tests;

        float coverage = 1.0f;
        const bool visible_raw = is_player_visible_impl(player, local, *camera_pos, *occluders, coverage, max_tests);
        const float alpha = (state.last_time == 0.0) ? 1.0f : (visible_raw ? 0.4f : 0.28f);
        state.smoothed = state.smoothed * (1.0f - alpha) + coverage * alpha;

        if (visible_raw)
        {
            state.visible_confirmation = (std::min)(state.visible_confirmation + 1, 16);
            state.occluded_confirmation = 0;
        }
        else
        {
            state.occluded_confirmation = (std::min)(state.occluded_confirmation + 1, 16);
            state.visible_confirmation = 0;
        }

        if (visible_raw)
        {
            state.smoothed = (std::max)(state.smoothed, coverage);
        }

        constexpr float on_threshold = 0.48f;
        constexpr float off_threshold = 0.22f;
        constexpr int k_visible_confirm_ticks = 2;
        constexpr int k_occluded_confirm_ticks = 4;
        if (state.visible)
        {
            if (!visible_raw &&
                state.smoothed <= off_threshold &&
                state.occluded_confirmation >= k_occluded_confirm_ticks)
            {
                state.visible = false;
            }
        }
        else
        {
            if ((state.smoothed >= on_threshold && state.visible_confirmation >= k_visible_confirm_ticks) ||
                state.visible_confirmation >= (k_visible_confirm_ticks + 1))
            {
                state.visible = true;
            }
        }

        if (state.last_time == 0.0)
        {
            state.visible = visible_raw;
            state.smoothed = coverage;
            state.visible_confirmation = visible_raw ? 1 : 0;
            state.occluded_confirmation = visible_raw ? 0 : 1;
        }

        state.last_time = now;
        state.last_query_time = now;
        state.last_result = visibility_result{ state.visible, state.smoothed };
        return state.last_result;
    }

    void get_debug_occluder_primitives(std::vector<debug_occluder_primitive>& out_primitives, std::size_t max_count)
    {
        out_primitives.clear();

        auto cache_ptr = occluder_cache_storage().load(std::memory_order_acquire);
        if (!cache_ptr)
        {
            return;
        }

        const std::vector<occluder_part>& source_parts = cache_ptr->debug_parts.empty()
            ? cache_ptr->parts
            : cache_ptr->debug_parts;
        if (source_parts.empty())
        {
            return;
        }

        if (max_count == 0 || source_parts.size() <= max_count)
        {
            const std::size_t count = source_parts.size();
            out_primitives.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto& src = source_parts[i];
                debug_occluder_primitive dst{};
                dst.primitive = src.primitive;
                dst.position = src.position;
                dst.half_size = src.half_size;
                dst.rotation[0] = src.rotation._11;
                dst.rotation[1] = src.rotation._12;
                dst.rotation[2] = src.rotation._13;
                dst.rotation[3] = src.rotation._21;
                dst.rotation[4] = src.rotation._22;
                dst.rotation[5] = src.rotation._23;
                dst.rotation[6] = src.rotation._31;
                dst.rotation[7] = src.rotation._32;
                dst.rotation[8] = src.rotation._33;
                out_primitives.push_back(dst);
            }
            return;
        }

        const std::size_t stride = (source_parts.size() + max_count - 1) / max_count;
        out_primitives.reserve(max_count);
        for (std::size_t i = 0; i < source_parts.size() && out_primitives.size() < max_count; i += stride)
        {
            const auto& src = source_parts[i];
            debug_occluder_primitive dst{};
            dst.primitive = src.primitive;
            dst.position = src.position;
            dst.half_size = src.half_size;
            dst.rotation[0] = src.rotation._11;
            dst.rotation[1] = src.rotation._12;
            dst.rotation[2] = src.rotation._13;
            dst.rotation[3] = src.rotation._21;
            dst.rotation[4] = src.rotation._22;
            dst.rotation[5] = src.rotation._23;
            dst.rotation[6] = src.rotation._31;
            dst.rotation[7] = src.rotation._32;
            dst.rotation[8] = src.rotation._33;
            out_primitives.push_back(dst);
        }
    }


    void reset_occluder_cache()
    {
        auto& cache_ref = occluder_cache_storage();
        cache_ref.store(std::make_shared<occluder_cache>(), std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(occluder_builder_mutex());
            occluder_builder_storage().reset();
        }
    }
}
