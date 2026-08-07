#pragma once

#include "sdk/instance.h"
#include "../gui/widgets/keybind/keybind.h"
#include "../gui/colors/colors.h"
#include <imgui.h>
#include <atomic>
#include <memory>
#include <cstdint>
#include <string>

inline void reset_feature_runtime();

struct globals_state
{
    rbx::instance_t datamodel;
    rbx::instance_t visualengine;
    rbx::instance_t players;
    rbx::instance_t workspace;
    rbx::instance_t lighting;
    rbx::instance_t renderview;
    rbx::instance_t mouse_service;
    rbx::instance_t user_input_service;
    rbx::instance_t text_chat_service;
    rbx::instance_t chat_input_bar_configuration;
    rbx::instance_t pf_local_player_model;

    std::int64_t game_id = 0;

    void reset()
    {
        datamodel = {};
        visualengine = {};
        players = {};
        workspace = {};
        lighting = {};
        renderview = {};
        mouse_service = {};
        text_chat_service = {};
        chat_input_bar_configuration = {};
        pf_local_player_model = {};
        game_id = 0;
        reset_feature_runtime();
    }
};

struct features_state
{
    bool enable_vsync = true;
    bool debug_overlay = false;
    bool team_check = false;
    int phantom_forces_team_filter = 0;

    bool show_watermark = true;
    bool show_spotify_player = false;
    bool show_keybinds_list = true;
    bool show_player_list = true;
    bool show_ai_chat_window = false;
    bool show_appearance_window = true;
    bool show_configs_window = true;
    bool show_lua_editor_window = false;
    bool show_console_window = false;
    bool show_esp_preview_window = true;
    bool show_testing_explorer_window = false;
    bool explorer_auto_refresh = false;
    bool show_visibility_debug_primitives = false;
    bool show_tool_part_labels = false;
    float tool_part_trace_lifetime = 0.85f;
    float esp_preview_avatar_scale = 0.75f;
    rbx::Vector3 esp_preview_camera_offset{ 0.0f, 2.55f, 0.0f };

    int menu_theme = 0;
    bool hide_console = false;
    bool stream_proof = false;

    bool desync = false;
    c_keybind desync_keybind = c_keybind("desync");

    bool desync_marker_active = false;
    rbx::Vector3 desync_marker_position{};

    bool enable_auto_shooter = false;
    float host_click_delay_ms = 50.0f;
    float bullets_sent = 1.0f;
    ImVec4 host_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    bool freeze_players = false;
    c_keybind freeze_players_keybind = c_keybind("freeze_players");
    bool enable_tickrate_modifier = false;
    float tickrate_modifier_value = 60.0f;

    bool enable_fly = false;
    c_keybind fly_keybind = c_keybind("fly");
    float fly_speed = 60.0f;
    float fly_vertical_boost = 1.0f;
    float fly_damping = 10.0f;
    bool fly_check_typing = false;

    bool enable_walkspeed = false;
    c_keybind walkspeed_keybind = c_keybind("walkspeed");
    float walkspeed_value = 32.0f;
    int walkspeed_mode = 0;

    bool enable_bhop = false;
    c_keybind bhop_keybind = c_keybind("bhop");
    float bhop_speed = 32.0f;

    bool enable_noclip = false;
    c_keybind noclip_keybind = c_keybind("noclip");

    bool enable_aimbot = false;
    c_keybind aimbot_keybind = c_keybind("aimbot");
    int aimbot_mode = 0;
    bool aimbot_limit_fov = false;
    float aimbot_fov_radius = 150.0f;
    bool aimbot_draw_fov = false;
    int aimbot_fov_mode = 0;
    int aimbot_hitbox = 0;
    bool aimbot_nearest_part = false;
    bool enable_aimbot_closest_point = false;
    bool aimbot_sticky = true;
    bool aimbot_only_enemies = false;
    bool aimbot_offscreen_check = true;
    bool aimbot_check_team = false;
    bool aimbot_check_health = false;
    bool aimbot_check_knocked = false;
    bool aimbot_check_grabbed = false;
    bool aimbot_check_reloading = false;
    bool aimbot_check_typing = false;
    bool enable_aimbot_smooth = false;
    float aimbot_smooth_x = 6.0f;
    float aimbot_smooth_y = 6.0f;
    bool enable_aimbot_prediction = false;
    int aimbot_prediction_mode = 0;
    float aimbot_prediction_x = 0.0f;
    float aimbot_prediction_y = 0.0f;
    float aimbot_max_distance = 500.0f;
    bool enable_target_hud = false;
    int target_hud_anchor = 0;
    float target_hud_offset = 40.0f;

