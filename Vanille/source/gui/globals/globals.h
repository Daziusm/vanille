#pragma once

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <imgui/imgui.h>

namespace c_fonts
{
    inline ImFont* tahoma = nullptr;
    inline float tahoma_size = 13.0f;
    inline ImFont* tahoma_regular = nullptr;
    inline float tahoma_regular_size = 14.0f;

    inline ImFont* tahoma_bold = nullptr;
    inline float tahoma_bold_size = 14.0f;

    inline ImFont* verdana_regular = nullptr;
    inline float verdana_regular_size = 14.0f;

    inline ImFont* verdana_bold = nullptr;
    inline float verdana_bold_size = 15.0f;

    inline ImFont* ui_title = nullptr;
    inline float ui_title_size = 22.0f;

    inline ImFont* ui_section = nullptr;
    inline float ui_section_size = 11.0f;

    inline ImFont* ui_tab = nullptr;
    inline float ui_tab_size = 12.0f;

    inline ImFont* ui_tab_bold = nullptr;
    inline float ui_tab_bold_size = 12.0f;

    inline ImFont* pixel7 = nullptr;
    inline float pixel7_size = 16.0f;

    inline ImFont* smallest_pixel = nullptr;
    inline float smallest_pixel_size = 10.0f;

    inline ImFont* proggy_clean = nullptr;
    inline float proggy_clean_size = 13.0f;

    inline ImFont* proggy_tiny = nullptr;
    inline float proggy_tiny_size = 10.0f;
}

namespace c_textures
{
    inline ImTextureID cursor = 0;
    inline ImVec2 cursor_size = ImVec2(0.0f, 0.0f);
    inline ImTextureID logo = 0;
    inline ImVec2 logo_size = ImVec2(0.0f, 0.0f);
    inline ImTextureID death_image = 0;
    inline ImVec2 death_image_size = ImVec2(0.0f, 0.0f);
    inline ImTextureID grenade_icon = 0;
    inline ImVec2 grenade_icon_size = ImVec2(0.0f, 0.0f);
    inline ImTextureID death_image_custom = 0;
    inline ImVec2 death_image_custom_size = ImVec2(0.0f, 0.0f);
}
