#include "sdk/mesh_part.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <array>
#include <string_view>
#include <stdexcept>

#include "memory/memory.h"
#include "sdk/offsets.h"
#include "sdk/player.h"
#include "sdk/part.h"
#include "utils/logger.h"

namespace rbx::mesh_part
{
    namespace
    {
        std::optional<std::uint64_t> extract_digits(const std::string& value)
        {
            std::string digits;
            digits.reserve(value.size());
            for (char c : value)
            {
                if (std::isdigit(static_cast<unsigned char>(c)))
                {
                    digits.push_back(c);
                }
            }

            if (digits.empty())
            {
                return std::nullopt;
            }

            try
            {
                return std::stoull(digits, nullptr, 10);
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        bool is_special_mesh_class_name(const std::string& class_name)
        {
            if (class_name.empty())
            {
                return false;
            }

            std::string lowered = class_name;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            return lowered == "specialmesh" || lowered == "filemesh" || lowered == "headmesh";
        }

        std::optional<std::string> read_content_id(std::uintptr_t object_ptr, std::uintptr_t field_offset)
        {
            if (object_ptr == 0 || field_offset == 0)
            {
                return std::nullopt;
            }

            const auto mesh_field = object_ptr + field_offset;
            const auto content_ptr = memory->read<std::uintptr_t>(mesh_field);

            std::string value;
            if (content_ptr)
            {
                char buffer[512] = {};
                if (memory->read_raw(buffer, content_ptr, sizeof(buffer) - 1))
                {
                    value = buffer;
                }
            }

            if (value.empty())
            {
                value = memory->read_string(mesh_field);
            }

            if (value.empty() || value == "Unknown")
            {
                return std::nullopt;
            }

            auto pos = value.find("id=");
            if (pos != std::string::npos)
            {
                value = value.substr(pos + 3);
            }

            return value;
        }

        std::optional<std::string> get_mesh_part_mesh_id(const rbx::instance_t& instance)
        {
            if (!instance.is_valid() || !roblox::offsets::mesh_part::mesh_id)
            {
                return std::nullopt;
            }

            return read_content_id(instance.get_address(), roblox::offsets::mesh_part::mesh_id);
        }

        std::optional<std::string> get_special_mesh_id(const rbx::instance_t& part_instance)
        {
            if (!part_instance.is_valid())
            {
                return std::nullopt;
            }

            const auto children = part_instance.get_children();
            for (const auto& child : children)
            {
                if (!child.is_valid())
                {
                    continue;
                }

                if (!is_special_mesh_class_name(child.get_class_name()))
                {
                    continue;
                }

                const auto special_mesh_offset = roblox::offsets::mesh_part::special_mesh_id;
                std::optional<std::string> id = special_mesh_offset
                    ? read_content_id(child.get_address(), special_mesh_offset)
                    : std::nullopt;

                if (!id && roblox::offsets::mesh_part::mesh_id &&
                    roblox::offsets::mesh_part::mesh_id != special_mesh_offset)
                {
                    id = read_content_id(child.get_address(), roblox::offsets::mesh_part::mesh_id);
                }

                if (id)
                {
                    return id;
                }
            }

            return std::nullopt;
        }
    }

    std::optional<std::string> get_mesh_id(const rbx::instance_t& instance)
    {
        if (!instance.is_valid())
        {
            return std::nullopt;
        }

        const std::string class_name = instance.get_class_name();
        if (class_name == "MeshPart")
        {
            if (const auto id = get_mesh_part_mesh_id(instance))
            {
                return id;
            }
        }

        if (class_name == "Part")
        {
            if (const auto id = get_special_mesh_id(instance))
            {
                return id;
            }
        }

        return std::nullopt;
    }

    std::optional<std::uint64_t> get_mesh_asset_id(const rbx::instance_t& instance)
    {
        const auto id = get_mesh_id(instance);
        if (!id)
        {
            return std::nullopt;
        }

        return extract_digits(*id);
    }

    std::optional<transform> get_transform(const rbx::instance_t& instance, std::uintptr_t primitive)
    {
        if (!instance.is_valid())
        {
            return std::nullopt;
        }

        if (!primitive)
        {
            primitive = rbx::part::get_primitive(instance);
        }

        const auto position = instance.get_position(primitive);
        if (!position)
        {
            return std::nullopt;
        }

        transform result{};
        result.position = *position;

        if (primitive && roblox::offsets::base_part::cframe_rotation)
        {
            struct rot_t
            {
                float m[3][3];
            };

            const rot_t rot = memory->read<rot_t>(primitive + roblox::offsets::base_part::cframe_rotation);
            bool finite = true;
            for (const auto& row : rot.m)
            {
                for (float v : row)
                {
                    if (!std::isfinite(v))
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
                std::memcpy(result.rotation, rot.m, sizeof(result.rotation));
                result.has_rotation = true;
            }
        }

        return result;
    }
}

namespace rbx::mesh_parse
{
    namespace
    {
        struct file_mesh_header_v2
        {
            std::uint16_t sizeof_header{};
            std::uint8_t sizeof_vertex{};
            std::uint8_t sizeof_face{};
            std::uint32_t num_verts{};
            std::uint32_t num_faces{};
        };

        struct file_mesh_vertex36
        {
            float px{}, py{}, pz{};
            float nx{}, ny{}, nz{};
            float tu{}, tv{};
            std::int8_t tx{}, ty{}, tz{}, ts{};
        };

        struct file_mesh_vertex40
        {
            float px{}, py{}, pz{};
            float nx{}, ny{}, nz{};
            float tu{}, tv{};
            std::int8_t tx{}, ty{}, tz{}, ts{};
            std::uint8_t r{}, g{}, b{}, a{};
        };

        struct file_mesh_face
        {
            std::uint32_t a{}, b{}, c{};
        };

        std::string trim_copy(const std::string& value)
        {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return {};
            }
            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1u);
        }

        template <typename T>
        T read_value(std::ifstream& file)
        {
            T v{};
            file.read(reinterpret_cast<char*>(&v), sizeof(T));
            if (!file)
            {
                throw std::runtime_error("unexpected end of file");
            }
            return v;
        }

        void skip_bytes(std::ifstream& file, std::streamoff count)
        {
            file.seekg(count, std::ios::cur);
            if (!file)
            {
                throw std::runtime_error("failed to skip bytes");
            }
        }

        mesh_data load_version1(std::ifstream& file, const std::string& path, bool scale_positions, std::string_view version_tag, bool debug)
        {
            mesh_data mesh;

            std::string faces_line;
            if (!std::getline(file, faces_line))
            {
                return mesh;
            }
            if (!faces_line.empty() && faces_line.back() == '\r')
            {
                faces_line.pop_back();
            }

            faces_line = trim_copy(faces_line);
            const auto digit_pos = faces_line.find_first_of("0123456789");
            if (digit_pos == std::string::npos)
            {
                return mesh;
            }

            std::uint32_t num_faces = 0;
            {
                std::istringstream ss(faces_line.substr(digit_pos));
                ss >> num_faces;
            }
            if (num_faces == 0 || num_faces > 500'000u)
            {
                return mesh;
            }

            std::string data_line;
            if (!std::getline(file, data_line))
            {
                return mesh;
            }
            if (!data_line.empty() && data_line.back() == '\r')
            {
                data_line.pop_back();
            }

            const std::size_t expected_vectors = static_cast<std::size_t>(num_faces) * 9u;
            std::vector<std::array<float, 3u>> vectors;
            vectors.reserve(expected_vectors);

            std::size_t pos = 0;
            while (pos < data_line.size())
            {
                if (data_line[pos] != '[')
                {
                    ++pos;
                    continue;
                }
                const std::size_t end = data_line.find(']', pos);
                if (end == std::string::npos)
                {
                    return mesh_data{};
                }
                std::string values = data_line.substr(pos + 1u, end - pos - 1u);
                for (char& ch : values)
                {
                    if (ch == ',')
                    {
                        ch = ' ';
                    }
                }
                std::istringstream vs(values);
                std::array<float, 3u> triple{};
                if (!(vs >> triple[0] >> triple[1] >> triple[2]))
                {
                    return mesh_data{};
                }
                vectors.emplace_back(triple);
                pos = end + 1u;
            }

            if (vectors.size() != expected_vectors)
            {
                return mesh;
            }

            mesh.vertices.reserve(static_cast<std::size_t>(num_faces) * 9u);
            mesh.indices.reserve(static_cast<std::size_t>(num_faces) * 3u);

            for (std::uint32_t face_idx = 0; face_idx < num_faces; ++face_idx)
            {
                const std::size_t face_base = static_cast<std::size_t>(face_idx) * 9u;

                for (std::uint32_t vertex = 0; vertex < 3u; ++vertex)
                {
                    const std::size_t position_index = face_base + static_cast<std::size_t>(vertex) * 3u;
                    std::array<float, 3u> position = vectors[position_index];

                    if (scale_positions)
                    {
                        position[0] *= 0.5f;
                        position[1] *= 0.5f;
                        position[2] *= 0.5f;
                    }

                    const std::uint32_t vertex_index = static_cast<std::uint32_t>(mesh.vertices.size() / 3u);
                    mesh.vertices.insert(mesh.vertices.end(), { position[0], position[1], position[2] });
                    mesh.indices.push_back(vertex_index);
                }
            }

            if (debug)
            {
                const std::uint32_t vertex_count = static_cast<std::uint32_t>(mesh.vertices.size() / 3u);
                logger_core::log_info("loaded v1 mesh {} ({} verts, {} faces, version {})",
                    path, vertex_count, mesh.indices.size() / 3u, version_tag);
            }

            return mesh;
        }

        mesh_data load_version2(std::ifstream& file, const std::string& path, bool debug)
        {
            mesh_data mesh;

            file_mesh_header_v2 header{};
            file.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (!file || header.num_verts == 0 || header.num_faces == 0 ||
                header.num_verts > 1'000'000u || header.num_faces > 1'000'000u)
            {
                return mesh;
            }

            mesh.vertices.reserve(header.num_verts * 3u);
            mesh.indices.reserve(header.num_faces * 3u);

            if (header.sizeof_vertex == sizeof(file_mesh_vertex36))
            {
                for (std::uint32_t i = 0; i < header.num_verts; ++i)
                {
                    file_mesh_vertex36 v{};
                    file.read(reinterpret_cast<char*>(&v), sizeof(v));
                    if (!file) return mesh_data{};
                    mesh.vertices.insert(mesh.vertices.end(), { v.px, v.py, v.pz });
                }
            }
            else if (header.sizeof_vertex == sizeof(file_mesh_vertex40))
            {
                for (std::uint32_t i = 0; i < header.num_verts; ++i)
                {
                    file_mesh_vertex40 v{};
                    file.read(reinterpret_cast<char*>(&v), sizeof(v));
                    if (!file) return mesh_data{};
                    mesh.vertices.insert(mesh.vertices.end(), { v.px, v.py, v.pz });
                }
            }
            else
            {
                return mesh;
            }

            for (std::uint32_t i = 0; i < header.num_faces; ++i)
            {
                file_mesh_face f{};
                file.read(reinterpret_cast<char*>(&f), sizeof(f));
                if (!file) return mesh_data{};
                mesh.indices.insert(mesh.indices.end(), { f.a, f.b, f.c });
            }

            if (debug)
            {
                logger_core::log_info("loaded v2 mesh {} ({} verts, {} faces)",
                    path, header.num_verts, header.num_faces);
            }
            return mesh;
        }

        mesh_data load_version3(std::ifstream& file, const std::string& path, bool debug)
        {
            mesh_data mesh;

            struct file_mesh_header_v3
            {
                std::uint16_t sizeof_header{};
                std::uint8_t sizeof_vertex{};
                std::uint8_t sizeof_face{};
                std::uint16_t sizeof_lod_offset{};
                std::uint16_t num_lod_offsets{};
                std::uint32_t num_verts{};
                std::uint32_t num_faces{};
            } header{};

            file.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (!file || header.num_verts == 0 || header.num_faces == 0 ||
                header.num_verts > 1'000'000u || header.num_faces > 1'000'000u)
            {
                return mesh;
            }

            mesh.vertices.reserve(static_cast<std::size_t>(header.num_verts) * 3u);

            auto read_vertices = [&](auto read_fn) -> bool
            {
                for (std::uint32_t i = 0; i < header.num_verts; ++i)
                {
                    if (!read_fn()) return false;
                }
                return true;
            };

            bool vertices_ok = false;
            if (header.sizeof_vertex == sizeof(file_mesh_vertex36))
            {
                file_mesh_vertex36 v{};
                vertices_ok = read_vertices([&]()
                {
                    file.read(reinterpret_cast<char*>(&v), sizeof(v));
                    if (!file) return false;
                    mesh.vertices.insert(mesh.vertices.end(), { v.px, v.py, v.pz });
                    return true;
                });
            }
            else if (header.sizeof_vertex == sizeof(file_mesh_vertex40))
            {
                file_mesh_vertex40 v{};
                vertices_ok = read_vertices([&]()
                {
                    file.read(reinterpret_cast<char*>(&v), sizeof(v));
                    if (!file) return false;
                    mesh.vertices.insert(mesh.vertices.end(), { v.px, v.py, v.pz });
                    return true;
                });
            }
            else
            {
                return mesh;
            }

            if (!vertices_ok)
            {
                return mesh_data{};
            }

            std::vector<file_mesh_face> faces(header.num_faces);
            if (header.sizeof_face == sizeof(file_mesh_face))
            {
                file.read(reinterpret_cast<char*>(faces.data()), sizeof(file_mesh_face) * header.num_faces);
                if (!file) return mesh_data{};
            }
            else
            {
                std::vector<std::uint8_t> buffer(header.sizeof_face);
                for (std::uint32_t i = 0; i < header.num_faces; ++i)
                {
                    file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
                    if (!file) return mesh_data{};
                    file_mesh_face f{};
                    const std::size_t copy_bytes = (std::min)(buffer.size(), sizeof(file_mesh_face));
                    std::memcpy(&f, buffer.data(), copy_bytes);
                    faces[i] = f;
                }
            }

            std::vector<std::uint32_t> lod_offsets(header.num_lod_offsets);
            for (std::uint16_t i = 0; i < header.num_lod_offsets; ++i)
            {
                lod_offsets[i] = read_value<std::uint32_t>(file);
            }

            std::size_t start_face = 0;
            std::size_t end_face = faces.size();
            if (!lod_offsets.empty())
            {
                start_face = (std::min)(static_cast<std::size_t>(lod_offsets[0]), faces.size());
                if (lod_offsets.size() >= 2)
                {
                    end_face = (std::min)(static_cast<std::size_t>(lod_offsets[1]), faces.size());
                }
            }
            if (end_face <= start_face)
            {
                start_face = 0;
                end_face = faces.size();
            }

            mesh.indices.reserve((end_face - start_face) * 3u);
            for (std::size_t i = start_face; i < end_face; ++i)
            {
                const auto& f = faces[i];
                mesh.indices.insert(mesh.indices.end(), { f.a, f.b, f.c });
            }

            if (debug)
            {
                logger_core::log_info("loaded v3 mesh {} ({} verts, {} faces)",
                    path, header.num_verts, end_face - start_face);
            }
            return mesh;
        }

        mesh_data load_version4(std::ifstream& file, const std::string& path, bool debug)
        {
            mesh_data mesh;

            (void)read_value<std::uint16_t>(file); // header size
            (void)read_value<std::uint16_t>(file); // lod type

            const std::uint32_t num_verts = read_value<std::uint32_t>(file);
            const std::uint32_t num_faces = read_value<std::uint32_t>(file);
            std::uint16_t num_lods = read_value<std::uint16_t>(file);
            const std::uint16_t num_bones = read_value<std::uint16_t>(file);
            const std::uint32_t bone_names_size = read_value<std::uint32_t>(file);
            const std::uint16_t num_subsets = read_value<std::uint16_t>(file);
            (void)read_value<std::uint16_t>(file); // hq lod count

            if (num_verts == 0 || num_faces == 0 || num_verts > 2'000'000u || num_faces > 2'000'000u)
            {
                return mesh;
            }

            mesh.vertices.reserve(static_cast<std::size_t>(num_verts) * 3u);
            mesh.indices.reserve(static_cast<std::size_t>(num_faces) * 3u);

            for (std::uint32_t i = 0; i < num_verts; ++i)
            {
                const float px = read_value<float>(file);
                const float py = read_value<float>(file);
                const float pz = read_value<float>(file);
                mesh.vertices.insert(mesh.vertices.end(), { px, py, pz });

                skip_bytes(file, sizeof(float) * 3); // normal
                skip_bytes(file, sizeof(float) * 2); // uv
                skip_bytes(file, sizeof(std::uint32_t)); // tangent
                skip_bytes(file, 4); // rgba
            }

            if (num_bones > 0)
            {
                skip_bytes(file, static_cast<std::streamoff>(num_verts) * 8);
            }

            for (std::uint32_t i = 0; i < num_faces; ++i)
            {
                const std::uint32_t a = read_value<std::uint32_t>(file);
                const std::uint32_t b = read_value<std::uint32_t>(file);
                const std::uint32_t c = read_value<std::uint32_t>(file);
                mesh.indices.insert(mesh.indices.end(), { a, b, c });
            }

            std::vector<std::uint32_t> lod_offsets(num_lods);
            for (std::uint16_t i = 0; i < num_lods; ++i)
            {
                lod_offsets[i] = read_value<std::uint32_t>(file);
            }

            if (num_bones > 0)
            {
                for (std::uint16_t i = 0; i < num_bones; ++i)
                {
                    (void)read_value<std::int32_t>(file);
                    (void)read_value<std::uint16_t>(file);
                    (void)read_value<std::uint16_t>(file);
                    (void)read_value<float>(file);
                    for (int row = 0; row < 4; ++row)
                    {
                        (void)read_value<float>(file);
                        (void)read_value<float>(file);
                        (void)read_value<float>(file);
                    }
                }
                skip_bytes(file, static_cast<std::streamoff>(bone_names_size));
                for (std::uint16_t subset = 0; subset < num_subsets; ++subset)
                {
                    (void)read_value<std::uint32_t>(file);
                    (void)read_value<std::uint32_t>(file);
                    (void)read_value<std::uint32_t>(file);
                    (void)read_value<std::uint32_t>(file);
                    (void)read_value<std::uint32_t>(file);
                    for (int idx = 0; idx < 26; ++idx)
                    {
                        (void)read_value<std::uint16_t>(file);
                    }
                }
            }

            if (debug)
            {
                logger_core::log_info("loaded v4 mesh {} ({} verts, {} faces, {} lods)",
                    path, num_verts, num_faces, num_lods);
            }
            return mesh;
        }

        mesh_data load_version5(std::ifstream& file, const std::string& path, bool debug)
        {
            mesh_data mesh;

            const std::uint16_t header_size = read_value<std::uint16_t>(file);
            const std::uint16_t num_meshes = read_value<std::uint16_t>(file);
            const std::uint32_t num_verts = read_value<std::uint32_t>(file);
            const std::uint32_t num_faces = read_value<std::uint32_t>(file);
            const std::uint16_t num_lods = read_value<std::uint16_t>(file);
            const std::uint16_t num_bones = read_value<std::uint16_t>(file);
            const std::uint32_t bone_names_size = read_value<std::uint32_t>(file);
            const std::uint16_t num_subsets = read_value<std::uint16_t>(file);
            (void)read_value<std::uint8_t>(file); // num_high_quality_lods
            (void)read_value<std::uint8_t>(file); // padding
            const std::uint32_t facs_data_format = read_value<std::uint32_t>(file);
            const std::uint32_t facs_data_size = read_value<std::uint32_t>(file);

            if (header_size == 0 || num_meshes == 0 || num_verts == 0 || num_faces == 0 ||
                num_verts > 2'000'000u || num_faces > 2'000'000u)
            {
                return mesh;
            }

            mesh.vertices.reserve(static_cast<std::size_t>(num_verts) * 3u);
            mesh.indices.reserve(static_cast<std::size_t>(num_faces) * 3u);

            for (std::uint32_t i = 0; i < num_verts; ++i)
            {
                const float px = read_value<float>(file);
                const float py = read_value<float>(file);
                const float pz = read_value<float>(file);
                mesh.vertices.insert(mesh.vertices.end(), { px, py, pz });

                skip_bytes(file, sizeof(float) * 3);
                skip_bytes(file, sizeof(float) * 2);
                skip_bytes(file, sizeof(std::uint32_t));
                skip_bytes(file, 4);
            }

            if (num_bones > 0)
            {
                skip_bytes(file, static_cast<std::streamoff>(num_verts) * 8);
            }

            for (std::uint32_t i = 0; i < num_faces; ++i)
            {
                const std::uint32_t a = read_value<std::uint32_t>(file);
                const std::uint32_t b = read_value<std::uint32_t>(file);
                const std::uint32_t c = read_value<std::uint32_t>(file);
                mesh.indices.insert(mesh.indices.end(), { a, b, c });
            }

            std::vector<std::uint32_t> lod_offsets(num_lods);
            for (std::uint16_t i = 0; i < num_lods; ++i)
            {
                lod_offsets[i] = read_value<std::uint32_t>(file);
            }

            if (num_bones > 0)
            {
                for (std::uint16_t i = 0; i < num_bones; ++i)
                {
                    (void)read_value<std::int32_t>(file);
                    (void)read_value<std::uint16_t>(file);
                    (void)read_value<std::uint16_t>(file);
                    (void)read_value<float>(file);
                    for (int row = 0; row < 4; ++row)
                    {
                        (void)read_value<float>(file);
                        (void)read_value<float>(file);
                        (void)read_value<float>(file);
                    }
                }

                skip_bytes(file, static_cast<std::streamoff>(bone_names_size));

                for (std::uint16_t subset = 0; subset < num_subsets; ++subset)
                {
                    (void)read_value<std::uint32_t>(file);
                    (void)read_value<std::uint32_t>(file);
                    (void)read_value<std::uint32_t>(file);
                    (void)read_value<std::uint32_t>(file);
                    (void)read_value<std::uint32_t>(file);
                    for (int idx = 0; idx < 26; ++idx)
                    {
                        (void)read_value<std::uint16_t>(file);
                    }
                }
            }

            if (facs_data_size > 0)
            {
                skip_bytes(file, static_cast<std::streamoff>(facs_data_size));
            }

            if (debug)
            {
                logger_core::log_info("loaded v5 mesh {} ({} verts, {} faces, facs={}, facs_bytes={})",
                    path, num_verts, num_faces, facs_data_format, facs_data_size);
            }
            return mesh;
        }
    } // namespace

    mesh_data load_obj(const std::string& file_path, bool debug)
    {
        mesh_data mesh;
        std::ifstream file(file_path);
        if (!file.is_open())
        {
            return mesh;
        }

        std::string line;
        while (std::getline(file, line))
        {
            std::istringstream iss(line);
            std::string prefix;
            iss >> prefix;
            if (prefix == "v")
            {
                float x, y, z;
                iss >> x >> y >> z;
                mesh.vertices.insert(mesh.vertices.end(), { x, y, z });
            }
            else if (prefix == "f")
            {
                unsigned int a, b, c;
                iss >> a >> b >> c;
                mesh.indices.insert(mesh.indices.end(), { a - 1u, b - 1u, c - 1u });
            }
        }
        if (debug)
        {
            logger_core::log_info("loaded obj {} ({} verts, {} faces)",
                file_path, mesh.vertices.size() / 3u, mesh.indices.size() / 3u);
        }
        return mesh;
    }

    mesh_data load_roblox_mesh(const std::string& file_path, bool debug)
    {
        mesh_data mesh;
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open())
        {
            return mesh;
        }

        std::string version_line;
        std::getline(file, version_line);
        if (!version_line.empty() && version_line.back() == '\r')
        {
            version_line.pop_back();
        }

        auto starts_with = [](const std::string& str, const std::string& prefix)
        {
            return str.rfind(prefix, 0) == 0;
        };

        try
        {
            if (starts_with(version_line, "version 1.00"))
            {
                mesh = load_version1(file, file_path, true, "1.00", debug);
                return mesh;
            }
            if (starts_with(version_line, "version 1.01"))
            {
                mesh = load_version1(file, file_path, false, "1.01", debug);
                return mesh;
            }
            if (starts_with(version_line, "version 2.00"))
            {
                mesh = load_version2(file, file_path, debug);
                return mesh;
            }
            if (starts_with(version_line, "version 3.00") || starts_with(version_line, "version 3.01"))
            {
                mesh = load_version3(file, file_path, debug);
                return mesh;
            }
            if (starts_with(version_line, "version 4.00") || starts_with(version_line, "version 4.01"))
            {
                mesh = load_version4(file, file_path, debug);
                return mesh;
            }
            if (starts_with(version_line, "version 5.00"))
            {
                mesh = load_version5(file, file_path, debug);
                return mesh;
            }

            if (debug)
            {
                logger_core::log_info("unsupported mesh version in {} (header={})", file_path, version_line);
            }
        }
        catch (const std::exception&)
        {
            return mesh_data{};
        }

        if (debug && !mesh.vertices.empty() && !mesh.indices.empty())
        {
            logger_core::log_info("loaded mesh {} (verts={}, faces={})",
                file_path, mesh.vertices.size() / 3u, mesh.indices.size() / 3u);
        }

        return mesh;
    }
}
