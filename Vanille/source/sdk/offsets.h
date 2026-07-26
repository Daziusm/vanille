#pragma once

#include <cstdint>
#include <vector>

namespace offset {
    struct descriptor {
        const char* ns;
        const char* name;
        std::uintptr_t* value;
    };

    inline std::vector<descriptor>& registry() {
        static std::vector<descriptor> descriptors;
        return descriptors;
    }

    namespace detail {
        struct offset_registrar {
            offset_registrar(const char* ns, const char* name, std::uintptr_t* value) {
                registry().push_back({ ns, name, value });
            }
        };
    }

    inline const std::vector<descriptor>& get_descriptors() {
        return registry();
    }
}

#define OFFSET_ENTRY(ns, name) \
    inline std::uintptr_t name = 0; \
    inline offset::detail::offset_registrar registrar_##ns##_##name(#ns, #name, &name);

namespace roblox {
namespace offsets {
    namespace task_scheduler {
        OFFSET_ENTRY(task_scheduler, pointer)
        OFFSET_ENTRY(task_scheduler, job_start)
        OFFSET_ENTRY(task_scheduler, job_end)
        OFFSET_ENTRY(task_scheduler, job_name)
        OFFSET_ENTRY(task_scheduler, job_stride) // 0x10
        OFFSET_ENTRY(task_scheduler, render_job_to_fake_datamodel)
        OFFSET_ENTRY(task_scheduler, fake_datamodel_to_datamodel)
        OFFSET_ENTRY(task_scheduler, render_job_to_renderview)
        OFFSET_ENTRY(task_scheduler, max_fps)
        OFFSET_ENTRY(task_scheduler, target_fps)
    }

    namespace datamodel {
        OFFSET_ENTRY(datamodel, datamodel_ptr0)
        OFFSET_ENTRY(datamodel, datamodel_ptr1)
        OFFSET_ENTRY(datamodel, place_id)
    }

    namespace visualengine {
        OFFSET_ENTRY(visualengine, visualengine_ptr)
        OFFSET_ENTRY(visualengine, view_matrix)
        OFFSET_ENTRY(visualengine, dimensions)
    }

    namespace renderview {
        OFFSET_ENTRY(renderview, force_flag_byte) // LightingValid
        OFFSET_ENTRY(renderview, force_flag_bool) // SkyboxValid
    }

    namespace players {
        OFFSET_ENTRY(players, local_player)
    }

    namespace player {
        OFFSET_ENTRY(player, display_name)
        OFFSET_ENTRY(player, user_id)
        OFFSET_ENTRY(player, team)
        OFFSET_ENTRY(player, team_color) // 0xD0
        OFFSET_ENTRY(player, character)
    }

    namespace base_part {
        OFFSET_ENTRY(base_part, primitive)
        OFFSET_ENTRY(base_part, material)
        OFFSET_ENTRY(base_part, transparency) // 0xF0
        OFFSET_ENTRY(base_part, color3) // 0x194
        OFFSET_ENTRY(base_part, size)
        OFFSET_ENTRY(base_part, position)
        OFFSET_ENTRY(base_part, primitive_properties) // 0xA0
        OFFSET_ENTRY(base_part, primitive_position) // 0x90
        OFFSET_ENTRY(base_part, validate) // 0x8
        OFFSET_ENTRY(base_part, cframe_rotation)
        OFFSET_ENTRY(base_part, assembly_linear_velocity)
        OFFSET_ENTRY(base_part, assembly_angular_velocity)
        OFFSET_ENTRY(base_part, can_collide)
        OFFSET_ENTRY(base_part, can_collide_mask)
    }

    namespace humanoid {
        OFFSET_ENTRY(humanoid, humanoid_state_id)
        OFFSET_ENTRY(humanoid, move_direction)
        OFFSET_ENTRY(humanoid, floor_material)
        OFFSET_ENTRY(humanoid, health)
        OFFSET_ENTRY(humanoid, hip_height)
        OFFSET_ENTRY(humanoid, jump_height)
        OFFSET_ENTRY(humanoid, jump_power)
        OFFSET_ENTRY(humanoid, max_health)
        OFFSET_ENTRY(humanoid, max_slope_angle)
        OFFSET_ENTRY(humanoid, rig_type)
        OFFSET_ENTRY(humanoid, walk_speed)
        OFFSET_ENTRY(humanoid, auto_rotate)
        OFFSET_ENTRY(humanoid, jump)
        OFFSET_ENTRY(humanoid, humanoid_state)
        OFFSET_ENTRY(humanoid, walk_speed_check)
    }

    namespace value_bool {
        OFFSET_ENTRY(value_bool, value)
    }

    namespace value_int {
        OFFSET_ENTRY(value_int, value)
    }

