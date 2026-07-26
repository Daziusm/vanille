#pragma once

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace esp
{
    struct esp_preview_render_info
    {
        ImRect bounds;
        ImVec2 head_pos;
        ImVec2 root_pos;
        ImVec2 clip_min;
        ImVec2 clip_max;
        std::vector<ImVec2> projected_points;
        std::vector<std::vector<ImVec2>> subset_hulls;
        float health = 0.0f;
        float max_health = 0.0f;
        float distance = 0.0f;
        int armor = 0;
        bool is_host = false;
        std::string name;
    };

    void render_esp();
    void render_radar();
    void render_crosshair();
    void render_free_aim_fov();
    void render_aimbot_fov();
    void render_triggerbot_fov();
    void render_esp_preview(ImDrawList* draw, const esp_preview_render_info& info);
    void render_highlight_mesh_material_pass(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target_view);
}
