#include "menu.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <Windows.h>
#include <commdlg.h>

#include "../overlay.hpp"

#pragma comment(lib, "Comdlg32.lib")

static std::string utf8_from_wide(const wchar_t* wide)
{
    if (!wide || !*wide)
    {
        return {};
    }

    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
    {
        return {};
    }

    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

static bool open_image_file_dialog(std::string& out_path)
{
    wchar_t buffer[2048] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    HWND owner = vanille::overlay::g_rbx_window ? vanille::overlay::g_rbx_window : vanille::overlay::g_overlay_window;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif;*.tif;*.tiff;*.webp\0All Files\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(_countof(buffer));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (vanille::overlay::g_overlay_window)
    {
        ::ShowWindow(vanille::overlay::g_overlay_window, SW_HIDE);
    }

    const BOOL picked = GetOpenFileNameW(&ofn);

    if (vanille::overlay::g_overlay_window)
    {
        ::ShowWindow(vanille::overlay::g_overlay_window, SW_SHOW);
    }

    if (!picked)
    {
        return false;
    }

    out_path = utf8_from_wide(buffer);
    return !out_path.empty();
}

void menu::render_visuals()
{
    c_widgets::checkbox("Enabled", &features->enable_esp);

    c_widgets::checkbox("Bounding Box", &features->enable_bounding_box);
    if (features->enable_bounding_box)
    {
        c_widgets::colorpicker("##box_color", features->bounding_box_color, ImGui::GetFrameHeight() * 0.825f);
        static const char* box_styles[] = { "Full", "Corner" };
        c_widgets::dropdown("Box Style", &features->bounding_box_style, box_styles, IM_ARRAYSIZE(box_styles));
    }

    c_widgets::checkbox("Name ESP", &features->enable_name_esp);
    if (features->enable_name_esp)
    {
        c_widgets::colorpicker("##name_color", features->name_esp_color, ImGui::GetFrameHeight() * 0.825f);
        static const char* name_mode_items[] = { "Username", "Display Name" };
        c_widgets::dropdown("Name Mode", &features->name_esp_mode, name_mode_items, IM_ARRAYSIZE(name_mode_items));
        static const char* name_font_items[] = { "Tahoma", "Verdana Regular", "Verdana Bold", "Smallest Pixel", "ProggyClean" };
        c_widgets::dropdown("Name Font", &features->name_esp_font, name_font_items, IM_ARRAYSIZE(name_font_items));
    }

    c_widgets::checkbox("Distance", &features->enable_distance);
    if (features->enable_distance)
    {
        c_widgets::colorpicker("##distance_color", features->distance_color, ImGui::GetFrameHeight() * 0.825f);
    }

    c_widgets::checkbox("Healthbar", &features->enable_healthbar);
    if (features->enable_healthbar)
    {
        static const char* healthbar_modes[] = { "Gradient", "Health-based" };
        if (features->healthbar_color_mode == 0)
        {
            c_widgets::colorpicker_dual("##healthbar_colors", features->healthbar_top_color, features->healthbar_bottom_color, ImGui::GetFrameHeight() * 0.825f);
        }
        c_widgets::dropdown("Healthbar Mode", &features->healthbar_color_mode, healthbar_modes, IM_ARRAYSIZE(healthbar_modes));
    }
    c_widgets::checkbox("Armor Bar", &features->enable_armor_bar);

    c_widgets::checkbox("Offscreen Arrows", &features->enable_offscreen_arrows);
    if (features->enable_offscreen_arrows)
    {
        c_widgets::colorpicker("##offscreen_arrow_color", features->offscreen_arrow_color, ImGui::GetFrameHeight() * 0.825f);
        c_widgets::slider_float("Arrow Size", &features->offscreen_arrow_size, 6.0f, 36.0f, "%.0f");
        const char* arrow_items[] = { "Name", "Distance", "Healthbar" };
        bool arrow_values[] = {
            features->offscreen_arrow_name,
            features->offscreen_arrow_distance,
            features->offscreen_arrow_healthbar
        };
        if (c_widgets::multi_dropdown("Arrow Options", arrow_values, arrow_items, IM_ARRAYSIZE(arrow_items)))
        {
            features->offscreen_arrow_name = arrow_values[0];
            features->offscreen_arrow_distance = arrow_values[1];
            features->offscreen_arrow_healthbar = arrow_values[2];
        }
    }

    c_widgets::checkbox("Body Status", &features->enable_body_status);
    if (features->enable_body_status)
    {
        c_widgets::colorpicker("##status_color", features->status_color, ImGui::GetFrameHeight() * 0.825f);

        const char* status_items[] = { "Movement", "Reload", "Grabbed", "GunFiring", "K.O" };
        bool status_values[] = {
            features->show_status_movement,
            features->show_status_reload,
            features->show_status_grabbed,
            features->show_status_gun_firing,
            features->show_status_knocked
        };

        if (c_widgets::multi_dropdown("Status Flags", status_values, status_items, IM_ARRAYSIZE(status_items)))
        {
            features->show_status_movement = status_values[0];
            features->show_status_reload = status_values[1];
            features->show_status_grabbed = status_values[2];
            features->show_status_gun_firing = status_values[3];
            features->show_status_knocked = status_values[4];
        }
    }

    c_widgets::checkbox("Skeleton", &features->enable_skeleton);
    if (features->enable_skeleton)
    {
        if (c_widgets::colorpicker("##skeleton_color", features->skeleton_color, ImGui::GetFrameHeight() * 0.825f))
        {
            features->skeleton_color.w = std::clamp(features->skeleton_color.w, 0.1f, 1.0f);
        }
        c_widgets::checkbox("Skeleton Outline", &features->enable_skeleton_outline);
        if (features->enable_skeleton_outline)
        {
            c_widgets::colorpicker("##skeleton_outline_color", features->skeleton_outline_color, ImGui::GetFrameHeight() * 0.825f);
        }
    }

    c_widgets::checkbox("Highlight", &features->enable_highlight);
    c_widgets::colorpicker_dual("##highlight_colors", features->highlight_fill_color, features->highlight_outline_color, ImGui::GetFrameHeight() * 0.825f);
    if (features->enable_highlight)
    {
        static const char* highlight_modes[] = { "Default", "Meshes", "Wireframe" };
        c_widgets::dropdown("Highlight Mode", &features->highlight_mode, highlight_modes, IM_ARRAYSIZE(highlight_modes));
        if (features->highlight_mode == 1)
        {
            static const char* highlight_materials[] = { "Flat", "Metallic", "Monochrome", "Acrylic", "Glowing" };
            c_widgets::dropdown("Mesh Material", &features->highlight_mesh_material, highlight_materials, IM_ARRAYSIZE(highlight_materials));
            if (std::clamp(features->highlight_mesh_material, 0, static_cast<int>(IM_ARRAYSIZE(highlight_materials)) - 1) != 0)
            {
                features->highlight_outline_color.w = 0.0f;
            }
        }

    }

}

void menu::render_effects()
{
    c_widgets::checkbox("Draw Dormant", &features->draw_dormant);
    c_widgets::checkbox("Fade Dormant", &features->fade_dormant);
    c_widgets::slider_float("Fade Time", &features->dormant_fade_time, 0.0f, 5.0f, "%.2f");
    c_widgets::checkbox("Override Dormant Color", &features->override_dormant_color);
    if (features->override_dormant_color)
    {
        c_widgets::colorpicker("##dormant_color", features->dormant_color, ImGui::GetFrameHeight() * 0.825f);
    }

    c_widgets::checkbox("Hit Flash", &features->enable_hit_flash);

    c_widgets::checkbox("Hit Tracer", &features->enable_hit_trace);
    if (features->enable_hit_trace)
    {
        c_widgets::colorpicker("##hit_trace_color", features->hit_trace_color, ImGui::GetFrameHeight() * 0.825f);
        static const char* hit_trace_effect_items[] = { "Classic", "Beam", "Pulse", "Spark" };
        c_widgets::dropdown("Hit Tracer Effect", &features->hit_trace_effect, hit_trace_effect_items, IM_ARRAYSIZE(hit_trace_effect_items));
        c_widgets::checkbox("Hit Tracer Outline", &features->hit_trace_outline);
        c_widgets::slider_float("Hit Tracer Width", &features->hit_trace_width, 0.5f, 8.0f, "%.1f");
        c_widgets::slider_float("Hit Tracer Lifespan", &features->hit_trace_lifespan, 0.05f, 2.0f, "%.2fs");
        c_widgets::checkbox("Hit Tracer Cross", &features->hit_trace_cross_enabled);
        if (features->hit_trace_cross_enabled)
        {
            c_widgets::colorpicker("##hit_trace_cross_color", features->hit_trace_cross_color, ImGui::GetFrameHeight() * 0.825f);
            c_widgets::slider_float("Hit Tracer Cross Size", &features->hit_trace_cross_size, 2.0f, 20.0f, "%.0f");
            c_widgets::slider_float("Hit Tracer Cross Thickness", &features->hit_trace_cross_thickness, 0.5f, 6.0f, "%.1f");
        }
    }

    c_widgets::checkbox("Hit Logs", &features->enable_hit_logs);
    if (features->enable_hit_logs)
    {
        static const char* hit_log_positions[] = { "Top Left", "Top Right", "Bottom Left", "Bottom Right", "Middle" };
        c_widgets::dropdown("Hit Log Position", &features->hit_log_position, hit_log_positions, IM_ARRAYSIZE(hit_log_positions));
    }

    c_widgets::checkbox("Death Overlay", &features->enable_death_overlay);
    if (features->enable_death_overlay)
    {
        if (c_widgets::button("Open Image"))
        {
            std::string selected_path;
            if (open_image_file_dialog(selected_path))
            {
                features->death_overlay_image_path = selected_path;
            }
        }

        if (!features->death_overlay_image_path.empty())
        {
            const std::string& path = features->death_overlay_image_path;
            std::string filename = path;
            const size_t slash = filename.find_last_of("\\/");
            if (slash != std::string::npos && slash + 1 < filename.size())
            {
                filename = filename.substr(slash + 1);
            }
            c_widgets::text("Image: %s", filename.c_str());
            if (c_widgets::button("Use Default"))
            {
                features->death_overlay_image_path.clear();
            }
        }
    }
}

void menu::render_additional_esp_settings()
{
    c_widgets::slider_float("Max Distance", &features->max_distance, 50.0f, 5000.0f, "%.0f");
    c_widgets::checkbox("Team Check", &features->team_check);
    c_widgets::checkbox("Occluded Check", &features->enable_visibility_check);
    if (features->enable_visibility_check)
    {
        c_widgets::colorpicker("##occluded_color", features->occluded_color, ImGui::GetFrameHeight() * 0.825f);
    }
    c_widgets::checkbox("Only Show Enemies", &features->esp_enemy_only);

    c_widgets::checkbox("Target Snapline", &features->enable_target_snapline);
    if (features->enable_target_snapline)
    {
        c_widgets::colorpicker("##target_snapline_color", features->target_snapline_color, ImGui::GetFrameHeight() * 0.825f);
        c_widgets::slider_float("Snapline Width", &features->target_snapline_width, 0.5f, 8.0f, "%.1f");
        static const char* target_snapline_styles[] = { "Default", "Dotted", "Animated" };
        c_widgets::dropdown("Snapline Style", &features->target_snapline_style, target_snapline_styles, IM_ARRAYSIZE(target_snapline_styles));
        c_widgets::checkbox("Snapline Outline", &features->target_snapline_outline);
        if (features->target_snapline_outline)
        {
            c_widgets::colorpicker("##target_snapline_outline_color", features->target_snapline_outline_color, ImGui::GetFrameHeight() * 0.825f);
        }
    }

    c_widgets::checkbox("Grenade Indicator", &features->enable_grenade_indicator);
    if (features->enable_grenade_indicator)
    {
        c_widgets::colorpicker_dual("##grenade_indicator_colors", features->grenade_indicator_safe_color, features->grenade_indicator_danger_color, ImGui::GetFrameHeight() * 0.825f);
        c_widgets::checkbox("Grenade Timer", &features->grenade_indicator_show_timer);
    }

    c_widgets::checkbox("Crosshair", &features->enable_crosshair);
    if (features->enable_crosshair)
    {
        if (!features->crosshair_animated_fill)
        {
            c_widgets::colorpicker("##crosshair_color", features->crosshair_color, ImGui::GetFrameHeight() * 0.825f);
        }
        c_widgets::text("Outline Color");
        c_widgets::colorpicker("##crosshair_outline_color", features->crosshair_outline_color, ImGui::GetFrameHeight() * 0.825f);
        c_widgets::slider_float("Line Width", &features->crosshair_line_width, 0.5f, 8.0f, "%.1f");
        c_widgets::slider_float("Gap", &features->crosshair_gap, 0.0f, 48.0f, "%.0f");
        c_widgets::slider_float("Size", &features->crosshair_size, 2.0f, 64.0f, "%.0f");
        c_widgets::checkbox("Middle Dot", &features->crosshair_middle_dot);
        if (features->crosshair_middle_dot)
        {
            c_widgets::slider_float("Middle Dot Size", &features->crosshair_middle_dot_size, 1.0f, 12.0f, "%.1f");
        }
        c_widgets::checkbox("Animated Lines", &features->crosshair_animated_lines);
        if (features->crosshair_animated_lines)
        {
            c_widgets::slider_float("Spin Speed", &features->crosshair_spin_speed, 16.0f, 720.0f, "%.0f");
        }
        c_widgets::checkbox("Animated Colors", &features->crosshair_animated_fill);
        if (features->crosshair_animated_fill)
        {
            c_widgets::colorpicker_dual("##wave_colors", features->crosshair_animated_color_a, features->crosshair_animated_color_b, ImGui::GetFrameHeight() * 0.825f);
            c_widgets::slider_float("Wave Speed", &features->crosshair_gradient_speed, 0.05f, 3.0f, "%.2f");
        }
        c_widgets::checkbox("ADS Gap", &features->crosshair_ads_gap);
        if (features->crosshair_ads_gap)
        {
            c_widgets::slider_float("ADS Gap Scale", &features->crosshair_ads_gap_scale, 0.05f, 0.75f, "%.2f");
            c_widgets::slider_float("ADS Gap Lerp Speed", &features->crosshair_ads_gap_lerp_speed, 1.0f, 15.0f, "%.1f");
        }
        c_widgets::checkbox("Lerp Crosshair", &features->crosshair_lerp);
        if (features->crosshair_lerp)
        {
            c_widgets::slider_float("Crosshair Lerp Speed", &features->crosshair_lerp_speed, 1.0f, 30.0f, "%.1f");
        }
    }

    c_widgets::checkbox("Radar", &features->enable_radar);
    if (features->enable_radar)
    {
        c_widgets::slider_float("Radar Size", &features->radar_size, 80.0f, 400.0f, "%.0f");
        c_widgets::slider_float("Radar Zoom", &features->radar_zoom, 0.25f, 4.0f, "%.2f");
        c_widgets::checkbox("Auto Zoom", &features->radar_auto_zoom);
        c_widgets::slider_float("Radar Transparency", &features->radar_alpha, 0.05f, 1.0f, "%.2f");
        static const char* radar_modes[] = { "2D", "3D" };
        c_widgets::dropdown("Radar Mode", &features->radar_mode, radar_modes, IM_ARRAYSIZE(radar_modes));
        static const char* radar_positions[] = { "Top Left", "Top Right", "Bottom Left", "Bottom Right" };
        c_widgets::dropdown("Radar Position", &features->radar_position, radar_positions, IM_ARRAYSIZE(radar_positions));
    }

    if (globals->game_id == 292439477)
    {
        c_widgets::checkbox("Arm Modifier", &features->enable_arm_modifier);
        if (features->enable_arm_modifier)
        {
            c_widgets::colorpicker("##arm_modifier_color", features->arm_modifier_color, ImGui::GetFrameHeight() * 0.825f);

            static const char* material_names[] = {
                "Off",
                "Plastic",
                "SmoothPlastic",
                "Neon",
                "Wood",
                "WoodPlanks",
                "Marble",
                "Basalt",
                "Slate",
                "CrackedLava",
                "Concrete",
                "Limestone",
                "Granite",
                "Pavement",
                "Brick",
                "Pebble",
                "Cobblestone",
                "Rock",
                "Sandstone",
                "CorrodedMetal",
                "DiamondPlate",
                "Foil",
                "Metal",
                "Grass",
                "LeafyGrass",
                "Sand",
                "Fabric",
                "Snow",
                "Mud",
                "Ground",
                "Asphalt",
                "Salt",
                "Ice",
                "Glacier",
                "Glass",
                "ForceField",
                "Air",
                "Water",
                "Cardboard",
                "Carpet",
                "CeramicTiles",
                "ClayRoofTiles",
                "RoofShingles",
                "Leather",
                "Plaster",
                "Rubber"
            };

            static const int material_values[] = {
                0,
                256,
                272,
                288,
                512,
                528,
                784,
                788,
                800,
                804,
                816,
                820,
                832,
                836,
                848,
                864,
                880,
                896,
                912,
                1040,
                1056,
                1072,
                1088,
                1280,
                1284,
                1296,
                1312,
                1328,
                1344,
                1360,
                1376,
                1392,
                1536,
                1552,
                1568,
                1584,
                1792,
                2048,
                2304,
                2305,
                2306,
                2307,
                2308,
                2309,
                2310,
                2311
            };

            int material_index = 0;
            for (int i = 0; i < IM_ARRAYSIZE(material_values); ++i)
            {
                if (material_values[i] == features->arm_modifier_material)
                {
                    material_index = i;
                    break;
                }
            }

            if (c_widgets::dropdown("Material", &material_index, material_names, IM_ARRAYSIZE(material_names)))
            {
                material_index = std::clamp(material_index, 0, static_cast<int>(IM_ARRAYSIZE(material_values)) - 1);
                features->arm_modifier_material = material_values[material_index];
            }
        }
    }
    //c_widgets::checkbox("Aim Trace", &features->enable_aim_trace);
    //c_widgets::keybind("##aim_trace_key", features->aim_trace_keybind);
    //if (features->enable_aim_trace)
    //{
    //    c_widgets::slider_float("Trace Distance", &features->aim_trace_max_distance, 50.0f, 10000.0f, "%.0f");
    //}
}