    bool enable_triggerbot = false;
    c_keybind triggerbot_keybind = c_keybind("triggerbot");
    int triggerbot_hitbox = 0;
    float triggerbot_delay_ms = 50.0f;
    bool triggerbot_hold_fire = true;
    bool triggerbot_only_enemies = false;
    bool triggerbot_check_team = false;
    bool triggerbot_check_health = false;
    bool triggerbot_check_knocked = false;
    bool triggerbot_check_grabbed = false;
    bool triggerbot_check_reloading = false;
    bool triggerbot_check_typing = false;
    bool triggerbot_nearest_part = false;
    bool enable_triggerbot_closest_point = false;
    bool triggerbot_sticky = true;
    bool triggerbot_draw_fov = false;
    bool triggerbot_limit_fov = false;
    float triggerbot_fov_radius = 150.0f;
    int triggerbot_fov_mode = 0;
    float triggerbot_max_distance = 500.0f;

    bool enable_free_aim = false;
    c_keybind free_aim_keybind = c_keybind("free_aim");
    int free_aim_silent_mode = 0;
    bool free_aim_limit_fov = false;
    float free_aim_fov_radius = 150.0f;
    int free_aim_hitbox = 0;
    bool free_aim_nearest_part = false;
    bool enable_free_aim_closest_point = false;
    bool free_aim_sticky = true;
    bool free_aim_only_enemies = false;
    bool free_aim_check_team = false;
    bool free_aim_check_health = false;
    bool free_aim_check_knocked = false;
    bool free_aim_check_grabbed = false;
    bool free_aim_check_typing = false;
    bool free_aim_mouse_spoof = false;
    bool free_aim_draw_fov = false;
    int free_aim_fov_mode = 0;
    float free_aim_max_distance = 500.0f;
    bool free_aim_enable_prediction = false;
    int free_aim_prediction_mode = 0;
    float free_aim_prediction_x = 0.0f;
    float free_aim_prediction_y = 0.0f;

    bool enable_aim_trace = false;
    c_keybind aim_trace_keybind = []()
    {
        c_keybind bind("aim_trace");
        bind.key = 'P';
        bind.type = c_keybind::HOLD;
        return bind;
    }();
    float aim_trace_max_distance = 5000.0f;
    bool enable_hit_trace = false;
    float hit_trace_width = 1.6f;
    bool hit_trace_outline = true;
    ImVec4 hit_trace_color = ImVec4(1.0f, 1.0f, 1.0f, 0.9f);
    float hit_trace_lifespan = 0.25f;
    bool hit_trace_cross_enabled = true;
    ImVec4 hit_trace_cross_color = ImVec4(1.0f, 1.0f, 1.0f, 0.9f);
    float hit_trace_cross_size = 8.0f;
    float hit_trace_cross_thickness = 1.6f;
    int hit_trace_effect = 0;
    bool enable_hit_logs = false;
    int hit_log_position = 0;

    bool enable_esp = false;
    bool esp_enemy_only = false;
    bool enable_grenade_indicator = false;
    bool grenade_indicator_show_timer = true;
    float grenade_indicator_fuse_time = 5.0f;
    float grenade_indicator_radius = 12.0f;
    ImVec4 grenade_indicator_safe_color = ImVec4(1.0f, 0.86f, 0.22f, 0.98f);
    ImVec4 grenade_indicator_danger_color = ImVec4(1.0f, 0.2f, 0.2f, 0.98f);
    bool enable_target_snapline = false;
    ImVec4 target_snapline_color = ImVec4(1.0f, 1.0f, 1.0f, 0.9f);
    bool target_snapline_outline = false;
    ImVec4 target_snapline_outline_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    float target_snapline_width = 1.6f;
    int target_snapline_style = 0;
    bool enable_crosshair = false;
    ImVec4 crosshair_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 crosshair_outline_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    float crosshair_line_width = 1.5f;
    float crosshair_gap = 8.0f;
    float crosshair_size = 8.0f;
    bool crosshair_middle_dot = false;
    float crosshair_middle_dot_size = 2.0f;
    bool crosshair_animated_lines = false;
    float crosshair_spin_speed = 32.0f;
    bool crosshair_animated_fill = false;
    ImVec4 crosshair_animated_color_a = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 crosshair_animated_color_b = ImVec4(0.3f, 0.8f, 1.0f, 1.0f);
    float crosshair_gradient_speed = 0.35f;
    bool crosshair_ads_gap = false;
    float crosshair_ads_gap_scale = 0.50f;
    float crosshair_ads_gap_lerp_speed = 3.0f;
    bool crosshair_lerp = false;
    float crosshair_lerp_speed = 3.0f;
    bool enable_bounding_box = false;
    int bounding_box_style = 0;
    ImVec4 bounding_box_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    bool draw_dormant = false;
    bool fade_dormant = true;
    float dormant_fade_time = 0.5f;
    bool override_dormant_color = false;
    ImVec4 dormant_color = ImVec4(1.0f, 0.35f, 0.8f, 1.0f);
    bool enable_highlight = false;
    bool enable_hit_flash = false;
    bool enable_death_overlay = false;
    std::string death_overlay_image_path;
    int highlight_mode = 0;
    int highlight_mesh_material = 0;
    ImVec4 highlight_fill_color = ImVec4(1.0f, 1.0f, 1.0f, 0.18f);
    ImVec4 highlight_outline_color = ImVec4(1.0f, 1.0f, 1.0f, 0.9f);
    bool enable_raycast_engine = false;
    bool enable_visibility_check = false;
    ImVec4 occluded_color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
    bool aimbot_visibility_check = false;
    bool triggerbot_visibility_check = false;
    bool free_aim_visibility_check = false;