    namespace value_number {
        OFFSET_ENTRY(value_number, value)
    }

    namespace instance {
        OFFSET_ENTRY(instance, attribute_container)
        OFFSET_ENTRY(instance, attribute_list)
        OFFSET_ENTRY(instance, attribute_to_next)
        OFFSET_ENTRY(instance, attribute_to_value)
        OFFSET_ENTRY(instance, children_end)
        OFFSET_ENTRY(instance, children_start)
        OFFSET_ENTRY(instance, class_base)
        OFFSET_ENTRY(instance, class_descriptor)
        OFFSET_ENTRY(instance, class_name)
        OFFSET_ENTRY(instance, name)
        OFFSET_ENTRY(instance, parent)
        OFFSET_ENTRY(instance, current_camera) // 0x460
        OFFSET_ENTRY(instance, children_stride) // 0x10
    }

    namespace gui_object {
        OFFSET_ENTRY(gui_object, background_color3)
        OFFSET_ENTRY(gui_object, border_color3)
        OFFSET_ENTRY(gui_object, image)
        OFFSET_ENTRY(gui_object, layout_order)
        OFFSET_ENTRY(gui_object, position)
        OFFSET_ENTRY(gui_object, frame_position_x)
        OFFSET_ENTRY(gui_object, frame_position_y)
        OFFSET_ENTRY(gui_object, rich_text)
        OFFSET_ENTRY(gui_object, rotation)
        OFFSET_ENTRY(gui_object, screen_gui_enabled)
        OFFSET_ENTRY(gui_object, size)
        OFFSET_ENTRY(gui_object, text)
        OFFSET_ENTRY(gui_object, text_color3)
        OFFSET_ENTRY(gui_object, text_color3_fallback) // 0xEB8
        OFFSET_ENTRY(gui_object, visible)
    }

    namespace mouse_service {
        OFFSET_ENTRY(mouse_service, input_object)
        OFFSET_ENTRY(mouse_service, mouse_position)
    }

    namespace camera {
        OFFSET_ENTRY(camera, position)
        OFFSET_ENTRY(camera, rotation)
        OFFSET_ENTRY(camera, subject)
        OFFSET_ENTRY(camera, position_offset)
        OFFSET_ENTRY(camera, viewport) // 0x2AC
        OFFSET_ENTRY(camera, viewport_size) // 0x2E8
    }

    namespace chat {
        OFFSET_ENTRY(chat, is_focused)
    }

    namespace lighting {
        OFFSET_ENTRY(lighting, ambient)
        OFFSET_ENTRY(lighting, brightness)
        OFFSET_ENTRY(lighting, colorshift_bottom)
        OFFSET_ENTRY(lighting, colorshift_top)
        OFFSET_ENTRY(lighting, exposure_compensation)
        OFFSET_ENTRY(lighting, fog_color)
        OFFSET_ENTRY(lighting, fog_end)
        OFFSET_ENTRY(lighting, fog_start)
        OFFSET_ENTRY(lighting, geographic_latitude)
        OFFSET_ENTRY(lighting, outdoor_ambient)
        OFFSET_ENTRY(lighting, sky)
    }

    namespace sky {
        OFFSET_ENTRY(sky, moon_angular_size)
        OFFSET_ENTRY(sky, moon_texture_id)
        OFFSET_ENTRY(sky, skybox_bk)
        OFFSET_ENTRY(sky, skybox_dn)
        OFFSET_ENTRY(sky, skybox_ft)
        OFFSET_ENTRY(sky, skybox_lf)
        OFFSET_ENTRY(sky, skybox_orientation)
        OFFSET_ENTRY(sky, skybox_rt)
        OFFSET_ENTRY(sky, skybox_up)
        OFFSET_ENTRY(sky, star_count)
        OFFSET_ENTRY(sky, sun_angular_size)
        OFFSET_ENTRY(sky, sun_texture_id)
    }

    namespace mesh_part {
        OFFSET_ENTRY(mesh_part, mesh_id)
        OFFSET_ENTRY(mesh_part, special_mesh_id) // 0x108
    }

    namespace team {
        OFFSET_ENTRY(team, team_color) // 0x350
    }

    namespace rbx_string {
        OFFSET_ENTRY(rbx_string, length) // 0x10
    }

    namespace workspace {
        OFFSET_ENTRY(workspace, gravity)
        OFFSET_ENTRY(workspace, gravity_container)
        OFFSET_ENTRY(workspace, primitives_pointer1)
        OFFSET_ENTRY(workspace, primitives_pointer2)
    }

    namespace replicator {
        OFFSET_ENTRY(replicator, nextgen_replicator)
    }

    namespace fflags {
        OFFSET_ENTRY(fflags, target_time_delay_facctor_tenths)
    }
}
}

#undef OFFSET_ENTRY