    bool enable_skeleton = false;
    ImVec4 skeleton_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    bool enable_skeleton_outline = false;
    ImVec4 skeleton_outline_color = ImVec4(0.0f, 0.0f, 0.0f, 0.75f);
    
    bool enable_name_esp = false;
    int name_esp_mode = 0;
    int name_esp_font = 0;
    ImVec4 name_esp_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    bool enable_offscreen_arrows = false;
    bool offscreen_arrow_glow = true;
    bool offscreen_arrow_name = false;
    bool offscreen_arrow_distance = false;
    bool offscreen_arrow_healthbar = false;
    float offscreen_arrow_size = 10.0f;
    ImVec4 offscreen_arrow_color = ImVec4(1.0f, 1.0f, 1.0f, 0.9f);

    bool enable_healthbar = false;
    int healthbar_color_mode = 1;
    ImVec4 healthbar_top_color = c_colors::scale_color(c_colors::top_accent_color, 1.2f);
    ImVec4 healthbar_bottom_color = c_colors::scale_color(c_colors::top_accent_color, 0.8f);
    bool enable_armor_bar = false;

    bool enable_distance = false;
    float max_distance = 500.0f;
    ImVec4 distance_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    bool enable_body_status = false;
    bool show_status_movement = false;
    bool show_status_reload = false;
    bool show_status_grabbed = false;
    bool show_status_gun_firing = false;
    bool show_status_knocked = false;
    ImVec4 status_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    bool enable_radar = false;
    float radar_size = 180.0f;
    float radar_zoom = 1.0f;
    bool radar_auto_zoom = false;
    float radar_alpha = 0.85f;
    int radar_mode = 0;
    int radar_position = 3;

    bool lighting_enable_brightness = false;
    float lighting_brightness_value = 1.0f;

    bool lighting_enable_ambient = false;
    rbx::Vector3 lighting_ambient_color = { 1.0f, 1.0f, 1.0f };

    bool lighting_enable_outdoor_ambient = false;
    rbx::Vector3 lighting_outdoor_ambient_color = { 1.0f, 1.0f, 1.0f };

    bool lighting_enable_fog_color = false;
    rbx::Vector3 lighting_fog_color = { 0.75f, 0.75f, 0.75f };

    bool lighting_enable_fog_start = false;
    float lighting_fog_start = 0.0f;

    bool lighting_enable_fog_end = false;
    float lighting_fog_end = 1000.0f;

    bool lighting_enable_exposure_compensation = false;
    float lighting_exposure_compensation_value = 0.0f;

    bool lighting_enable_colorshift_top = false;
    rbx::Vector3 lighting_colorshift_top = { 0.0f, 0.0f, 0.0f };

    bool lighting_enable_colorshift_bottom = false;
    rbx::Vector3 lighting_colorshift_bottom = { 0.0f, 0.0f, 0.0f };

    bool lighting_enable_skybox = false;
    int lighting_skybox_preset = 0;
    int lighting_star_count = 3000;
    bool lighting_enable_sun_texture = false;
    std::string lighting_sun_texture = "rbxassetid://94572375606601";
    bool lighting_enable_moon_texture = false;
    std::string lighting_moon_texture = "rbxassetid://94572375606601";
    float lighting_reapply_interval_seconds = 1.0f;

    bool enable_arm_modifier = false;
    int arm_modifier_material = 1584;
    ImVec4 arm_modifier_color = ImVec4(1.0f, 0.0f, 1.0f, 1.0f);

    void reset_runtime_state()
    {
        desync = false;
        desync_keybind.enabled = false;
        desync_marker_active = false;
        desync_marker_position = {};
        enable_auto_shooter = false;
        host_click_delay_ms = 75.0f;
        bullets_sent = 1.0f;
        host_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
        freeze_players = false;
        freeze_players_keybind.enabled = false;
        enable_tickrate_modifier = false;
    }
};

inline std::unique_ptr<globals_state> globals = std::make_unique<globals_state>();
inline std::unique_ptr<features_state> features = std::make_unique<features_state>();
inline std::atomic<float> g_overlay_dt{ 1.0f / 60.0f };
inline std::atomic<float> g_overlay_fps{ 60.0f };

inline void reset_feature_runtime()
{
    if (features)
        features->reset_runtime_state();
}
