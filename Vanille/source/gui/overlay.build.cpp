#include "overlay.hpp"

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include "globals/globals_fixed.h"
#include "widgets/widgets.h"
#include "menu/menu.h"
#include "cache/local_player_cache.h"
#include "cache/player_cache.h"
#include "fonts/font_verdana_regular.h"
#include "fonts/font_verdana_bold.h"
#include "../../extern/resources/fonts/font_smallest_pixel-7.h"
#include "../../extern/resources/fonts/proggy-tiny.h"
#include "fonts/cursor_bytes.h"
#include "resources/image.h"
#include "resources/grenade_icon.h"
#include <imgui/imgui_freetype.h>
#include "features/esp.h"
#include "features/target_hud.h"
#include "features/visibility.h"
#include "features/lighting.h"
#include "features/free_aim.h"
#include "features/tests.h"
#include "assistant.h"
#include "media_session.h"
#include "configs/config_manager.h"
#include "explorer/explorer_service.h"
#include "lua/lua_vm.h"
#include "mcp/mcp_bridge.h"
#include "lua/lua_ui_bridge.h"
#include "utils/console.h"
#include "utils/logger.h"
#include <memory/memory.h>
#include "sdk/part.h"
#include "sdk/mesh_part.h"
#include "sdk/camera.h"
#include "sdk/player.h"
#include "sdk/value.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "wininet.lib")
#include <d3d11.h>
#include <d3dcompiler.h>
#include <tchar.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <array>
#include <filesystem>
#include <string>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <vector>
#include <optional>
#include <unordered_map>
#include <cstdint>
#include <cfloat>
#include <initializer_list>
#include <future>
#include <mutex>
#include <chrono>
#include <atomic>
#include <d2d1.h>
#include <winhttp.h>
#include <wininet.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <tinygltf/json.hpp>
#include <sstream>
#include <unordered_set>
#include <sstream>
#include <random>
#include <ctime>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef WDA_MONITOR
#define WDA_MONITOR 0x00000001
#endif
#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace logo_asset
{
// Brand logo PNG bytes — see vanille/docs/BRAND_LOGO.md for replace instructions.
#include "../../extern/resources/fonts/logo.c"
}

namespace splash_asset
{
#include "../../extern/resources/fonts/splash_sprite.c"
}

namespace vanille
{
    namespace overlay {
        HWND g_rbx_window = ::FindWindowA(nullptr, "Roblox");
        HWND g_overlay_window = nullptr;
    }
}

extern IMGUI_IMPL_API ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM wParam, LPARAM lParam);

namespace
{
    std::atomic<bool> g_menu_open_state{ false };
    std::atomic<bool> g_menu_showing_state{ false };
    std::atomic<bool> g_overlay_accepts_input{ false };

    LONG overlay_ex_style(bool accept_input)
    {
        return accept_input
            ? (WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)
            : (WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    }

    void apply_overlay_window_style(HWND hwnd, bool accept_input)
    {
        if (!hwnd)
            return;

        static LONG last_style = 0;
        static bool initialized = false;
        const LONG style = overlay_ex_style(accept_input);
        if (!initialized || style != last_style)
        {
            SetWindowLong(hwnd, GWL_EXSTYLE, style);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            last_style = style;
            initialized = true;
        }
    }

    void poll_overlay_mouse_input(HWND hwnd, ImGuiIO& io, bool menu_showing)
    {
        if (!hwnd || !g_overlay_accepts_input.load(std::memory_order_relaxed))
            return;

        // While the menu is open ImGui_ImplWin32 already receives WM mouse input.
        // Injecting button state here duplicates clicks and can flip tabs/widgets.
        static bool was_menu_showing = false;
        if (menu_showing)
        {
            was_menu_showing = true;
            return;
        }

        POINT cursor{};
        if (::GetCursorPos(&cursor) && ::ScreenToClient(hwnd, &cursor))
            io.AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));

        static bool prev_down[5]{};
        if (was_menu_showing)
        {
            const int vk_buttons[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
            for (int button = 0; button < 5; ++button)
                prev_down[button] = (::GetAsyncKeyState(vk_buttons[button]) & 0x8000) != 0;
            was_menu_showing = false;
        }

        const int vk_buttons[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
        for (int button = 0; button < 5; ++button)
        {
            const bool down = (::GetAsyncKeyState(vk_buttons[button]) & 0x8000) != 0;
            if (down != prev_down[button])
                io.AddMouseButtonEvent(button, down);
            prev_down[button] = down;
        }
    }

    bool overlay_vk_down(int vk)
    {
        return (::GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    void poll_overlay_keyboard_input(ImGuiIO& io, bool menu_showing)
    {
        if (!g_overlay_accepts_input.load(std::memory_order_relaxed))
            return;

        if (menu_showing)
            return;

        io.AddKeyEvent(ImGuiMod_Ctrl, overlay_vk_down(VK_CONTROL));
        io.AddKeyEvent(ImGuiMod_Shift, overlay_vk_down(VK_SHIFT));
        io.AddKeyEvent(ImGuiMod_Alt, overlay_vk_down(VK_MENU));
        io.AddKeyEvent(ImGuiMod_Super, overlay_vk_down(VK_LWIN) || overlay_vk_down(VK_RWIN));

        static bool prev_down[256]{};
        BYTE keyboard_state[256]{};
        ::GetKeyboardState(keyboard_state);

        for (int vk = 0x08; vk < 256; ++vk)
        {
            const bool down = overlay_vk_down(vk);
            if (down == prev_down[vk])
                continue;
            prev_down[vk] = down;

            const ImGuiKey key = ImGui_ImplWin32_KeyEventToImGuiKey(static_cast<WPARAM>(vk), 0);
            if (key != ImGuiKey_None)
                io.AddKeyEvent(key, down);

            if (!down)
                continue;

            wchar_t chars[8]{};
            const UINT scan_code = ::MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
            const int char_count = ::ToUnicode(static_cast<UINT>(vk), scan_code, keyboard_state, chars, IM_ARRAYSIZE(chars), 0);
            for (int index = 0; index < char_count; ++index)
            {
                if (chars[index] == '\t' || chars[index] == '\n' || chars[index] >= 32)
                    io.AddInputCharacter(static_cast<unsigned int>(chars[index]));
            }
        }
    }
}

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static bool g_overlay_blur_enabled = false;
static float g_overlay_blur_amount = 0.7f;
c_keybind g_menu_key("menu");
bool g_menu_key_initialized = false;
static ImVec2 g_menu_last_pos = ImVec2(0.0f, 0.0f);
static ImVec2 g_menu_last_size = ImVec2(0.0f, 0.0f);
static bool g_menu_has_frame = false;
static ImVec2 g_configs_last_pos = ImVec2(0.0f, 0.0f);
static ImVec2 g_configs_last_size = ImVec2(0.0f, 0.0f);
static bool g_configs_has_frame = false;
static ImVec2 g_aux_window_size = ImVec2(240.0f, 280.0f);
static ImVec2 g_esp_preview_drag_target(0.0f, 0.0f);
static ImVec2 g_esp_preview_drag_value(0.0f, 0.0f);
static float g_esp_preview_zoom_target = 0.0f;
static float g_esp_preview_zoom_value = 0.0f;

struct esp_preview_frame_info
{
    bool ready = false;
    ImRect bounds{ ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f) };
    ImVec2 head_pos{ 0.0f, 0.0f };
    ImVec2 root_pos{ 0.0f, 0.0f };
    ImVec2 dimensions{ 0.0f, 0.0f };
    float distance = 0.0f;
    std::vector<ImVec2> projected_points;
    std::vector<std::vector<ImVec2>> subset_hulls;
};

static esp_preview_frame_info g_esp_preview_frame_info{};

namespace c_ui { c_keybind& menu_key_bind(); }

inline ImVec4 scale_colors(ImVec4 color, float factor)
{
    color.x = ImClamp(color.x * factor, 0.0f, 1.0f);
    color.y = ImClamp(color.y * factor, 0.0f, 1.0f);
    color.z = ImClamp(color.z * factor, 0.0f, 1.0f);
    return color;
}

inline void draw_raycast_engine_warning()
{
    if (!visibility::should_show_raycast_engine_warning())
    {
        return;
    }

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (!draw)
    {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
    {
        return;
    }

    constexpr const char* warning_text = "Please, enable \"Raycast Engine\" for Occluded Check.";
    ImFont* font = c_fonts::verdana_bold ? c_fonts::verdana_bold : ImGui::GetFont();
    const float font_size = c_fonts::verdana_bold ? c_fonts::verdana_bold->LegacySize : ImGui::GetFontSize();
    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, warning_text);
    const ImVec2 pos(
        (io.DisplaySize.x - text_size.x) * 0.5f,
        (io.DisplaySize.y - text_size.y) * 0.5f
    );

    draw->AddText(font, font_size, ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 220), warning_text);
    draw->AddText(font, font_size, pos, IM_COL32(255, 50, 50, 255), warning_text);
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

ID3D11ShaderResourceView* g_custom_cursor_srv = nullptr;
static ID3D11ShaderResourceView* g_logo_srv = nullptr;
static ID3D11ShaderResourceView* g_splash_sprite_srv = nullptr;
static ImTextureID g_splash_sprite_tex = 0;
static ImVec2 g_splash_sprite_size{};
static ID3D11ShaderResourceView* g_media_art_srv = nullptr;
static std::uint64_t g_media_art_revision = 0;
static RECT g_media_island_client_rect{};
static bool g_media_island_hittest_active = false;
static RECT g_media_lyrics_panel_client_rect{};
static bool g_media_lyrics_panel_hittest_active = false;
static bool g_media_lyrics_open = false;
static float g_lyrics_scroll_offset = 0.0f;
static float g_lyrics_panel_open_anim = 0.0f;
static std::string g_lyrics_scroll_track_key;
static constexpr float k_media_lyrics_full_height = 228.0f;
static constexpr float k_media_lyrics_progress_height = 46.0f;
static constexpr float k_media_lyrics_expanded_min_width = 368.0f;
static constexpr float k_lyrics_scroll_anim_duration = 0.28f;
static constexpr double k_lyrics_seek_threshold_seconds = 1.5;
static constexpr double k_lyrics_forward_resync_seconds = 0.35;

static int g_lyrics_display_line = -1;
static float g_lyrics_scroll_line = 0.0f;
static float g_lyrics_scroll_anim_from = 0.0f;
static double g_lyrics_scroll_anim_start = -1.0;
static std::string g_lyrics_media_key;
static double g_lyrics_last_snap_position = -1.0;

struct lyrics_playback_clock
{
    std::string media_key;
    double anchor_position = 0.0;
    std::chrono::steady_clock::time_point anchor_time{};
    bool initialized = false;
};

static lyrics_playback_clock g_lyrics_clock;
static RECT g_watermark_island_client_rect{};
static bool g_watermark_island_hittest_active = false;
static ImVec2 g_watermark_island_offset{};
static ImVec2 g_media_island_offset{};
static int g_island_drag_target = 0;

struct media_island_layout
{
    ImVec2 island_pos{};
    ImVec2 island_size{};
    ImVec2 player_size{};
    ImRect strip_rect{};
    ImRect lyrics_button_rect{};
    ImRect lyrics_rect{};
    ImRect interactive_rect{};
};

static std::string ellipsize_text(ImFont* font, float font_size, const std::string& text, float max_width);
static media_island_layout compute_media_island_layout(const vanille::media::snapshot& snap);
static void reset_lyrics_scroll_state();
static bool cursor_over_floating_islands(HWND hwnd);

static bool point_in_client_rect(HWND hwnd, const RECT& rect)
{
    POINT cursor{};
    if (!::GetCursorPos(&cursor) || !::ScreenToClient(hwnd, &cursor))
        return false;
    return ::PtInRect(&rect, cursor) != FALSE;
}

static RECT screen_rect_to_client_rect(HWND hwnd, const ImVec2& screen_min, const ImVec2& screen_max)
{
    POINT top_left{ static_cast<LONG>(screen_min.x), static_cast<LONG>(screen_min.y) };
    POINT bottom_right{ static_cast<LONG>(screen_max.x), static_cast<LONG>(screen_max.y) };
    if (hwnd)
    {
        ::ScreenToClient(hwnd, &top_left);
        ::ScreenToClient(hwnd, &bottom_right);
    }
    return RECT{ top_left.x, top_left.y, bottom_right.x, bottom_right.y };
}

static void refresh_overlay_input_for_floating_widgets(HWND hwnd)
{
    if (!hwnd)
        return;

    const bool islands_now = cursor_over_floating_islands(hwnd);
    const bool aux_now = lua_vm::cursor_over_aux_windows(hwnd);
    const bool menu_now = g_menu_open_state.load(std::memory_order_relaxed);
    if (!islands_now && !aux_now && !menu_now)
        return;

    g_overlay_accepts_input.store(true, std::memory_order_relaxed);
}

static void register_current_aux_window_hittest()
{
    if (ImGuiWindow* window = ImGui::GetCurrentWindow())
    {
        lua_vm::register_aux_window_hittest_rect(
            window->Pos,
            window->Size,
            vanille::overlay::g_overlay_window);
    }
}

static bool cursor_over_floating_islands(HWND hwnd)
{
    g_media_island_hittest_active = false;
    g_watermark_island_hittest_active = false;
    if (!hwnd)
        return false;

    bool over = false;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    if (::features->show_watermark)
    {
        const float logo_px = 26.0f;
        const float section_gap = 12.0f;
        const float sep_gap = 9.0f;
        const ImVec2 padding(16.0f, 9.0f);
        ImFont* title_font = c_fonts::verdana_bold ? c_fonts::verdana_bold : ImGui::GetFont();
        ImFont* body_font = c_fonts::verdana_regular ? c_fonts::verdana_regular : ImGui::GetFont();
        const ImVec2 title_size = title_font->CalcTextSizeA(title_font->LegacySize, FLT_MAX, 0.0f, "vanille");
        const ImVec2 sep_size = body_font->CalcTextSizeA(body_font->LegacySize, FLT_MAX, 0.0f, "|");
        const ImVec2 fps_slot_size = body_font->CalcTextSizeA(body_font->LegacySize, FLT_MAX, 0.0f, "999 fps");
        const float content_height = (std::max)(logo_px, title_size.y);
        const float content_width = logo_px + section_gap + title_size.x + section_gap + sep_size.x + sep_gap + 80.0f + sep_gap + sep_size.x + section_gap + fps_slot_size.x;
        const float island_w = content_width + padding.x * 2.0f;
        const float island_h = content_height + padding.y * 2.0f;
        const float island_x = viewport->WorkPos.x + (viewport->WorkSize.x - island_w) * 0.5f + g_watermark_island_offset.x;
        const float island_y = viewport->WorkPos.y + 10.0f + g_watermark_island_offset.y;

        g_watermark_island_client_rect = screen_rect_to_client_rect(
            hwnd,
            ImVec2(island_x, island_y),
            ImVec2(island_x + island_w, island_y + island_h));
        g_watermark_island_hittest_active = true;
        over |= point_in_client_rect(hwnd, g_watermark_island_client_rect);
    }

    if (::features->show_spotify_player)
    {
        const auto snap = vanille::media::get_snapshot();
        const media_island_layout layout = compute_media_island_layout(snap);
        g_media_island_client_rect = screen_rect_to_client_rect(
            hwnd,
            layout.island_pos,
            ImVec2(layout.island_pos.x + layout.island_size.x, layout.island_pos.y + layout.island_size.y));
        g_media_island_hittest_active = true;
        over |= point_in_client_rect(hwnd, g_media_island_client_rect);
        g_media_lyrics_panel_hittest_active = false;
    }

    return over;
}
static ID3D11ShaderResourceView* g_grenade_icon_srv = nullptr;
static ID3D11ShaderResourceView* g_death_image_srv = nullptr;
static ID3D11ShaderResourceView* g_death_image_custom_srv = nullptr;

namespace
{
    std::vector<vanille::overlay::player_list_entry> g_player_entries;
    std::vector<int> g_player_status_values;
    std::vector<bool> g_player_host_values;
    int g_selected_player_index = 0;
    float g_player_list_width_override = 0.0f;
    bool g_is_spectating = false;
    std::uintptr_t g_spectate_target_humanoid = 0;
    std::string g_death_image_path_cached;
    std::unordered_map<std::uint64_t, int> g_player_status_by_id;
    std::unordered_map<std::string, int> g_player_status_by_name;
    std::mutex g_player_status_mutex;
    std::unordered_map<std::uint64_t, bool> g_player_host_by_id;
    std::unordered_map<std::string, bool> g_player_host_by_name;
    std::mutex g_player_host_mutex;
    enum class avatar_state
    {
        not_requested,
        downloading,
        ready,
        failed
    };
    struct avatar_entry
    {
        ImTextureID texture = 0;
        avatar_state state = avatar_state::not_requested;
        int width = 0;
        int height = 0;
    };
    std::unordered_map<std::uint64_t, avatar_entry> g_avatar_cache;
    std::unordered_map<std::uint64_t, std::future<std::vector<unsigned char>>> g_avatar_pending;
    std::mutex g_avatar_mutex;
    using Microsoft::WRL::ComPtr;
    void log_avatar3d_debug(const std::string& message, std::uint64_t user_id = 0)
    {
        std::ostringstream oss;
        oss << "[avatar3d] ";
        if (user_id != 0)
            oss << "(user " << user_id << ") ";
        oss << message;
        logger_core::log_info("{}", oss.str());
    }

    enum class avatar3d_state
    {
        not_requested,
        downloading,
        ready,
        failed
    };

    struct avatar3d_metadata
    {
        rbx::Vector3 camera_position{};
        rbx::Vector3 camera_direction{};
        float camera_fov = 30.0f;
        rbx::Vector3 aabb_min{};
        rbx::Vector3 aabb_max{};
        std::string mtl_id;
        std::string obj_id;
        std::vector<std::string> textures;
    };

    struct avatar_vertex
    {
        rbx::Vector3 position{};
        rbx::Vector3 normal{};
        rbx::Vector2 uv{};
    };

    struct avatar3d_mesh_subset
    {
        std::uint32_t start_index = 0;
        std::uint32_t index_count = 0;
        int material_index = -1;
    };

    struct avatar3d_material_cpu
    {
        std::string name;
        std::string diffuse_map_id;
        rbx::Vector3 diffuse_color{ 1.0f, 1.0f, 1.0f };
    };

    struct avatar3d_material_gpu
    {
        std::string name;
        std::string diffuse_map_id;
        rbx::Vector3 diffuse_color{ 1.0f, 1.0f, 1.0f };
        ComPtr<ID3D11ShaderResourceView> texture;
    };

    struct avatar3d_geometry
    {
        std::vector<avatar_vertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<avatar3d_mesh_subset> subsets;
        std::vector<avatar3d_material_cpu> materials;
    };

    struct avatar3d_download_result
    {
        bool success = false;
        bool retryable = true;
        avatar3d_metadata meta{};
        avatar3d_geometry geometry{};
        std::unordered_map<std::string, std::vector<unsigned char>> texture_bytes;
    };

    struct avatar3d_entry
    {
        avatar3d_state state = avatar3d_state::not_requested;
        avatar3d_metadata meta{};
        avatar3d_geometry geometry{};
        std::vector<avatar3d_material_gpu> materials;
        ComPtr<ID3D11Buffer> vertex_buffer;
        ComPtr<ID3D11Buffer> index_buffer;
        ComPtr<ID3D11Texture2D> preview_texture;
        ComPtr<ID3D11RenderTargetView> preview_rtv;
        ComPtr<ID3D11ShaderResourceView> preview_srv;
        ComPtr<ID3D11Texture2D> preview_depth;
        ComPtr<ID3D11DepthStencilView> preview_dsv;
        int preview_width = 0;
        int preview_height = 0;
    };

    struct avatar_preview_pipeline
    {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11InputLayout> layout;
        ComPtr<ID3D11Buffer> constant_buffer;
        ComPtr<ID3D11SamplerState> sampler;
        ComPtr<ID3D11RasterizerState> rasterizer;
        ComPtr<ID3D11DepthStencilState> depth_state;
        ComPtr<ID3D11BlendState> blend_state;
        bool ready = false;
    };

    struct avatar_preview_constants
    {
        rbx::Matrix world;
        rbx::Matrix view;
        rbx::Matrix proj;
        rbx::Vector3 light_dir;
        float ambient = 0.2f;
    };

    avatar_preview_pipeline g_avatar_preview_pipeline{};
    std::unordered_map<std::uint64_t, avatar3d_entry> g_avatar3d_cache;
    std::unordered_map<std::uint64_t, std::future<avatar3d_download_result>> g_avatar3d_pending;
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> g_avatar3d_retry_after;
    std::unordered_map<std::uint64_t, int> g_avatar3d_retry_count;
    std::unordered_set<std::uint64_t> g_avatar3d_logged_failures;
    std::mutex g_avatar3d_mutex;
    bool is_printable_ascii(const std::string& text)
    {
        for (unsigned char c : text)
        {
            if (c < 32 || c > 126)
                return false;
        }
        return true;
    }
    std::string sanitize_display_name(const std::string& display, const std::string& username)
    {
        if (display.empty())
            return {};
        if (!is_printable_ascii(display))
            return username;
        return display;
    }
    void clear_status_maps()
    {
        std::lock_guard<std::mutex> lock(g_player_status_mutex);
        g_player_status_by_id.clear();
        g_player_status_by_name.clear();
    }

    void clear_host_maps()
    {
        std::lock_guard<std::mutex> lock(g_player_host_mutex);
        g_player_host_by_id.clear();
        g_player_host_by_name.clear();
    }

    void set_host_for_entry(const vanille::overlay::player_list_entry& entry, bool is_host)
    {
        std::lock_guard<std::mutex> lock(g_player_host_mutex);
        if (entry.user_id != 0)
        {
            g_player_host_by_id[entry.user_id] = is_host;
        }
        if (!entry.name.empty())
        {
            g_player_host_by_name[entry.name] = is_host;
        }
    }

    bool get_host_for_keys(std::uint64_t user_id, const std::string& name)
    {
        std::lock_guard<std::mutex> lock(g_player_host_mutex);
        if (user_id != 0)
        {
            if (auto it = g_player_host_by_id.find(user_id); it != g_player_host_by_id.end())
                return it->second;
        }
        if (!name.empty())
        {
            if (auto it = g_player_host_by_name.find(name); it != g_player_host_by_name.end())
                return it->second;
        }
        return false;
    }

    void set_status_for_entry(const vanille::overlay::player_list_entry& entry, int status)
    {
        auto set_name_status = [&](const std::string& value)
        {
            if (value.empty())
            {
                return;
            }
            std::string key = value;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            g_player_status_by_name[key] = status;
        };

        std::lock_guard<std::mutex> lock(g_player_status_mutex);
        if (entry.user_id != 0)
        {
            g_player_status_by_id[entry.user_id] = status;
        }
        else
        {
            set_name_status(entry.name);
            set_name_status(entry.display_name);
            set_name_status(entry.username);
        }
    }

    int get_status_for_keys(std::uint64_t user_id, const std::string& name)
    {
        std::lock_guard<std::mutex> lock(g_player_status_mutex);
        if (user_id != 0)
        {
            if (auto it = g_player_status_by_id.find(user_id); it != g_player_status_by_id.end())
                return it->second;
            return 0;
        }
        if (!name.empty())
        {
            std::string key = name;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            if (auto it = g_player_status_by_name.find(key); it != g_player_status_by_name.end())
                return it->second;
        }
        return 0;
    }
    IWICImagingFactory* g_wic_factory = nullptr;

    std::wstring wide_from_utf8(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (needed <= 1)
        {
            return {};
        }

        std::wstring out(static_cast<size_t>(needed - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), needed);
        return out;
    }

    inline ImVec4 apply_alpha(ImVec4 color, float alpha)
    {
        color.w = ImClamp(color.w * alpha, 0.0f, 1.0f);
        return color;
    }

    inline std::optional<rbx::Vector3> get_part_position(const cache::primitive_part& part)
    {
        if (!part.instance.is_valid())
            return std::nullopt;
        return part.instance.get_position(part.primitive);
    }

    IWICImagingFactory* get_wic_factory()
    {
        if (g_wic_factory)
            return g_wic_factory;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&g_wic_factory));
        if (FAILED(hr))
            g_wic_factory = nullptr;
        return g_wic_factory;
    }

    struct theme_preset
    {
        ImVec4 accent;
        ImVec4 window;
        ImVec4 child;
        ImVec4 outline;
        ImVec4 alt_border;
        ImVec4 text_title;
        ImVec4 text;
    };

    struct theme_style_snapshot
    {
        ImVec4 accent;
        ImVec4 window_top;
        ImVec4 window_bottom;
        ImVec4 child_top;
        ImVec4 child_bottom;
        ImVec4 outline;
        ImVec4 outter_outline;
        ImVec4 text;
        ImVec4 text_disabled;
        ImVec4 window_bg;
        ImVec4 child_bg;
        ImVec4 border;
        ImVec4 frame_bg;
        ImVec4 frame_bg_hovered;
        ImVec4 frame_bg_active;
        ImVec4 button;
        ImVec4 button_hovered;
        ImVec4 button_active;
        ImVec4 header;
        ImVec4 header_hovered;
        ImVec4 header_active;
        ImVec4 slider_grab;
        ImVec4 slider_grab_active;
        ImVec4 check_mark;
        ImVec4 title_bg;
        ImVec4 title_bg_active;
        ImVec4 separator;
        ImVec4 separator_hovered;
        ImVec4 separator_active;
    };

    static bool g_default_theme_captured = false;
    static theme_style_snapshot g_default_theme{};

    const theme_preset k_theme_presets[] = {
        
        { ImVec4(175.0f / 255.0f, 50.0f / 255.0f, 100.0f / 255.0f, 1.0f), ImVec4(30.0f / 255.0f, 6.0f / 255.0f, 16.0f / 255.0f, 1.0f), ImVec4(22.0f / 255.0f, 4.0f / 255.0f, 12.0f / 255.0f, 1.0f), ImVec4(15.0f / 255.0f, 2.0f / 255.0f, 7.0f / 255.0f, 1.0f), ImVec4(10.0f / 255.0f, 2.0f / 255.0f, 6.0f / 255.0f, 1.0f), ImVec4(136.0f / 255.0f, 136.0f / 255.0f, 136.0f / 255.0f, 1.0f), ImVec4(180.0f / 255.0f, 180.0f / 255.0f, 180.0f / 255.0f, 1.0f) },
        
        { ImVec4(42.0f / 255.0f, 122.0f / 255.0f, 222.0f / 255.0f, 1.0f), ImVec4(0.0f / 255.0f, 2.0f / 255.0f, 23.0f / 255.0f, 1.0f), ImVec4(3.0f / 255.0f, 1.0f / 255.0f, 18.0f / 255.0f, 1.0f), ImVec4(0.0f, 0.0f, 0.0f, 1.0f), ImVec4(0.0f, 0.0f, 0.0f, 0.0f), ImVec4(210.0f / 255.0f, 210.0f / 255.0f, 210.0f / 255.0f, 1.0f), ImVec4(210.0f / 255.0f, 210.0f / 255.0f, 210.0f / 255.0f, 1.0f) },
        
        { ImVec4(155.0f / 255.0f, 125.0f / 255.0f, 175.0f / 255.0f, 1.0f), ImVec4(42.0f / 255.0f, 42.0f / 255.0f, 56.0f / 255.0f, 1.0f), ImVec4(36.0f / 255.0f, 36.0f / 255.0f, 48.0f / 255.0f, 1.0f), ImVec4(32.0f / 255.0f, 32.0f / 255.0f, 38.0f / 255.0f, 1.0f), ImVec4(28.0f / 255.0f, 24.0f / 255.0f, 32.0f / 255.0f, 1.0f), ImVec4(180.0f / 255.0f, 180.0f / 255.0f, 180.0f / 255.0f, 1.0f), ImVec4(180.0f / 255.0f, 180.0f / 255.0f, 180.0f / 255.0f, 1.0f) },
        
        { ImVec4(156.0f / 255.0f, 199.0f / 255.0f, 40.0f / 255.0f, 1.0f), ImVec4(20.0f / 255.0f, 20.0f / 255.0f, 20.0f / 255.0f, 1.0f), ImVec4(12.0f / 255.0f, 12.0f / 255.0f, 12.0f / 255.0f, 1.0f), ImVec4(12.0f / 255.0f, 12.0f / 255.0f, 12.0f / 255.0f, 1.0f), ImVec4(0.0f, 0.0f, 0.0f, 0.0f), ImVec4(195.0f / 255.0f, 195.0f / 255.0f, 195.0f / 255.0f, 1.0f), ImVec4(205.0f / 255.0f, 205.0f / 255.0f, 205.0f / 255.0f, 1.0f) },
        
        { ImVec4(252.0f / 255.0f, 154.0f / 255.0f, 29.0f / 255.0f, 1.0f), ImVec4(30.0f / 255.0f, 29.0f / 255.0f, 34.0f / 255.0f, 1.0f), ImVec4(18.0f / 255.0f, 17.0f / 255.0f, 22.0f / 255.0f, 1.0f), ImVec4(13.0f / 255.0f, 12.0f / 255.0f, 17.0f / 255.0f, 1.0f), ImVec4(0.0f, 0.0f, 0.0f, 0.0f), ImVec4(184.0f / 255.0f, 183.0f / 255.0f, 188.0f / 255.0f, 1.0f), ImVec4(233.0f / 255.0f, 232.0f / 255.0f, 237.0f / 255.0f, 1.0f) },
        
        { ImVec4(139.0f / 255.0f, 152.0f / 255.0f, 199.0f / 255.0f, 1.0f), ImVec4(25.0f / 255.0f, 28.0f / 255.0f, 37.0f / 255.0f, 1.0f), ImVec4(10.0f / 255.0f, 11.0f / 255.0f, 16.0f / 255.0f, 1.0f), ImVec4(0.0f, 0.0f, 0.0f, 1.0f), ImVec4(0.0f, 0.0f, 0.0f, 0.0f), ImVec4(221.0f / 255.0f, 234.0f / 255.0f, 246.0f / 255.0f, 1.0f), ImVec4(221.0f / 255.0f, 234.0f / 255.0f, 246.0f / 255.0f, 1.0f) },
        
        { ImVec4(152.0f / 255.0f, 122.0f / 255.0f, 173.0f / 255.0f, 1.0f), ImVec4(25.0f / 255.0f, 25.0f / 255.0f, 25.0f / 255.0f, 1.0f), ImVec4(14.0f / 255.0f, 15.0f / 255.0f, 14.0f / 255.0f, 1.0f), ImVec4(10.0f / 255.0f, 10.0f / 255.0f, 13.0f / 255.0f, 1.0f), ImVec4(0.0f, 0.0f, 0.0f, 0.0f), ImVec4(120.0f / 255.0f, 121.0f / 255.0f, 121.0f / 255.0f, 1.0f), ImVec4(254.0f / 255.0f, 255.0f / 255.0f, 254.0f / 255.0f, 1.0f) },
    };

    void capture_default_theme()
    {
        if (g_default_theme_captured)
            return;
        g_default_theme_captured = true;
        ImGuiStyle& style = ImGui::GetStyle();
        g_default_theme.accent = c_colors::top_accent_color;
        g_default_theme.window_top = c_colors::top_window_background;
        g_default_theme.window_bottom = c_colors::bottom_window_background;
        g_default_theme.child_top = c_colors::top_child_background;
        g_default_theme.child_bottom = c_colors::bottom_child_background;
        g_default_theme.outline = c_colors::main_border;
        g_default_theme.outter_outline = c_colors::outter_border;
        g_default_theme.text = style.Colors[ImGuiCol_Text];
        g_default_theme.text_disabled = style.Colors[ImGuiCol_TextDisabled];
        g_default_theme.window_bg = style.Colors[ImGuiCol_WindowBg];
        g_default_theme.child_bg = style.Colors[ImGuiCol_ChildBg];
        g_default_theme.border = style.Colors[ImGuiCol_Border];
        g_default_theme.frame_bg = style.Colors[ImGuiCol_FrameBg];
        g_default_theme.frame_bg_hovered = style.Colors[ImGuiCol_FrameBgHovered];
        g_default_theme.frame_bg_active = style.Colors[ImGuiCol_FrameBgActive];
        g_default_theme.button = style.Colors[ImGuiCol_Button];
        g_default_theme.button_hovered = style.Colors[ImGuiCol_ButtonHovered];
        g_default_theme.button_active = style.Colors[ImGuiCol_ButtonActive];
        g_default_theme.header = style.Colors[ImGuiCol_Header];
        g_default_theme.header_hovered = style.Colors[ImGuiCol_HeaderHovered];
        g_default_theme.header_active = style.Colors[ImGuiCol_HeaderActive];
        g_default_theme.slider_grab = style.Colors[ImGuiCol_SliderGrab];
        g_default_theme.slider_grab_active = style.Colors[ImGuiCol_SliderGrabActive];
        g_default_theme.check_mark = style.Colors[ImGuiCol_CheckMark];
        g_default_theme.title_bg = style.Colors[ImGuiCol_TitleBg];
        g_default_theme.title_bg_active = style.Colors[ImGuiCol_TitleBgActive];
        g_default_theme.separator = style.Colors[ImGuiCol_Separator];
        g_default_theme.separator_hovered = style.Colors[ImGuiCol_SeparatorHovered];
        g_default_theme.separator_active = style.Colors[ImGuiCol_SeparatorActive];
    }

    static void apply_theme_preset_internal(int index)
    {
        capture_default_theme();
        if (index < 0)
            return;

        if (index == 0)
        {
            ImGuiStyle& style = ImGui::GetStyle();
            c_colors::top_accent_color = g_default_theme.accent;
            c_colors::bottom_accent_color = c_colors::derive_bottom_accent(c_colors::top_accent_color);
            c_colors::top_window_background = g_default_theme.window_top;
            c_colors::bottom_window_background = g_default_theme.window_bottom;
            c_colors::top_child_background = g_default_theme.child_top;
            c_colors::bottom_child_background = g_default_theme.child_bottom;
            c_colors::main_border = g_default_theme.outline;
            c_colors::outter_border = g_default_theme.outter_outline;

            style.Colors[ImGuiCol_Text] = g_default_theme.text;
            style.Colors[ImGuiCol_TextDisabled] = g_default_theme.text_disabled;
            style.Colors[ImGuiCol_WindowBg] = g_default_theme.window_bg;
            style.Colors[ImGuiCol_ChildBg] = g_default_theme.child_bg;
            style.Colors[ImGuiCol_FrameBg] = g_default_theme.frame_bg;
            style.Colors[ImGuiCol_FrameBgHovered] = g_default_theme.frame_bg_hovered;
            style.Colors[ImGuiCol_FrameBgActive] = g_default_theme.frame_bg_active;
            style.Colors[ImGuiCol_Button] = g_default_theme.button;
            style.Colors[ImGuiCol_ButtonHovered] = g_default_theme.button_hovered;
            style.Colors[ImGuiCol_ButtonActive] = g_default_theme.button_active;
            style.Colors[ImGuiCol_Header] = g_default_theme.header;
            style.Colors[ImGuiCol_HeaderHovered] = g_default_theme.header_hovered;
            style.Colors[ImGuiCol_HeaderActive] = g_default_theme.header_active;
            style.Colors[ImGuiCol_SliderGrab] = g_default_theme.slider_grab;
            style.Colors[ImGuiCol_SliderGrabActive] = g_default_theme.slider_grab_active;
            style.Colors[ImGuiCol_CheckMark] = g_default_theme.check_mark;
            style.Colors[ImGuiCol_TitleBg] = g_default_theme.title_bg;
            style.Colors[ImGuiCol_TitleBgActive] = g_default_theme.title_bg_active;
            style.Colors[ImGuiCol_Separator] = g_default_theme.separator;
            style.Colors[ImGuiCol_SeparatorHovered] = g_default_theme.separator_hovered;
            style.Colors[ImGuiCol_SeparatorActive] = g_default_theme.separator_active;
            return;
        }

        const int preset_index = index - 1;
        if (preset_index < 0 || preset_index >= static_cast<int>(IM_ARRAYSIZE(k_theme_presets)))
            return;

        const theme_preset& t = k_theme_presets[preset_index];
        auto clamp01 = [](float v) { return ImClamp(v, 0.0f, 1.0f); };
        auto adjust = [&](ImVec4 base, float delta) -> ImVec4
        {
            base.x = clamp01(base.x + delta);
            base.y = clamp01(base.y + delta);
            base.z = clamp01(base.z + delta);
            return base;
        };
        const float delta = 0.018f;

        c_colors::top_accent_color = t.accent;
        c_colors::bottom_accent_color = c_colors::derive_bottom_accent(t.accent);

        c_colors::top_window_background = adjust(t.window, -delta * 0.35f);
        c_colors::bottom_window_background = c_colors::top_window_background;

        c_colors::top_child_background = adjust(t.child, delta * 0.35f);
        c_colors::bottom_child_background = c_colors::top_child_background;

        if (preset_index == 0)
        {
            c_colors::main_border = ImVec4(175.0f / 255.0f, 50.0f / 255.0f, 100.0f / 255.0f, 0.075f);
            c_colors::outter_border = ImVec4(0.016f, 0.016f, 0.016f, 1.0f);
        }
        else if (preset_index == 1) 
        {
            c_colors::main_border = ImVec4(42.0f / 255.0f, 122.0f / 255.0f, 222.0f / 255.0f, 0.075f);
            c_colors::outter_border = ImVec4(0.016f, 0.016f, 0.016f, 1.0f);
        }
        else if (preset_index == 2) 
        {
            c_colors::main_border = ImVec4(0.205f, 0.205f, 0.205f, 1.0f);
            c_colors::outter_border = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
        }
        else
        {
            c_colors::main_border = g_default_theme.outline;
            c_colors::outter_border = g_default_theme.outter_outline;
        }

        ImGuiStyle& style = ImGui::GetStyle();
        style.Colors[ImGuiCol_Text] = t.text;
        style.Colors[ImGuiCol_TextDisabled] = t.text_title;
        style.Colors[ImGuiCol_WindowBg] = c_colors::top_window_background;
        style.Colors[ImGuiCol_ChildBg] = c_colors::top_child_background;
        style.Colors[ImGuiCol_FrameBg] = c_colors::top_child_background;
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.3f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.6f);
        style.Colors[ImGuiCol_Button] = c_colors::top_child_background;
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.35f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.6f);
        style.Colors[ImGuiCol_Header] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.4f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.5f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.6f);
        style.Colors[ImGuiCol_SliderGrab] = t.accent;
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.9f);
        style.Colors[ImGuiCol_CheckMark] = t.accent;
        style.Colors[ImGuiCol_TitleBg] = c_colors::top_window_background;
        style.Colors[ImGuiCol_TitleBgActive] = c_colors::top_window_background;
        style.Colors[ImGuiCol_Separator] = t.outline;
        style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.5f);
        style.Colors[ImGuiCol_SeparatorActive] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.7f);
    }

    bool set_camera_subject(std::uintptr_t camera_address, std::uintptr_t humanoid_address)
    {
        const auto subject_offset = roblox::offsets::camera::subject;
        if (camera_address == 0 || humanoid_address == 0 || subject_offset == 0)
            return false;
        memory->write<std::uintptr_t>(camera_address + subject_offset, humanoid_address);
        return true;
    }

    std::uintptr_t get_local_root_primitive(const cache::local_player_state& local_state)
    {
        if (local_state.parts.humanoid_root_part.primitive)
            return local_state.parts.humanoid_root_part.primitive;
        if (local_state.parts.lower_torso.primitive)
            return local_state.parts.lower_torso.primitive;
        if (local_state.parts.torso.primitive)
            return local_state.parts.torso.primitive;
        return 0;
    }

    std::uintptr_t get_local_humanoid_address(const cache::local_player_state& local_state)
    {
        if (!local_state.character.is_valid())
            return 0;
        const auto humanoid = local_state.character.find_first_child("Humanoid");
        return humanoid.is_valid() ? humanoid.get_address() : 0;
    }

    std::uintptr_t get_entry_root_primitive(const vanille::overlay::player_list_entry& entry)
    {
        if (entry.root_part_primitive)
            return entry.root_part_primitive;

        if (entry.humanoid_root_part != 0)
        {
            rbx::instance_t root(entry.humanoid_root_part);
            if (root.is_valid())
                return rbx::part::get_primitive(root);
        }

        return 0;
    }

    bool can_teleport_entry(const vanille::overlay::player_list_entry& entry)
    {
        const auto local = cache::localplayer->snapshot();
        if (local.address == 0)
            return false;
        if (entry.address != 0 && entry.address == local.address)
            return false;
        const std::uintptr_t local_root = get_local_root_primitive(local);
        if (local_root == 0)
            return false;
        return get_entry_root_primitive(entry) != 0;
    }

    bool teleport_to_entry(const vanille::overlay::player_list_entry& entry)
    {
        const auto local = cache::localplayer->snapshot();
        if (local.address == 0)
            return false;
        if (entry.address != 0 && entry.address == local.address)
            return false;

        const std::uintptr_t local_root = get_local_root_primitive(local);
        if (local_root == 0)
            return false;

        const std::uintptr_t target_primitive = get_entry_root_primitive(entry);
        if (target_primitive == 0)
            return false;

        const auto position_offset = roblox::offsets::camera::position_offset
            ? roblox::offsets::camera::position_offset
            : roblox::offsets::base_part::position;
        if (position_offset == 0)
            return false;

        const auto target_pos = memory->read<rbx::Vector3>(target_primitive + position_offset);
        if (!std::isfinite(target_pos.x) || !std::isfinite(target_pos.y) || !std::isfinite(target_pos.z))
            return false;

        bool wrote = false;
        for (int i = 0; i < 50; ++i)
        {
            memory->write<rbx::Vector3>(local_root + position_offset, target_pos);
            wrote = true;
        }
        rbx::part::clear_velocity(local_root);
        return wrote;
    }

    void stop_spectate()
    {
        const auto local = cache::localplayer->snapshot();
        if (local.address != 0 && local.camera.is_valid())
        {
            if (const auto humanoid_addr = get_local_humanoid_address(local); humanoid_addr != 0)
            {
                set_camera_subject(local.camera.get_address(), humanoid_addr);
            }
        }
        g_is_spectating = false;
        g_spectate_target_humanoid = 0;
    }

    bool start_spectate(const vanille::overlay::player_list_entry& entry)
    {
        const auto local = cache::localplayer->snapshot();
        if (local.address == 0 || !local.camera.is_valid())
            return false;

        const std::uintptr_t target_humanoid = entry.humanoid;
        if (target_humanoid == 0)
            return false;
        if (local.address == entry.address)
            return false;

        if (set_camera_subject(local.camera.get_address(), target_humanoid))
        {
            g_is_spectating = true;
            g_spectate_target_humanoid = target_humanoid;
            return true;
        }
        return false;
    }

    bool can_spectate_entry(const vanille::overlay::player_list_entry& entry)
    {
        const auto local = cache::localplayer->snapshot();
        if (local.address == 0 || !local.camera.is_valid())
            return false;
        if (entry.address != 0 && entry.address == local.address)
            return false;
        return entry.humanoid != 0;
    }

    std::wstring build_roblox_cookie_headers()
    {
        char buffer[8192]{};
        DWORD size = sizeof(buffer);
        if (!InternetGetCookieA("https://www.roblox.com/", ".ROBLOSECURITY", buffer, &size))
            return {};
        std::string cookie_data(buffer);
        if (cookie_data.empty())
            return {};
        std::string header = "Cookie: " + cookie_data + "\r\n";
        return std::wstring(header.begin(), header.end());
    }

    std::vector<unsigned char> download_bytes(const std::wstring& host, const std::wstring& path, const std::wstring& extra_headers = {})
    {
        std::vector<unsigned char> result;
        HINTERNET session = WinHttpOpen(L"vanille/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            return result;

        WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);

        HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect)
        {
            WinHttpCloseHandle(session);
            return result;
        }

        HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request)
        {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return result;
        }

        {
            DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
            WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
            WinHttpAddRequestHeaders(request, L"Accept-Encoding: gzip, deflate\r\n", (ULONG)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            if (!extra_headers.empty())
                WinHttpAddRequestHeaders(request, extra_headers.c_str(), (ULONG)-1, WINHTTP_ADDREQ_FLAG_ADD);
        }

        BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok)
            ok = WinHttpReceiveResponse(request, nullptr);
        if (ok)
        {
            DWORD available = 0;
            do
            {
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
                    break;
                std::vector<unsigned char> chunk;
                chunk.resize(available);
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
                    break;
                chunk.resize(read);
                result.insert(result.end(), chunk.begin(), chunk.end());
            } while (available > 0);
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    std::vector<unsigned char> download_avatar(std::uint64_t user_id)
    {
        std::wstring api_path = L"/v1/users/avatar-headshot?userIds=" + std::to_wstring(user_id) + L"&size=150x150&format=Png&isCircular=false";
        auto json_bytes = download_bytes(L"thumbnails.roblox.com", api_path);
        if (json_bytes.empty())
            return {};
        std::string json(json_bytes.begin(), json_bytes.end());
        std::string key = "\"imageUrl\":\"";
        auto pos = json.find(key);
        if (pos == std::string::npos)
            return {};
        pos += key.size();
        auto end = json.find('"', pos);
        if (end == std::string::npos || end <= pos)
            return {};
        std::string url = json.substr(pos, end - pos);
        auto scheme = url.find("://");
        if (scheme == std::string::npos)
            return {};
        std::string host_path = url.substr(scheme + 3);
        auto slash = host_path.find('/');
        if (slash == std::string::npos)
            return {};
        std::string host = host_path.substr(0, slash);
        std::string path = host_path.substr(slash);
        std::wstring whost(host.begin(), host.end());
        std::wstring wpath(path.begin(), path.end());
        return download_bytes(whost, wpath);
    }

    bool split_url(const std::string& url, std::wstring& out_host, std::wstring& out_path)
    {
        auto scheme_pos = url.find("://");
        std::string no_scheme = (scheme_pos == std::string::npos) ? url : url.substr(scheme_pos + 3);
        auto slash_pos = no_scheme.find('/');
        if (slash_pos == std::string::npos)
            return false;
        std::string host = no_scheme.substr(0, slash_pos);
        std::string path = no_scheme.substr(slash_pos);
        out_host.assign(host.begin(), host.end());
        out_path.assign(path.begin(), path.end());
        return true;
    }

    std::vector<unsigned char> download_url_bytes(const std::string& url)
    {
        std::wstring host, path;
        if (!split_url(url, host, path))
            return {};
        return download_bytes(host, path);
    }

    std::string compute_cdn_url(const std::string& hash)
    {
        if (hash.empty())
            return {};
        if (hash.rfind("http://", 0) == 0 || hash.rfind("https://", 0) == 0)
            return hash;
        int accumulator = 31;
        const size_t max_len = std::min<std::size_t>(hash.size(), 38);
        for (size_t i = 0; i < max_len; ++i)
        {
            accumulator ^= static_cast<int>(hash[i]);
        }
        int server = accumulator % 8;
        if (server < 0)
            server += 8;
        return "https://t" + std::to_string(server) + ".rbxcdn.com/" + hash;
    }

    std::optional<avatar3d_metadata> fetch_avatar3d_metadata(std::uint64_t user_id, bool* out_retryable = nullptr)
    {
        if (out_retryable)
            *out_retryable = true;

        avatar3d_metadata meta{};
        std::wstring api_path = L"/v1/users/avatar-3d?userId=" + std::to_wstring(user_id);
        const std::wstring cookie_headers = build_roblox_cookie_headers();
        auto initial_bytes = download_bytes(L"thumbnails.roblox.com", api_path, cookie_headers);
        if (initial_bytes.empty())
        {
            //log_avatar3d_debug("avatar-3d API bytes empty", user_id);
            return std::nullopt;
        }

        std::string initial_json(initial_bytes.begin(), initial_bytes.end());
        nlohmann::json root = nlohmann::json::parse(initial_json, nullptr, false);
        if (root.is_discarded())
        {
            //log_avatar3d_debug("avatar-3d API JSON parse failed", user_id);
            return std::nullopt;
        }

        if (root.contains("errors") && root["errors"].is_array() && !root["errors"].is_null() && !root["errors"].empty())
        {
            if (out_retryable)
                *out_retryable = false;
            return std::nullopt;
        }
        auto parse_entry = [&](const nlohmann::json& entry) -> bool
            {
                std::string state = entry.value("state", "");
                if (state != "Completed")
                {
                    //log_avatar3d_debug("avatar-3d API state not Completed: " + state + " (will retry)", user_id);
                    return false;
                }
                std::string image_url = entry.value("imageUrl", "");
                if (image_url.empty())
                {
                    //log_avatar3d_debug("avatar-3d API imageUrl empty", user_id);
                    return false;
                }

                auto meta_bytes = download_url_bytes(image_url);
                if (meta_bytes.empty())
                {
                    //log_avatar3d_debug("avatar-3d metadata fetch empty", user_id);
                    return false;
                }
                std::string meta_json(meta_bytes.begin(), meta_bytes.end());
                nlohmann::json meta_root = nlohmann::json::parse(meta_json, nullptr, false);

                if (meta_root.is_discarded())
                {
                    
                    meta.obj_id = image_url;
                    if (image_url.find("-Obj") != std::string::npos)
                    {
                        std::string mtl_url = image_url;
                        size_t pos = mtl_url.rfind("-Obj");
                        mtl_url.replace(pos, 4, "-Mtl");
                        meta.mtl_id = mtl_url;
                    }
                    //log_avatar3d_debug("metadata JSON parse failed; using direct OBJ/MTL fallback", user_id);
                    return !meta.obj_id.empty() && !meta.mtl_id.empty();
                }

                if (auto cam = meta_root.find("camera"); cam != meta_root.end() && cam->is_object())
                {
                    auto& c = *cam;
                    meta.camera_position = { c["position"].value("x", 0.0f), c["position"].value("y", 0.0f), c["position"].value("z", 0.0f) };
                    meta.camera_direction = { c["direction"].value("x", 0.0f), c["direction"].value("y", 0.0f), c["direction"].value("z", 0.0f) };
                    meta.camera_fov = c.value("fov", 30.0f);
                }

                if (auto aabb = meta_root.find("aabb"); aabb != meta_root.end() && aabb->is_object())
                {
                    meta.aabb_min = { (*aabb)["min"].value("x", 0.0f), (*aabb)["min"].value("y", 0.0f), (*aabb)["min"].value("z", 0.0f) };
                    meta.aabb_max = { (*aabb)["max"].value("x", 0.0f), (*aabb)["max"].value("y", 0.0f), (*aabb)["max"].value("z", 0.0f) };
                }

                meta.mtl_id = meta_root.value("mtl", meta.mtl_id);
                meta.obj_id = meta_root.value("obj", meta.obj_id);
                if (auto tex = meta_root.find("textures"); tex != meta_root.end() && tex->is_array())
                {
                    for (const auto& t : *tex)
                    {
                        if (t.is_string())
                            meta.textures.push_back(t.get<std::string>());
                    }
                }
                if (meta.mtl_id.empty() || meta.obj_id.empty())
                    return false;
                return true;
            };

        if (root.contains("data") && root["data"].is_array() && !root["data"].empty())
        {
            const auto& entry = root["data"].front();
            if (parse_entry(entry))
                return meta;
        }
        else if (root.contains("imageUrl"))
        {
            if (parse_entry(root))
                return meta;
            std::string truncated = initial_json.substr(0, 256);
            //log_avatar3d_debug("avatar-3d API data missing but imageUrl present; body=" + truncated, user_id);
        }
        else
        {
            std::string truncated = initial_json.substr(0, 256);
            //log_avatar3d_debug("avatar-3d API data missing; body=" + truncated, user_id);
        }
        return std::nullopt;
    }

    ImTextureID create_texture_from_png(const std::vector<unsigned char>& data, int& out_w, int& out_h)
    {
        out_w = 0;
        out_h = 0;
        if (data.empty() || !g_pd3dDevice || !g_pd3dDeviceContext)
            return 0;
        IWICImagingFactory* factory = get_wic_factory();
        if (!factory)
            return 0;

        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        ImTextureID texture_id = 0;

        auto release_all = [&]()
        {
            if (converter) { converter->Release(); converter = nullptr; }
            if (frame) { frame->Release(); frame = nullptr; }
            if (decoder) { decoder->Release(); decoder = nullptr; }
            if (stream) { stream->Release(); stream = nullptr; }
        };

        do
        {
            if (FAILED(factory->CreateStream(&stream)))
                break;
            if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data.data()), static_cast<DWORD>(data.size()))))
                break;
            if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)))
                break;
            if (FAILED(decoder->GetFrame(0, &frame)))
                break;
            if (FAILED(factory->CreateFormatConverter(&converter)))
                break;
            if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom)))
                break;

            UINT width = 0, height = 0;
            frame->GetSize(&width, &height);
            out_w = static_cast<int>(width);
            out_h = static_cast<int>(height);
            std::vector<unsigned char> pixels;
            pixels.resize(width * height * 4);
            const UINT stride = width * 4;
            if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data())))
                break;

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA sub = {};
            sub.pSysMem = pixels.data();
            sub.SysMemPitch = stride;

            ID3D11Texture2D* tex = nullptr;
            if (FAILED(g_pd3dDevice->CreateTexture2D(&desc, &sub, &tex)))
            {
                break;
            }

            ID3D11ShaderResourceView* srv = nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format = desc.Format;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MipLevels = 1;
            if (FAILED(g_pd3dDevice->CreateShaderResourceView(tex, &srv_desc, &srv)))
            {
                tex->Release();
                break;
            }
            tex->Release();
            texture_id = reinterpret_cast<ImTextureID>(srv);
        } while (false);

        release_all();
        return texture_id;
    }

    ImTextureID create_texture_from_file(const std::wstring& path, int& out_w, int& out_h)
    {
        out_w = 0;
        out_h = 0;
        if (path.empty() || !g_pd3dDevice || !g_pd3dDeviceContext)
            return 0;
        IWICImagingFactory* factory = get_wic_factory();
        if (!factory)
            return 0;

        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        ImTextureID texture_id = 0;

        auto release_all = [&]()
        {
            if (converter) { converter->Release(); converter = nullptr; }
            if (frame) { frame->Release(); frame = nullptr; }
            if (decoder) { decoder->Release(); decoder = nullptr; }
        };

        do
        {
            if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
                break;
            if (FAILED(decoder->GetFrame(0, &frame)))
                break;
            if (FAILED(factory->CreateFormatConverter(&converter)))
                break;
            if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom)))
                break;

            UINT width = 0, height = 0;
            frame->GetSize(&width, &height);
            out_w = static_cast<int>(width);
            out_h = static_cast<int>(height);
            std::vector<unsigned char> pixels;
            pixels.resize(width * height * 4);
            const UINT stride = width * 4;
            if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data())))
                break;

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA sub = {};
            sub.pSysMem = pixels.data();
            sub.SysMemPitch = stride;

            ID3D11Texture2D* tex = nullptr;
            if (FAILED(g_pd3dDevice->CreateTexture2D(&desc, &sub, &tex)))
            {
                break;
            }

            ID3D11ShaderResourceView* srv = nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format = desc.Format;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MipLevels = 1;
            if (FAILED(g_pd3dDevice->CreateShaderResourceView(tex, &srv_desc, &srv)))
            {
                tex->Release();
                break;
            }
            tex->Release();
            texture_id = reinterpret_cast<ImTextureID>(srv);
        } while (false);

        release_all();
        return texture_id;
    }

    void request_avatar(std::uint64_t user_id)
    {
        if (user_id == 0)
            return;
        std::lock_guard<std::mutex> lock(g_avatar_mutex);
        auto it = g_avatar_cache.find(user_id);
        if (it != g_avatar_cache.end() && (it->second.state == avatar_state::ready || it->second.state == avatar_state::downloading))
            return;
        if (g_avatar_pending.find(user_id) != g_avatar_pending.end())
            return;
        g_avatar_cache[user_id].state = avatar_state::downloading;
        g_avatar_pending[user_id] = std::async(std::launch::async, [user_id]()
            {
                return download_avatar(user_id);
            });
    }

    void process_avatar_downloads()
    {
        std::lock_guard<std::mutex> lock(g_avatar_mutex);
        for (auto it = g_avatar_pending.begin(); it != g_avatar_pending.end();)
        {
            if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                std::vector<unsigned char> bytes = it->second.get();
                int w = 0, h = 0;
                ImTextureID tex = create_texture_from_png(bytes, w, h);
                avatar_entry& entry = g_avatar_cache[it->first];
                entry.texture = tex;
                entry.width = w;
                entry.height = h;
                entry.state = tex != 0 ? avatar_state::ready : avatar_state::failed;
                it = g_avatar_pending.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    bool parse_mtl_file(const std::string& data, std::vector<avatar3d_material_cpu>& materials, std::unordered_map<std::string, int>& material_lookup)
    {
        materials.clear();
        material_lookup.clear();

        std::istringstream stream(data);
        std::string line;
        avatar3d_material_cpu current{};
        bool has_current = false;
        while (std::getline(stream, line))
        {
            if (line.empty())
                continue;
            std::istringstream ls(line);
            std::string op;
            ls >> op;
            if (op.empty() || op[0] == '#')
                continue;
            if (op == "newmtl")
            {
                if (has_current)
                {
                    material_lookup[current.name] = static_cast<int>(materials.size());
                    materials.push_back(current);
                }
                current = avatar3d_material_cpu{};
                ls >> current.name;
                has_current = true;
            }
            else if (op == "Kd")
            {
                ls >> current.diffuse_color.x >> current.diffuse_color.y >> current.diffuse_color.z;
            }
            else if (op == "map_Kd")
            {
                ls >> current.diffuse_map_id;
            }
        }
        if (has_current)
        {
            material_lookup[current.name] = static_cast<int>(materials.size());
            materials.push_back(current);
        }
        return !materials.empty();
    }

    struct face_index
    {
        int v = -1;
        int vt = -1;
        int vn = -1;
    };

    bool parse_face_component(const std::string& token, face_index& out)
    {
        out = {};
        if (token.empty())
            return false;
        size_t first = token.find('/');
        if (first == std::string::npos)
        {
            out.v = std::atoi(token.c_str());
            return out.v != 0;
        }

        size_t second = token.find('/', first + 1);
        std::string v_str = token.substr(0, first);
        std::string vt_str;
        std::string vn_str;
        if (second == std::string::npos)
        {
            vt_str = token.substr(first + 1);
        }
        else
        {
            vt_str = token.substr(first + 1, second - first - 1);
            vn_str = token.substr(second + 1);
        }

        out.v = v_str.empty() ? 0 : std::atoi(v_str.c_str());
        out.vt = vt_str.empty() ? 0 : std::atoi(vt_str.c_str());
        out.vn = vn_str.empty() ? 0 : std::atoi(vn_str.c_str());
        return out.v != 0;
    }

    int resolve_index(int idx, int count)
    {
        if (idx > 0)
            return idx - 1;
        return count + idx;
    }

    bool parse_obj_file(const std::string& data, const std::unordered_map<std::string, int>& material_lookup, avatar3d_geometry& out_geo)
    {
        out_geo.vertices.clear();
        out_geo.indices.clear();
        out_geo.subsets.clear();

        std::vector<rbx::Vector3> positions;
        std::vector<rbx::Vector3> normals;
        std::vector<rbx::Vector2> uvs;

        int current_material = -1;
        avatar3d_mesh_subset current_subset{};
        current_subset.start_index = 0;
        current_subset.index_count = 0;
        current_subset.material_index = current_material;

        auto finalize_subset = [&]()
            {
                if (current_subset.index_count > 0)
                    out_geo.subsets.push_back(current_subset);
                current_subset.start_index = static_cast<std::uint32_t>(out_geo.indices.size());
                current_subset.index_count = 0;
                current_subset.material_index = current_material;
            };

        std::istringstream stream(data);
        std::string line;
        while (std::getline(stream, line))
        {
            if (line.empty())
                continue;
            std::istringstream ls(line);
            std::string op;
            ls >> op;
            if (op.empty() || op[0] == '#')
                continue;
            if (op == "v")
            {
                rbx::Vector3 p;
                ls >> p.x >> p.y >> p.z;
                positions.push_back(p);
            }
            else if (op == "vt")
            {
                rbx::Vector2 t;
                ls >> t.x >> t.y;
                t.y = 1.0f - t.y;
                uvs.push_back(t);
            }
            else if (op == "vn")
            {
                rbx::Vector3 n;
                ls >> n.x >> n.y >> n.z;
                normals.push_back(n);
            }
            else if (op == "usemtl")
            {
                std::string name;
                ls >> name;
                auto it = material_lookup.find(name);
                current_material = (it != material_lookup.end()) ? it->second : -1;
                finalize_subset();
            }
            else if (op == "f")
            {
                std::vector<face_index> face_indices;
                std::string token;
                while (ls >> token)
                {
                    face_index idx{};
                    if (parse_face_component(token, idx))
                        face_indices.push_back(idx);
                }
                if (face_indices.size() < 3)
                    continue;

                for (size_t tri = 1; tri + 1 < face_indices.size(); ++tri)
                {
                    face_index idx0 = face_indices[0];
                    face_index idx1 = face_indices[tri];
                    face_index idx2 = face_indices[tri + 1];

                    int v0 = resolve_index(idx0.v, static_cast<int>(positions.size()));
                    int v1 = resolve_index(idx1.v, static_cast<int>(positions.size()));
                    int v2 = resolve_index(idx2.v, static_cast<int>(positions.size()));
                    if (v0 < 0 || v0 >= static_cast<int>(positions.size()) || v1 < 0 || v1 >= static_cast<int>(positions.size()) || v2 < 0 || v2 >= static_cast<int>(positions.size()))
                        continue;

                    rbx::Vector3 p0 = positions[v0];
                    rbx::Vector3 p1 = positions[v1];
                    rbx::Vector3 p2 = positions[v2];

                    rbx::Vector3 face_normal = (p1 - p0).Cross(p2 - p0);
                    if (face_normal.LengthSquared() > 0.0f)
                        face_normal.Normalize();
                    else
                        face_normal = rbx::Vector3(0.0f, 1.0f, 0.0f);

                    auto make_vertex = [&](const face_index& idx) -> avatar_vertex
                        {
                            avatar_vertex v{};
                            int pos_index = resolve_index(idx.v, static_cast<int>(positions.size()));
                            if (pos_index >= 0 && pos_index < static_cast<int>(positions.size()))
                                v.position = positions[pos_index];
                            int uv_index = (idx.vt != 0) ? resolve_index(idx.vt, static_cast<int>(uvs.size())) : -1;
                            if (uv_index >= 0 && uv_index < static_cast<int>(uvs.size()))
                                v.uv = uvs[uv_index];
                            int n_index = (idx.vn != 0) ? resolve_index(idx.vn, static_cast<int>(normals.size())) : -1;
                            if (n_index >= 0 && n_index < static_cast<int>(normals.size()))
                                v.normal = normals[n_index];
                            else
                                v.normal = face_normal;
                            return v;
                        };

                    avatar_vertex vtx0 = make_vertex(idx0);
                    avatar_vertex vtx1 = make_vertex(idx1);
                    avatar_vertex vtx2 = make_vertex(idx2);

                    std::uint32_t base_index = static_cast<std::uint32_t>(out_geo.vertices.size());
                    out_geo.vertices.push_back(vtx0);
                    out_geo.vertices.push_back(vtx1);
                    out_geo.vertices.push_back(vtx2);

                    out_geo.indices.push_back(base_index);
                    out_geo.indices.push_back(base_index + 1);
                    out_geo.indices.push_back(base_index + 2);

                    current_subset.index_count += 3;
                }
            }
        }

        finalize_subset();
        return !out_geo.vertices.empty() && !out_geo.indices.empty();
    }

    avatar3d_download_result download_avatar3d(std::uint64_t user_id)
    {
        avatar3d_download_result result{};
        //log_avatar3d_debug("begin download avatar3d", user_id);
        auto meta_opt = fetch_avatar3d_metadata(user_id, &result.retryable);
        if (!meta_opt)
            return result;
        result.meta = *meta_opt;
        //log_avatar3d_debug("meta: obj=" + result.meta.obj_id + " mtl=" + result.meta.mtl_id + " textures=" + std::to_string(result.meta.textures.size()), user_id);

        auto obj_url = compute_cdn_url(result.meta.obj_id);
        auto mtl_url = compute_cdn_url(result.meta.mtl_id);
        auto obj_bytes = download_url_bytes(obj_url);
        auto mtl_bytes = download_url_bytes(mtl_url);
        if (obj_bytes.empty() || mtl_bytes.empty())
        {
            //log_avatar3d_debug("obj or mtl fetch empty (obj " + std::to_string(obj_bytes.size()) + " bytes, mtl " + std::to_string(mtl_bytes.size()) + " bytes)", user_id);
            return result;
        }

        std::string obj_data(obj_bytes.begin(), obj_bytes.end());
        std::string mtl_data(mtl_bytes.begin(), mtl_bytes.end());
        auto preview_chunk = [](const std::string& s) -> std::string
            {
                std::string out;
                const size_t len = std::min<size_t>(s.size(), 120);
                for (size_t i = 0; i < len; ++i)
                {
                    char c = s[i];
                    out.push_back((c >= 32 && c < 127) ? c : '.');
                }
                return out;
            };
        //log_avatar3d_debug("obj head: " + preview_chunk(obj_data), user_id);
        //log_avatar3d_debug("mtl head: " + preview_chunk(mtl_data), user_id);

        std::unordered_map<std::string, int> material_lookup;
        if (!parse_mtl_file(mtl_data, result.geometry.materials, material_lookup))
        {
            //log_avatar3d_debug("mtl parse failed", user_id);
            return result;
        }
        if (!parse_obj_file(obj_data, material_lookup, result.geometry))
        {
            //log_avatar3d_debug("obj parse failed (no geometry)", user_id);
            return result;
        }

        //log_avatar3d_debug("parsed geometry: verts=" + std::to_string(result.geometry.vertices.size()) + " indices=" + std::to_string(result.geometry.indices.size()) + " materials=" + std::to_string(result.geometry.materials.size()), user_id);

        std::unordered_set<std::string> texture_ids;
        for (const auto& mat : result.geometry.materials)
        {
            if (!mat.diffuse_map_id.empty())
                texture_ids.insert(mat.diffuse_map_id);
        }
        for (const auto& tex : result.meta.textures)
        {
            if (!tex.empty())
                texture_ids.insert(tex);
        }

        for (const auto& tex_id : texture_ids)
        {
            auto url = compute_cdn_url(tex_id);
            auto bytes = download_url_bytes(url);
            if (!bytes.empty())
                result.texture_bytes[tex_id] = std::move(bytes);
        }

        result.success = !result.geometry.vertices.empty() && !result.geometry.indices.empty();
        //log_avatar3d_debug(result.success ? "download avatar3d success" : "download avatar3d geometry empty", user_id);
        
        if (result.success)
        {
            if (result.meta.aabb_min == rbx::Vector3{} && result.meta.aabb_max == rbx::Vector3{})
            {
                rbx::Vector3 min_v(FLT_MAX, FLT_MAX, FLT_MAX);
                rbx::Vector3 max_v(-FLT_MAX, -FLT_MAX, -FLT_MAX);
                for (const auto& v : result.geometry.vertices)
                {
                    min_v.x = (std::min)(min_v.x, v.position.x);
                    min_v.y = (std::min)(min_v.y, v.position.y);
                    min_v.z = (std::min)(min_v.z, v.position.z);
                    max_v.x = (std::max)(max_v.x, v.position.x);
                    max_v.y = (std::max)(max_v.y, v.position.y);
                    max_v.z = (std::max)(max_v.z, v.position.z);
                }
                result.meta.aabb_min = min_v;
                result.meta.aabb_max = max_v;
                //log_avatar3d_debug("derived AABB from geometry", user_id);
            }
            if (result.meta.camera_fov <= 0.0f)
                result.meta.camera_fov = 30.0f;
            if (result.meta.camera_position == rbx::Vector3{})
            {
                rbx::Vector3 center = (result.meta.aabb_min + result.meta.aabb_max) * 0.5f;
                rbx::Vector3 size = result.meta.aabb_max - result.meta.aabb_min;
                float dist = size.Length() > 0.0f ? size.Length() * 1.6f : 12.0f;
                result.meta.camera_position = center + rbx::Vector3(0.0f, size.y * 0.2f, -dist);
                //log_avatar3d_debug("derived camera from geometry", user_id);
            }
        }
        return result;
    }

    bool ensure_avatar_preview_pipeline()
    {
        if (g_avatar_preview_pipeline.ready)
            return true;
        if (!g_pd3dDevice)
            return false;

        static const char* kPreviewVS = R"(
cbuffer SceneConstants : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 proj;
    float3 lightDir;
    float ambient;
};

struct VSInput
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

PSInput main(VSInput input)
{
    PSInput o;
    float4 worldPos = mul(float4(input.pos, 1.0f), world);
    float4 viewPos = mul(worldPos, view);
    o.pos = mul(viewPos, proj);
    float3 n = mul(float4(input.normal, 0.0f), world).xyz;
    o.normal = normalize(n);
    o.uv = input.uv;
    return o;
}
)";

        static const char* kPreviewPS = R"(
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

cbuffer SceneConstants : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 proj;
    float3 lightDir;
    float ambient;
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    float3 n = normalize(input.normal);
    float ndl = saturate(dot(n, -lightDir));
    float3 tex = tex0.Sample(samp0, input.uv).rgb;
    float lighting = ambient + ndl * (1.0f - ambient);
    return float4(tex * lighting, 1.0f);
}
)";

        ComPtr<ID3DBlob> vs_blob;
        ComPtr<ID3DBlob> ps_blob;
        ComPtr<ID3DBlob> error_blob;

        auto compile_shader = [&](const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& blob) -> bool
            {
                HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error_blob);
                if (FAILED(hr))
                {
                    
                    return false;
                }
                return true;
            };

        if (!compile_shader(kPreviewVS, "main", "vs_5_0", vs_blob))
            return false;
        if (!compile_shader(kPreviewPS, "main", "ps_5_0", ps_blob))
            return false;

        if (FAILED(g_pd3dDevice->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_avatar_preview_pipeline.vs)))
            return false;
        if (FAILED(g_pd3dDevice->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &g_avatar_preview_pipeline.ps)))
            return false;

        D3D11_INPUT_ELEMENT_DESC layout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(g_pd3dDevice->CreateInputLayout(layout, 3, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &g_avatar_preview_pipeline.layout)))
            return false;

        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(avatar_preview_constants);
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g_pd3dDevice->CreateBuffer(&cbd, nullptr, &g_avatar_preview_pipeline.constant_buffer)))
            return false;

        D3D11_SAMPLER_DESC samp{};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samp.MinLOD = 0;
        samp.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(g_pd3dDevice->CreateSamplerState(&samp, &g_avatar_preview_pipeline.sampler)))
            return false;

        D3D11_RASTERIZER_DESC rast{};
        rast.FillMode = D3D11_FILL_SOLID;
        rast.CullMode = D3D11_CULL_NONE;
        rast.DepthClipEnable = TRUE;
        rast.MultisampleEnable = FALSE;
        if (FAILED(g_pd3dDevice->CreateRasterizerState(&rast, &g_avatar_preview_pipeline.rasterizer)))
            return false;

        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(g_pd3dDevice->CreateDepthStencilState(&ds, &g_avatar_preview_pipeline.depth_state)))
            return false;

        D3D11_BLEND_DESC blend{};
        blend.AlphaToCoverageEnable = FALSE;
        blend.IndependentBlendEnable = FALSE;
        blend.RenderTarget[0].BlendEnable = FALSE;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(g_pd3dDevice->CreateBlendState(&blend, &g_avatar_preview_pipeline.blend_state)))
            return false;

        g_avatar_preview_pipeline.ready = true;
        return true;
    }

    bool create_avatar_buffers(avatar3d_entry& entry)
    {
        if (!g_pd3dDevice || entry.geometry.vertices.empty() || entry.geometry.indices.empty())
            return false;

        D3D11_BUFFER_DESC vbd{};
        vbd.ByteWidth = static_cast<UINT>(entry.geometry.vertices.size() * sizeof(avatar_vertex));
        vbd.Usage = D3D11_USAGE_DEFAULT;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vinit{};
        vinit.pSysMem = entry.geometry.vertices.data();
        if (FAILED(g_pd3dDevice->CreateBuffer(&vbd, &vinit, &entry.vertex_buffer)))
            return false;

        D3D11_BUFFER_DESC ibd{};
        ibd.ByteWidth = static_cast<UINT>(entry.geometry.indices.size() * sizeof(std::uint32_t));
        ibd.Usage = D3D11_USAGE_DEFAULT;
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA iinit{};
        iinit.pSysMem = entry.geometry.indices.data();
        if (FAILED(g_pd3dDevice->CreateBuffer(&ibd, &iinit, &entry.index_buffer)))
            return false;

        return true;
    }

    bool create_avatar_preview_target(avatar3d_entry& entry, int width, int height)
    {
        if (!g_pd3dDevice || !g_pd3dDeviceContext)
            return false;
        width = (std::max)(width, 32);
        height = (std::max)(height, 32);

        if (entry.preview_texture && entry.preview_width == width && entry.preview_height == height)
            return true;

        entry.preview_texture.Reset();
        entry.preview_rtv.Reset();
        entry.preview_srv.Reset();
        entry.preview_depth.Reset();
        entry.preview_dsv.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(g_pd3dDevice->CreateTexture2D(&td, nullptr, &entry.preview_texture)))
            return false;
        if (FAILED(g_pd3dDevice->CreateRenderTargetView(entry.preview_texture.Get(), nullptr, &entry.preview_rtv)))
            return false;
        if (FAILED(g_pd3dDevice->CreateShaderResourceView(entry.preview_texture.Get(), nullptr, &entry.preview_srv)))
            return false;

        D3D11_TEXTURE2D_DESC depth_desc{};
        depth_desc.Width = width;
        depth_desc.Height = height;
        depth_desc.MipLevels = 1;
        depth_desc.ArraySize = 1;
        depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count = 1;
        depth_desc.Usage = D3D11_USAGE_DEFAULT;
        depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        if (FAILED(g_pd3dDevice->CreateTexture2D(&depth_desc, nullptr, &entry.preview_depth)))
            return false;
        if (FAILED(g_pd3dDevice->CreateDepthStencilView(entry.preview_depth.Get(), nullptr, &entry.preview_dsv)))
            return false;

        entry.preview_width = width;
        entry.preview_height = height;
        return true;
    }

    bool build_avatar_resources(avatar3d_entry& entry, const std::unordered_map<std::string, std::vector<unsigned char>>& textures)
    {
        entry.materials.clear();
        entry.materials.reserve(entry.geometry.materials.size());

        if (entry.geometry.materials.empty())
        {
            avatar3d_material_cpu fallback{};
            fallback.name = "default";
            fallback.diffuse_color = { 1.0f,1.0f,1.0f };
            entry.geometry.materials.push_back(fallback);
        }

        for (const auto& mat : entry.geometry.materials)
        {
            avatar3d_material_gpu gpu{};
            gpu.name = mat.name;
            gpu.diffuse_map_id = mat.diffuse_map_id;
            gpu.diffuse_color = mat.diffuse_color;
            if (gpu.diffuse_map_id.empty() && !entry.meta.textures.empty())
                gpu.diffuse_map_id = entry.meta.textures.front();
            auto tex_it = textures.find(gpu.diffuse_map_id);
            if (tex_it == textures.end() && !gpu.diffuse_map_id.empty())
            {
                
                size_t slash = gpu.diffuse_map_id.find_last_of("/\\");
                if (slash != std::string::npos && slash + 1 < gpu.diffuse_map_id.size())
                {
                    std::string base = gpu.diffuse_map_id.substr(slash + 1);
                    tex_it = textures.find(base);
                }
            }
            if (tex_it != textures.end())
            {
                int w = 0, h = 0;
                ImTextureID tex = create_texture_from_png(tex_it->second, w, h);
                if (tex)
                {
                    gpu.texture.Attach(reinterpret_cast<ID3D11ShaderResourceView*>(tex));
                }
            }
            entry.materials.push_back(std::move(gpu));
        }

        
        for (auto& subset : entry.geometry.subsets)
        {
            if (subset.material_index < 0 || subset.material_index >= static_cast<int>(entry.materials.size()))
                subset.material_index = 0;
        }

        if (entry.geometry.subsets.empty())
        {
            avatar3d_mesh_subset subset{};
            subset.start_index = 0;
            subset.index_count = static_cast<std::uint32_t>(entry.geometry.indices.size());
            subset.material_index = 0;
            entry.geometry.subsets.push_back(subset);
        }

        return create_avatar_buffers(entry);
    }

    bool request_avatar3d(std::uint64_t user_id)
    {
        if (user_id == 0)
            return false;
        std::lock_guard<std::mutex> lock(g_avatar3d_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (auto it_retry = g_avatar3d_retry_after.find(user_id); it_retry != g_avatar3d_retry_after.end())
        {
            if (now < it_retry->second)
            {
                
                static std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> last_skip_log;
                auto it_last = last_skip_log.find(user_id);
                if (it_last == last_skip_log.end() || now - it_last->second > std::chrono::seconds(1))
                {
                    //log_avatar3d_debug("skip queue (backoff)", user_id);
                    last_skip_log[user_id] = now;
                }
                return false;
            }
        }
        auto it = g_avatar3d_cache.find(user_id);
        if (it != g_avatar3d_cache.end() && (it->second.state == avatar3d_state::ready || it->second.state == avatar3d_state::downloading))
            return false;
        if (g_avatar3d_pending.find(user_id) != g_avatar3d_pending.end())
        {
            //log_avatar3d_debug("skip queue (already pending)", user_id);
            return false;
        }
        //log_avatar3d_debug("queue download", user_id);
        g_avatar3d_cache[user_id].state = avatar3d_state::downloading;
        g_avatar3d_pending[user_id] = std::async(std::launch::async, [user_id]()
            {
                return download_avatar3d(user_id);
            });
        return true;
    }

    void process_avatar3d_downloads()
    {
        std::lock_guard<std::mutex> lock(g_avatar3d_mutex);
        for (auto it = g_avatar3d_pending.begin(); it != g_avatar3d_pending.end();)
        {
            if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                avatar3d_download_result res = it->second.get();
                avatar3d_entry& entry = g_avatar3d_cache[it->first];
                if (!res.success)
                {
                    if (res.retryable)
                    {
                        int& retry_count = g_avatar3d_retry_count[it->first];
                        ++retry_count;
                        constexpr int max_retries = 5;
                        if (retry_count >= max_retries)
                        {
                            if (g_avatar3d_logged_failures.insert(it->first).second)
                                log_avatar3d_debug("metadata unavailable", it->first);
                            entry.state = avatar3d_state::failed;
                            g_avatar3d_retry_after.erase(it->first);
                            g_avatar3d_retry_count.erase(it->first);
                        }
                        else
                        {
                            entry.state = avatar3d_state::not_requested;
                            const int delay_seconds = 3 * (1 << (retry_count - 1));
                            g_avatar3d_retry_after[it->first] = std::chrono::steady_clock::now() + std::chrono::seconds(delay_seconds);
                        }
                    }
                    else
                    {
                        if (g_avatar3d_logged_failures.insert(it->first).second)
                            log_avatar3d_debug("metadata unavailable", it->first);
                        entry.state = avatar3d_state::failed;
                        g_avatar3d_retry_after.erase(it->first);
                        g_avatar3d_retry_count.erase(it->first);
                    }
                    it = g_avatar3d_pending.erase(it);
                    continue;
                }

                g_avatar3d_retry_count.erase(it->first);

                entry.meta = res.meta;
                entry.geometry = std::move(res.geometry);
                entry.state = build_avatar_resources(entry, res.texture_bytes) ? avatar3d_state::ready : avatar3d_state::failed;
                //log_avatar3d_debug(entry.state == avatar3d_state::ready ? "resources built" : "resource build failed", it->first);
                if (entry.state == avatar3d_state::ready)
                    g_avatar3d_retry_after.erase(it->first);

                it = g_avatar3d_pending.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void reset_esp_preview_frame_info()
    {
        g_esp_preview_frame_info.ready = false;
        g_esp_preview_frame_info.bounds = ImRect(ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f));
        g_esp_preview_frame_info.head_pos = ImVec2(0.0f, 0.0f);
        g_esp_preview_frame_info.root_pos = ImVec2(0.0f, 0.0f);
        g_esp_preview_frame_info.dimensions = ImVec2(0.0f, 0.0f);
        g_esp_preview_frame_info.distance = 0.0f;
        g_esp_preview_frame_info.projected_points.clear();
        g_esp_preview_frame_info.subset_hulls.clear();
    }

    bool is_hair_material(const avatar3d_material_cpu& mat)
    {
        auto contains_token = [](std::string text, const char* token)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string needle(token);
            std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text.find(needle) != std::string::npos;
        };

        if (contains_token(mat.name, "hair") || contains_token(mat.name, "hat") || contains_token(mat.name, "accessory"))
            return true;
        if (contains_token(mat.diffuse_map_id, "hair") || contains_token(mat.diffuse_map_id, "hat") || contains_token(mat.diffuse_map_id, "accessory"))
            return true;
        return false;
    }

    void update_esp_preview_frame_info(const avatar3d_entry& entry, const rbx::Matrix& mvp, const avatar3d_metadata& meta, float distance, int width, int height)
    {
        reset_esp_preview_frame_info();
        if (width <= 0 || height <= 0)
            return;

        float min_x = FLT_MAX;
        float min_y = FLT_MAX;
        float max_x = -FLT_MAX;
        float max_y = -FLT_MAX;
        bool has_point = false;

        std::vector<std::optional<ImVec2>> projected_vertices(entry.geometry.vertices.size());
        std::vector<std::vector<ImVec2>> subset_projected(entry.geometry.subsets.size());
        std::vector<bool> subset_allowed(entry.geometry.subsets.size(), true);
        if (!entry.geometry.materials.empty())
        {
            for (size_t i = 0; i < entry.geometry.subsets.size(); ++i)
            {
                const auto& subset = entry.geometry.subsets[i];
                if (subset.material_index >= 0 && subset.material_index < static_cast<int>(entry.geometry.materials.size()))
                {
                    subset_allowed[i] = !is_hair_material(entry.geometry.materials[subset.material_index]);
                }
            }
        }

        auto project_point = [&](const rbx::Vector3& pos) -> std::optional<ImVec2>
        {
            auto screen = rbx::camera::world_to_screen(pos, mvp, rbx::Vector2(static_cast<float>(width), static_cast<float>(height)));
            if (!screen)
                return std::nullopt;
            return ImVec2(screen->x, screen->y);
        };

        if (!entry.geometry.indices.empty() && !entry.geometry.subsets.empty())
        {
            for (size_t si = 0; si < entry.geometry.subsets.size(); ++si)
            {
                if (!subset_allowed[si])
                    continue;
                const auto& subset = entry.geometry.subsets[si];
                if (subset.start_index + subset.index_count > entry.geometry.indices.size())
                    continue;
                auto& subset_pts = subset_projected[si];
                subset_pts.reserve(subset.index_count);
                for (uint32_t idx_i = subset.start_index; idx_i < subset.start_index + subset.index_count; ++idx_i)
                {
                    uint32_t v_idx = entry.geometry.indices[idx_i];
                    if (v_idx >= entry.geometry.vertices.size())
                        continue;
                    if (!projected_vertices[v_idx])
                        projected_vertices[v_idx] = project_point(entry.geometry.vertices[v_idx].position);
                    if (projected_vertices[v_idx])
                        subset_pts.push_back(*projected_vertices[v_idx]);
                }
            }
        }

        // If no subset filtering applied, project all vertices to keep legacy behavior
        bool any_projected = false;
        for (const auto& opt : projected_vertices)
        {
            if (opt)
            {
                any_projected = true;
                break;
            }
        }
        if (!any_projected)
        {
            for (size_t i = 0; i < entry.geometry.vertices.size(); ++i)
            {
                if (!projected_vertices[i])
                    projected_vertices[i] = project_point(entry.geometry.vertices[i].position);
            }
        }

        for (size_t i = 0; i < projected_vertices.size(); ++i)
        {
            if (!projected_vertices[i])
                continue;
            const ImVec2& screen = *projected_vertices[i];
            has_point = true;
            min_x = (std::min)(min_x, screen.x);
            min_y = (std::min)(min_y, screen.y);
            max_x = (std::max)(max_x, screen.x);
            max_y = (std::max)(max_y, screen.y);
        }

        if (!has_point)
        {
            const rbx::Vector3 min = meta.aabb_min;
            const rbx::Vector3 max = meta.aabb_max;
            for (int xi : { 0, 1 })
            {
                for (int yi : { 0, 1 })
                {
                    for (int zi : { 0, 1 })
                    {
                        rbx::Vector3 corner(
                            xi == 0 ? min.x : max.x,
                            yi == 0 ? min.y : max.y,
                            zi == 0 ? min.z : max.z);
                        if (auto screen = project_point(corner))
                        {
                            has_point = true;
                            min_x = (std::min)(min_x, screen->x);
                            min_y = (std::min)(min_y, screen->y);
                            max_x = (std::max)(max_x, screen->x);
                            max_y = (std::max)(max_y, screen->y);
                            g_esp_preview_frame_info.projected_points.push_back(*screen);
                        }
                    }
                }
            }
        }

        if (!has_point)
            return;

        min_x = ImClamp(min_x, 0.0f, static_cast<float>(width));
        min_y = ImClamp(min_y, 0.0f, static_cast<float>(height));
        max_x = ImClamp(max_x, 0.0f, static_cast<float>(width));
        max_y = ImClamp(max_y, 0.0f, static_cast<float>(height));
        if (min_x >= max_x || min_y >= max_y)
            return;

        ImRect bounds(ImVec2(min_x, min_y), ImVec2(max_x, max_y));
        const float mid_x = (bounds.Min.x + bounds.Max.x) * 0.5f;
        const float mid_z = (meta.aabb_min.z + meta.aabb_max.z) * 0.5f;
        auto head = project_point(rbx::Vector3((meta.aabb_min.x + meta.aabb_max.x) * 0.5f, meta.aabb_max.y, mid_z));
        auto root = project_point(rbx::Vector3((meta.aabb_min.x + meta.aabb_max.x) * 0.5f, meta.aabb_min.y, mid_z));
        ImVec2 fallback_center((bounds.Min.x + bounds.Max.x) * 0.5f, (bounds.Min.y + bounds.Max.y) * 0.5f);

        g_esp_preview_frame_info.ready = true;
        g_esp_preview_frame_info.bounds = bounds;
        g_esp_preview_frame_info.head_pos = head ? *head : ImVec2(fallback_center.x, bounds.Min.y);
        g_esp_preview_frame_info.root_pos = root ? *root : ImVec2(fallback_center.x, bounds.Max.y);
        g_esp_preview_frame_info.dimensions = ImVec2(static_cast<float>(width), static_cast<float>(height));
        g_esp_preview_frame_info.distance = distance;
        g_esp_preview_frame_info.projected_points.clear();
        for (size_t i = 0; i < projected_vertices.size(); ++i)
        {
            if (projected_vertices[i])
                g_esp_preview_frame_info.projected_points.push_back(*projected_vertices[i]);
        }
        g_esp_preview_frame_info.subset_hulls = std::move(subset_projected);
    }

    ImTextureID render_avatar3d(std::uint64_t user_id, const ImVec2& requested_size, avatar3d_state& out_state)
    {
        out_state = avatar3d_state::not_requested;
        reset_esp_preview_frame_info();
        if (user_id == 0 || !g_pd3dDevice || !g_pd3dDeviceContext)
        {
            //log_avatar3d_debug("render aborted: missing user or device", user_id);
            return ImTextureID{};
        }

        avatar3d_state cached_state = avatar3d_state::not_requested;
        bool needs_request = false;
        {
            std::lock_guard<std::mutex> lock(g_avatar3d_mutex);
            auto it = g_avatar3d_cache.find(user_id);
            if (it == g_avatar3d_cache.end())
            {
                needs_request = true;
                cached_state = avatar3d_state::downloading;
            }
            else
            {
                cached_state = it->second.state;
                if (cached_state == avatar3d_state::not_requested)
                {
                    needs_request = true;
                    cached_state = avatar3d_state::downloading;
                }
            }
        }

        if (needs_request)
        {
            bool queued = request_avatar3d(user_id);
            out_state = avatar3d_state::downloading;
            
            return ImTextureID{};
        }

        std::lock_guard<std::mutex> lock(g_avatar3d_mutex);
        auto it = g_avatar3d_cache.find(user_id);
        if (it == g_avatar3d_cache.end())
        {
            out_state = cached_state;
            //log_avatar3d_debug("cache missing after request, state " + std::to_string(static_cast<int>(cached_state)), user_id);
            return ImTextureID{};
        }

        avatar3d_entry& entry = it->second;
        out_state = entry.state;
        if (entry.state != avatar3d_state::ready)
        {
            //log_avatar3d_debug("render state not ready: " + std::to_string(static_cast<int>(entry.state)), user_id);
            return ImTextureID{};
        }

        if (!ensure_avatar_preview_pipeline())
        {
            entry.state = avatar3d_state::failed;
            out_state = avatar3d_state::failed;
            //log_avatar3d_debug("render pipeline creation failed", user_id);
            return ImTextureID{};
        }

        int width = static_cast<int>(ImMax(32.0f, requested_size.x));
        int height = static_cast<int>(ImMax(32.0f, requested_size.y));

        if (!create_avatar_preview_target(entry, width, height))
        {
            entry.state = avatar3d_state::failed;
            out_state = avatar3d_state::failed;
            //log_avatar3d_debug("render target creation failed", user_id);
            return ImTextureID{};
        }

        ID3D11RenderTargetView* old_rtv = nullptr;
        ID3D11DepthStencilView* old_dsv = nullptr;
        g_pd3dDeviceContext->OMGetRenderTargets(1, &old_rtv, &old_dsv);

        UINT old_vp_count = 1;
        D3D11_VIEWPORT old_vp{};
        g_pd3dDeviceContext->RSGetViewports(&old_vp_count, &old_vp);
        if (old_vp_count == 0)
        {
            old_vp_count = 1;
            old_vp = {};
        }

        D3D11_VIEWPORT vp{};
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = static_cast<float>(width);
        vp.Height = static_cast<float>(height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        g_pd3dDeviceContext->RSSetViewports(1, &vp);

        ID3D11RenderTargetView* rtv = entry.preview_rtv.Get();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &rtv, entry.preview_dsv.Get());

        const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_pd3dDeviceContext->ClearRenderTargetView(entry.preview_rtv.Get(), clear_color);
        g_pd3dDeviceContext->ClearDepthStencilView(entry.preview_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

        g_pd3dDeviceContext->IASetInputLayout(g_avatar_preview_pipeline.layout.Get());
        UINT stride = sizeof(avatar_vertex);
        UINT offset = 0;
        ID3D11Buffer* vb = entry.vertex_buffer.Get();
        g_pd3dDeviceContext->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        g_pd3dDeviceContext->IASetIndexBuffer(entry.index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        g_pd3dDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        g_pd3dDeviceContext->VSSetShader(g_avatar_preview_pipeline.vs.Get(), nullptr, 0);
        g_pd3dDeviceContext->PSSetShader(g_avatar_preview_pipeline.ps.Get(), nullptr, 0);

        ID3D11Buffer* cb = g_avatar_preview_pipeline.constant_buffer.Get();
        g_pd3dDeviceContext->VSSetConstantBuffers(0, 1, &cb);
        g_pd3dDeviceContext->PSSetConstantBuffers(0, 1, &cb);

        ID3D11SamplerState* samp = g_avatar_preview_pipeline.sampler.Get();
        g_pd3dDeviceContext->PSSetSamplers(0, 1, &samp);

        float blend_factor[4] = { 0, 0, 0, 0 };
        g_pd3dDeviceContext->OMSetBlendState(g_avatar_preview_pipeline.blend_state.Get(), blend_factor, 0xFFFFFFFF);
        g_pd3dDeviceContext->OMSetDepthStencilState(g_avatar_preview_pipeline.depth_state.Get(), 0);
        g_pd3dDeviceContext->RSSetState(g_avatar_preview_pipeline.rasterizer.Get());

        rbx::Vector3 center = (entry.meta.aabb_min + entry.meta.aabb_max) * 0.5f;
        rbx::Vector3 size = entry.meta.aabb_max - entry.meta.aabb_min;
        center.y += size.y * 0.02f;
        float time = static_cast<float>(ImGui::GetTime());
        float auto_yaw = time * 0.35f;
        float drag_yaw = g_esp_preview_drag_value.x * 0.01f;
        float drag_pitch = 0.0f;
        rbx::Matrix rot = rbx::Matrix::CreateFromYawPitchRoll(auto_yaw + drag_yaw, drag_pitch, 0.0f);
        rbx::Matrix world = rbx::Matrix::CreateTranslation(-center) * rot * rbx::Matrix::CreateTranslation(center);
        float aspect = (height > 0) ? (static_cast<float>(width) / static_cast<float>(height)) : 1.0f;
        float fov = (std::max)(12.0f, entry.meta.camera_fov - 4.0f);
        float fov_rad = DirectX::XMConvertToRadians(fov);
        float fov_horizontal = 2.0f * std::atan(std::tan(fov_rad * 0.5f) * aspect);
        float avatar_scale = std::clamp(::features->esp_preview_avatar_scale, 0.25f, 3.0f);
        float padding = 1.08f;
        rbx::Vector3 padded_size = size * padding;
        float half_height = padded_size.y * 0.5f;
        float half_width = (std::max)(padded_size.x, padded_size.z) * 0.5f;
        float dist_y = half_height > 0.0f ? half_height / std::tan(fov_rad * 0.5f) : 0.0f;
        float dist_x = half_width > 0.0f ? half_width / std::tan(fov_horizontal * 0.5f) : 0.0f;
        float base_dist = (std::max)(dist_x, dist_y);
        if (base_dist <= 0.0f)
            base_dist = 10.0f;
        float target_dist = base_dist / avatar_scale;
        float required_dist_y = padded_size.y > 0.0f ? padded_size.y / (2.0f * std::tan(fov_rad * 0.5f)) : 0.0f;
        float required_dist_x = (std::max)(padded_size.x, padded_size.z) > 0.0f ? (std::max)(padded_size.x, padded_size.z) / (2.0f * std::tan(fov_horizontal * 0.5f)) : 0.0f;
        target_dist = (std::max)(target_dist, (std::max)(required_dist_x, required_dist_y));
        if (height > 0)
        {
            float screen_offset_px = 10.0f;
            float world_offset = 2.0f * target_dist * std::tan(fov_rad * 0.5f) * (screen_offset_px / static_cast<float>(height));
            center.y -= world_offset;
        }
        float zoom_scale = std::exp(g_esp_preview_zoom_value * 0.12f);
        target_dist = target_dist * (zoom_scale > 0.001f ? zoom_scale : 1.0f);
        if (target_dist < 0.5f)
            target_dist = 0.5f;

        rbx::Vector3 forward = entry.meta.camera_direction;
        if (forward.LengthSquared() <= 1e-4f && entry.meta.camera_position != rbx::Vector3{})
            forward = center - entry.meta.camera_position;
        if (forward.LengthSquared() <= 1e-4f)
            forward = rbx::Vector3(0.0f, 0.0f, 1.0f);
        forward.Normalize();

        rbx::Vector3 up_dir(0.0f, 1.0f, 0.0f);
        rbx::Vector3 right = forward.Cross(up_dir);
        if (right.LengthSquared() < 1e-6f)
            right = rbx::Vector3(1.0f, 0.0f, 0.0f);
        right.Normalize();
        up_dir = right.Cross(forward);
        up_dir.Normalize();

        rbx::Vector3 cam_pos = center - forward * target_dist;
        cam_pos.y += size.y * 0.08f;
        cam_pos += ::features->esp_preview_camera_offset;

        rbx::Matrix view = rbx::Matrix::CreateLookAt(cam_pos, center, rbx::Vector3(0.0f, 1.0f, 0.0f));
        rbx::Matrix proj = rbx::Matrix::CreatePerspectiveFieldOfView(fov_rad, aspect, 0.1f, 500.0f);
        float preview_distance = (cam_pos - center).Length();
        rbx::Matrix mvp = (world * view * proj).Transpose();
        update_esp_preview_frame_info(entry, mvp, entry.meta, preview_distance, width, height);

        avatar_preview_constants constants{};
        constants.world = world.Transpose();
        constants.view = view.Transpose();
        constants.proj = proj.Transpose();
        constants.light_dir = rbx::Vector3(-0.25f, -1.0f, -0.35f);
        constants.light_dir.Normalize();
        constants.ambient = 0.25f;
        g_pd3dDeviceContext->UpdateSubresource(g_avatar_preview_pipeline.constant_buffer.Get(), 0, nullptr, &constants, 0, 0);

        for (const auto& subset : entry.geometry.subsets)
        {
            ID3D11ShaderResourceView* srv = nullptr;
            if (subset.material_index >= 0 && subset.material_index < static_cast<int>(entry.materials.size()))
                srv = entry.materials[subset.material_index].texture.Get();
            g_pd3dDeviceContext->PSSetShaderResources(0, 1, &srv);
            g_pd3dDeviceContext->DrawIndexed(subset.index_count, subset.start_index, 0);
        }
        ID3D11ShaderResourceView* null_srv = nullptr;
        g_pd3dDeviceContext->PSSetShaderResources(0, 1, &null_srv);

        g_pd3dDeviceContext->RSSetViewports(old_vp_count, &old_vp);
        g_pd3dDeviceContext->OMSetRenderTargets(1, &old_rtv, old_dsv);

        if (old_rtv) old_rtv->Release();
        if (old_dsv) old_dsv->Release();

        return reinterpret_cast<ImTextureID>(entry.preview_srv.Get());
    }



    void update_player_list_from_cache()
    {
        std::uint64_t selected_user_id = 0;
        std::string selected_name;
        int selected_status = 0;
        bool selected_host = false;
        if (!g_player_entries.empty() && g_selected_player_index >= 0 && g_selected_player_index < static_cast<int>(g_player_entries.size()))
        {
            selected_user_id = g_player_entries[g_selected_player_index].user_id;
            selected_name = g_player_entries[g_selected_player_index].name;
            selected_status = g_player_status_values[g_selected_player_index];
            if (g_selected_player_index < static_cast<int>(g_player_host_values.size()))
                selected_host = g_player_host_values[g_selected_player_index];
        }

        std::unordered_map<std::uint64_t, int> status_by_id;
        std::unordered_map<std::string, int> status_by_name;
        std::unordered_map<std::uint64_t, bool> host_by_id;
        std::unordered_map<std::string, bool> host_by_name;
        auto normalize_name_key = [](const std::string& value) -> std::string
        {
            std::string key = value;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            return key;
        };
        auto set_name_status = [&](const std::string& value, int status)
        {
            if (!value.empty())
            {
                status_by_name[normalize_name_key(value)] = status;
            }
        };
        auto get_name_status = [&](const std::string& value) -> std::optional<int>
        {
            if (value.empty())
            {
                return std::nullopt;
            }
            auto it = status_by_name.find(normalize_name_key(value));
            if (it == status_by_name.end())
            {
                return std::nullopt;
            }
            return it->second;
        };
        for (size_t i = 0; i < g_player_entries.size(); ++i)
        {
            int status = (i < g_player_status_values.size()) ? g_player_status_values[i] : 0;
            if (g_player_entries[i].user_id != 0)
            {
                status_by_id[g_player_entries[i].user_id] = status;
            }
            else
            {
                set_name_status(g_player_entries[i].name, status);
                set_name_status(g_player_entries[i].display_name, status);
                set_name_status(g_player_entries[i].username, status);
            }

            bool host_value = (i < g_player_host_values.size()) ? g_player_host_values[i] : false;
            if (g_player_entries[i].user_id != 0)
                host_by_id[g_player_entries[i].user_id] = host_value;
            else
                host_by_name[g_player_entries[i].name] = host_value;
        }

        g_player_entries.clear();
        g_player_status_values.clear();
        g_player_host_values.clear();
        g_selected_player_index = 0;
        clear_status_maps();
        clear_host_maps();

        const auto local = cache::localplayer->snapshot();
        if (local.address == 0)
        {
            stop_spectate();
            return;
        }

        auto local_root_pos = get_part_position(local.parts.humanoid_root_part);
        auto players_snapshot = cache::players_cache->snapshot();
        int new_selected = -1;
        int first_selectable = -1;

        {
            vanille::overlay::player_list_entry entry{};
            entry.user_id = local.user_id;
            std::string local_display = sanitize_display_name(local.name, local.name);
            if (local_display.empty())
                local_display = local.name;
            entry.display_name = local_display;
            entry.username = "Client";
            entry.name = entry.display_name;
            entry.role = "Local";
            entry.distance = 0.0f;
            entry.selectable = false;
            entry.is_local = true;
            entry.address = local.address;
            entry.character = local.character.get_address();
            entry.humanoid = get_local_humanoid_address(local);
            entry.root_part_primitive = local.parts.humanoid_root_part.primitive;
            entry.humanoid_root_part = local.parts.humanoid_root_part.instance.get_address();
            g_player_entries.push_back(entry);
            g_player_status_values.push_back(0);
            set_status_for_entry(g_player_entries.back(), 0);
            g_player_host_values.push_back(false);
            set_host_for_entry(g_player_entries.back(), false);
        }

        if (players_snapshot)
        {
            for (const auto& player : *players_snapshot)
            {
                if (player.address == local.address)
                    continue;

            vanille::overlay::player_list_entry entry{};
            entry.user_id = player.user_id;
            std::string player_display = sanitize_display_name(player.display_name, player.name);
            if (player_display.empty())
                player_display = player.name;
            entry.display_name = player_display;
            entry.username = player.name;
            if (entry.display_name.empty() && entry.username.empty())
                entry.display_name = "player";
            if (entry.username.empty())
                entry.username = entry.display_name;
            entry.name = entry.display_name;
            entry.role = "None";
            if (player.team != 0)
            {
                rbx::instance_t team(player.team);
                if (team.is_valid())
                {
                    std::string team_name = team.get_name();
                    if (!team_name.empty())
                        entry.role = team_name;
                }
            }

            if (local_root_pos)
            {
                if (auto target_pos = get_part_position(player.parts.humanoid_root_part))
                    entry.distance = (*target_pos - *local_root_pos).Length();
            }

            entry.address = player.address;
            entry.character = player.character.get_address();
            entry.humanoid = player.humanoid.get_address();
            entry.root_part_primitive = player.parts.humanoid_root_part.primitive;
            entry.humanoid_root_part = player.parts.humanoid_root_part.instance.get_address();

            g_player_entries.push_back(entry);
            if (entry.user_id != 0)
                request_avatar(entry.user_id);
            int restored_status = 0;
            bool has_restored_status = false;
            if (entry.user_id != 0)
            {
                auto it = status_by_id.find(entry.user_id);
                if (it != status_by_id.end())
                {
                    restored_status = it->second;
                    has_restored_status = true;
                }
            }
            if (!has_restored_status && entry.user_id == 0)
            {
                if (const auto by_display = get_name_status(entry.name))
                {
                    restored_status = *by_display;
                    has_restored_status = true;
                }
                else if (const auto by_username = get_name_status(entry.username))
                {
                    restored_status = *by_username;
                    has_restored_status = true;
                }
                else if (const auto by_alt_display = get_name_status(entry.display_name))
                {
                    restored_status = *by_alt_display;
                    has_restored_status = true;
                }
            }
            g_player_status_values.push_back(restored_status);
            set_status_for_entry(entry, restored_status);
            bool restored_host = false;
            if (entry.user_id != 0)
            {
                if (auto it = host_by_id.find(entry.user_id); it != host_by_id.end())
                    restored_host = it->second;
            }
            else
            {
                if (auto it = host_by_name.find(entry.name); it != host_by_name.end())
                    restored_host = it->second;
            }
            g_player_host_values.push_back(restored_host);
            set_host_for_entry(entry, restored_host);

            const bool match_user = entry.user_id != 0 && entry.user_id == selected_user_id;
            const bool match_name = !match_user && !selected_name.empty() && entry.name == selected_name;
            if (new_selected == -1 && (match_user || match_name))
            {
                new_selected = static_cast<int>(g_player_entries.size()) - 1;
                g_player_status_values.back() = selected_status;
                g_player_host_values.back() = selected_host;
                set_host_for_entry(g_player_entries.back(), selected_host);
                set_status_for_entry(g_player_entries.back(), selected_status);
            }
                if (first_selectable == -1)
                    first_selectable = static_cast<int>(g_player_entries.size()) - 1;
            }
        }

        if (!g_player_entries.empty())
        {
            if (new_selected >= 0 && new_selected < static_cast<int>(g_player_entries.size()))
                g_selected_player_index = new_selected;
            else if (first_selectable >= 0)
                g_selected_player_index = first_selectable;
            else
                g_selected_player_index = 0;
        }
    }
}

float EaseOutExpo(float t)
{
    t = ImClamp(t, 0.0f, 1.0f);
    if (t >= 1.0f)
        return 1.0f;
    return 1.0f - std::pow(2.0f, -10.0f * t);
}

void draw_custom_cursor(const ImGuiIO& io)
{
    if (!c_textures::cursor)
        return;

    HWND hwnd = vanille::overlay::g_overlay_window;
    POINT cursor_pt{};
    if (!hwnd || !::GetCursorPos(&cursor_pt) || !::ScreenToClient(hwnd, &cursor_pt))
        return;

    const ImVec2 cursor_pos(static_cast<float>(cursor_pt.x), static_cast<float>(cursor_pt.y));

    ::SetCursor(nullptr);
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    const float dpi_scale = io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;
    const float target_height = 12.0f * dpi_scale;
    float scale = (c_textures::cursor_size.y > 0.0f) ? (target_height / c_textures::cursor_size.y) : 1.0f;
    scale = ImClamp(scale, 0.014f, 1.5f);
    ImVec2 cursor_size = ImVec2(c_textures::cursor_size.x * scale, c_textures::cursor_size.y * scale);
    ImVec2 cursor_min = cursor_pos;
    ImVec2 cursor_max = ImVec2(cursor_min.x + cursor_size.x, cursor_min.y + cursor_size.y);

    ImDrawList* draw_list = ImGui::GetForegroundDrawList();

    const ImVec2 shadow_offset(1.0f, -2.0f);
    ImVec2 shadow_min = ImVec2(cursor_min.x + shadow_offset.x, cursor_min.y + shadow_offset.y);
    ImVec2 shadow_max = ImVec2(shadow_min.x + cursor_size.x, shadow_min.y + cursor_size.y);

    draw_list->AddImage(
        c_textures::cursor,
        shadow_min,
        shadow_max,
        ImVec2(0.0f, 0.0f),
        ImVec2(1.0f, 1.0f),
        ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.65f))
    );

    draw_list->AddImage(
        c_textures::cursor,
        cursor_min,
        cursor_max,
        ImVec2(0.0f, 0.0f),
        ImVec2(1.0f, 1.0f),
        ImGui::GetColorU32(c_colors::top_accent_color)
    );
}

void apply_console_visibility()
{
    static bool last_hidden = false;
    const HWND console_window = GetConsoleWindow();
    const bool target_hidden = features->hide_console;

    if (console_window && target_hidden != last_hidden)
    {
        ::ShowWindow(console_window, target_hidden ? SW_HIDE : SW_SHOW);
        last_hidden = target_hidden;
    }
}

namespace
{
    constexpr float k_loading_splash_duration = 2.0f;

    bool is_loading_splash_active(double elapsed_seconds)
    {
        return elapsed_seconds < static_cast<double>(k_loading_splash_duration);
    }

    void draw_loading_splash(double elapsed_seconds)
    {
        if (!is_loading_splash_active(elapsed_seconds))
        {
            return;
        }

        float alpha = 1.0f;
        if (elapsed_seconds > static_cast<double>(k_loading_splash_duration - 0.75f))
        {
            const float fade_t = static_cast<float>(
                (elapsed_seconds - static_cast<double>(k_loading_splash_duration - 0.75f)) / 0.75f);
            alpha = 1.0f - ImClamp(fade_t, 0.0f, 1.0f);
        }
        if (alpha <= 0.001f)
        {
            return;
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 origin = viewport->Pos;
        const ImVec2 size = viewport->Size;
        const ImVec2 bottom_right(origin.x + size.x, origin.y + size.y);

        ImGui::SetNextWindowPos(origin);
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoBringToFrontOnFocus;

        if (ImGui::Begin("##vanille_loading_splash", nullptr, flags))
        {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddRectFilled(origin, bottom_right, IM_COL32(0, 0, 0, static_cast<int>(185.0f * alpha)));

            if (g_splash_sprite_tex != 0 && g_splash_sprite_size.x > 0.0f && g_splash_sprite_size.y > 0.0f)
            {
                const ImVec2 sprite_max(bottom_right.x, bottom_right.y);
                const ImVec2 sprite_min(
                    sprite_max.x - g_splash_sprite_size.x,
                    sprite_max.y - g_splash_sprite_size.y);
                draw_list->AddImage(g_splash_sprite_tex, sprite_min, sprite_max);
            }

            const int phase = static_cast<int>(ImGui::GetTime() * 4.0) % 4;
            std::string loading_text = "Loading";
            loading_text.append(static_cast<size_t>(phase), '.');

            const float pulse = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime() * 4.0)));
            const ImVec4 accent = c_colors::top_accent_color;
            const ImU32 text_color = ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, alpha * pulse));

            ImFont* font = c_fonts::ui_title ? c_fonts::ui_title : c_fonts::verdana_bold;
            const float font_size = font ? font->LegacySize * 1.65f : ImGui::GetFontSize() * 1.65f;
            const ImVec2 text_size = font
                ? font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, loading_text.c_str())
                : ImGui::CalcTextSize(loading_text.c_str());
            const ImVec2 text_pos(
                origin.x + (size.x - text_size.x) * 0.5f,
                origin.y + (size.y - text_size.y) * 0.5f);

            if (font)
            {
                draw_list->AddText(font, font_size, text_pos, text_color, loading_text.c_str());
            }
            else
            {
                draw_list->AddText(text_pos, text_color, loading_text.c_str());
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}

void update_stream_proof_affinity(HWND hwnd, bool enable, bool force = false)
{
    static bool last_applied = false;

    if (!hwnd)
        return;

    if (!force && enable == last_applied)
        return;

    DWORD affinity = enable ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    if (!SetWindowDisplayAffinity(hwnd, affinity) && enable)
    {
        SetWindowDisplayAffinity(hwnd, WDA_MONITOR);
    }

    last_applied = enable;
}

void blurred_window(HWND hwnd, bool enable, float amount)
{
    if (!hwnd)
        return;

    struct ACCENTPOLICY
    {
        int na; 
        int nf; 
        int nc; 
        int nA; 
    };
    struct WINCOMPATTRDATA
    {
        int na; 
        PVOID pd;
        ULONG ul;
    };

    using pSetWindowCompositionAttribute = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);
    static pSetWindowCompositionAttribute set_wca = nullptr;
    static HMODULE user32 = nullptr;
    static bool attempted_load = false;

    if (!attempted_load)
    {
        user32 = LoadLibraryA("user32.dll");
        if (user32)
            set_wca = reinterpret_cast<pSetWindowCompositionAttribute>(GetProcAddress(user32, "SetWindowCompositionAttribute"));
        attempted_load = true;
    }

    if (!set_wca)
        return;

    amount = ImClamp(amount, 0.0f, 1.0f);
    if (!enable || amount <= 0.001f)
    {
        ACCENTPOLICY policy = { 0, 0, 0, 0 };
        WINCOMPATTRDATA data = { 19, &policy, sizeof(ACCENTPOLICY) };
        set_wca(hwnd, &data);
        return;
    }

    const int alpha = static_cast<int>(amount * 255.0f) & 0xFF;
    const int blur_gradient = (alpha << 24);
    ACCENTPOLICY acrylic = { 3, 0, blur_gradient, 0 };
    WINCOMPATTRDATA data = { 19, &acrylic, sizeof(ACCENTPOLICY) };
    if (!set_wca(hwnd, &data))
    {
        ACCENTPOLICY blur = { 3, 0, blur_gradient, 0 };
        WINCOMPATTRDATA data_fallback = { 19, &blur, sizeof(ACCENTPOLICY) };
        set_wca(hwnd, &data_fallback);
    }
}

static ImVec4 lighten_color(const ImVec4& col, float t)
{
    ImVec4 out = col;
    out.x = ImLerp(col.x, 1.0f, t);
    out.y = ImLerp(col.y, 1.0f, t);
    out.z = ImLerp(col.z, 1.0f, t);
    return out;
}

static ImVec4 darken_color(const ImVec4& col, float t)
{
    ImVec4 out = col;
    out.x = ImLerp(col.x, 0.0f, t);
    out.y = ImLerp(col.y, 0.0f, t);
    out.z = ImLerp(col.z, 0.0f, t);
    return out;
}

static void draw_gradient_text(const char* text, const ImVec4& left_color, const ImVec4& right_color)
{
    if (!text || !*text)
        return;

    ImFont* font = ImGui::GetFont();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 text_size = ImGui::CalcTextSize(text);

    if (!font || !draw_list || text_size.x <= 0.0f || text_size.y <= 0.0f)
    {
        ImGui::TextUnformatted(text ? text : "");
        return;
    }

    const float font_size = ImGui::GetFontSize();
    const ImVec2 text_pos = ImGui::GetCursorScreenPos();
    float cursor_offset = 0.0f;

    for (const char* c = text; *c; ++c)
    {
        const char ch[2] = { *c, 0 };
        const ImVec2 char_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, ch);
        const float t = (cursor_offset + char_size.x * 0.5f) / text_size.x;
        const ImVec4 col = ImLerp(left_color, right_color, t);
        draw_list->AddText(font, font_size, ImVec2(text_pos.x + cursor_offset, text_pos.y), ImGui::ColorConvertFloat4ToU32(col), ch);
        cursor_offset += char_size.x;
    }

    ImGui::Dummy(text_size);
}

static void draw_accent_gradient_text(const char* text)
{
    const ImVec4 left = lighten_color(c_colors::top_accent_color, 0.35f);
    const ImVec4 right = darken_color(c_colors::top_accent_color, 0.25f);
    draw_gradient_text(text, left, right);
}

static void logo_icon_uv(const ImVec2& tex_size, ImVec2& uv0, ImVec2& uv1)
{
    uv0 = ImVec2(0.0f, 0.0f);
    uv1 = ImVec2(1.0f, 1.0f);
    if (tex_size.x <= 0.0f || tex_size.y <= 0.0f)
        return;

    const float aspect = tex_size.x / tex_size.y;
    if (aspect < 1.0f)
    {
        const float h_frac = tex_size.x / tex_size.y;
        uv1.y = h_frac;
    }
    else if (aspect > 1.0f)
    {
        const float w_frac = tex_size.y / tex_size.x;
        const float margin = (1.0f - w_frac) * 0.5f;
        uv0.x = margin;
        uv1.x = 1.0f - margin;
    }
}

static bool draw_logo_icon(float size_px)
{
    if (!c_textures::logo || c_textures::logo_size.x <= 0.0f || c_textures::logo_size.y <= 0.0f)
        return false;

    ImVec2 uv0;
    ImVec2 uv1;
    logo_icon_uv(c_textures::logo_size, uv0, uv1);

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddImage(
        c_textures::logo,
        pos,
        ImVec2(pos.x + size_px, pos.y + size_px),
        uv0,
        uv1);
    ImGui::Dummy(ImVec2(size_px, size_px));
    return true;
}

static void handle_island_drag(int drag_id, const ImVec2& island_pos, const ImVec2& island_size, ImVec2& drag_offset, bool can_start_drag)
{
    const ImVec2 island_max(island_pos.x + island_size.x, island_pos.y + island_size.y);
    const bool hovered = ImGui::IsMouseHoveringRect(island_pos, island_max, false);

    if (g_island_drag_target == drag_id)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            drag_offset += ImGui::GetIO().MouseDelta;
        else
            g_island_drag_target = 0;
        return;
    }

    if (can_start_drag && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && g_island_drag_target == 0)
        g_island_drag_target = drag_id;
}

enum class MediaControlKind
{
    Previous,
    Play,
    Pause,
    Next
};

static void draw_media_control_glyph(ImDrawList* draw_list, const ImVec2& center, float half_extent, MediaControlKind kind, ImU32 color)
{
    const float h = half_extent;
    const float bar_w = ImMax(1.0f, half_extent * 0.22f);

    switch (kind)
    {
    case MediaControlKind::Previous:
        draw_list->AddRectFilled(
            ImVec2(center.x - h * 0.95f, center.y - h),
            ImVec2(center.x - h * 0.95f + bar_w, center.y + h),
            color,
            1.0f);
        draw_list->AddTriangleFilled(
            ImVec2(center.x - h * 0.45f, center.y - h),
            ImVec2(center.x - h * 0.45f, center.y + h),
            ImVec2(center.x + h * 0.75f, center.y),
            color);
        break;
    case MediaControlKind::Play:
        draw_list->AddTriangleFilled(
            ImVec2(center.x - h * 0.55f, center.y - h),
            ImVec2(center.x - h * 0.55f, center.y + h),
            ImVec2(center.x + h * 0.85f, center.y),
            color);
        break;
    case MediaControlKind::Pause:
        draw_list->AddRectFilled(
            ImVec2(center.x - h * 0.72f, center.y - h),
            ImVec2(center.x - h * 0.18f, center.y + h),
            color,
            1.5f);
        draw_list->AddRectFilled(
            ImVec2(center.x + h * 0.18f, center.y - h),
            ImVec2(center.x + h * 0.72f, center.y + h),
            color,
            1.5f);
        break;
    case MediaControlKind::Next:
        draw_list->AddTriangleFilled(
            ImVec2(center.x - h * 0.75f, center.y - h),
            ImVec2(center.x - h * 0.75f, center.y + h),
            ImVec2(center.x + h * 0.45f, center.y),
            color);
        draw_list->AddRectFilled(
            ImVec2(center.x + h * 0.95f - bar_w, center.y - h),
            ImVec2(center.x + h * 0.95f, center.y + h),
            color,
            1.0f);
        break;
    }
}

static void draw_media_control_strip(ImDrawList* draw_list, const ImVec2& min, const ImVec2& max)
{
    const float rounding = (max.y - min.y) * 0.5f;
    draw_list->AddRectFilled(min, max, ImGui::GetColorU32(c_colors::surface_inset), rounding);
    draw_list->AddRect(min, max, ImGui::GetColorU32(c_colors::main_border), rounding, 0, 1.0f);
}

static void draw_media_control_button_visual(
    ImDrawList* draw_list,
    const ImVec2& min,
    const ImVec2& max,
    MediaControlKind kind,
    bool primary,
    bool hovered,
    bool held)
{
    constexpr float kBorderThickness = 1.0f;

    ImRect outer(
        ImVec2(IM_ROUND(min.x), IM_ROUND(min.y)),
        ImVec2(IM_ROUND(max.x), IM_ROUND(max.y)));
    ImRect inner(
        ImVec2(outer.Min.x + kBorderThickness, outer.Min.y + kBorderThickness),
        ImVec2(outer.Max.x - kBorderThickness, outer.Max.y - kBorderThickness));
    ImRect fill(
        ImVec2(inner.Min.x + 1.0f, inner.Min.y + 1.0f),
        ImVec2(inner.Max.x - 1.0f, inner.Max.y - 1.0f));

    ImVec4 fill_col;
    ImVec4 border_col;
    ImVec4 glyph_col;

    if (primary)
    {
        fill_col = held ? c_colors::scale_color(c_colors::top_accent_color, 0.94f)
                        : (hovered ? c_colors::scale_color(c_colors::top_accent_color, 1.06f) : c_colors::top_accent_color);
        border_col = c_colors::top_accent_color;
        glyph_col = c_colors::accent_on;
    }
    else
    {
        fill_col = held ? c_colors::scale_color(c_colors::surface_raised, 0.92f)
                        : (hovered ? c_colors::scale_color(c_colors::surface_raised, 1.04f) : c_colors::surface_raised);
        border_col = hovered ? c_colors::border_soft : c_colors::main_border;
        glyph_col = hovered ? c_colors::white : c_colors::text_muted;
    }

    const float rounding = c_colors::widget_rounding;
    const ImDrawListFlags old_flags = draw_list->Flags;
    draw_list->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;

    draw_list->AddRect(outer.Min, outer.Max, ImGui::GetColorU32(c_colors::outter_border), rounding, 0, kBorderThickness);
    draw_list->AddRect(inner.Min, inner.Max, ImGui::GetColorU32(border_col), ImMax(0.0f, rounding - 1.0f), 0, kBorderThickness);
    draw_list->AddRectFilled(fill.Min, fill.Max, ImGui::GetColorU32(fill_col), ImMax(0.0f, rounding - 1.0f));

    const float button_size = max.y - min.y;
    const float icon_half_extent = button_size * 0.20f;
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    draw_media_control_glyph(draw_list, center, icon_half_extent, kind, ImGui::GetColorU32(glyph_col));

    draw_list->Flags = old_flags;
}

static bool media_island_pointer_state(
    const ImVec2& min,
    const ImVec2& max,
    bool lbutton_clicked,
    bool* hovered,
    bool* held,
    bool* pressed)
{
    const bool over = ImGui::IsMouseHoveringRect(min, max, false);
    if (hovered)
        *hovered = over;
    if (held)
        *held = over && ImGui::IsMouseDown(ImGuiMouseButton_Left);

    const bool click = over && lbutton_clicked;
    if (pressed)
        *pressed = click;
    return click;
}

static void draw_floating_island_background_ex(
    ImDrawList* draw_list,
    const ImVec2& pos,
    const ImVec2& size,
    float rounding,
    ImDrawFlags round_flags)
{
    const ImVec2 br(pos.x + size.x, pos.y + size.y);
    const ImU32 top_col = ImGui::GetColorU32(c_colors::top_window_background);
    const ImU32 bottom_col = ImGui::GetColorU32(c_colors::bottom_window_background);
    const ImU32 outer_border_col = ImGui::GetColorU32(c_colors::main_border);

    if (round_flags == ImDrawFlags_RoundCornersAll)
    {
        // Opaque underfill prevents bright game pixels bleeding through rounded-corner AA.
        draw_list->AddRectFilled(pos, br, top_col, rounding);
        c_colors::draw_rounded_gradient_rect(draw_list, pos, br, top_col, bottom_col, rounding);

        constexpr float border_thickness = 1.0f;
        const ImVec2 inner_min(pos.x + border_thickness, pos.y + border_thickness);
        const ImVec2 inner_max(br.x - border_thickness, br.y - border_thickness);
        const float inner_rounding = ImMax(0.0f, rounding - border_thickness);
        draw_list->AddRect(inner_min, inner_max, outer_border_col, inner_rounding, 0, border_thickness);
        return;
    }
    else if (rounding <= 0.0f)
    {
        c_colors::draw_rounded_gradient_rect(draw_list, pos, br, top_col, bottom_col, 0.0f);
    }
    else if (size.y <= rounding * 2.0f)
    {
        draw_list->AddRectFilled(pos, br, top_col, rounding, round_flags);
    }
    else
    {
        const float mid_y = (pos.y + br.y) * 0.5f;
        const ImDrawFlags top_flags = round_flags & ImDrawFlags_RoundCornersTop;
        const ImDrawFlags bottom_flags = round_flags & ImDrawFlags_RoundCornersBottom;
        if (top_flags != 0)
            draw_list->AddRectFilled(pos, ImVec2(br.x, mid_y + 0.5f), top_col, rounding, top_flags);
        else
            draw_list->AddRectFilled(pos, ImVec2(br.x, mid_y + 0.5f), top_col);
        if (bottom_flags != 0)
            draw_list->AddRectFilled(ImVec2(pos.x, mid_y - 0.5f), br, bottom_col, rounding, bottom_flags);
        else
            draw_list->AddRectFilled(ImVec2(pos.x, mid_y - 0.5f), br, bottom_col);
    }

    constexpr float border_thickness = 1.0f;
    draw_list->AddRect(pos, br, outer_border_col, rounding, round_flags, border_thickness);
}

static void draw_floating_island_background(ImDrawList* draw_list, const ImVec2& pos, const ImVec2& size, float rounding)
{
    draw_floating_island_background_ex(draw_list, pos, size, rounding, ImDrawFlags_RoundCornersAll);
}

static void draw_watermark_separator(ImDrawList* draw_list, ImFont* font, float font_size, float x, float center_y)
{
    const ImVec2 sep_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "|");
    draw_list->AddText(
        font,
        font_size,
        ImVec2(x, center_y - sep_size.y * 0.5f),
        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.22f)),
        "|");
}

static void draw_panel_background(ImDrawList* draw_list, const ImVec2& window_pos, const ImVec2& window_size, float rounding)
{
    const ImVec2 window_pos_br(window_pos.x + window_size.x, window_pos.y + window_size.y);

    ImU32 top_col = ImGui::GetColorU32(c_colors::top_window_background);
    ImU32 bottom_col = ImGui::GetColorU32(c_colors::bottom_window_background);
    ImU32 outer_border_col = ImGui::GetColorU32(c_colors::main_border);

    c_colors::draw_rounded_gradient_rect(draw_list, window_pos, window_pos_br, top_col, bottom_col, rounding);

    const float border_thickness = 1.0f;
    const ImVec2 border_offset(border_thickness, border_thickness);
    ImVec2 inner_min = ImVec2(window_pos.x + border_offset.x, window_pos.y + border_offset.y);
    ImVec2 inner_max = ImVec2(window_pos_br.x - border_offset.x, window_pos_br.y - border_offset.y);
    draw_list->AddRect(inner_min, inner_max, outer_border_col, rounding, 0, border_thickness);
}

static void draw_panel_background_at(const ImVec2& window_pos, const ImVec2& window_size, float rounding)
{
    draw_panel_background(ImGui::GetWindowDrawList(), window_pos, window_size, rounding);
}

void draw_window_background()
{
    if (ImGui::IsWindowCollapsed())
        return;

    draw_panel_background_at(ImGui::GetWindowPos(), ImGui::GetWindowSize(), c_colors::window_rounding);
}

static void draw_draggable_window_header(const char* drag_id, const char* title, bool /*use_accent_gradient*/ = true)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window)
        return;

    ImDrawList* draw_list = window->DrawList;
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 window_pos = window->Pos;
    const float window_width = window->Size.x;

    const float pad_x = 12.0f;
    const float pad_y = 9.0f;
    ImFont* font = c_fonts::verdana_bold ? c_fonts::verdana_bold : ImGui::GetFont();
    const float font_size = font->LegacySize;
    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, title);
    const float header_height = text_size.y + pad_y * 2.0f;

    const ImVec2 band_min = window_pos;
    const ImVec2 band_max(window_pos.x + window_width, window_pos.y + header_height);
    draw_list->AddRectFilled(band_min, band_max, ImGui::GetColorU32(c_colors::top_child_background),
                             c_colors::window_rounding, ImDrawFlags_RoundCornersTop);
    draw_list->AddLine(
        ImVec2(band_min.x + 1.0f, band_max.y),
        ImVec2(band_max.x - 1.0f, band_max.y),
        ImGui::GetColorU32(c_colors::main_border),
        1.0f);

    ImGui::SetCursorScreenPos(band_min);
    ImGui::InvisibleButton(drag_id, ImVec2(window_width, header_height), ImGuiButtonFlags_MouseButtonLeft);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
        ImGui::SetWindowPos(window->Pos + ImGui::GetIO().MouseDelta);

    draw_list->AddText(
        font,
        font_size,
        ImVec2(window_pos.x + pad_x, window_pos.y + pad_y),
        ImGui::GetColorU32(c_colors::white),
        title);

    ImGui::SetCursorPos(ImVec2(style.WindowPadding.x, header_height - style.WindowPadding.y));
}

void vanille::overlay::clear_player_list()
{
        g_player_entries.clear();
        g_player_status_values.clear();
        g_player_host_values.clear();
        clear_status_maps();
        clear_host_maps();
        g_selected_player_index = 0;
}

void vanille::overlay::add_player_to_list(const player_list_entry& entry)
{
    g_player_entries.push_back(entry);
    g_player_status_values.push_back(0);
    set_status_for_entry(entry, 0);
    g_player_host_values.push_back(false);
    set_host_for_entry(entry, false);
}

void vanille::overlay::set_player_list_width(float width)
{
    g_player_list_width_override = ImMax(0.0f, width);
}

void vanille::overlay::request_avatar_texture(std::uint64_t user_id)
{
    if (user_id == 0)
    {
        return;
    }

    request_avatar(user_id);
}

ImTextureID vanille::overlay::get_avatar_texture(std::uint64_t user_id, int* out_width, int* out_height)
{
    if (out_width)
    {
        *out_width = 0;
    }
    if (out_height)
    {
        *out_height = 0;
    }

    if (user_id == 0)
    {
        return ImTextureID_Invalid;
    }

    std::lock_guard<std::mutex> lock(g_avatar_mutex);
    auto it = g_avatar_cache.find(user_id);
    if (it == g_avatar_cache.end())
    {
        return ImTextureID_Invalid;
    }

    const avatar_entry& entry = it->second;
    if (entry.state != avatar_state::ready || entry.texture == 0)
    {
        return ImTextureID_Invalid;
    }

    if (out_width)
    {
        *out_width = entry.width;
    }
    if (out_height)
    {
        *out_height = entry.height;
    }

    return entry.texture;
}

void vanille::overlay::draw_player_list(const ImVec2& pos, const ImVec2& size, float alpha)
{
    float clamped_alpha = ImClamp(alpha, 0.0f, 1.0f);
    if (clamped_alpha <= 0.0f)
        return;

    ImVec2 window_size = size;
    ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver);
    if (g_player_list_width_override > 0.0f)
        window_size.x = g_player_list_width_override;
    if (window_size.x <= 0.0f)
        window_size.x = 496.0f;
    if (window_size.y <= 0.0f)
        window_size.y = 360.0f;

    ImGui::SetNextWindowSize(window_size);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, clamped_alpha);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("player_list_window", nullptr, window_flags))
    {
        draw_window_background();

        draw_draggable_window_header("##player_list_drag", "Player List", false);

        if (c_widgets::begin_padded_child("player_list_main", 0, 0, ImVec2(-1.0f, -1.0f), true, true, false, false, false))
        {
            ImVec2 list_avail = ImGui::GetContentRegionAvail();
            ImVec2 list_size = list_avail;

            ImVec2 inner_size(list_size.x, list_size.y);
            if (c_widgets::begin_padded_child("player_list_inner", 0, 0, inner_size, true, true, false, false, false))
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2.0f);
                c_widgets::section_label("Players");
                ImGui::Spacing();

                ImVec2 body_avail = ImGui::GetContentRegionAvail();
                static char player_search_buffer[64] = {};
                c_widgets::input_text("##player_search", player_search_buffer, IM_ARRAYSIZE(player_search_buffer));
                ImGui::Spacing();

                ImVec2 body_size(body_avail.x, ImMax(0.0f, body_avail.y * 0.42f));
                if (c_widgets::begin_padded_child("player_list_body", 0, 0, body_size, true, true, false, true, false))
                {
                    auto center_row_text = [&](const std::string& text, bool use_accent, bool hovered)
                    {
                        ImVec2 avail = ImGui::GetContentRegionAvail();
                        ImVec2 size = ImGui::CalcTextSize(text.c_str());
                        float offset = ImMax(0.0f, (avail.x - size.x) * 0.5f);
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
                        ImVec4 col;
                        if (use_accent)
                            col = c_colors::top_accent_color;
                        else if (hovered)
                            col = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                        else
                            col = scale_colors(ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.8f);
                        ImGui::PushStyleColor(ImGuiCol_Text, apply_alpha(col, clamped_alpha));
                        ImGui::TextUnformatted(text.c_str());
                        ImGui::PopStyleColor();
                    };

                    if (g_player_entries.empty())
                    {
                        center_row_text("No players", false, false);
                    }
                    else
                    {
                        if (g_selected_player_index >= static_cast<int>(g_player_entries.size()))
                            g_selected_player_index = static_cast<int>(g_player_entries.size()) - 1;

                        const float row_spacing = 2.0f;
                        ImGui::Dummy(ImVec2(0.0f, 2.0f));

                        std::string search_text(player_search_buffer);
                        std::transform(search_text.begin(), search_text.end(), search_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                        for (size_t i = 0; i < g_player_entries.size(); ++i)
                        {
                            const player_list_entry& entry = g_player_entries[i];

                            if (!search_text.empty())
                            {
                                std::string name_lc = entry.display_name;
                                std::string user_lc = entry.username;
                                std::transform(name_lc.begin(), name_lc.end(), name_lc.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                std::transform(user_lc.begin(), user_lc.end(), user_lc.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                                if (name_lc.find(search_text) == std::string::npos && user_lc.find(search_text) == std::string::npos)
                                {
                                    continue;
                                }
                            }

                            std::string row_label = entry.display_name.empty() ? entry.username : entry.display_name;
                            if (!entry.username.empty() && !row_label.empty() && row_label != entry.username)
                                row_label += " (" + entry.username + ")";
                            ImVec2 avail = ImGui::GetContentRegionAvail();
                            ImVec2 text_size = ImGui::CalcTextSize(row_label.c_str());
                            float row_height = text_size.y;
                            ImVec2 row_size(avail.x, row_height);
                            ImVec2 row_pos = ImGui::GetCursorScreenPos();

                            ImGui::PushID(static_cast<int>(i));
                            bool pressed = ImGui::InvisibleButton("player_row", row_size);
                            bool hovered = ImGui::IsItemHovered();
                            ImGui::PopID();

                            if (pressed && entry.selectable)
                                g_selected_player_index = static_cast<int>(i);

                            float text_x = row_pos.x + ImMax(0.0f, (row_size.x - text_size.x) * 0.5f);
                            float text_y = row_pos.y;
                            bool is_selected = entry.selectable && static_cast<int>(i) == g_selected_player_index;
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            ImVec4 col;
                            int status_value = (i < g_player_status_values.size()) ? g_player_status_values[i] : 0;
                            if (entry.is_local)
                            {
                                col = ImVec4(1.0f, 1.0f, 1.0f, 0.5f);
                            }
                            else if (status_value == 1) 
                            {
                                col = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
                            }
                            else if (status_value == 2) 
                            {
                                col = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
                            }
                            else if (is_selected)
                            {
                                col = c_colors::top_accent_color;
                            }
                            else if (hovered)
                            {
                                col = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                            }
                            else
                            {
                                col = scale_colors(ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.8f);
                            }
                            dl->AddText(ImVec2(text_x, text_y), ImGui::GetColorU32(apply_alpha(col, clamped_alpha)), row_label.c_str());

                            ImGui::Dummy(ImVec2(0.0f, row_spacing));
                        }
                    }
                }

                c_widgets::end_padded_child();

                ImGui::Spacing();

                ImVec2 info_avail = ImGui::GetContentRegionAvail();
                ImVec2 info_size(info_avail.x - 2.0f, ImMax(140.0f, info_avail.y - 6.0f));
                if (c_widgets::begin_padded_child("player_info", 0, ImGuiWindowFlags_NoScrollbar, info_size, true, true, false, false, false))
                {
                    const player_list_entry* selected_entry = nullptr;
                    if (!g_player_entries.empty() && g_selected_player_index >= 0 && g_selected_player_index < static_cast<int>(g_player_entries.size()))
                        selected_entry = &g_player_entries[g_selected_player_index];
                    ImTextureID avatar_tex = 0;
                    avatar_state avatar_tex_state = avatar_state::not_requested;
                    std::uint64_t selected_user_id = selected_entry ? selected_entry->user_id : 0;
                    if (selected_user_id != 0)
                    {
                        std::lock_guard<std::mutex> lock(g_avatar_mutex);
                        auto it = g_avatar_cache.find(selected_user_id);
                        if (it != g_avatar_cache.end())
                        {
                            avatar_tex = it->second.texture;
                            avatar_tex_state = it->second.state;
                        }
                    }
                    if (selected_user_id != 0 && avatar_tex_state == avatar_state::not_requested)
                        request_avatar(selected_user_id);

                    const ImVec2 info_region = ImGui::GetContentRegionAvail();
                    const float col_spacing = ImGui::GetStyle().ItemSpacing.x;
                    const float col0_width = IM_FLOOR(120.0f);
                    const float col2_width = IM_FLOOR(156.0f);
                    float col1_width = IM_FLOOR(ImMax(0.0f, info_region.x - col0_width - col2_width - col_spacing * 2.0f));

                    ImGui::Columns(3, nullptr, false);
                    ImGui::SetColumnWidth(0, col0_width);
                    ImGui::SetColumnWidth(2, col2_width);

                    ImVec2 avatar_size(104.0f, 104.0f);
                    ImVec2 avatar_start = ImGui::GetCursorPos();
                    ImGui::Dummy(avatar_size);
                    ImGui::SetCursorPos(avatar_start);
                    if (c_widgets::begin_padded_child("player_avatar", 0, 0, avatar_size, false, true, false, false, false))
                    {
                        if (avatar_tex)
                        {
                            ImVec2 content_pos = ImGui::GetCursorPos();
                            ImVec2 content_avail = ImGui::GetContentRegionAvail();
                            ImVec2 draw_size(
                                ImMax(0.0f, content_avail.x - 4.0f),
                                ImMax(0.0f, content_avail.y - 4.0f));
                            ImVec2 image_pos(
                                content_pos.x + ImMax(0.0f, (content_avail.x - draw_size.x) * 0.5f),
                                content_pos.y + ImMax(0.0f, (content_avail.y - draw_size.y) * 0.5f));
                            ImGui::SetCursorPos(image_pos);
                            ImGui::Image(avatar_tex, draw_size);
                        }
                        else
                        {
                            const char* label = (selected_user_id == 0) ? "None" : "Loading...";
                            if (selected_user_id != 0 && avatar_tex_state == avatar_state::failed)
                                label = "Failed";
                            ImVec2 text_size = ImGui::CalcTextSize(label);
                            ImVec2 pos = ImGui::GetCursorPos();
                            ImVec2 content_avail = ImGui::GetContentRegionAvail();
                            ImGui::SetCursorPos(ImVec2(pos.x + ImMax(0.0f, (content_avail.x - text_size.x) * 0.5f),
                                pos.y + ImMax(0.0f, (content_avail.y - text_size.y) * 0.5f)));
                            ImGui::TextUnformatted(label);
                        }
                    }
                    c_widgets::end_padded_child();
                    ImGui::NextColumn();

                    const ImGuiStyle& style = ImGui::GetStyle();
                    float button_spacing = style.ItemSpacing.y;
                    float button_height = ImGui::GetFontSize() + style.FramePadding.y * 2.0f + 28.0f;
                    float total_buttons_height = button_height * 2.0f + button_spacing;
                    float vertical_offset = ImMax(0.0f, (avatar_size.y - total_buttons_height) * 0.5f) - 3.0f;
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + vertical_offset);
                    ImVec2 button_size(IM_FLOOR(ImGui::GetContentRegionAvail().x), button_height);
                    bool has_selection = selected_entry != nullptr && selected_entry->selectable;

                    const bool teleport_enabled = has_selection && can_teleport_entry(*selected_entry);
                    ImGui::BeginDisabled(!teleport_enabled);
                    if (c_widgets::button("Teleport", button_size) && teleport_enabled)
                    {
                        teleport_to_entry(*selected_entry);
                    }
                    ImGui::EndDisabled();

                    ImGui::Spacing();

                    const bool is_target = has_selection && g_is_spectating && g_spectate_target_humanoid != 0 && selected_entry->humanoid == g_spectate_target_humanoid;
                    const bool spectate_enabled = has_selection && (is_target || can_spectate_entry(*selected_entry));
                    const char* spectate_label = is_target ? "Unspectate" : "Spectate";
                    ImGui::BeginDisabled(!spectate_enabled);
                    if (c_widgets::button(spectate_label, button_size) && spectate_enabled)
                    {
                        if (is_target)
                        {
                            stop_spectate();
                        }
                        else
                        {
                            if (start_spectate(*selected_entry))
                            {
                                
                            }
                        }
                    }
                    ImGui::EndDisabled();

                    const char* display_name = selected_entry ? selected_entry->name.c_str() : "None";

                    ImGui::NextColumn();

                    ImGui::Text("%s", display_name);
                    ImGui::Spacing();

                    ImGui::PushItemWidth(IM_FLOOR(ImGui::GetContentRegionAvail().x));
                    const char* status_options[] = { "None", "Enemy", "Friendly" };
                    int status_index = 0;
                    if (selected_entry && g_selected_player_index >= 0 && g_selected_player_index < static_cast<int>(g_player_status_values.size()))
                        status_index = g_player_status_values[g_selected_player_index];
                    c_widgets::dropdown("Status", &status_index, status_options, IM_ARRAYSIZE(status_options));
                    if (selected_entry && g_selected_player_index >= 0 && g_selected_player_index < static_cast<int>(g_player_status_values.size()))
                    {
                        g_player_status_values[g_selected_player_index] = status_index;
                        set_status_for_entry(*selected_entry, status_index);
                    }
                    if (features->enable_auto_shooter)
                    {
                        ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
                        bool host_checked = false;
                        if (selected_entry && g_selected_player_index >= 0 && g_selected_player_index < static_cast<int>(g_player_host_values.size()))
                            host_checked = g_player_host_values[g_selected_player_index];
                        if (c_widgets::checkbox("Host", &host_checked) && selected_entry && g_selected_player_index >= 0 && g_selected_player_index < static_cast<int>(g_player_host_values.size()))
                        {
                            g_player_host_values[g_selected_player_index] = host_checked;
                            set_host_for_entry(*selected_entry, host_checked);
                        }
                    }
                    ImGui::PopItemWidth();

                    ImGui::Columns(1);
                }
                c_widgets::end_padded_child();
            }
            c_widgets::end_padded_child();
        }
        c_widgets::end_padded_child();
    }
    register_current_aux_window_hittest();
    ImGui::End();

    ImGui::PopStyleVar();
}

int vanille::overlay::get_player_status(std::uint64_t user_id, const std::string& name)
{
    return get_status_for_keys(user_id, name);
}

bool vanille::overlay::is_host(std::uint64_t user_id, const std::string& name)
{
    if (!features->enable_auto_shooter)
    {
        return false;
    }
    return get_host_for_keys(user_id, name);
}

void render_lua_scripts_window()
{
    if (features->show_lua_editor_window)
    {
        lua_vm::render_editor_window(g_menu_has_frame, g_menu_last_pos, g_menu_last_size);
    }

    if (features->show_console_window)
    {
        lua_vm::render_console_window(g_menu_has_frame, g_menu_last_pos, g_menu_last_size);
    }
}

void render_configs_window()
{
    static char config_name[128] = "";
    static int selected_config = -1;

    config_manager::initialize();
    config_manager::refresh();

    auto update_selection = [&](const std::string& target)
        {
            selected_config = -1;
            if (target.empty())
                return;
            const auto& updated_configs = config_manager::get_configs();
            for (int i = 0; i < static_cast<int>(updated_configs.size()); ++i)
            {
                if (updated_configs[i] == target)
                {
                    selected_config = i;
                    break;
                }
            }
        };

    auto apply_sanitized_selection = [&](const std::string& source)
        {
            std::string sanitized = config_manager::sanitize(source);
            if (!sanitized.empty())
            {
                strncpy_s(config_name, sanitized.c_str(), _TRUNCATE);
                update_selection(sanitized);
            }
        };

    const auto& configs = config_manager::get_configs();
    if (selected_config >= static_cast<int>(configs.size()))
        selected_config = configs.empty() ? -1 : static_cast<int>(configs.size()) - 1;

    ImGui::SetNextWindowSize(ImVec2(240.0f, 280.0f), ImGuiCond_FirstUseEver);
    if (g_menu_has_frame && g_menu_last_size.x > 0.0f && g_menu_last_size.y > 0.0f)
    {
        const float gap = 12.0f;
        ImVec2 pos(g_menu_last_pos.x + g_menu_last_size.x + gap, g_menu_last_pos.y);
        ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver);
    }
    else
    {
        ImGui::SetNextWindowPos(ImVec2(860.0f, 68.0f), ImGuiCond_FirstUseEver);
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    bool opened = ImGui::Begin("Configs##aux_window", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    ImGui::PopStyleColor();
    if (opened)
    {
        draw_window_background();

        draw_draggable_window_header("##configs_drag", "Configs");

        if (c_widgets::begin_padded_child("##configs_main_child", 0, 0, ImVec2(-1.0f, -1.0f), true, true, true, true, false))
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2.0f);
            c_widgets::section_label("Configurations");

            if (c_widgets::begin_padded_child("##configs_list", 0, ImGuiWindowFlags_NoScrollbar, ImVec2(-1.0f, 142.0f), true, true, false, true, false, false, false, true))
            {
                const ImGuiStyle& style = ImGui::GetStyle();
                ImDrawList* list_draw_list = ImGui::GetWindowDrawList();
                const ImVec4 base_text = style.Colors[ImGuiCol_Text];

                auto text_color = [&](bool selected, bool hovered_item) -> ImVec4
                    {
                        if (selected)
                        {
                            ImVec4 col = c_colors::top_accent_color;
                            return hovered_item ? scale_colors(col, 1.15f) : col;
                        }
                        ImVec4 base = base_text;
                        return hovered_item ? scale_colors(base, 1.05f) : scale_colors(base, 0.75f);
                    };

                const ImGuiSelectableFlags sel_flags = ImGuiSelectableFlags_SpanAvailWidth;
                const float row_height = ImGui::GetFontSize() + style.FramePadding.y * 0.8f;

                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * 1.5f, style.ItemSpacing.y * 0.4f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));

                if (configs.empty())
                {
                    const char* label = "<no configs>";
                    ImGui::PushID(0);
                    ImGui::Selectable("##configs_entry_placeholder", false, sel_flags, ImVec2(0.0f, row_height));
                    ImRect item_bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                    ImVec2 text_size = ImGui::CalcTextSize(label);
                    ImVec2 text_pos(
                        item_bb.Min.x + (item_bb.GetWidth() - text_size.x) * 0.5f,
                        item_bb.Min.y + (item_bb.GetHeight() - text_size.y) * 0.5f
                    );
                    list_draw_list->AddText(text_pos, ImGui::GetColorU32(text_color(false, false)), label);
                    ImGui::PopID();
                    selected_config = -1;
                }
                else
                {
                    if (!configs.empty())
                    {
                        if (selected_config < 0) selected_config = 0;
                        if (selected_config >= static_cast<int>(configs.size())) selected_config = static_cast<int>(configs.size()) - 1;
                    }
                    for (int i = 0; i < static_cast<int>(configs.size()); ++i)
                    {
                        ImGui::PushID(i);
                        bool selected = selected_config == i;
                        if (ImGui::Selectable("##configs_entry", selected, sel_flags, ImVec2(0.0f, row_height)))
                        {
                            selected_config = i;
                            strncpy_s(config_name, configs[i].c_str(), _TRUNCATE);
                        }
                        ImGui::PopID();

                        ImRect item_bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                        bool hovered_item = ImGui::IsItemHovered();

                        if (selected)
                        {
                            list_draw_list->AddRectFilled(item_bb.Min, item_bb.Max,
                                                          ImGui::GetColorU32(c_colors::accent_soft),
                                                          c_colors::widget_rounding);
                        }
                        else if (hovered_item)
                        {
                            list_draw_list->AddRectFilled(item_bb.Min, item_bb.Max,
                                                          ImGui::GetColorU32(c_colors::surface_raised),
                                                          c_colors::widget_rounding);
                        }

                        ImVec2 text_size = ImGui::CalcTextSize(configs[i].c_str());
                        ImVec2 text_pos(
                            item_bb.Min.x + (item_bb.GetWidth() - text_size.x) * 0.5f,
                            item_bb.Min.y + (item_bb.GetHeight() - text_size.y) * 0.5f
                        );
                        list_draw_list->AddText(text_pos, ImGui::GetColorU32(text_color(selected, hovered_item)), configs[i].c_str());
                    }
                }

                ImGui::PopStyleColor(4);
                ImGui::PopStyleVar();
            }
            c_widgets::end_padded_child();

            c_widgets::input_text("##config_name", config_name, IM_ARRAYSIZE(config_name));

            const ImGuiStyle& style = ImGui::GetStyle();
            const float button_spacing = style.ItemSpacing.x;
            float button_row_width = ImGui::GetContentRegionAvail().x;
            float button_width = (button_row_width - button_spacing) * 0.5f;
            ImVec2 button_size(button_width, 0.0f);

            if (c_widgets::button("Load", button_size))
            {
                if (config_manager::load(config_name))
                {
                    apply_sanitized_selection(config_name);
                }
            }
            ImGui::SameLine();
            if (c_widgets::button("Save", button_size))
            {
                if (config_manager::save(config_name))
                {
                    apply_sanitized_selection(config_name);
                }
            }

            if (c_widgets::button("Create", button_size))
            {
                if (config_manager::create(config_name))
                {
                    apply_sanitized_selection(config_name);
                }
            }

            bool has_selection = !configs.empty() && selected_config >= 0 && selected_config < static_cast<int>(configs.size());

            ImGui::SameLine();
            if (c_widgets::button("Remove", button_size))
            {
                std::string to_remove;
                if (has_selection)
                    to_remove = configs[selected_config];
                else
                    to_remove = config_manager::sanitize(config_name);

                if (!to_remove.empty() && config_manager::remove(to_remove))
                {
                    const auto& updated_configs = config_manager::get_configs();
                    if (updated_configs.empty())
                    {
                        selected_config = -1;
                        config_name[0] = '\0';
                    }
                    else
                    {
                        selected_config = std::min<int>(selected_config, static_cast<int>(updated_configs.size()) - 1);
                        strncpy_s(config_name, updated_configs[selected_config].c_str(), _TRUNCATE);
                        update_selection(updated_configs[selected_config]);
                    }
                }
            }

            g_configs_last_pos = ImGui::GetWindowPos();
            g_configs_last_size = ImGui::GetWindowSize();
            g_configs_has_frame = true;
            g_aux_window_size = g_configs_last_size;

            c_widgets::end_padded_child();
        }
        else
        {
            g_configs_has_frame = false;
        }
    }
    else
    {
        g_configs_has_frame = false;
    }
    if (opened)
    {
        register_current_aux_window_hittest();
        ImGui::End();
    }
}

static ImVec2 g_appearance_last_pos(0.0f, 0.0f);
static ImVec2 g_appearance_last_size(0.0f, 0.0f);
static bool g_appearance_has_frame = false;

static ImVec2 g_esp_preview_last_pos(0.0f, 0.0f);
static ImVec2 g_esp_preview_last_size(0.0f, 0.0f);
static bool g_esp_preview_has_frame = false;

void draw_esp_preview_frame(const ImVec2& requested_size)
{
    const float min_width = 220.0f;
    const float min_height = 180.0f;
    ImVec2 size(ImMax(requested_size.x, min_width), ImMax(requested_size.y, min_height));

    ImGui::PushID("esp_preview_frame");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    if (ImGui::BeginChild("##esp_preview_canvas", size, false, ImGuiWindowFlags_NoScrollbar))
    {
        
        std::uint64_t preview_user_id = 0;
        const auto local = cache::localplayer->snapshot();
        preview_user_id = local.user_id;

        avatar3d_state state = avatar3d_state::not_requested;
        ImTextureID tex = render_avatar3d(preview_user_id, size, state);

        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 draw_size(ImMax(0.0f, avail.x), ImMax(0.0f, avail.y));
        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();

        ImTextureID fallback_tex = ImTextureID{};
        if ((!tex || state != avatar3d_state::ready) && preview_user_id != 0)
        {
            request_avatar(preview_user_id);
            fallback_tex = vanille::overlay::get_avatar_texture(preview_user_id);
        }

        if (tex && state == avatar3d_state::ready)
        {
            ImGui::Image(tex, draw_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
            if (g_esp_preview_frame_info.ready && draw_size.x > 0.0f && draw_size.y > 0.0f)
            {
                ImVec2 clip_min = canvas_pos;
                ImVec2 clip_max = ImVec2(canvas_pos.x + draw_size.x, canvas_pos.y + draw_size.y);
                ImVec2 dims = g_esp_preview_frame_info.dimensions;
                float scale_x = dims.x > 0.0f ? draw_size.x / dims.x : 1.0f;
                float scale_y = dims.y > 0.0f ? draw_size.y / dims.y : 1.0f;
                auto scale_point = [&](ImVec2 p)
                {
                    return ImVec2(canvas_pos.x + p.x * scale_x, canvas_pos.y + p.y * scale_y);
                };
                ImRect scaled_bounds(scale_point(g_esp_preview_frame_info.bounds.Min), scale_point(g_esp_preview_frame_info.bounds.Max));
                esp::esp_preview_render_info preview_info{};
                preview_info.bounds = scaled_bounds;
                preview_info.head_pos = scale_point(g_esp_preview_frame_info.head_pos);
                preview_info.root_pos = scale_point(g_esp_preview_frame_info.root_pos);
                preview_info.clip_min = clip_min;
                preview_info.clip_max = clip_max;
                preview_info.projected_points.reserve(g_esp_preview_frame_info.projected_points.size());
                for (const auto& pt : g_esp_preview_frame_info.projected_points)
                    preview_info.projected_points.push_back(scale_point(pt));
                preview_info.subset_hulls.reserve(g_esp_preview_frame_info.subset_hulls.size());
                for (const auto& hull : g_esp_preview_frame_info.subset_hulls)
                {
                    std::vector<ImVec2> scaled;
                    scaled.reserve(hull.size());
                    for (const auto& pt : hull)
                        scaled.push_back(scale_point(pt));
                    preview_info.subset_hulls.push_back(std::move(scaled));
                }
                preview_info.distance = g_esp_preview_frame_info.distance;
                float preview_health = 100.0f;
                float preview_max_health = 100.0f;
                int preview_armor = local.body_effects.armor;
                auto player_snapshot = cache::players_cache->snapshot();
                if (player_snapshot)
                {
                    for (const auto& p : *player_snapshot)
                    {
                        bool match_user = local.user_id != 0 && p.user_id == local.user_id;
                        bool match_character = local.character.is_valid() && p.character.get_address() != 0 && p.character.get_address() == local.character.get_address();
                        bool match_name = !local.name.empty() && p.name == local.name;
                        if (match_user || match_character || match_name)
                        {
                            preview_health = p.health;
                            preview_max_health = p.max_health;
                            preview_armor = p.body_effects.armor;
                            break;
                        }
                    }
                }
                preview_info.health = preview_health > 0.0f ? preview_health : 100.0f;
                preview_info.max_health = preview_max_health > 0.0f ? preview_max_health : 100.0f;
                preview_info.armor = preview_armor;
                preview_info.name = sanitize_display_name(local.display_name, local.name);
                if (preview_info.name.empty())
                    preview_info.name = "player";
                preview_info.is_host = false;
                esp::render_esp_preview(ImGui::GetWindowDrawList(), preview_info);
            }
        }
        else if (fallback_tex)
        {
            ImGui::Image(fallback_tex, draw_size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
        }
        else
        {
            const char* disabled_text = "3D avatar preview";
            if (preview_user_id == 0)
                disabled_text = "Select a player";
            else if (state == avatar3d_state::downloading)
                disabled_text = "Loading...";
            else if (state == avatar3d_state::failed)
                disabled_text = "Avatar preview unavailable";

            ImVec2 text_size = ImGui::CalcTextSize(disabled_text);
            ImVec2 cursor = ImGui::GetCursorPos();
            ImVec2 text_pos(
                cursor.x + ImMax(0.0f, (draw_size.x - text_size.x) * 0.5f),
                cursor.y + ImMax(0.0f, (draw_size.y - text_size.y) * 0.5f));
            ImGui::SetCursorPos(text_pos);
            ImGui::TextUnformatted(disabled_text);
        }
        if (state == avatar3d_state::ready)
        {
            ImGui::SetCursorScreenPos(canvas_pos);
            ImGui::InvisibleButton("##esp_preview_drag", draw_size, ImGuiButtonFlags_MouseButtonLeft);
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                g_esp_preview_drag_target.x += ImGui::GetIO().MouseDelta.x;
                g_esp_preview_drag_target.y = 0.0f;
            }
            if (ImGui::IsItemHovered())
            {
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f)
                {
                    g_esp_preview_zoom_target = ImClamp(g_esp_preview_zoom_target - wheel * 0.25f, 0.0f, 4.0f);
                }
            }
            float smooth = ImClamp(ImGui::GetIO().DeltaTime * 12.0f, 0.0f, 1.0f);
            g_esp_preview_drag_value = ImLerp(g_esp_preview_drag_value, g_esp_preview_drag_target, smooth);
            float zoom_smooth = smooth;
            g_esp_preview_zoom_value = ImLerp(g_esp_preview_zoom_value, g_esp_preview_zoom_target, zoom_smooth);
            if (g_esp_preview_zoom_value < 0.0f)
                g_esp_preview_zoom_value = 0.0f;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void draw_esp_preview_window()
{
    if (g_configs_has_frame && g_configs_last_size.x > 0.0f && g_configs_last_size.y > 0.0f)
        g_aux_window_size = g_configs_last_size;

    ImGui::SetNextWindowSize(ImVec2(360.0f, 380.0f), ImGuiCond_FirstUseEver);
    ImVec2 base_pos = ImVec2(1104.0f, 68.0f);
    ImGui::SetNextWindowPos(base_pos, ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    bool opened = ImGui::Begin("ESP Preview##aux_window", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    ImGui::PopStyleColor();
    if (opened)
    {
        process_avatar3d_downloads();
        process_avatar_downloads();
        draw_window_background();

        draw_draggable_window_header("##esp_preview_drag", "ESP Preview");

        if (c_widgets::begin_padded_child("##esp_preview_main_child", 0, 0, ImVec2(-1.0f, -1.0f), true, true, false, true, false))
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2.0f);
            c_widgets::section_label("Preview");
            ImGui::Dummy(ImVec2(0.0f, 4.0f));

            ImVec2 content_avail = ImGui::GetContentRegionAvail();
            float controls_height = ImGui::GetFrameHeightWithSpacing();
            ImVec2 preview_size = ImVec2(content_avail.x, ImMax(180.0f, content_avail.y - controls_height));
            draw_esp_preview_frame(preview_size);

        }
        c_widgets::end_padded_child();

        g_esp_preview_last_pos = ImGui::GetWindowPos();
        g_esp_preview_last_size = ImGui::GetWindowSize();
        g_esp_preview_has_frame = true;
    }
    else
    {
        g_esp_preview_has_frame = false;
    }
    register_current_aux_window_hittest();
    ImGui::End();
}

void draw_appearance_window()
{
    if (g_configs_has_frame && g_configs_last_size.x > 0.0f && g_configs_last_size.y > 0.0f)
        g_aux_window_size = g_configs_last_size;

    ImGui::SetNextWindowSize(ImVec2(240.0f, 280.0f), ImGuiCond_Always);

    ImVec2 base_pos = ImVec2(848.0f, 68.0f);
    ImGui::SetNextWindowPos(base_pos, ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    bool opened = ImGui::Begin("Appearance##aux_window", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    ImGui::PopStyleColor();
    if (opened)
    {
        ImGui::SetWindowSize(g_aux_window_size, ImGuiCond_Always);

        draw_window_background();

        draw_draggable_window_header("##appearance_drag", "Appearance");

        if (c_widgets::begin_padded_child("##appearance_main_child", 0, 0, ImVec2(-1.0f, -1.0f), true, true, false, true, false))
        {
            c_widgets::section_label("Overlay & Theme");

            ImGuiStyle& style = ImGui::GetStyle();

            c_widgets::slider_float("Shadow Size", &style.WindowShadowSize, 0.0f, 128.0f, "%.1f");
            c_widgets::slider_float("Shadow Offset Distance", &style.WindowShadowOffsetDist, 0.0f, 64.0f, "%.0f");
            c_widgets::text("Shadow Color");
            c_widgets::colorpicker("##appearance_shadow_color", style.Colors[ImGuiCol_WindowShadow], ImGui::GetFrameHeight() * 0.825f);

            c_widgets::text("Accent Color");
            if (c_widgets::colorpicker("##appearance_accent_color", c_colors::top_accent_color, ImGui::GetFrameHeight() * 0.825f))
            {
                c_colors::bottom_accent_color = c_colors::derive_bottom_accent(c_colors::top_accent_color);
            }

            c_widgets::checkbox("Menu Blur", &g_overlay_blur_enabled);

            static int last_applied_theme = -1;
            static const char* appearance_preset_names[] = { "Default", "Cherry", "Blue", "Purplish", "Gamesense", "Onetap", "Assembly", "Dracula" };
            const int theme_count = IM_ARRAYSIZE(appearance_preset_names);
            features->menu_theme = std::clamp(features->menu_theme, 0, theme_count - 1);

            if (last_applied_theme != features->menu_theme)
            {
                apply_theme_preset_internal(features->menu_theme);
                last_applied_theme = features->menu_theme;
            }

            c_widgets::text("Preset");
            c_widgets::dropdown("##appearance_theme_combo", &features->menu_theme, appearance_preset_names, theme_count);

            c_widgets::text("Menu Bind");
            ImGui::SameLine();
            c_widgets::keybind("##appearance_menu_key", c_ui::menu_key_bind());
        }
        c_widgets::end_padded_child();

        g_appearance_last_pos = ImGui::GetWindowPos();
        g_appearance_last_size = ImGui::GetWindowSize();
        g_appearance_has_frame = true;
    }
    else
    {
        g_appearance_has_frame = false;
    }
    register_current_aux_window_hittest();
    ImGui::End();
}

namespace
{
    void render_testing_explorer();
}

void draw_testing_explorer_window()
{
    ImGui::SetNextWindowSize(ImVec2(460.0f, 520.0f), ImGuiCond_FirstUseEver);
    ImVec2 base_pos = ImVec2(1104.0f, 420.0f);
    ImGui::SetNextWindowPos(base_pos, ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    bool opened = ImGui::Begin("Testing Explorer##aux_window", nullptr, ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleColor();
    if (opened)
    {
        draw_window_background();

        draw_draggable_window_header("##testing_explorer_drag", "Testing Explorer");

        if (c_widgets::begin_padded_child("##testing_explorer_child", 0, 0, ImVec2(-1.0f, -1.0f), true, true, false, true, false))
        {
            render_testing_explorer();
        }
        c_widgets::end_padded_child();
    }
    register_current_aux_window_hittest();
    ImGui::End();
}

void render_ai_chat_window()
{
    static char input[1024] = "";
    vanille::assistant::update();

    const float gap = 12.0f;
    ImVec2 window_size(360.0f, 260.0f);
    ImVec2 window_pos(980.0f, 560.0f);

    if (g_esp_preview_has_frame && g_esp_preview_last_size.x > 0.0f && g_esp_preview_last_size.y > 0.0f)
    {
        window_size.x = g_esp_preview_last_size.x;
        window_pos = ImVec2(g_esp_preview_last_pos.x, g_esp_preview_last_pos.y + g_esp_preview_last_size.y + gap);
    }
    else if (g_menu_has_frame && g_menu_last_size.x > 0.0f && g_menu_last_size.y > 0.0f)
    {
        ImVec2 pos(g_menu_last_pos.x + g_menu_last_size.x + gap, g_menu_last_pos.y + g_menu_last_size.y + gap);
        window_pos = pos;
    }

    const bool lock_to_esp_preview = g_esp_preview_has_frame && g_esp_preview_last_size.x > 0.0f && g_esp_preview_last_size.y > 0.0f;
    ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(window_pos, lock_to_esp_preview ? ImGuiCond_Always : ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    bool opened = ImGui::Begin("AI Chat##aux_window", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    ImGui::PopStyleColor();
    if (opened)
    {
        draw_window_background();

        draw_draggable_window_header("##ai_chat_drag", "AI Chat");

        if (c_widgets::begin_padded_child("##ai_chat_main_child", 0, 0, ImVec2(-1.0f, -1.0f), true, true, false, true, false))
        {
            const auto& messages = vanille::assistant::messages();
            const bool pending = vanille::assistant::pending();
            const std::string& error = vanille::assistant::error();
            const ImGuiStyle& style = ImGui::GetStyle();
            const float input_height = ImGui::GetFrameHeight();
            const float error_height = error.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing();
            float reserved_height = input_height + style.ItemSpacing.y * 2.0f + error_height;
            float log_height = ImGui::GetContentRegionAvail().y - reserved_height;
            if (log_height < 140.0f)
                log_height = 140.0f;

            if (c_widgets::begin_padded_child("##ai_chat_log", 0, ImGuiWindowFlags_NoScrollbar, ImVec2(-1.0f, log_height), true, true, false, true))
            {
                ImGui::PushTextWrapPos(0.0f);
                for (const auto& msg : messages)
                {
                    std::string line = msg.is_user ? "You: " : "";
                    line += msg.text;
                    if (msg.is_user)
                        ImGui::TextColored(c_colors::top_accent_color, "%s", line.c_str());
                    else
                        ImGui::TextWrapped("%s", line.c_str());
                    ImGui::Dummy(ImVec2(0.0f, 4.0f));
                }
                if (pending)
                {
                    ImGui::TextColored(scale_colors(ImGui::GetStyle().Colors[ImGuiCol_Text], 0.7f), "%s", "Thinking...");
                }
                ImGui::PopTextWrapPos();

                if (vanille::assistant::consume_scroll_to_bottom())
                {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            c_widgets::end_padded_child();

            const float send_width = 70.0f;
            float input_width = ImGui::GetContentRegionAvail().x - send_width - style.ItemSpacing.x;
            if (input_width < 80.0f)
                input_width = 80.0f;

            ImGui::PushItemWidth(input_width);
            bool send_enter = c_widgets::input_text("##ai_chat_input", input, IM_ARRAYSIZE(input), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopItemWidth();
            ImGui::SameLine();
            bool send_clicked = c_widgets::button_primary("Send", ImVec2(send_width, 0.0f));

            ImGui::SameLine();
            const bool clear_clicked = c_widgets::button("Clear", ImVec2(62.0f, 0.0f));

            if (clear_clicked)
            {
                vanille::assistant::clear_messages();
                vanille::assistant::clear_error();
            }

            const bool wants_send = send_enter || send_clicked;
            const bool has_input = std::strlen(input) > 0;
            if (wants_send && has_input)
            {
                const std::string input_text = input;
                if (vanille::assistant::send_message(input_text))
                    std::memset(input, 0, sizeof(input));
            }

            if (!error.empty())
            {
                c_widgets::text_colored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", error.c_str());
            }
        }
        c_widgets::end_padded_child();
    }
    register_current_aux_window_hittest();
    ImGui::End();
}

namespace c_ui
{
    c_keybind& menu_key_bind()
    {
        if (!g_menu_key_initialized)
        {
            g_menu_key.key = VK_INSERT;
            g_menu_key.type = c_keybind::TOGGLE;
            g_menu_key_initialized = true;
        }
        return g_menu_key;
    }
}

c_keybind& vanille::overlay::menu_key_bind()
{
    return c_ui::menu_key_bind();
}

void vanille::overlay::apply_theme_preset(int index)
{
    apply_theme_preset_internal(index);
}

bool& vanille::overlay::overlay_blur_enabled()
{
    return g_overlay_blur_enabled;
}

bool vanille::overlay::is_menu_open()
{
    return g_menu_open_state.load(std::memory_order_relaxed);
}

bool LoadCustomCursorTexture()
{
    if (g_custom_cursor_srv)
        return true;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = c_cursor_asset::width;
    desc.Height = c_cursor_asset::height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem = c_cursor_asset::pixels;
    init_data.SysMemPitch = c_cursor_asset::width * 4;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = g_pd3dDevice->CreateTexture2D(&desc, &init_data, &texture);
    if (FAILED(hr))
        return false;

    hr = g_pd3dDevice->CreateShaderResourceView(texture, nullptr, &g_custom_cursor_srv);
    texture->Release();
    if (FAILED(hr))
        return false;

    c_textures::cursor = (ImTextureID)g_custom_cursor_srv;
    c_textures::cursor_size = ImVec2(
        static_cast<float>(c_cursor_asset::width),
        static_cast<float>(c_cursor_asset::height)
    );
    return true;
}

bool LoadLogoTexture()
{
    if (g_logo_srv)
        return true;

    if (!g_pd3dDevice || !g_pd3dDeviceContext)
        return false;

    wchar_t module_path[MAX_PATH]{};
    const DWORD module_len = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    const std::filesystem::path exe_dir = (module_len != 0)
        ? std::filesystem::path(module_path).parent_path()
        : std::filesystem::current_path();
    const std::filesystem::path cwd = std::filesystem::current_path();

    const std::filesystem::path logo_candidates[] = {
        exe_dir / L"logo.png",
        exe_dir / L"assets" / L"logo.png",
        cwd / L"logo.png",
        cwd / L"assets" / L"logo.png",
    };

    for (const auto& candidate : logo_candidates)
    {
        if (!std::filesystem::exists(candidate))
            continue;

        int w = 0;
        int h = 0;
        ImTextureID tex = create_texture_from_file(candidate.wstring(), w, h);
        if (!tex)
            continue;

        g_logo_srv = reinterpret_cast<ID3D11ShaderResourceView*>(tex);
        c_textures::logo = tex;
        c_textures::logo_size = ImVec2(static_cast<float>(w), static_cast<float>(h));
        return true;
    }

    std::vector<unsigned char> data(logo_asset::vanille_png, logo_asset::vanille_png + logo_asset::vanille_png_len);

    int w = 0;
    int h = 0;
    ImTextureID tex = create_texture_from_png(data, w, h);
    if (!tex)
        return false;

    g_logo_srv = reinterpret_cast<ID3D11ShaderResourceView*>(tex);
    c_textures::logo = tex;
    c_textures::logo_size = ImVec2(static_cast<float>(w), static_cast<float>(h));
    return true;
}

bool LoadSplashSpriteTexture()
{
    if (g_splash_sprite_srv)
    {
        return true;
    }

    if (!g_pd3dDevice || !g_pd3dDeviceContext)
    {
        return false;
    }

    wchar_t module_path[MAX_PATH]{};
    const DWORD module_len = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    const std::filesystem::path exe_dir = (module_len != 0)
        ? std::filesystem::path(module_path).parent_path()
        : std::filesystem::current_path();
    const std::filesystem::path cwd = std::filesystem::current_path();

    const std::filesystem::path splash_candidates[] = {
        exe_dir / L"assets" / L"splash_sprite.png",
        exe_dir / L"splash_sprite.png",
        cwd / L"assets" / L"splash_sprite.png",
        cwd / L"splash_sprite.png",
    };

    for (const auto& candidate : splash_candidates)
    {
        if (!std::filesystem::exists(candidate))
        {
            continue;
        }

        int w = 0;
        int h = 0;
        ImTextureID tex = create_texture_from_file(candidate.wstring(), w, h);
        if (!tex)
        {
            continue;
        }

        g_splash_sprite_srv = reinterpret_cast<ID3D11ShaderResourceView*>(tex);
        g_splash_sprite_tex = tex;
        g_splash_sprite_size = ImVec2(static_cast<float>(w), static_cast<float>(h));
        return true;
    }

    std::vector<unsigned char> data(
        splash_asset::splash_sprite_png,
        splash_asset::splash_sprite_png + splash_asset::splash_sprite_png_len);

    int w = 0;
    int h = 0;
    ImTextureID tex = create_texture_from_png(data, w, h);
    if (!tex)
    {
        return false;
    }

    g_splash_sprite_srv = reinterpret_cast<ID3D11ShaderResourceView*>(tex);
    g_splash_sprite_tex = tex;
    g_splash_sprite_size = ImVec2(static_cast<float>(w), static_cast<float>(h));
    return true;
}

bool LoadGrenadeTexture()
{
    if (g_grenade_icon_srv)
        return true;

    if (!g_pd3dDevice || !g_pd3dDeviceContext)
        return false;

    std::vector<unsigned char> data(
        grenade_icon_asset::grenade_png,
        grenade_icon_asset::grenade_png + grenade_icon_asset::grenade_png_len);

    int w = 0;
    int h = 0;
    ImTextureID tex = create_texture_from_png(data, w, h);
    if (!tex)
        return false;

    g_grenade_icon_srv = reinterpret_cast<ID3D11ShaderResourceView*>(tex);
    c_textures::grenade_icon = tex;
    c_textures::grenade_icon_size = ImVec2(static_cast<float>(w), static_cast<float>(h));
    return true;
}

bool LoadDeathImageTexture()
{
    if (g_death_image_srv)
        return true;

    if (!g_pd3dDevice)
        return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = IMAGE_WIDTH;
    desc.Height = IMAGE_HEIGHT;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem = image_data;
    init_data.SysMemPitch = IMAGE_WIDTH * 4;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = g_pd3dDevice->CreateTexture2D(&desc, &init_data, &texture);
    if (FAILED(hr))
        return false;

    hr = g_pd3dDevice->CreateShaderResourceView(texture, nullptr, &g_death_image_srv);
    texture->Release();
    if (FAILED(hr))
        return false;

    c_textures::death_image = (ImTextureID)g_death_image_srv;
    c_textures::death_image_size = ImVec2(static_cast<float>(IMAGE_WIDTH), static_cast<float>(IMAGE_HEIGHT));
    return true;
}

void ClearDeathImageCustomTexture()
{
    if (g_death_image_custom_srv)
    {
        g_death_image_custom_srv->Release();
        g_death_image_custom_srv = nullptr;
    }

    c_textures::death_image_custom = 0;
    c_textures::death_image_custom_size = ImVec2(0.0f, 0.0f);
}

bool LoadDeathImageCustomTexture(const std::wstring& path)
{
    ClearDeathImageCustomTexture();

    int w = 0;
    int h = 0;
    ImTextureID tex = create_texture_from_file(path, w, h);
    if (!tex)
    {
        return false;
    }

    g_death_image_custom_srv = reinterpret_cast<ID3D11ShaderResourceView*>(tex);
    c_textures::death_image_custom = tex;
    c_textures::death_image_custom_size = ImVec2(static_cast<float>(w), static_cast<float>(h));
    return true;
}

void render_keybinds_window()
{
    struct keybind_row_entry
    {
        const char* label;
        const c_keybind* bind;
        bool show;
    };

    const keybind_row_entry rows[] = {
        { "Aimbot",      &::features->aimbot_keybind,          ::features->enable_aimbot },
        { "Triggerbot",  &::features->triggerbot_keybind,      ::features->enable_triggerbot },
        { "Silent Aim",  &::features->free_aim_keybind,        ::features->enable_free_aim },
        { "Fly",         &::features->fly_keybind,             ::features->enable_fly },
        { "Walkspeed",   &::features->walkspeed_keybind,       ::features->enable_walkspeed },
        { "Bhop",        &::features->bhop_keybind,            ::features->enable_bhop },
        { "Noclip",      &::features->noclip_keybind,          ::features->enable_noclip },
        { "Freeze",      &::features->freeze_players_keybind,  ::features->freeze_players },
        { "Desync",      &::features->desync_keybind,          ::features->desync },
        { "Aim Trace",   &::features->aim_trace_keybind,       ::features->enable_aim_trace },
    };

    ImFont* body_font = c_fonts::verdana_regular ? c_fonts::verdana_regular : ImGui::GetFont();
    const float body_font_size = body_font->LegacySize;
    const float row_h = body_font_size + 8.0f;

    float max_label_w = 0.0f;
    float max_key_w = 0.0f;
    int visible_rows = 0;

    for (const keybind_row_entry& row : rows)
    {
        if (!row.show)
            continue;

        ++visible_rows;
        std::string key_str = row.bind->get_key_name();
        if (key_str.empty())
            key_str = "-";
        else
            std::transform(key_str.begin(), key_str.end(), key_str.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        max_label_w = (std::max)(max_label_w, body_font->CalcTextSizeA(body_font_size, FLT_MAX, 0.0f, row.label).x);
        max_key_w = (std::max)(max_key_w, body_font->CalcTextSizeA(body_font_size, FLT_MAX, 0.0f, key_str.c_str()).x);
    }

    if (visible_rows == 0)
        return;

    ImFont* header_font = c_fonts::verdana_bold ? c_fonts::verdana_bold : ImGui::GetFont();
    const float header_h = header_font->CalcTextSizeA(header_font->LegacySize, FLT_MAX, 0.0f, "Keybinds").y + 18.0f;

    constexpr float child_margin = 2.0f;
    constexpr float child_padding = 8.0f;
    constexpr float column_gap = 16.0f;
    const float content_w = max_label_w + column_gap + max_key_w;
    const float window_w = std::clamp(content_w + (child_margin + child_padding) * 2.0f + 8.0f, 170.0f, 220.0f);
    const float window_h = header_h + (child_margin * 2.0f) + (child_padding * 2.0f) + visible_rows * row_h + 4.0f;

    const ImVec2 margin(28.0f, 28.0f);
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const ImVec2 pos(margin.x, screen.y - margin.y);
    ImGui::SetNextWindowSize(ImVec2(window_w, window_h), ImGuiCond_Always);
    ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver, ImVec2(0.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    const bool opened = ImGui::Begin("Keybinds##aux_window", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    if (!opened)
    {
        ImGui::End();
        return;
    }

    draw_window_background();
    draw_draggable_window_header("##keybinds_drag", "Keybinds", false);

    if (c_widgets::begin_padded_child("##keybinds_child", 0, ImGuiWindowFlags_NoScrollbar, ImVec2(-1.0f, -1.0f), true, true, false, true, false))
    {
        ImGui::PushFont(body_font);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f));

        if (ImGui::BeginTable("##keybinds_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
        {
            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch, 0.62f);
            ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthStretch, 0.38f);

            for (const keybind_row_entry& row : rows)
            {
                if (!row.show)
                    continue;

                std::string key_str = row.bind->get_key_name();
                if (key_str.empty())
                    key_str = "-";
                else
                    std::transform(key_str.begin(), key_str.end(), key_str.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                const bool active = row.bind->enabled;
                const ImVec4 label_col = active ? c_colors::top_accent_color : c_colors::text_muted;
                const ImVec4 key_col = (key_str == "-")
                    ? c_colors::text_muted
                    : c_colors::top_accent_color;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(label_col, "%s", row.label);
                ImGui::TableSetColumnIndex(1);
                const float col_width = ImGui::GetColumnWidth();
                const float text_width = body_font->CalcTextSizeA(body_font_size, FLT_MAX, 0.0f, key_str.c_str()).x;
                const float offset = (col_width > text_width) ? (col_width - text_width) : 0.0f;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
                ImGui::TextColored(key_col, "%s", key_str.c_str());
            }

            ImGui::EndTable();
        }

        ImGui::PopStyleVar();
        ImGui::PopFont();
    }
    c_widgets::end_padded_child();

    register_current_aux_window_hittest();
    ImGui::End();
}

void render_watermark_window()
{
    const auto local = cache::localplayer->snapshot();
    const std::string player_name = !local.display_name.empty()
        ? local.display_name
        : (!local.name.empty() ? local.name : "unknown");
    const float fps = ImGui::GetIO().Framerate;

    const float logo_px = 26.0f;
    const float section_gap = 12.0f;
    const float sep_gap = 9.0f;
    const ImVec2 padding(16.0f, 9.0f);
    const bool has_logo = (c_textures::logo != 0 && c_textures::logo_size.y > 0.0f);

    ImFont* title_font = c_fonts::verdana_bold ? c_fonts::verdana_bold : ImGui::GetFont();
    ImFont* body_font = c_fonts::verdana_regular ? c_fonts::verdana_regular : ImGui::GetFont();
    const float title_font_size = title_font->LegacySize;
    const float body_font_size = body_font->LegacySize;

    const ImVec2 title_size = title_font->CalcTextSizeA(title_font_size, FLT_MAX, 0.0f, "vanille");
    const ImVec2 sep_size = body_font->CalcTextSizeA(body_font_size, FLT_MAX, 0.0f, "|");
    const ImVec2 name_size = body_font->CalcTextSizeA(body_font_size, FLT_MAX, 0.0f, player_name.c_str());

    const ImVec2 fps_slot_size = body_font->CalcTextSizeA(body_font_size, FLT_MAX, 0.0f, "999 fps");
    char fps_buffer[16] = {};
    ImFormatString(fps_buffer, IM_ARRAYSIZE(fps_buffer), "%.0f fps", fps);
    const ImVec2 fps_size = body_font->CalcTextSizeA(body_font_size, FLT_MAX, 0.0f, fps_buffer);

    const float content_height = (std::max)(has_logo ? logo_px : 0.0f, (std::max)(title_size.y, (std::max)(name_size.y, fps_size.y)));
    const float content_width =
        (has_logo ? logo_px + section_gap : 0.0f)
        + title_size.x + section_gap
        + sep_size.x + sep_gap + name_size.x + sep_gap + sep_size.x + section_gap
        + fps_slot_size.x;
    const ImVec2 island_size(content_width + padding.x * 2.0f, content_height + padding.y * 2.0f);
    const float island_rounding = island_size.y * 0.5f;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 island_pos(
        viewport->WorkPos.x + (viewport->WorkSize.x - island_size.x) * 0.5f + g_watermark_island_offset.x,
        viewport->WorkPos.y + 10.0f + g_watermark_island_offset.y);

    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    draw_floating_island_background(draw_list, island_pos, island_size, island_rounding);
    handle_island_drag(1, island_pos, island_size, g_watermark_island_offset, true);

    const float row_center_y = island_pos.y + padding.y + content_height * 0.5f;
    float cursor_x = island_pos.x + padding.x;

    if (has_logo)
    {
        ImVec2 uv0;
        ImVec2 uv1;
        logo_icon_uv(c_textures::logo_size, uv0, uv1);
        const float logo_y = row_center_y - logo_px * 0.5f;
        draw_list->AddImage(
            c_textures::logo,
            ImVec2(cursor_x, logo_y),
            ImVec2(cursor_x + logo_px, logo_y + logo_px),
            uv0,
            uv1);
        cursor_x += logo_px + section_gap;
    }

    draw_list->AddText(
        title_font,
        title_font_size,
        ImVec2(cursor_x, row_center_y - title_size.y * 0.5f),
        ImGui::GetColorU32(c_colors::top_accent_color),
        "vanille");
    cursor_x += title_size.x + section_gap;

    draw_watermark_separator(draw_list, body_font, body_font_size, cursor_x, row_center_y);
    cursor_x += sep_size.x + sep_gap;

    draw_list->AddText(
        body_font,
        body_font_size,
        ImVec2(cursor_x, row_center_y - name_size.y * 0.5f),
        ImGui::GetColorU32(c_colors::text_muted),
        player_name.c_str());
    cursor_x += name_size.x + sep_gap;

    draw_watermark_separator(draw_list, body_font, body_font_size, cursor_x, row_center_y);
    cursor_x += sep_size.x + section_gap;

    const float fps_x = cursor_x + fps_slot_size.x - fps_size.x;
    draw_list->AddText(
        body_font,
        body_font_size,
        ImVec2(fps_x, row_center_y - fps_size.y * 0.5f),
        ImGui::GetColorU32(c_colors::text_muted),
        fps_buffer);
}

static void destroy_media_art_texture()
{
    if (g_media_art_srv)
    {
        g_media_art_srv->Release();
        g_media_art_srv = nullptr;
    }
    g_media_art_revision = 0;
}

static void process_media_art_texture()
{
    const auto snap = vanille::media::get_snapshot();
    if (snap.art_revision == g_media_art_revision)
        return;

    g_media_art_revision = snap.art_revision;
    destroy_media_art_texture();
    if (snap.art_bytes.empty())
        return;

    int width = 0;
    int height = 0;
    const ImTextureID tex = create_texture_from_png(snap.art_bytes, width, height);
    g_media_art_srv = reinterpret_cast<ID3D11ShaderResourceView*>(tex);
}

static std::string ellipsize_text(ImFont* font, float font_size, const std::string& text, float max_width)
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

static void draw_island_icon_button_visual(
    ImDrawList* draw_list,
    const ImVec2& min,
    const ImVec2& max,
    const char* label,
    ImFont* font,
    float font_size,
    bool hovered)
{
    if (hovered)
        draw_list->AddRectFilled(min, max, IM_COL32(255, 255, 255, 18), 6.0f);

    const ImVec2 label_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label);
    draw_list->AddText(
        font,
        font_size,
        ImVec2(min.x + (max.x - min.x - label_size.x) * 0.5f, min.y + (max.y - min.y - label_size.y) * 0.5f),
        ImGui::GetColorU32(hovered ? c_colors::white : c_colors::text_muted),
        label);
}

static float smoothstep01(float t)
{
    t = ImClamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float get_media_lyrics_section_height()
{
    return k_media_lyrics_full_height * smoothstep01(g_lyrics_panel_open_anim);
}

static ImFont* media_font_regular()
{
    return c_fonts::media_regular ? c_fonts::media_regular : (c_fonts::verdana_regular ? c_fonts::verdana_regular : ImGui::GetFont());
}

static ImFont* media_font_bold()
{
    return c_fonts::media_bold ? c_fonts::media_bold : (c_fonts::verdana_bold ? c_fonts::verdana_bold : ImGui::GetFont());
}

static ImFont* media_font_lyrics_active()
{
    return c_fonts::media_lyrics_active ? c_fonts::media_lyrics_active : media_font_bold();
}

static ImFont* media_font_lyrics_inactive()
{
    return c_fonts::media_lyrics_inactive ? c_fonts::media_lyrics_inactive : media_font_regular();
}

static ImFont* media_font_caption()
{
    return c_fonts::media_caption ? c_fonts::media_caption : media_font_regular();
}

static void format_media_timestamp(char* buffer, size_t buffer_size, double seconds, bool remaining)
{
    if (seconds < 0.0)
        seconds = 0.0;
    const int total = static_cast<int>(seconds + 0.5);
    const int minutes = total / 60;
    const int secs = total % 60;
    if (remaining)
        ImFormatString(buffer, buffer_size, "-%d:%02d", minutes, secs);
    else
        ImFormatString(buffer, buffer_size, "%d:%02d", minutes, secs);
}

static void draw_media_progress_section(
    ImDrawList* draw_list,
    const ImRect& lyrics_rect,
    const vanille::media::snapshot& snap,
    double playback_seconds)
{
    ImFont* caption_font = media_font_caption();
    const float caption_size = c_fonts::media_caption ? c_fonts::media_caption_size : 12.0f;
    constexpr float pad_x = 16.0f;
    constexpr float section_bottom_pad = 12.0f;
    constexpr float bar_h = 3.0f;

    const float section_top = lyrics_rect.Max.y - k_media_lyrics_progress_height;
    const float bar_y = section_top + 8.0f;
    const float bar_x0 = lyrics_rect.Min.x + pad_x;
    const float bar_x1 = lyrics_rect.Max.x - pad_x;
    const float progress = snap.duration_seconds > 0.0
        ? ImClamp(static_cast<float>(playback_seconds / snap.duration_seconds), 0.0f, 1.0f)
        : 0.0f;
    const float fill_x = ImLerp(bar_x0, bar_x1, progress);

    draw_list->AddRectFilled(
        ImVec2(bar_x0, bar_y),
        ImVec2(bar_x1, bar_y + bar_h),
        IM_COL32(255, 255, 255, 42),
        bar_h * 0.5f);
    if (fill_x > bar_x0)
    {
        draw_list->AddRectFilled(
            ImVec2(bar_x0, bar_y),
            ImVec2(fill_x, bar_y + bar_h),
            IM_COL32(255, 255, 255, 235),
            bar_h * 0.5f);
    }

    char elapsed_text[16]{};
    char remaining_text[16]{};
    format_media_timestamp(elapsed_text, IM_ARRAYSIZE(elapsed_text), playback_seconds, false);
    if (snap.duration_seconds > 0.0)
        format_media_timestamp(remaining_text, IM_ARRAYSIZE(remaining_text), snap.duration_seconds - playback_seconds, true);

    const float time_y = bar_y + bar_h + 8.0f;
    const ImU32 time_col = IM_COL32(255, 255, 255, 170);
    draw_list->AddText(caption_font, caption_size, ImVec2(bar_x0, time_y), time_col, elapsed_text);
    if (remaining_text[0] != '\0')
    {
        const ImVec2 remaining_size = caption_font->CalcTextSizeA(caption_size, FLT_MAX, 0.0f, remaining_text);
        draw_list->AddText(
            caption_font,
            caption_size,
            ImVec2(bar_x1 - remaining_size.x, time_y),
            time_col,
            remaining_text);
    }
}

static float get_media_lyrics_scroll_height()
{
    return k_media_lyrics_full_height - k_media_lyrics_progress_height;
}

static std::string make_media_track_key(const vanille::media::snapshot& snap)
{
    return snap.title + '\x1f' + snap.artist;
}

static int find_active_lyrics_line_index(
    const std::vector<vanille::media::lyrics_line>& lines,
    double playback_seconds)
{
    if (lines.empty())
        return -1;

    int current = 0;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i)
    {
        if (lines[static_cast<size_t>(i)].time_seconds <= playback_seconds)
            current = i;
        else
            break;
    }
    return current;
}

static float ease_out_cubic(float t)
{
    const float u = 1.0f - ImClamp(t, 0.0f, 1.0f);
    return 1.0f - u * u * u;
}

static bool lyrics_position_seeked(double position_seconds)
{
    if (g_lyrics_last_snap_position < 0.0)
        return false;
    return std::abs(position_seconds - g_lyrics_last_snap_position) > k_lyrics_seek_threshold_seconds;
}

static double advance_lyrics_playback_seconds(const vanille::media::snapshot& snap, float /*dt*/)
{
    if (!snap.active || snap.title.empty())
    {
        g_lyrics_clock = {};
        g_lyrics_last_snap_position = -1.0;
        return snap.position_seconds;
    }

    const std::string media_key = make_media_track_key(snap);
    const bool track_changed = !g_lyrics_clock.initialized || media_key != g_lyrics_clock.media_key;
    const bool seeked = g_lyrics_clock.initialized && !track_changed && lyrics_position_seeked(snap.position_seconds);
    const auto now = std::chrono::steady_clock::now();

    if (track_changed || seeked)
    {
        g_lyrics_clock.media_key = media_key;
        g_lyrics_clock.anchor_position = snap.position_seconds;
        g_lyrics_clock.anchor_time = now;
        g_lyrics_clock.initialized = true;
        g_lyrics_last_snap_position = snap.position_seconds;
        return snap.position_seconds;
    }

    if (!snap.is_playing)
    {
        g_lyrics_clock.anchor_position = snap.position_seconds;
        g_lyrics_clock.anchor_time = now;
        g_lyrics_last_snap_position = snap.position_seconds;
        return snap.position_seconds;
    }

    const double elapsed = std::chrono::duration<double>(now - g_lyrics_clock.anchor_time).count();
    double playback_seconds = g_lyrics_clock.anchor_position + elapsed;

    if (snap.position_seconds > playback_seconds + k_lyrics_forward_resync_seconds)
    {
        g_lyrics_clock.anchor_position = snap.position_seconds;
        g_lyrics_clock.anchor_time = now;
        playback_seconds = snap.position_seconds;
    }

    if (snap.duration_seconds > 0.0)
        playback_seconds = (std::min)(playback_seconds, snap.duration_seconds);

    g_lyrics_last_snap_position = snap.position_seconds;
    return playback_seconds;
}

static void reset_lyrics_scroll_state()
{
    g_lyrics_display_line = -1;
    g_lyrics_scroll_line = 0.0f;
    g_lyrics_scroll_anim_from = 0.0f;
    g_lyrics_scroll_anim_start = -1.0;
    g_lyrics_media_key.clear();
    g_lyrics_last_snap_position = -1.0;
    g_lyrics_scroll_track_key.clear();
    g_lyrics_clock = {};
}

static int stabilize_lyrics_active_line(int active_line, const vanille::media::snapshot& snap, bool force_snap)
{
    if (active_line < 0)
        return g_lyrics_display_line;

    if (force_snap || g_lyrics_display_line < 0)
        return active_line;

    if (!snap.is_playing)
        return active_line;

    if (active_line >= g_lyrics_display_line)
        return active_line;

    if (g_lyrics_display_line - active_line == 1)
        return g_lyrics_display_line;

    return active_line;
}

static int update_lyrics_display_line(
    int active_line,
    const vanille::media::snapshot& snap,
    bool force_snap)
{
    const int target_line = stabilize_lyrics_active_line(active_line, snap, force_snap);
    if (target_line < 0)
        return g_lyrics_display_line;

    const std::string media_key = make_media_track_key(snap);
    const bool track_changed = !media_key.empty() && media_key != g_lyrics_media_key;
    if (track_changed)
        g_lyrics_media_key = media_key;

    if (track_changed || force_snap || g_lyrics_display_line < 0)
    {
        g_lyrics_display_line = target_line;
        g_lyrics_scroll_line = static_cast<float>(target_line);
        g_lyrics_scroll_anim_from = static_cast<float>(target_line);
        g_lyrics_scroll_anim_start = -1.0;
        return g_lyrics_display_line;
    }

    if (target_line == g_lyrics_display_line)
        return g_lyrics_display_line;

    g_lyrics_scroll_anim_from = g_lyrics_scroll_line;
    g_lyrics_scroll_anim_start = ImGui::GetTime();
    g_lyrics_display_line = target_line;
    return g_lyrics_display_line;
}

static void update_lyrics_scroll_line()
{
    if (g_lyrics_display_line < 0)
        return;

    const float target = static_cast<float>(g_lyrics_display_line);
    if (g_lyrics_scroll_anim_start < 0.0)
    {
        g_lyrics_scroll_line = target;
        return;
    }

    const float elapsed = static_cast<float>(ImGui::GetTime() - g_lyrics_scroll_anim_start);
    const float t = ImClamp(elapsed / k_lyrics_scroll_anim_duration, 0.0f, 1.0f);
    g_lyrics_scroll_line = ImLerp(g_lyrics_scroll_anim_from, target, ease_out_cubic(t));
    if (t >= 1.0f)
    {
        g_lyrics_scroll_line = target;
        g_lyrics_scroll_anim_start = -1.0;
    }
}

static media_island_layout compute_media_island_layout(const vanille::media::snapshot& snap)
{
    constexpr float art_px = 40.0f;
    constexpr float section_gap = 12.0f;
    constexpr float button_px = 28.0f;
    constexpr float button_gap = 6.0f;
    constexpr float strip_pad = 4.0f;
    constexpr float lyrics_btn_px = 24.0f;
    constexpr float lyrics_gap = 8.0f;
    constexpr ImVec2 padding(12.0f, 10.0f);
    const float lyrics_h = get_media_lyrics_section_height();
    const bool lyrics_expanded = lyrics_h > 0.5f;
    const float text_column_w = lyrics_expanded ? 228.0f : 190.0f;

    ImFont* title_font = media_font_bold();
    ImFont* body_font = media_font_regular();
    const float title_font_size = c_fonts::media_bold ? c_fonts::media_bold_size : title_font->LegacySize;
    const float body_font_size = c_fonts::media_regular ? c_fonts::media_regular_size : body_font->LegacySize;

    const std::string title_text = snap.active && !snap.title.empty()
        ? snap.title
        : (snap.is_spotify ? "Not playing" : "No media");
    const std::string artist_text = snap.active && !snap.artist.empty()
        ? snap.artist
        : (snap.is_spotify ? "Spotify" : (snap.app_name.empty() ? "Waiting for Spotify" : snap.app_name));

    const std::string title_draw = ellipsize_text(title_font, title_font_size, title_text, text_column_w);
    const std::string artist_draw = ellipsize_text(body_font, body_font_size, artist_text, text_column_w);
    const ImVec2 title_size = title_font->CalcTextSizeA(title_font_size, FLT_MAX, 0.0f, title_draw.c_str());
    const ImVec2 artist_size = body_font->CalcTextSizeA(body_font_size, FLT_MAX, 0.0f, artist_draw.c_str());
    const float text_block_h = title_size.y + 3.0f + artist_size.y;

    const float strip_h = button_px + strip_pad * 2.0f;
    const float strip_w = strip_pad * 2.0f + button_px * 3.0f + button_gap * 2.0f;
    const float content_height = (std::max)(art_px, (std::max)(text_block_h, strip_h));
    const float content_width = art_px + section_gap + text_column_w + section_gap + strip_w + lyrics_gap + lyrics_btn_px;
    ImVec2 island_size(content_width + padding.x * 2.0f, content_height + padding.y * 2.0f);
    if (lyrics_expanded)
        island_size.x = (std::max)(island_size.x, k_media_lyrics_expanded_min_width);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float island_y = viewport->WorkPos.y + 10.0f + g_media_island_offset.y;
    if (::features->show_watermark)
        island_y += 48.0f;
    const ImVec2 island_pos(
        viewport->WorkPos.x + (viewport->WorkSize.x - island_size.x) * 0.5f + g_media_island_offset.x,
        island_y);

    const float row_center_y = island_pos.y + padding.y + content_height * 0.5f;
    float cursor_x = island_pos.x + padding.x + art_px + section_gap + text_column_w + section_gap;
    const float strip_y = row_center_y - strip_h * 0.5f;
    const ImVec2 strip_min(cursor_x, strip_y);
    const ImVec2 strip_max(cursor_x + strip_w, strip_y + strip_h);
    cursor_x += strip_w + lyrics_gap;
    const float lyrics_y = row_center_y - lyrics_btn_px * 0.5f;
    const ImVec2 lyrics_min(cursor_x, lyrics_y);
    const ImVec2 lyrics_max(cursor_x + lyrics_btn_px, lyrics_y + lyrics_btn_px);

    media_island_layout layout;
    layout.island_pos = island_pos;
    layout.player_size = island_size;
    layout.island_size = island_size;

    if (lyrics_h > 0.5f)
    {
        layout.island_size.y += lyrics_h;
        layout.lyrics_rect = ImRect(
            ImVec2(island_pos.x, island_pos.y + island_size.y),
            ImVec2(island_pos.x + island_size.x, island_pos.y + island_size.y + lyrics_h));
    }

    layout.strip_rect = ImRect(strip_min, strip_max);
    layout.lyrics_button_rect = ImRect(lyrics_min, lyrics_max);
    layout.interactive_rect = ImRect(strip_min, lyrics_max);
    return layout;
}

static void update_media_island_hittest_rects(HWND hwnd, const media_island_layout& layout)
{
    g_media_island_client_rect = screen_rect_to_client_rect(
        hwnd,
        layout.island_pos,
        ImVec2(layout.island_pos.x + layout.island_size.x, layout.island_pos.y + layout.island_size.y));
    g_media_island_hittest_active = true;
    g_media_lyrics_panel_hittest_active = false;
}

static void render_media_lyrics_content(
    ImDrawList* draw_list,
    const ImRect& lyrics_rect,
    const vanille::media::snapshot& snap,
    const vanille::media::lyrics_snapshot& lyrics,
    float dt)
{
    if (lyrics_rect.GetHeight() < 1.0f)
        return;

    constexpr float pad_x = 16.0f;
    ImFont* active_font = media_font_lyrics_active();
    ImFont* inactive_font = media_font_lyrics_inactive();
    ImFont* message_font = media_font_regular();
    const float active_size = c_fonts::media_lyrics_active ? c_fonts::media_lyrics_active_size : 24.0f;
    const float inactive_size = c_fonts::media_lyrics_inactive ? c_fonts::media_lyrics_inactive_size : 18.0f;
    const float message_size = c_fonts::media_regular ? c_fonts::media_regular_size : message_font->LegacySize;
    const float line_step = active_size + 14.0f;
    const float scroll_viewport_h = get_media_lyrics_scroll_height();
    const float max_text_w = lyrics_rect.GetWidth() - pad_x * 2.0f;
    const bool seeked = lyrics_position_seeked(snap.position_seconds);
    const double playback_seconds = advance_lyrics_playback_seconds(snap, dt);
    const ImRect scroll_rect(
        lyrics_rect.Min,
        ImVec2(lyrics_rect.Max.x, lyrics_rect.Max.y - k_media_lyrics_progress_height));

    draw_list->AddLine(
        ImVec2(scroll_rect.Min.x + pad_x, scroll_rect.Min.y),
        ImVec2(scroll_rect.Max.x - pad_x, scroll_rect.Min.y),
        IM_COL32(255, 255, 255, 18),
        1.0f);

    const auto draw_centered_message = [&](const char* message)
    {
        const ImVec2 text_size = message_font->CalcTextSizeA(message_size, FLT_MAX, 0.0f, message);
        draw_list->AddText(
            message_font,
            message_size,
            ImVec2(
                scroll_rect.Min.x + (scroll_rect.GetWidth() - text_size.x) * 0.5f,
                scroll_rect.Min.y + (scroll_rect.GetHeight() - text_size.y) * 0.5f),
            IM_COL32(255, 255, 255, 120),
            message);
        draw_media_progress_section(draw_list, lyrics_rect, snap, playback_seconds);
    };

    if (lyrics.track_key != g_lyrics_scroll_track_key)
    {
        g_lyrics_scroll_track_key = lyrics.track_key;
        g_lyrics_display_line = -1;
        g_lyrics_scroll_line = 0.0f;
        g_lyrics_scroll_anim_from = 0.0f;
        g_lyrics_scroll_anim_start = -1.0;
    }

    if (lyrics.state == vanille::media::lyrics_state::loading)
    {
        draw_centered_message("Loading lyrics...");
        return;
    }
    if (lyrics.state == vanille::media::lyrics_state::failed)
    {
        draw_centered_message("Could not fetch lyrics.");
        return;
    }
    if (lyrics.state == vanille::media::lyrics_state::not_found)
    {
        draw_centered_message("No lyrics found.");
        return;
    }

    std::vector<std::string> display_lines;
    const bool has_synced = lyrics.has_synced && !lyrics.synced_lines.empty();
    if (has_synced)
    {
        display_lines.reserve(lyrics.synced_lines.size());
        for (const vanille::media::lyrics_line& line : lyrics.synced_lines)
            display_lines.push_back(line.text);
    }
    else if (!lyrics.plain_lines.empty())
    {
        display_lines = lyrics.plain_lines;
    }
    else
    {
        draw_centered_message("No lyrics available.");
        return;
    }

    int display_line = 0;
    if (has_synced && !lyrics.synced_lines.empty())
    {
        const int active_line = find_active_lyrics_line_index(lyrics.synced_lines, playback_seconds);
        display_line = update_lyrics_display_line(active_line, snap, !snap.is_playing || seeked);
    }
    else
    {
        const int plain_line_count = static_cast<int>(display_lines.size());
        int current_plain_line = 0;
        if (plain_line_count > 0 && snap.duration_seconds > 0.0)
        {
            const float progress = static_cast<float>(playback_seconds / snap.duration_seconds);
            current_plain_line = ImClamp(
                static_cast<int>(progress * static_cast<float>(plain_line_count)),
                0,
                plain_line_count - 1);
        }
        display_line = update_lyrics_display_line(current_plain_line, snap, !snap.is_playing || seeked);
    }

    update_lyrics_scroll_line();

    draw_list->PushClipRect(scroll_rect.Min, scroll_rect.Max, true);
    const float center_y = scroll_rect.Min.y + scroll_viewport_h * 0.5f;

    for (int i = 0; i < static_cast<int>(display_lines.size()); ++i)
    {
        const float line_center_y = center_y + (static_cast<float>(i) - g_lyrics_scroll_line) * line_step;
        if (line_center_y < scroll_rect.Min.y - line_step || line_center_y > scroll_rect.Max.y + line_step)
            continue;

        const bool is_current = i == display_line;
        ImFont* line_font = is_current ? active_font : inactive_font;
        const float line_font_size = is_current ? active_size : inactive_size;
        const std::string text = ellipsize_text(line_font, line_font_size, display_lines[static_cast<size_t>(i)], max_text_w);
        const ImVec2 text_size = line_font->CalcTextSizeA(line_font_size, FLT_MAX, 0.0f, text.c_str());

        const float dist = ImAbs(line_center_y - center_y);
        const float fade_span = scroll_viewport_h * 0.42f;
        const float fade = ImLerp(1.0f, 0.22f, ImClamp(dist / fade_span, 0.0f, 1.0f));
        const ImU32 color = is_current
            ? IM_COL32(255, 255, 255, static_cast<int>(255.0f * fade))
            : IM_COL32(255, 255, 255, static_cast<int>(108.0f * fade));

        draw_list->AddText(
            line_font,
            line_font_size,
            ImVec2(scroll_rect.Min.x + pad_x, line_center_y - text_size.y * 0.5f),
            color,
            text.c_str());
    }

    draw_list->PopClipRect();
    draw_media_progress_section(draw_list, lyrics_rect, snap, playback_seconds);
}

void render_media_player_widget()
{
    process_media_art_texture();
    const float dt = ImGui::GetIO().DeltaTime;
    const float target_anim = g_media_lyrics_open ? 1.0f : 0.0f;
    g_lyrics_panel_open_anim += (target_anim - g_lyrics_panel_open_anim) * ImMin(1.0f, dt * 10.0f);
    if (!g_media_lyrics_open && g_lyrics_panel_open_anim < 0.01f)
        reset_lyrics_scroll_state();

    const auto snap = vanille::media::get_snapshot();
    const auto lyrics = vanille::media::get_lyrics();
    const media_island_layout layout = compute_media_island_layout(snap);

    constexpr float art_px = 40.0f;
    constexpr float section_gap = 12.0f;
    constexpr float button_px = 28.0f;
    constexpr float button_gap = 6.0f;
    constexpr float strip_pad = 4.0f;
    constexpr ImVec2 padding(12.0f, 10.0f);
    const float lyrics_h = get_media_lyrics_section_height();
    const bool lyrics_expanded = lyrics_h > 0.5f;
    const float text_column_w = lyrics_expanded ? 228.0f : 190.0f;

    ImFont* title_font = media_font_bold();
    ImFont* body_font = media_font_regular();
    const float title_font_size = c_fonts::media_bold ? c_fonts::media_bold_size : title_font->LegacySize;
    const float body_font_size = c_fonts::media_regular ? c_fonts::media_regular_size : body_font->LegacySize;

    const std::string title_text = snap.active && !snap.title.empty()
        ? snap.title
        : (snap.is_spotify ? "Not playing" : "No media");
    const std::string artist_text = snap.active && !snap.artist.empty()
        ? snap.artist
        : (snap.is_spotify ? "Spotify" : (snap.app_name.empty() ? "Waiting for Spotify" : snap.app_name));

    const std::string title_draw = ellipsize_text(title_font, title_font_size, title_text, text_column_w);
    const std::string artist_draw = ellipsize_text(body_font, body_font_size, artist_text, text_column_w);
    const ImVec2 title_size = title_font->CalcTextSizeA(title_font_size, FLT_MAX, 0.0f, title_draw.c_str());
    const ImVec2 artist_size = body_font->CalcTextSizeA(body_font_size, FLT_MAX, 0.0f, artist_draw.c_str());
    const float text_block_h = title_size.y + 3.0f + artist_size.y;
    const float strip_h = button_px + strip_pad * 2.0f;
    const float content_height = (std::max)(art_px, (std::max)(text_block_h, strip_h));

    const ImVec2 island_pos = layout.island_pos;
    const ImVec2 island_size = layout.island_size;
    const ImVec2 player_size = layout.player_size;
    constexpr float card_rounding = 14.0f;
    const bool lyrics_visible = lyrics_h > 0.5f;
    const float background_rounding = lyrics_visible ? card_rounding : player_size.y * 0.5f;
    const ImVec2 background_size = lyrics_visible ? island_size : player_size;

    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    draw_floating_island_background(draw_list, island_pos, background_size, background_rounding);

    const float row_center_y = island_pos.y + padding.y + content_height * 0.5f;
    float cursor_x = island_pos.x + padding.x;

    const float art_y = row_center_y - art_px * 0.5f;
    const ImVec2 art_min(cursor_x, art_y);
    const ImVec2 art_max(cursor_x + art_px, art_y + art_px);
    if (g_media_art_srv)
    {
        draw_list->AddImageRounded(
            reinterpret_cast<ImTextureID>(g_media_art_srv),
            art_min,
            art_max,
            ImVec2(0.0f, 0.0f),
            ImVec2(1.0f, 1.0f),
            IM_COL32(255, 255, 255, 255),
            8.0f);
    }
    else
    {
        draw_list->AddRectFilled(art_min, art_max, IM_COL32(28, 28, 30, 255), 8.0f);
        draw_list->AddRect(art_min, art_max, ImGui::GetColorU32(c_colors::main_border), 8.0f);
        const char* note = snap.is_spotify ? "♪" : "♫";
        const ImVec2 note_size = body_font->CalcTextSizeA(body_font_size + 4.0f, FLT_MAX, 0.0f, note);
        draw_list->AddText(
            body_font,
            body_font_size + 4.0f,
            ImVec2(art_min.x + (art_px - note_size.x) * 0.5f, art_min.y + (art_px - note_size.y) * 0.5f),
            ImGui::GetColorU32(c_colors::text_muted),
            note);
    }
    cursor_x += art_px + section_gap;

    const float text_top = row_center_y - text_block_h * 0.5f;
    draw_list->AddText(
        title_font,
        title_font_size,
        ImVec2(cursor_x, text_top),
        IM_COL32(255, 255, 255, 245),
        title_draw.c_str());
    draw_list->AddText(
        body_font,
        body_font_size,
        ImVec2(cursor_x, text_top + title_size.y + 3.0f),
        IM_COL32(255, 255, 255, 150),
        artist_draw.c_str());
    cursor_x += text_column_w + section_gap;

    const ImVec2 strip_min = layout.strip_rect.Min;
    const ImVec2 strip_max = layout.strip_rect.Max;
    draw_media_control_strip(draw_list, strip_min, strip_max);

    const float btn_y = strip_min.y + strip_pad;
    float btn_x = strip_min.x + strip_pad;
    const ImVec2 prev_min(btn_x, btn_y);
    const ImVec2 prev_max(btn_x + button_px, btn_y + button_px);
    btn_x += button_px + button_gap;
    const ImVec2 play_min(btn_x, btn_y);
    const ImVec2 play_max(btn_x + button_px, btn_y + button_px);
    btn_x += button_px + button_gap;
    const ImVec2 next_min(btn_x, btn_y);
    const ImVec2 next_max(btn_x + button_px, btn_y + button_px);
    const ImVec2 lyrics_min = layout.lyrics_button_rect.Min;
    const ImVec2 lyrics_max = layout.lyrics_button_rect.Max;

    static bool prev_lbutton_down = false;
    const bool lbutton_down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool lbutton_clicked = lbutton_down && !prev_lbutton_down;
    prev_lbutton_down = lbutton_down;

    bool prev_hovered = false;
    bool prev_held = false;
    bool prev_pressed = false;
    media_island_pointer_state(prev_min, prev_max, lbutton_clicked, &prev_hovered, &prev_held, &prev_pressed);
    draw_media_control_button_visual(
        draw_list,
        prev_min,
        prev_max,
        MediaControlKind::Previous,
        false,
        prev_hovered,
        prev_held);

    bool play_hovered = false;
    bool play_held = false;
    bool play_pressed = false;
    media_island_pointer_state(play_min, play_max, lbutton_clicked, &play_hovered, &play_held, &play_pressed);
    draw_media_control_button_visual(
        draw_list,
        play_min,
        play_max,
        snap.is_playing ? MediaControlKind::Pause : MediaControlKind::Play,
        true,
        play_hovered,
        play_held);

    bool next_hovered = false;
    bool next_held = false;
    bool next_pressed = false;
    media_island_pointer_state(next_min, next_max, lbutton_clicked, &next_hovered, &next_held, &next_pressed);
    draw_media_control_button_visual(
        draw_list,
        next_min,
        next_max,
        MediaControlKind::Next,
        false,
        next_hovered,
        next_held);

    bool lyrics_hovered = false;
    bool lyrics_held = false;
    bool lyrics_pressed = false;
    media_island_pointer_state(lyrics_min, lyrics_max, lbutton_clicked, &lyrics_hovered, &lyrics_held, &lyrics_pressed);
    draw_island_icon_button_visual(
        draw_list,
        lyrics_min,
        lyrics_max,
        g_media_lyrics_open ? "✕" : "♪",
        body_font,
        body_font_size - 1.0f,
        lyrics_hovered);

    if (prev_pressed)
        vanille::media::skip_previous();
    if (play_pressed)
        vanille::media::toggle_play_pause();
    if (next_pressed)
        vanille::media::skip_next();
    if (lyrics_pressed)
        g_media_lyrics_open = !g_media_lyrics_open;

    const bool over_interactive = ImGui::IsMouseHoveringRect(layout.interactive_rect.Min, layout.interactive_rect.Max, false);
    handle_island_drag(2, island_pos, island_size, g_media_island_offset, !over_interactive);

    if (lyrics_visible)
    {
        const ImRect lyrics_rect(
            ImVec2(island_pos.x, island_pos.y + player_size.y),
            ImVec2(island_pos.x + island_size.x, island_pos.y + island_size.y));
        render_media_lyrics_content(draw_list, lyrics_rect, snap, lyrics, dt);
    }

    if (vanille::overlay::g_overlay_window)
    {
        lua_vm::register_aux_window_hittest_rect(island_pos, island_size, vanille::overlay::g_overlay_window);
        update_media_island_hittest_rects(vanille::overlay::g_overlay_window, layout);
    }
}

void DestroyCustomCursorTexture()
{
    if (g_custom_cursor_srv)
    {
        g_custom_cursor_srv->Release();
        g_custom_cursor_srv = nullptr;
    }

    c_textures::cursor = 0;
    c_textures::cursor_size = ImVec2(0.0f, 0.0f);
}

void DestroyLogoTexture()
{
    if (g_logo_srv)
    {
        g_logo_srv->Release();
        g_logo_srv = nullptr;
    }

    c_textures::logo = 0;
    c_textures::logo_size = ImVec2(0.0f, 0.0f);
}

void DestroySplashSpriteTexture()
{
    if (g_splash_sprite_srv)
    {
        g_splash_sprite_srv->Release();
        g_splash_sprite_srv = nullptr;
    }

    g_splash_sprite_tex = 0;
    g_splash_sprite_size = ImVec2(0.0f, 0.0f);
}

void DestroyGrenadeTexture()
{
    if (g_grenade_icon_srv)
    {
        g_grenade_icon_srv->Release();
        g_grenade_icon_srv = nullptr;
    }

    c_textures::grenade_icon = 0;
    c_textures::grenade_icon_size = ImVec2(0.0f, 0.0f);
}

void DestroyDeathImageTexture()
{
    if (g_death_image_srv)
    {
        g_death_image_srv->Release();
        g_death_image_srv = nullptr;
    }

    c_textures::death_image = 0;
    c_textures::death_image_size = ImVec2(0.0f, 0.0f);

    ClearDeathImageCustomTexture();
}

namespace
{
    struct explorer_node
    {
        rbx::instance_t instance;
        std::string name;
        std::string class_name;
        std::vector<explorer_node> children;
    };

    static std::vector<explorer_node> g_explorer_tree;
    static rbx::instance_t g_explorer_selected_instance{};
    static std::uintptr_t g_explorer_selected_address = 0;
    static std::size_t g_explorer_node_count = 0;
    static bool g_explorer_initialized = false;
    static std::uintptr_t g_explorer_details_address = 0;
    static std::size_t g_explorer_details_children = 0;
    static std::size_t g_explorer_details_descendants = 0;
    static bool g_explorer_details_valid = false;
    struct explorer_build_result
    {
        std::vector<explorer_node> tree;
        std::size_t node_count = 0;
    };
    static std::future<explorer_build_result> g_explorer_refresh_future;
    static bool g_explorer_refresh_inflight = false;
    static double g_explorer_last_refresh_request_time = -1.0;
    static std::string g_explorer_last_export_message;

    std::string explorer_label_or_fallback(const std::string& text, const char* fallback)
    {
        return text.empty() ? std::string(fallback) : text;
    }

    std::string explorer_sanitize_label(std::string text)
    {
        if (text.empty())
        {
            return {};
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

        constexpr std::size_t k_max_label_len = 96;
        if (out.size() > k_max_label_len)
        {
            out.resize(k_max_label_len);
        }
        return out;
    }

    bool explorer_is_part_class(const std::string& class_name)
    {
        if (class_name.empty())
            return false;

        std::string lowered = class_name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered == "part" || lowered == "meshpart" || lowered == "basepart";
    }

    bool explorer_is_mesh_part_class(const std::string& class_name)
    {
        if (class_name.empty())
            return false;

        std::string lowered = class_name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered == "meshpart";
    }

    bool explorer_is_special_mesh_class(const std::string& class_name)
    {
        if (class_name.empty())
            return false;

        std::string lowered = class_name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered == "specialmesh";
    }

    std::optional<std::string> explorer_read_special_mesh_id(const rbx::instance_t& instance)
    {
        if (!instance.is_valid())
            return std::nullopt;

        if (!roblox::offsets::mesh_part::special_mesh_id)
            return std::nullopt;

        const std::uintptr_t mesh_field = instance.get_address() + roblox::offsets::mesh_part::special_mesh_id;

        try
        {
            std::string value;
            const auto content_ptr = memory->read<std::uintptr_t>(mesh_field);
            if (content_ptr)
            {
                char buffer[512] = {};
                if (memory->read_raw(buffer, content_ptr, sizeof(buffer) - 1))
                    value = buffer;
            }

            if (value.empty())
            {
                value = memory->read_string(mesh_field);
            }

            if (value.empty() || value == "Unknown")
                return std::nullopt;

            if (const auto pos = value.find("id="); pos != std::string::npos)
                value = value.substr(pos + 3);

            return value;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::size_t explorer_count_descendants(const explorer_node& node)
    {
        std::size_t total = node.children.size();
        for (const auto& child : node.children)
        {
            total += explorer_count_descendants(child);
        }
        return total;
    }

    bool explorer_find_node_counts(const explorer_node& node, std::uintptr_t address, std::size_t& children_out, std::size_t& descendants_out)
    {
        if (node.instance.get_address() == address)
        {
            children_out = node.children.size();
            descendants_out = explorer_count_descendants(node);
            return true;
        }

        for (const auto& child : node.children)
        {
            if (explorer_find_node_counts(child, address, children_out, descendants_out))
                return true;
        }
        return false;
    }

    void explorer_update_selected_counts()
    {
        if (g_explorer_selected_address == 0 || !g_explorer_selected_instance.is_valid())
        {
            g_explorer_details_valid = false;
            return;
        }

        if (g_explorer_details_address == g_explorer_selected_address && g_explorer_details_valid)
            return;

        g_explorer_details_address = g_explorer_selected_address;
        g_explorer_details_valid = false;

        for (const auto& root : g_explorer_tree)
        {
            if (explorer_find_node_counts(root, g_explorer_selected_address, g_explorer_details_children, g_explorer_details_descendants))
            {
                g_explorer_details_valid = true;
                break;
            }
        }

        if (!g_explorer_details_valid)
        {
            try
            {
                g_explorer_details_children = g_explorer_selected_instance.get_children().size();
                g_explorer_details_descendants = g_explorer_selected_instance.get_descendants().size();
                g_explorer_details_valid = true;
            }
            catch (...)
            {
                g_explorer_details_valid = false;
            }
        }
    }

    std::optional<ImU32> explorer_read_part_color(const rbx::instance_t& instance)
    {
        if (!instance.is_valid() || !roblox::offsets::base_part::color3)
            return std::nullopt;

        try
        {
            std::uint32_t packed = memory->read<std::uint32_t>(instance.get_address() + roblox::offsets::base_part::color3);
            packed &= 0x00FFFFFFu;
            const std::uint8_t r = static_cast<std::uint8_t>(packed & 0xFF);
            const std::uint8_t g = static_cast<std::uint8_t>((packed >> 8) & 0xFF);
            const std::uint8_t b = static_cast<std::uint8_t>((packed >> 16) & 0xFF);
            return IM_COL32(r, g, b, 255);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    rbx::instance_t explorer_get_parent(const rbx::instance_t& instance)
    {
        if (!instance.is_valid() || !roblox::offsets::instance::parent)
        {
            return {};
        }

        try
        {
            const std::uintptr_t parent_ptr = memory->read<std::uintptr_t>(instance.get_address() + roblox::offsets::instance::parent);
            if (!parent_ptr)
            {
                return {};
            }
            return rbx::instance_t(parent_ptr);
        }
        catch (...)
        {
            return {};
        }
    }

    std::optional<rbx::instance_t> explorer_find_player_for_character_model(const rbx::instance_t& model)
    {
        if (!model.is_valid() || !globals->players.is_valid())
        {
            return std::nullopt;
        }

        std::string model_class;
        std::string model_name;
        try
        {
            model_class = model.get_class_name();
            model_name = model.get_name();
        }
        catch (...)
        {
            return std::nullopt;
        }

        if (model_class != "Model" || model_name.empty())
        {
            return std::nullopt;
        }

        const std::uintptr_t model_address = model.get_address();
        try
        {
            const auto players_children = globals->players.get_children();
            for (const auto& child : players_children)
            {
                if (!child.is_valid())
                {
                    continue;
                }

                if (child.get_class_name() != "Player")
                {
                    continue;
                }

                if (child.get_name() == model_name)
                {
                    return child;
                }

                if (const auto character = rbx::player::get_character(child))
                {
                    if (character->is_valid() && character->get_address() == model_address)
                    {
                        return child;
                    }
                }
            }
        }
        catch (...)
        {
            return std::nullopt;
        }

        return std::nullopt;
    }

    std::optional<std::uint32_t> explorer_read_team_team_color(const rbx::instance_t& team)
    {
        if (!team.is_valid() || !roblox::offsets::team::team_color)
        {
            return std::nullopt;
        }

        try
        {
            return memory->read<std::uint32_t>(team.get_address() + roblox::offsets::team::team_color);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<sdk::math::color3> explorer_read_textlabel_text_color(const rbx::instance_t& instance)
    {
        if (!instance.is_valid())
        {
            return std::nullopt;
        }

        try
        {
            const std::uintptr_t offset = roblox::offsets::gui_object::text_color3
                ? roblox::offsets::gui_object::text_color3
                : roblox::offsets::gui_object::text_color3_fallback;
            if (!offset)
            {
                return std::nullopt;
            }
            sdk::math::color3 color = memory->read<sdk::math::color3>(instance.get_address() + offset);
            if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b))
            {
                return std::nullopt;
            }
            color.r = std::clamp(color.r, 0.0f, 1.0f);
            color.g = std::clamp(color.g, 0.0f, 1.0f);
            color.b = std::clamp(color.b, 0.0f, 1.0f);
            return color;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::string explorer_build_path(const rbx::instance_t& instance)
    {
        if (!instance.is_valid())
        {
            return {};
        }

        struct path_node
        {
            std::uintptr_t address = 0;
            std::string label;
        };

        std::vector<path_node> nodes;
        nodes.reserve(16);

        std::unordered_set<std::uintptr_t> seen;
        constexpr std::size_t k_max_depth = 128;
        const std::uintptr_t datamodel_addr = globals->datamodel.is_valid() ? globals->datamodel.get_address() : 0;

        rbx::instance_t current = instance;
        for (std::size_t depth = 0; depth < k_max_depth && current.is_valid(); ++depth)
        {
            const std::uintptr_t address = current.get_address();
            if (address == 0 || !seen.insert(address).second)
            {
                break;
            }

            std::string label;
            try { label = current.get_name(); }
            catch (...) {}
            if (label.empty())
            {
                try { label = current.get_class_name(); }
                catch (...) {}
            }
            if (label.empty() || !is_printable_ascii(label))
            {
                label = "Instance";
            }

            nodes.push_back({ address, std::move(label) });

            if (datamodel_addr != 0 && address == datamodel_addr)
            {
                break;
            }

            current = explorer_get_parent(current);
        }

        if (nodes.empty())
        {
            return {};
        }

        if (datamodel_addr != 0)
        {
            for (auto& node : nodes)
            {
                if (node.address == datamodel_addr)
                {
                    node.label = "game";
                    break;
                }
            }
        }

        std::reverse(nodes.begin(), nodes.end());
        std::string path;
        path.reserve(nodes.size() * 12);
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (i > 0)
            {
                path.push_back('.');
            }
            path += nodes[i].label;
        }

        return path;
    }

    explorer_node build_explorer_node(const rbx::instance_t& instance, std::size_t depth, std::size_t& counter)
    {
        explorer_node node{};
        node.instance = instance;
        try { node.name = explorer_sanitize_label(instance.get_name()); }
        catch (...) {}
        try { node.class_name = explorer_sanitize_label(instance.get_class_name()); }
        catch (...) {}

        ++counter;

        std::vector<rbx::instance_t> children;
        try
        {
            children = instance.get_children();
        }
        catch (...)
        {
            children.clear();
        }

        node.children.reserve(children.size());
        for (const auto& child : children)
        {
            if (!child.is_valid())
                continue;
            node.children.push_back(build_explorer_node(child, depth + 1, counter));
        }

        return node;
    }

    bool explorer_find_instance_by_address(const explorer_node& node, std::uintptr_t address, rbx::instance_t& out_instance)
    {
        if (node.instance.get_address() == address)
        {
            out_instance = node.instance;
            return true;
        }

        for (const auto& child : node.children)
        {
            if (explorer_find_instance_by_address(child, address, out_instance))
            {
                return true;
            }
        }

        return false;
    }

    explorer_build_result build_explorer_tree_snapshot()
    {
        explorer_build_result result{};
        if (!globals->datamodel.is_valid())
        {
            return result;
        }

        std::size_t counter = 0;
        try
        {
            explorer_node root = build_explorer_node(globals->datamodel, 0, counter);
            result.tree.push_back(std::move(root));
        }
        catch (...)
        {
            result.tree.clear();
            counter = 0;
        }

        result.node_count = counter;
        return result;
    }

    void apply_explorer_tree_snapshot(explorer_build_result&& snapshot)
    {
        g_explorer_tree = std::move(snapshot.tree);
        g_explorer_node_count = snapshot.node_count;
        g_explorer_details_valid = false;
        g_explorer_details_address = 0;
        g_explorer_initialized = true;

        if (g_explorer_tree.empty())
        {
            g_explorer_selected_address = 0;
            g_explorer_selected_instance = {};
            return;
        }

        if (g_explorer_selected_address != 0)
        {
            rbx::instance_t matched{};
            for (const auto& root : g_explorer_tree)
            {
                if (explorer_find_instance_by_address(root, g_explorer_selected_address, matched))
                {
                    g_explorer_selected_instance = matched;
                    return;
                }
            }
        }

        g_explorer_selected_address = g_explorer_tree.front().instance.get_address();
        g_explorer_selected_instance = g_explorer_tree.front().instance;
    }

    bool start_explorer_refresh_async()
    {
        if (g_explorer_refresh_inflight)
        {
            return false;
        }

        g_explorer_refresh_inflight = true;
        g_explorer_last_refresh_request_time = ImGui::GetTime();
        g_explorer_refresh_future = std::async(std::launch::async, []()
        {
            return build_explorer_tree_snapshot();
        });
        return true;
    }

    void pump_explorer_refresh_async()
    {
        if (!g_explorer_refresh_inflight || !g_explorer_refresh_future.valid())
        {
            return;
        }

        if (g_explorer_refresh_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return;
        }

        explorer_build_result snapshot{};
        try
        {
            snapshot = g_explorer_refresh_future.get();
        }
        catch (...)
        {
            snapshot = explorer_build_result{};
        }

        g_explorer_refresh_inflight = false;
        apply_explorer_tree_snapshot(std::move(snapshot));
    }

    void refresh_explorer_tree()
    {
        start_explorer_refresh_async();
    }

    void render_explorer_node_tree(const explorer_node& node, int depth = 0)
    {
        const bool selected = node.instance.get_address() == g_explorer_selected_address;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (node.children.empty())
            flags |= ImGuiTreeNodeFlags_Leaf;
        if (depth == 0)
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        if (selected)
            flags |= ImGuiTreeNodeFlags_Selected;

        const std::string display_name = explorer_label_or_fallback(node.name, "Instance");
        const std::string class_label = explorer_label_or_fallback(node.class_name, "Class");
        std::string label = display_name + " [" + class_label + "]";

        ImGui::PushID(reinterpret_cast<void*>(node.instance.get_address()));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0, 0, 0, 0));
        const bool open = ImGui::TreeNodeEx("node", flags, "%s", "");
        const bool hovered = ImGui::IsItemHovered();

        ImVec4 text_col = ImGui::GetStyle().Colors[ImGuiCol_Text];
        if (selected)
        {
            text_col = c_colors::top_accent_color;
        }
        else if (hovered)
        {
            text_col = scale_colors(text_col, 1.08f);
        }

        const ImVec2 min = ImGui::GetItemRectMin();
        const float label_offset_x = ImGui::GetTreeNodeToLabelSpacing();
        const ImVec2 text_pos(min.x + label_offset_x, min.y + ImGui::GetStyle().FramePadding.y);
        ImGui::GetWindowDrawList()->AddText(text_pos, ImGui::GetColorU32(text_col), label.c_str());
        ImGui::PopStyleColor(4);

        if (ImGui::IsItemClicked())
        {
            g_explorer_selected_address = node.instance.get_address();
            g_explorer_selected_instance = node.instance;
        }

        if (ImGui::BeginPopupContextItem())
        {
            const auto address = node.instance.get_address();
            if (ImGui::MenuItem("Copy Address", nullptr, false, address != 0))
            {
                std::ostringstream oss;
                oss << "0x" << std::hex << std::uppercase << static_cast<unsigned long long>(address);
                ImGui::SetClipboardText(oss.str().c_str());
            }
            ImGui::EndPopup();
        }

        if (open)
        {
            for (const auto& child : node.children)
            {
                render_explorer_node_tree(child, depth + 1);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void render_explorer_selected_info()
    {
        if (g_explorer_selected_address == 0 || !g_explorer_selected_instance.is_valid())
        {
            const ImVec4 disabled = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
            c_widgets::text_colored(disabled, "Select an instance to view details.");
            return;
        }

        const ImVec4 disabled = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        std::string name;
        std::string class_name;
        try { name = g_explorer_selected_instance.get_name(); }
        catch (...) {}
        try { class_name = g_explorer_selected_instance.get_class_name(); }
        catch (...) {}

        c_widgets::text("Name: %s", explorer_label_or_fallback(name, "Unknown").c_str());
        c_widgets::text("Class: %s", explorer_label_or_fallback(class_name, "Unknown").c_str());

        bool rendered_value = false;
        if (class_name == "BoolValue")
        {
            if (const auto value = rbx::value::get_bool(g_explorer_selected_instance))
            {
                c_widgets::text("Value: %s", *value ? "true" : "false");
            }
            else
            {
                c_widgets::text_colored(disabled, "Value: N/A");
            }
            rendered_value = true;
        }
        else if (class_name == "IntValue")
        {
            if (const auto value = rbx::value::get_int(g_explorer_selected_instance))
            {
                c_widgets::text("Value: %d", *value);
            }
            else
            {
                c_widgets::text_colored(disabled, "Value: N/A");
            }
            rendered_value = true;
        }
        else if (class_name == "NumberValue")
        {
            if (const auto value = rbx::value::get_number(g_explorer_selected_instance))
            {
                const float number_value = static_cast<float>(*value);
                if (std::isfinite(number_value))
                {
                    c_widgets::text("Value: %.3f", number_value);
                }
                else
                {
                    c_widgets::text_colored(disabled, "Value: N/A");
                }
            }
            else
            {
                c_widgets::text_colored(disabled, "Value: N/A");
            }
            rendered_value = true;
        }

        const bool is_value_class = class_name.size() >= 5
            && class_name.compare(class_name.size() - 5, 5, "Value") == 0;
        if (is_value_class && !rendered_value)
        {
            c_widgets::text_colored(disabled, "Value: Unsupported for %s", class_name.c_str());
        }

        const std::string path = explorer_build_path(g_explorer_selected_instance);
        if (!path.empty())
        {
            c_widgets::text("Path: %s", path.c_str());
        }
        else
        {
            c_widgets::text_colored(disabled, "Path: N/A");
        }

        explorer_update_selected_counts();
        if (g_explorer_details_valid)
        {
            c_widgets::text("Children: %zu", g_explorer_details_children);
            c_widgets::text("Descendants: %zu", g_explorer_details_descendants);
        }
        else
        {
            c_widgets::text_colored(disabled, "Children: N/A");
            c_widgets::text_colored(disabled, "Descendants: N/A");
        }

        if (class_name == "Model")
        {
            const auto matched_player = explorer_find_player_for_character_model(g_explorer_selected_instance);
            if (matched_player && matched_player->is_valid())
            {
                std::string player_name;
                try { player_name = matched_player->get_name(); }
                catch (...) {}
                c_widgets::text("Player: %s", explorer_label_or_fallback(player_name, "Unknown").c_str());

                const auto team_ptr = rbx::player::get_team(*matched_player);
                if (team_ptr && *team_ptr != 0)
                {
                    const auto team_color_offset = roblox::offsets::team::team_color;
                    const rbx::instance_t team_instance(*team_ptr);
                    std::string team_name;
                    try { team_name = team_instance.get_name(); }
                    catch (...) {}

                    c_widgets::text(
                        "Team: %s (0x%llX)",
                        explorer_label_or_fallback(team_name, "Unknown").c_str(),
                        static_cast<unsigned long long>(*team_ptr));

                    const auto team_color_raw = explorer_read_team_team_color(team_instance);
                    if (team_color_raw)
                    {
                        const std::uint8_t r = static_cast<std::uint8_t>(*team_color_raw & 0xFFu);
                        const std::uint8_t g = static_cast<std::uint8_t>((*team_color_raw >> 8) & 0xFFu);
                        const std::uint8_t b = static_cast<std::uint8_t>((*team_color_raw >> 16) & 0xFFu);
                        c_widgets::text(
                            "Team.TeamColor @0x%llX: 0x%08X (%u) rgb(%u, %u, %u)",
                            static_cast<unsigned long long>(team_color_offset),
                            *team_color_raw,
                            *team_color_raw,
                            static_cast<unsigned int>(r),
                            static_cast<unsigned int>(g),
                            static_cast<unsigned int>(b));
                    }
                    else
                    {
                        if (team_color_offset)
                        {
                            c_widgets::text_colored(disabled, "Team.TeamColor @0x%llX: N/A", static_cast<unsigned long long>(team_color_offset));
                        }
                        else
                        {
                            c_widgets::text_colored(disabled, "Team.TeamColor: N/A");
                        }
                    }
                }
                else
                {
                    c_widgets::text_colored(disabled, "Team: N/A");
                    if (roblox::offsets::team::team_color)
                    {
                        c_widgets::text_colored(disabled, "Team.TeamColor @0x%llX: N/A", static_cast<unsigned long long>(roblox::offsets::team::team_color));
                    }
                    else
                    {
                        c_widgets::text_colored(disabled, "Team.TeamColor: N/A");
                    }
                }
            }
            else
            {
                c_widgets::text_colored(disabled, "Player: N/A");
                c_widgets::text_colored(disabled, "Team: N/A");
                if (roblox::offsets::team::team_color)
                {
                    c_widgets::text_colored(disabled, "Team.TeamColor @0x%llX: N/A", static_cast<unsigned long long>(roblox::offsets::team::team_color));
                }
                else
                {
                    c_widgets::text_colored(disabled, "Team.TeamColor: N/A");
                }
            }
        }

        const bool is_part = explorer_is_part_class(class_name);
        const bool is_mesh_part = explorer_is_mesh_part_class(class_name);
        const bool is_special_mesh = explorer_is_special_mesh_class(class_name);
        const bool is_text_label = (class_name == "TextLabel");

        if (is_part)
        {
            const auto primitive = rbx::part::get_primitive(g_explorer_selected_instance);
            const auto pos = g_explorer_selected_instance.get_position(primitive);
            if (pos)
                c_widgets::text("Position: (%.2f, %.2f, %.2f)", pos->x, pos->y, pos->z);
            else
                c_widgets::text_colored(disabled, "Position: N/A");

            const auto size = rbx::part::get_size(primitive);
            if (size)
                c_widgets::text("Size: (%.2f, %.2f, %.2f)", size->x, size->y, size->z);
            else
                c_widgets::text_colored(disabled, "Size: N/A");

            const auto color = explorer_read_part_color(g_explorer_selected_instance);
            if (color)
            {
                const ImVec4 rgba = ImGui::ColorConvertU32ToFloat4(*color);
                const int r8 = static_cast<int>(rgba.x * 255.0f + 0.5f);
                const int g8 = static_cast<int>(rgba.y * 255.0f + 0.5f);
                const int b8 = static_cast<int>(rgba.z * 255.0f + 0.5f);
                c_widgets::text("Color: #%02X%02X%02X (%.2f, %.2f, %.2f)", r8, g8, b8, rgba.x, rgba.y, rgba.z);
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::ColorButton(
                    "##explorer_color_preview",
                    ImVec4(rgba.x, rgba.y, rgba.z, 1.0f),
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoDragDrop,
                    ImVec2(16.0f, 16.0f));
            }
            else
            {
                c_widgets::text_colored(disabled, "Color: N/A");
            }
        }

        if (is_mesh_part)
        {
            const auto mesh_id = rbx::mesh_part::get_mesh_id(g_explorer_selected_instance);
            if (mesh_id && !mesh_id->empty() && is_printable_ascii(*mesh_id))
                c_widgets::text("Mesh Id: %s", mesh_id->c_str());
            else
                c_widgets::text_colored(disabled, "Mesh Id: N/A");
        }

        if (is_special_mesh)
        {
            const auto special_mesh_id = explorer_read_special_mesh_id(g_explorer_selected_instance);
            if (special_mesh_id && !special_mesh_id->empty() && is_printable_ascii(*special_mesh_id))
                c_widgets::text("SpecialMesh Id: %s", special_mesh_id->c_str());
            else
                c_widgets::text_colored(disabled, "SpecialMesh Id: N/A");
        }

        if (is_text_label)
        {
            const auto text_color = explorer_read_textlabel_text_color(g_explorer_selected_instance);
            if (text_color)
            {
                const int r8 = static_cast<int>(text_color->r * 255.0f + 0.5f);
                const int g8 = static_cast<int>(text_color->g * 255.0f + 0.5f);
                const int b8 = static_cast<int>(text_color->b * 255.0f + 0.5f);
                c_widgets::text("TextColor3: #%02X%02X%02X (%.2f, %.2f, %.2f)", r8, g8, b8, text_color->r, text_color->g, text_color->b);
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::ColorButton(
                    "##explorer_textlabel_color_preview",
                    ImVec4(text_color->r, text_color->g, text_color->b, 1.0f),
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoDragDrop,
                    ImVec2(16.0f, 16.0f));
            }
            else
            {
                c_widgets::text_colored(disabled, "TextColor3: N/A");
            }
        }
    }

    void render_testing_explorer()
    {
        pump_explorer_refresh_async();

        if (!g_explorer_initialized && !g_explorer_refresh_inflight)
        {
            start_explorer_refresh_async();
        }

        constexpr double k_auto_refresh_interval_seconds = 1.0;
        if (features->explorer_auto_refresh && !g_explorer_refresh_inflight)
        {
            const double now = ImGui::GetTime();
            if (g_explorer_last_refresh_request_time < 0.0 ||
                (now - g_explorer_last_refresh_request_time) >= k_auto_refresh_interval_seconds)
            {
                start_explorer_refresh_async();
            }
        }

        const ImVec4 disabled = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];

        ImVec2 avail = ImGui::GetContentRegionAvail();
        const float button_height = ImGui::GetFrameHeight();
        const float spacing_y = ImGui::GetStyle().ItemSpacing.y;
        const float column_spacing = ImGui::GetStyle().ItemSpacing.x;
        const float footer_spacing = spacing_y;
        const float footer_height = button_height + footer_spacing;
        const float body_height = ImMax(0.0f, avail.y - footer_height);

        float info_width = ImMax(180.0f, avail.x * 0.42f);
        float tree_width = ImMax(120.0f, avail.x - info_width - column_spacing);
        const bool use_vertical_layout = (tree_width <= 0.0f || info_width <= 0.0f || avail.x < 260.0f);

        if (use_vertical_layout)
        {
            tree_width = avail.x;
            info_width = avail.x;
        }

        ImVec2 tree_size(tree_width, body_height * (use_vertical_layout ? 0.55f : 1.0f));
        ImVec2 info_size(info_width, use_vertical_layout ? body_height - tree_size.y - spacing_y : body_height);
        if (info_size.y < 0.0f)
            info_size.y = body_height * 0.45f;

        if (c_widgets::begin_padded_child("##testing_explorer_tree", 0, 0, ImVec2(tree_size.x, tree_size.y - 20.0f), true, true, false, true, false))
        {
            if (g_explorer_tree.empty())
            {
                c_widgets::text_colored(disabled, g_explorer_refresh_inflight ? "Refreshing tree..." : "No explorer nodes.");
            }
            for (const auto& root : g_explorer_tree)
            {
                render_explorer_node_tree(root, 0);
            }
        }
        c_widgets::end_padded_child();

        if (!use_vertical_layout)
            ImGui::SameLine(0.0f, column_spacing);

        if (c_widgets::begin_padded_child("##testing_explorer_info", 0, 0, ImVec2(info_size.x, info_size.y - 20.0f), true, true, false, true, false))
        {
            render_explorer_selected_info();
        }
        c_widgets::end_padded_child();

        ImGui::Dummy(ImVec2(0.0f, footer_spacing));
        ImVec2 footer_pos = ImGui::GetCursorPos();
        footer_pos.y = ImMax(footer_pos.y - ImGui::GetStyle().WindowPadding.y, 0.0f);
        ImGui::SetCursorPos(footer_pos);

        c_widgets::checkbox("Auto refresh (1s)", &features->explorer_auto_refresh);

        ImGui::BeginDisabled(g_explorer_refresh_inflight);
        if (c_widgets::button(g_explorer_refresh_inflight ? "Refreshing..." : "Refresh Explorer", ImVec2(-1.0f, 0.0f)))
        {
            refresh_explorer_tree();
        }
        ImGui::EndDisabled();

        const float export_button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (c_widgets::button("Export MCP JSON", ImVec2(export_button_width, 0.0f)))
        {
            if (vanille::explorer::export_mcp_snapshot())
            {
                g_explorer_last_export_message = "Exported to " + vanille::explorer::default_mcp_snapshot_path();
            }
            else
            {
                g_explorer_last_export_message = "Export failed";
            }
        }
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
        if (c_widgets::button("Copy JSON", ImVec2(export_button_width, 0.0f)))
        {
            const vanille::explorer::snapshot snap = vanille::explorer::capture();
            ImGui::SetClipboardText(vanille::explorer::to_json(snap, true).c_str());
            g_explorer_last_export_message = "Copied explorer JSON to clipboard";
        }

        if (!g_explorer_last_export_message.empty())
        {
            c_widgets::text_colored(disabled, "%s", g_explorer_last_export_message.c_str());
        }

        if (g_explorer_refresh_inflight)
        {
            c_widgets::text_colored(disabled, "Refreshing in background...");
        }
        c_widgets::text_colored(disabled, "%zu nodes", g_explorer_node_count);
    }
}

void vanille::overlay::run_overlay()
{
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEX wc = {};

    wc.cbClsExtra = NULL;
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.cbWndExtra = NULL;
    wc.hbrBackground = (HBRUSH)CreateSolidBrush(RGB(0, 0, 0));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = "Cheat Window";
    wc.lpszMenuName = nullptr;
    wc.style = CS_VREDRAW | CS_HREDRAW;

    ::RegisterClassEx(&wc);
    const HWND hwnd = ::CreateWindowEx(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, wc.lpszClassName, "Cheat",
        WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr, wc.hInstance, nullptr);
    vanille::overlay::g_overlay_window = hwnd;

    if (!create_device_d3d(hwnd))
    {
        cleanup_device_d3d();
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        return;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    style.Colors[ImGuiCol_Text] = c_colors::white;
    style.Colors[ImGuiCol_TextDisabled] = c_colors::text_muted;
    style.Colors[ImGuiCol_WindowBg] = c_colors::top_window_background;
    style.Colors[ImGuiCol_ChildBg] = c_colors::top_child_background;
    style.Colors[ImGuiCol_CheckMark] = c_colors::top_accent_color;
    style.Colors[ImGuiCol_SliderGrab] = c_colors::top_accent_color;
    style.Colors[ImGuiCol_SliderGrabActive] = c_colors::scale_color(c_colors::top_accent_color, 0.90f);
    style.Colors[ImGuiCol_FrameBg] = c_colors::surface_inset;
    style.Colors[ImGuiCol_FrameBgHovered] = c_colors::scale_color(c_colors::surface_inset, 1.08f);
    style.Colors[ImGuiCol_FrameBgActive] = c_colors::scale_color(c_colors::surface_inset, 0.95f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_Border] = c_colors::outter_border;
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_Separator] = c_colors::outter_border;
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TabHovered] = c_colors::top_child_background;
    style.Colors[ImGuiCol_TabActive] = c_colors::top_child_background;
	style.Colors[ImGuiCol_WindowShadow] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.WindowRounding = c_colors::window_rounding;
    style.ChildRounding = c_colors::panel_rounding;
    style.FrameRounding = c_colors::widget_rounding;
    style.PopupRounding = c_colors::widget_rounding;
    style.ScrollbarRounding = c_colors::widget_rounding;
    style.GrabRounding = c_colors::widget_rounding;
    style.TabRounding = c_colors::widget_rounding;

    const unsigned int smooth_freetype_flags = ImGuiFreeTypeBuilderFlags_LightHinting;
    io.Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());
    io.Fonts->FontLoaderFlags = smooth_freetype_flags;

    auto make_cfg = [&](unsigned int extra_flags = 0)
    {
        ImFontConfig cfg{};
        cfg.PixelSnapH = false;
        cfg.OversampleH = 3;
        cfg.OversampleV = 1;
        cfg.RasterizerMultiply = 1.12f;
        cfg.FontLoaderFlags = smooth_freetype_flags | extra_flags;
        return cfg;
    };

    auto load_windows_font = [&](const char* filename, float size, const ImFontConfig& base_cfg) -> ImFont*
    {
        char windows_dir[MAX_PATH]{};
        UINT len = GetWindowsDirectoryA(windows_dir, MAX_PATH);
        std::filesystem::path fonts_dir = (len > 0 && len < MAX_PATH)
            ? std::filesystem::path(windows_dir) / "Fonts"
            : std::filesystem::path("C:\\Windows\\Fonts");

        char module_path[MAX_PATH]{};
        const DWORD module_len = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
        const std::filesystem::path exe_dir = (module_len != 0)
            ? std::filesystem::path(module_path).parent_path()
            : std::filesystem::current_path();

        const std::array<std::filesystem::path, 5> search_paths = {
            exe_dir / "fonts" / filename,
            exe_dir / filename,
            fonts_dir / filename,
            std::filesystem::current_path() / filename,
            std::filesystem::current_path() / "fonts" / filename
        };

        for (const auto& path : search_paths)
        {
            if (std::filesystem::exists(path))
            {
                ImFontConfig cfg = base_cfg;
                cfg.FontDataOwnedByAtlas = false;
                return io.Fonts->AddFontFromFileTTF(path.string().c_str(), size, &cfg);
            }
        }

        return nullptr;
    };

    auto load_first_available_font = [&](std::initializer_list<const char*> filenames, float size,
                                           const ImFontConfig& base_cfg) -> ImFont*
    {
        for (const char* filename : filenames)
        {
            if (ImFont* font = load_windows_font(filename, size, base_cfg))
                return font;
        }
        return nullptr;
    };

    io.Fonts->FontLoaderFlags = smooth_freetype_flags;
    ImFontConfig ui_cfg = make_cfg();

    c_fonts::verdana_regular = load_first_available_font(
        {"segoeuisb.ttf", "Segoe UI Semibold.ttf", "segoeui.ttf", "Segoe UI.ttf",
         "PlusJakartaSans-Medium.ttf", "PlusJakartaSans-Regular.ttf", "Inter-Medium.otf", "Inter-Regular.otf"},
        c_fonts::verdana_regular_size, ui_cfg);

    c_fonts::verdana_bold = load_first_available_font(
        {"segoeuib.ttf", "Segoe UI Bold.ttf", "segoeuisb.ttf", "Segoe UI Semibold.ttf",
         "PlusJakartaSans-Bold.ttf", "PlusJakartaSans-SemiBold.ttf", "Inter-SemiBold.otf", "Inter-Bold.otf"},
        c_fonts::verdana_bold_size, ui_cfg);

    c_fonts::ui_title = load_first_available_font(
        {"segoeuib.ttf", "Segoe UI Bold.ttf", "PlusJakartaSans-Bold.ttf", "Inter-Bold.otf"},
        c_fonts::ui_title_size, ui_cfg);

    c_fonts::ui_section = load_first_available_font(
        {"segoeuisb.ttf", "Segoe UI Semibold.ttf", "segoeui.ttf", "PlusJakartaSans-Medium.ttf", "Inter-Medium.otf"},
        c_fonts::ui_section_size, ui_cfg);

    c_fonts::ui_tab = load_first_available_font(
        {"segoeuisb.ttf", "Segoe UI Semibold.ttf", "segoeui.ttf", "PlusJakartaSans-Medium.ttf", "Inter-Medium.otf"},
        c_fonts::ui_tab_size, ui_cfg);

    c_fonts::ui_tab_bold = load_first_available_font(
        {"segoeuib.ttf", "Segoe UI Bold.ttf", "segoeuisb.ttf", "PlusJakartaSans-Bold.ttf", "Inter-SemiBold.otf"},
        c_fonts::ui_tab_bold_size, ui_cfg);

    if (!c_fonts::verdana_regular || !c_fonts::verdana_bold)
    {
        ImFontConfig verdana_regular_cfg = make_cfg();
        verdana_regular_cfg.FontDataOwnedByAtlas = false;
        if (!c_fonts::verdana_regular)
        {
            c_fonts::verdana_regular = io.Fonts->AddFontFromMemoryTTF(
                (void*)font_verdana_regular, sizeof(font_verdana_regular), c_fonts::verdana_regular_size,
                &verdana_regular_cfg);
        }

        ImFontConfig verdana_bold_cfg = make_cfg();
        verdana_bold_cfg.FontDataOwnedByAtlas = false;
        if (!c_fonts::verdana_bold)
        {
            c_fonts::verdana_bold = io.Fonts->AddFontFromMemoryTTF((void*)font_verdana_bold, sizeof(font_verdana_bold),
                                                                   c_fonts::verdana_bold_size, &verdana_bold_cfg);
        }
    }

    if (!c_fonts::verdana_bold)
        c_fonts::verdana_bold = c_fonts::verdana_regular;

    if (!c_fonts::ui_title)
        c_fonts::ui_title = c_fonts::verdana_bold;
    if (!c_fonts::ui_section)
        c_fonts::ui_section = c_fonts::verdana_regular;
    if (!c_fonts::ui_tab)
        c_fonts::ui_tab = c_fonts::verdana_regular;
    if (!c_fonts::ui_tab_bold)
        c_fonts::ui_tab_bold = c_fonts::verdana_bold;

    ImFontConfig media_cfg = make_cfg();
    media_cfg.RasterizerMultiply = 1.14f;

    c_fonts::media_regular = load_first_available_font(
        {"segoeuisb.ttf", "Segoe UI Semibold.ttf", "segoeui.ttf", "Segoe UI.ttf"},
        c_fonts::media_regular_size,
        media_cfg);
    c_fonts::media_bold = load_first_available_font(
        {"segoeuib.ttf", "Segoe UI Bold.ttf", "segoeuisb.ttf", "Segoe UI Semibold.ttf"},
        c_fonts::media_bold_size,
        media_cfg);
    c_fonts::media_lyrics_active = load_first_available_font(
        {"segoeuib.ttf", "Segoe UI Bold.ttf", "segoeuisb.ttf", "Segoe UI Semibold.ttf"},
        c_fonts::media_lyrics_active_size,
        media_cfg);
    c_fonts::media_lyrics_inactive = load_first_available_font(
        {"segoeuisb.ttf", "Segoe UI Semibold.ttf", "segoeui.ttf", "Segoe UI.ttf"},
        c_fonts::media_lyrics_inactive_size,
        media_cfg);
    c_fonts::media_caption = load_first_available_font(
        {"segoeuisb.ttf", "Segoe UI Semibold.ttf", "segoeui.ttf", "Segoe UI.ttf"},
        c_fonts::media_caption_size,
        media_cfg);

    if (!c_fonts::media_bold)
        c_fonts::media_bold = c_fonts::verdana_bold;
    if (!c_fonts::media_regular)
        c_fonts::media_regular = c_fonts::verdana_regular;
    if (!c_fonts::media_lyrics_active)
        c_fonts::media_lyrics_active = c_fonts::media_bold;
    if (!c_fonts::media_lyrics_inactive)
        c_fonts::media_lyrics_inactive = c_fonts::media_regular;
    if (!c_fonts::media_caption)
        c_fonts::media_caption = c_fonts::media_regular;

    io.Fonts->FontLoaderFlags = smooth_freetype_flags;

    ImFontConfig tahoma_cfg = make_cfg();
    tahoma_cfg.RasterizerMultiply = 1.08f;
    c_fonts::tahoma = load_windows_font("tahoma.ttf", c_fonts::tahoma_size, tahoma_cfg);
    c_fonts::tahoma_regular = c_fonts::tahoma ? c_fonts::tahoma : c_fonts::verdana_regular;

    ImFontConfig tahoma_bold_cfg = tahoma_cfg;
    c_fonts::tahoma_bold = load_windows_font("tahomabd.ttf", c_fonts::tahoma_bold_size, tahoma_bold_cfg);
    if (!c_fonts::tahoma_bold)
    {
        c_fonts::tahoma_bold = c_fonts::tahoma_regular ? c_fonts::tahoma_regular : c_fonts::verdana_bold;
    }
    if (!c_fonts::tahoma)
    {
        c_fonts::tahoma = c_fonts::tahoma_regular;
    }

    io.Fonts->FontLoaderFlags = smooth_freetype_flags;
    ImFontConfig pixel7_cfg{};
    pixel7_cfg.PixelSnapH = true;
    pixel7_cfg.FontDataOwnedByAtlas = false;
    pixel7_cfg.FontLoaderFlags = ImGuiFreeTypeBuilderFlags_MonoHinting | ImGuiFreeTypeBuilderFlags_Monochrome;
    c_fonts::pixel7 = io.Fonts->AddFontFromMemoryTTF((void*)font_pixel7, sizeof(font_pixel7), c_fonts::pixel7_size, &pixel7_cfg);

    ImFontConfig smallest_pixel_cfg{};
    smallest_pixel_cfg.PixelSnapH = true;
    smallest_pixel_cfg.FontDataOwnedByAtlas = false;
    smallest_pixel_cfg.FontLoaderFlags = ImGuiFreeTypeBuilderFlags_MonoHinting | ImGuiFreeTypeBuilderFlags_Monochrome;
    c_fonts::smallest_pixel = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(font_smallest_pixel), sizeof(font_smallest_pixel), c_fonts::smallest_pixel_size, &smallest_pixel_cfg);

    ImFontConfig proggy_cfg = make_cfg();
    proggy_cfg.PixelSnapH = true;
    proggy_cfg.SizePixels = c_fonts::proggy_clean_size;
    c_fonts::proggy_clean = io.Fonts->AddFontDefault(&proggy_cfg);

    ImFontConfig proggy_tiny_cfg = make_cfg();
    proggy_tiny_cfg.PixelSnapH = true;
    proggy_tiny_cfg.FontDataOwnedByAtlas = false;
    c_fonts::proggy_tiny = io.Fonts->AddFontFromMemoryTTF(
        static_cast<void*>(ProggyTiny_ttf),
        static_cast<int>(ProggyTiny_ttf_len),
        c_fonts::proggy_tiny_size,
        &proggy_tiny_cfg);

    io.FontDefault = c_fonts::verdana_regular ? c_fonts::verdana_regular : c_fonts::proggy_clean;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    if (!lua_vm::initialize())
    {
        logger_core::log_warning("lua vm initialization failed");
    }
    vanille::mcp_bridge::initialize();

    if (!LoadCustomCursorTexture())
        OutputDebugStringA("Failed to load custom cursor texture from embedded bytes.\n");
    if (!LoadLogoTexture())
        OutputDebugStringA("Failed to load logo texture from embedded bytes.\n");
    if (!LoadSplashSpriteTexture())
        OutputDebugStringA("Failed to load splash sprite texture from embedded bytes.\n");
    if (!LoadGrenadeTexture())
        OutputDebugStringA("Failed to load grenade icon texture from embedded bytes.\n");
    if (!LoadDeathImageTexture())
        OutputDebugStringA("Failed to load death image texture from embedded bytes.\n");
    vanille::media::start();
    g_death_image_path_cached = features->death_overlay_image_path;
    if (!g_death_image_path_cached.empty())
    {
        const std::wstring wpath = wide_from_utf8(g_death_image_path_cached);
        if (wpath.empty() || !LoadDeathImageCustomTexture(wpath))
        {
            ClearDeathImageCustomTexture();
        }
    }

    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    const MARGINS margin = { -1, 0, 0, 0 };
    DwmExtendFrameIntoClientArea(hwnd, &margin);
    const ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    update_stream_proof_affinity(hwnd, features->stream_proof, true);

    bool done = false;
    bool exit_requested = false;
    bool roblox_exit_logged = false;
    bool menu_open = true;
    bool menu_key_down_previous = false;
    int last_menu_virtual_key = VK_INSERT;
    float menu_animation_progress = 1.0f;
    const float menu_animation_duration = 0.45f;
    const auto idle_sleep = std::chrono::milliseconds(16);
    const auto unfocused_sleep = std::chrono::milliseconds(25);
    const double splash_start_time = ImGui::GetTime();

    while (!done)
    {
        const auto frame_start = std::chrono::steady_clock::now();

        if (exit_requested)
        {
            done = true;
            ::PostQuitMessage(0);
        }

        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        if (!memory->is_process_alive())
        {
            if (!roblox_exit_logged)
            {
                logger_core::log_warning("roblox process exited -> shutting down vanille");
                roblox_exit_logged = true;
            }
            exit_requested = true;
        }

        if (g_rbx_window && !IsWindow(g_rbx_window))
        {
            exit_requested = true;
        }

        update_stream_proof_affinity(hwnd, features->stream_proof);
        apply_console_visibility();

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            std::this_thread::sleep_for(unfocused_sleep);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            cleanup_render_target();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            create_render_target();
        }

        if (g_rbx_window != nullptr)
            move_window(hwnd);

        c_keybind& menu_key = c_ui::menu_key_bind();
        int current_vk = menu_key.key != 0 ? menu_key.key : VK_INSERT;
        if (current_vk != last_menu_virtual_key)
        {
            last_menu_virtual_key = current_vk;
            menu_key_down_previous = false;
        }

        if (!menu_key.waiting_for_input)
        {
            const bool key_down = (GetAsyncKeyState(current_vk) & 0x8000) != 0;
            if (key_down && !menu_key_down_previous)
                menu_open = !menu_open;
            menu_key_down_previous = key_down;
        }
        else
        {
            menu_key_down_previous = false;
        }

        g_menu_open_state.store(menu_open, std::memory_order_relaxed);

        if (!is_window_focus(hwnd))
        {
            g_menu_showing_state.store(false, std::memory_order_relaxed);
            g_overlay_accepts_input.store(false, std::memory_order_relaxed);
            blurred_window(hwnd, false, 0.0f);
            const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
            g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
            std::this_thread::sleep_for(unfocused_sleep);
            continue;
        }

        const bool menu_accepts_input = (menu_open || menu_animation_progress > 0.0f);
        const bool menu_showing_for_cursor = menu_accepts_input;

        ImGuiIO& io = ImGui::GetIO();
        if (menu_showing_for_cursor)
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        else
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const double splash_elapsed = ImGui::GetTime() - splash_start_time;
        const bool splash_active = is_loading_splash_active(splash_elapsed);

        const bool islands_accepts_input = splash_active ? false : cursor_over_floating_islands(hwnd);
        const bool aux_accepts_input = splash_active ? false : lua_vm::cursor_over_aux_windows(hwnd);
        const bool accepts_input = splash_active ? false : (menu_accepts_input || islands_accepts_input || aux_accepts_input);
        g_overlay_accepts_input.store(accepts_input, std::memory_order_relaxed);
        apply_overlay_window_style(hwnd, accepts_input);
        poll_overlay_mouse_input(hwnd, ImGui::GetIO(), menu_accepts_input);
        poll_overlay_keyboard_input(ImGui::GetIO(), menu_accepts_input);
        {
            const ImGuiIO& io = ImGui::GetIO();
            g_overlay_dt.store(io.DeltaTime, std::memory_order_relaxed);
            g_overlay_fps.store(io.Framerate, std::memory_order_relaxed);
            lua_vm::on_frame(io.DeltaTime);
            vanille::mcp_bridge::tick();
        }

        if (features->death_overlay_image_path != g_death_image_path_cached)
        {
            g_death_image_path_cached = features->death_overlay_image_path;
            if (g_death_image_path_cached.empty())
            {
                ClearDeathImageCustomTexture();
            }
            else
            {
                const std::wstring wpath = wide_from_utf8(g_death_image_path_cached);
                if (wpath.empty() || !LoadDeathImageCustomTexture(wpath))
                {
                    ClearDeathImageCustomTexture();
                }
            }
        }

        {
            static double last_avatar_process = 0.0;
            const double now = ImGui::GetTime();
            if (now - last_avatar_process >= 0.05)
            {
                process_avatar_downloads();
                last_avatar_process = now;
            }
        }
        bool menu_showing = false;
        if (!splash_active)
        {
        esp::render_esp();
        target_hud::render();
        esp::render_radar();
        //free_aim::render_aim_frame();
        esp::render_free_aim_fov();
        esp::render_aimbot_fov();
        esp::render_triggerbot_fov();
        esp::render_crosshair();
        draw_raycast_engine_warning();

        if (menu_animation_duration > 0.0f)
        {
            const float step = io.DeltaTime / menu_animation_duration;
            if (menu_open)
                menu_animation_progress = ImClamp(menu_animation_progress + step, 0.0f, 1.0f);
            else
                menu_animation_progress = ImClamp(menu_animation_progress - step, 0.0f, 1.0f);
        }
        else
        {
            menu_animation_progress = menu_open ? 1.0f : 0.0f;
        }

        const float eased_alpha = EaseOutExpo(menu_animation_progress);
        menu_showing = (menu_open || menu_animation_progress > 0.0f);
        g_menu_showing_state.store(menu_showing, std::memory_order_relaxed);
        blurred_window(hwnd, (menu_showing && g_overlay_blur_enabled), g_overlay_blur_amount);

        if (menu_open || menu_animation_progress > 0.0f)
        {
            const float base_alpha = ImGui::GetStyle().Alpha;
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, base_alpha * eased_alpha);
            ImGui::SetNextWindowPos(ImVec2(60.0f, 68.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(520.0f, 650.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 400.0f), ImVec2(FLT_MAX, FLT_MAX));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

            const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar;
            ImGui::Begin("window", nullptr, window_flags);
            ImGui::PopStyleColor();

            g_menu_last_pos = ImGui::GetWindowPos();
            g_menu_last_size = ImGui::GetWindowSize();
            g_menu_has_frame = true;

            draw_window_background();

            ImVec2 header_pos = ImGui::GetCursorPos();
            const float text_height = ImGui::GetTextLineHeight();
            const float logo_px = 30.0f;
            const bool has_logo = (c_textures::logo != 0 && c_textures::logo_size.y > 0.0f);
            const float header_height = (std::max)(text_height, has_logo ? logo_px : 0.0f);
            const float drag_height = header_height + ImGui::GetStyle().WindowPadding.y * 0.5f;
            ImGui::SetCursorPos(header_pos);
            c_widgets::window_drag_handle("##menu_drag", drag_height);

            ImVec2 pen(header_pos.x + 2.0f, header_pos.y);

            if (has_logo)
            {
                const float logo_y = header_pos.y + (header_height - logo_px) * 0.5f;
                ImGui::SetCursorPos(ImVec2(pen.x, logo_y));
                draw_logo_icon(logo_px);
                pen.x += logo_px + 6.0f;
            }

            ImGui::SetCursorPos(ImVec2(pen.x, header_pos.y + (header_height - text_height) * 0.5f));
            ImGui::PushFont(c_fonts::ui_title ? c_fonts::ui_title : c_fonts::verdana_bold);
            ImGui::TextColored(c_colors::white, "vanille");
            ImGui::PopFont();

            ImGui::SetCursorPos(ImVec2(header_pos.x, header_pos.y + header_height + 8.0f));

            if (c_widgets::begin_padded_child("##main-child", 0, 0, ImVec2(-1.0f, -1.0f), true, false, false, false, false))
            {
                const float margin = c_widgets::padded_child_margin();
                const ImVec2 parent_child_padding = ImGui::GetStyle().WindowPadding;
                ImVec2 tabs_region_start = ImGui::GetCursorPos();

                ImVec2 flush_pos(
                    ImMax(0.0f, tabs_region_start.x - parent_child_padding.x - margin),
                    ImMax(0.0f, tabs_region_start.y - parent_child_padding.y)
                );
                ImGui::SetCursorPos(flush_pos);

                const float tab_button_height = 26.0f;
                const ImVec2 tab_child_size = ImVec2(-1.0f, tab_button_height + 8.0f);

                static int tab_index = 1;
                static int aimbot_subtab = 0;
                static int silent_subtab = 0;
                static int triggerbot_subtab = 0;
                std::vector<lua_ui_bridge::tab_descriptor> lua_tabs = lua_ui_bridge::get_tabs_snapshot();
                std::vector<std::string> tab_labels;
                tab_labels.reserve(3 + lua_tabs.size());
                tab_labels.emplace_back("Aimbot");
                tab_labels.emplace_back("Visuals");
                tab_labels.emplace_back("Misc");
                ImFontBaked* current_font_baked = ImGui::GetFontBaked();
                const bool has_bullet_glyph = current_font_baked && current_font_baked->FindGlyphNoFallback(static_cast<ImWchar>(0x2022)) != nullptr;
                const char* lua_tab_prefix = has_bullet_glyph ? "\xE2\x80\xA2 " : "* ";
                for (const auto& lua_tab : lua_tabs)
                {
                    std::string label = lua_tab_prefix;
                    label += lua_tab.name.empty() ? "lua_tab" : lua_tab.name;
                    tab_labels.push_back(std::move(label));
                }
                if (tab_labels.empty())
                {
                    tab_labels.emplace_back("Misc");
                }
                tab_index = std::clamp(tab_index, 0, static_cast<int>(tab_labels.size()) - 1);
                if (c_widgets::begin_padded_child("##tabs", ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar, tab_child_size, false, false, false))
                {
                    ImGuiWindow* tab_window = ImGui::GetCurrentWindow();
                    ImDrawList* tab_draw = tab_window->DrawList;
                    const float tab_min_width = 30.0f;
                    const float tab_side_padding = 12.0f;
                    const float tab_gap = 4.0f;
                    const float rounding = c_colors::widget_rounding;
                    ImVec2 tab_buttons_start = ImGui::GetCursorPos();
                    ImGui::SetCursorPos(ImVec2(tab_buttons_start.x + 1.0f, tab_buttons_start.y + 1.0f));

                    for (int i = 0; i < static_cast<int>(tab_labels.size()); ++i)
                    {
                        const char* tab_label = tab_labels[i].c_str();
                        const bool selected = (i == tab_index);
                        ImFont* tab_font = selected
                            ? (c_fonts::ui_tab_bold ? c_fonts::ui_tab_bold : ImGui::GetFont())
                            : (c_fonts::ui_tab ? c_fonts::ui_tab : ImGui::GetFont());
                        const float tab_font_size = tab_font ? tab_font->LegacySize : ImGui::GetFontSize();

                        const bool pushed_tab_font = (tab_font != ImGui::GetFont());
                        if (pushed_tab_font)
                            ImGui::PushFont(tab_font);

                        const ImVec2 text_size = ImGui::CalcTextSize(tab_label);
                        const float button_width = ImMax(tab_min_width, text_size.x + tab_side_padding * 2.0f);
                        const ImVec2 button_size(button_width, tab_button_height);

                        ImGui::PushID(i);
                        const ImVec2 pos = tab_window->DC.CursorPos;
                        const ImRect bb(pos, pos + button_size);
                        ImGui::ItemSize(bb);
                        const ImGuiID item_id = tab_window->GetID("##tab_pill");
                        if (ImGui::ItemAdd(bb, item_id))
                        {
                            bool hovered = false;
                            bool held = false;
                            const bool pressed = ImGui::ButtonBehavior(bb, item_id, &hovered, &held);
                            if (pressed)
                                tab_index = i;

                            if (selected)
                            {
                                tab_draw->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(c_colors::accent_soft), rounding);
                                tab_draw->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(c_colors::accent_border), rounding, 0, 1.0f);
                            }
                            else if (hovered)
                            {
                                tab_draw->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(c_colors::surface), rounding);
                            }

                            ImVec4 text_col = selected ? c_colors::top_accent_color
                                : (hovered ? c_colors::white : c_colors::text_muted);
                            const ImVec2 text_pos(
                                bb.Min.x + (bb.GetWidth() - text_size.x) * 0.5f,
                                bb.Min.y + (bb.GetHeight() - text_size.y) * 0.5f);
                            tab_draw->AddText(tab_font, tab_font_size, text_pos, ImGui::GetColorU32(text_col), tab_label);
                        }
                        ImGui::PopID();

                        if (pushed_tab_font)
                            ImGui::PopFont();

                        if (i + 1 < static_cast<int>(tab_labels.size()))
                            ImGui::SameLine(0.0f, tab_gap);
                    }
                }
                c_widgets::end_padded_child();

                ImVec2 after_tabs = ImGui::GetCursorPos();
                ImGui::SetCursorPos(ImVec2(tabs_region_start.x, after_tabs.y + margin));

                ImVec2 grid_origin = ImGui::GetCursorPos();
                ImVec2 grid_avail = ImGui::GetContentRegionAvail();
                const float grid_spacing = 4.0f;
                const float footer_spacing = 2.0f;
                const float footer_height = 32.0f;
                float panel_vertical_space = ImMax(0.0f, grid_avail.y - footer_height - footer_spacing);
                ImVec2 panel_size_half(
                    ImMax(0.0f, (grid_avail.x - grid_spacing) * 0.5f),
                    ImMax(0.0f, (panel_vertical_space - grid_spacing) * 0.5f));

                auto render_panel = [&](const char* id, const char* title, const ImVec2& pos, const ImVec2& size, auto&& content_fn, bool show_title = true, bool remove_accent_top_padding = false, bool allow_scroll = false)
                    {
                        ImGui::SetCursorPos(pos);
                        if (c_widgets::begin_padded_child(id, 0, 0, size, true, true, false, true, false, false, remove_accent_top_padding, allow_scroll))
                        {
                            if (show_title && title && *title)
                            {
                                c_widgets::section_label("%s", title);
                                ImGui::Dummy(ImVec2(0.0f, 2.0f));
                            }
                            content_fn();
                        }
                        c_widgets::end_padded_child();
                    };

                auto render_subtabs = [&](const char* id, int& index, const char* const* labels, int count, float /*inactive_bottom_trim*/ = 0.0f, float /*inactive_top_offset*/ = 0.0f, float /*row_offset_y*/ = 0.0f, float /*inactive_expand*/ = 0.0f)
                    {
                        if (!labels || count <= 0)
                            return;

                        ImGuiWindow* window = ImGui::GetCurrentWindow();
                        if (!window || window->SkipItems)
                            return;

                        const float padding_x = 12.0f;
                        const float padding_y = 5.0f;
                        const float underline = 2.0f;
                        const float row_height = ImGui::GetFontSize() + padding_y * 2.0f;
                        const ImVec2 row_start = ImGui::GetCursorScreenPos();
                        const float row_width = ImGui::GetContentRegionAvail().x;
                        const float border_y = row_start.y + row_height;

                        ImDrawList* draw_list = window->DrawList;

                        ImGui::PushID(id);
                        float tab_x = row_start.x;
                        for (int i = 0; i < count; ++i)
                        {
                            ImGui::PushID(i);

                            const char* text = labels[i] ? labels[i] : "";
                            const ImVec2 text_size = ImGui::CalcTextSize(text);
                            const float tab_width = text_size.x + padding_x * 2.0f;
                            const ImRect bb(ImVec2(tab_x, row_start.y), ImVec2(tab_x + tab_width, row_start.y + row_height));

                            ImGui::SetCursorScreenPos(bb.Min);
                            if (ImGui::InvisibleButton("##subtab", bb.GetSize()))
                                index = i;

                            const bool selected = (i == index);
                            const bool hovered = ImGui::IsItemHovered();

                            ImFont* font = selected
                                ? (c_fonts::ui_tab_bold ? c_fonts::ui_tab_bold : c_fonts::verdana_bold)
                                : (c_fonts::ui_tab ? c_fonts::ui_tab : ImGui::GetFont());
                            const float font_size = font ? font->LegacySize : ImGui::GetFontSize();

                            ImVec4 text_col = selected ? c_colors::top_accent_color : c_colors::text_muted;
                            if (!selected && hovered)
                                text_col = c_colors::white;

                            const ImVec2 text_pos(bb.Min.x + padding_x, bb.Min.y + padding_y);
                            draw_list->AddText(font, font_size, text_pos, ImGui::GetColorU32(text_col), text);

                            if (selected)
                            {
                                draw_list->AddRectFilled(
                                    ImVec2(bb.Min.x, border_y - underline),
                                    ImVec2(bb.Max.x, border_y),
                                    ImGui::GetColorU32(c_colors::top_accent_color));
                            }

                            tab_x += tab_width;
                            ImGui::PopID();
                        }
                        ImGui::PopID();

                        draw_list->AddLine(
                            ImVec2(row_start.x, border_y),
                            ImVec2(row_start.x + row_width, border_y),
                            ImGui::GetColorU32(c_colors::main_border),
                            1.0f);

                        ImGui::SetCursorScreenPos(ImVec2(row_start.x, border_y + 10.0f));
                    };

                ImVec2 top_left = grid_origin;
                ImVec2 top_right = ImVec2(grid_origin.x + panel_size_half.x + grid_spacing, grid_origin.y);
                ImVec2 bottom_left = ImVec2(grid_origin.x, grid_origin.y + panel_size_half.y + grid_spacing);
                ImVec2 bottom_right = ImVec2(top_right.x, bottom_left.y);
                if (tab_index == 0)
                {
                    const char* aimbot_tabs[] = { "Aiming", "Configuration" };
                    const char* silent_tabs[] = { "Silent Aim", "Configuration" };
                    const char* triggerbot_tabs[] = { "Triggerbot", "Configuration" };

                    render_panel("##panel_top_left", "Aimbot", top_left, panel_size_half, [&]()
                        {
                            render_subtabs("##aimbot_subtabs", aimbot_subtab, aimbot_tabs, IM_ARRAYSIZE(aimbot_tabs), 0.0f, 0.0f, 0.0f, 0.0f);
                            if (aimbot_subtab == 0)
                            {
                                c_widgets::checkbox("Enable", &::features->enable_aimbot);
                                c_widgets::keybind("##aimbot_key", ::features->aimbot_keybind);

                                static const char* modes[] = { "Camera", "Mouse" };
                                c_widgets::dropdown("Mode", &::features->aimbot_mode, modes, IM_ARRAYSIZE(modes));
                                static const char* hitbox_items[] = { "Head", "Upper Torso", "Humanoid Root" };
                                c_widgets::dropdown("Hitbox", &::features->aimbot_hitbox, hitbox_items, IM_ARRAYSIZE(hitbox_items));
                                c_widgets::checkbox("Nearest Part", &::features->aimbot_nearest_part);
                                c_widgets::checkbox("Closest Point", &::features->enable_aimbot_closest_point);
                                c_widgets::checkbox("Sticky Aim", &::features->aimbot_sticky);
                            }
                            else
                            {
                                c_widgets::checkbox("Only Enemies", &::features->aimbot_only_enemies);
                                c_widgets::checkbox("Occluded Check", &::features->aimbot_visibility_check);
                                c_widgets::checkbox("Offscreen Check", &::features->aimbot_offscreen_check);
                                const char* check_items[] = { "Team", "Health", "K.O", "Grabbed", "Reloading", "Typing" };
                                bool check_values[] = {
                                    ::features->aimbot_check_team,
                                    ::features->aimbot_check_health,
                                    ::features->aimbot_check_knocked,
                                    ::features->aimbot_check_grabbed,
                                    ::features->aimbot_check_reloading,
                                    ::features->aimbot_check_typing
                                };
                                if (c_widgets::multi_dropdown("Checks", check_values, check_items, IM_ARRAYSIZE(check_items)))
                                {
                                    ::features->aimbot_check_team = check_values[0];
                                    ::features->aimbot_check_health = check_values[1];
                                    ::features->aimbot_check_knocked = check_values[2];
                                    ::features->aimbot_check_grabbed = check_values[3];
                                    ::features->aimbot_check_reloading = check_values[4];
                                    ::features->aimbot_check_typing = check_values[5];
                                }

                                c_widgets::checkbox("Smoothing", &::features->enable_aimbot_smooth);
                                if (::features->enable_aimbot_smooth)
                                {
                                    const float smooth_max = (::features->aimbot_mode == 1) ? 50.0f : 100.0f;
                                    c_widgets::slider_float("Smoothness X", &::features->aimbot_smooth_x, 1.0f, smooth_max, "%.1f");
                                    c_widgets::slider_float("Smoothness Y", &::features->aimbot_smooth_y, 1.0f, smooth_max, "%.1f");
                                }

                                c_widgets::checkbox("Prediction", &features->enable_aimbot_prediction);
                                if (features->enable_aimbot_prediction)
                                {
                                    static const char* prediction_modes[] = { "Default", "Ballistic" };
                                    c_widgets::dropdown("Prediction Mode", &features->aimbot_prediction_mode, prediction_modes, IM_ARRAYSIZE(prediction_modes));
                                    if (features->aimbot_prediction_mode == 0)
                                    {
                                        c_widgets::slider_float("Prediction X", &features->aimbot_prediction_x, 0.0f, 20.0f, "%.2f");
                                        c_widgets::slider_float("Prediction Y", &features->aimbot_prediction_y, 0.0f, 20.0f, "%.2f");
                                    }
                                }

                                c_widgets::slider_float("Max Distance", &::features->aimbot_max_distance, 0.0f, 5000.0f, "%.0f");
                                c_widgets::checkbox("Draw FOV", &::features->aimbot_draw_fov);
                                c_widgets::checkbox("Target HUD", &::features->enable_target_hud);
                                if (::features->enable_target_hud)
                                {
                                    static const char* target_hud_anchors[] = {
                                        "Right", "Left", "Top", "Bottom",
                                        "Top Right", "Top Left", "Bottom Right", "Bottom Left"
                                    };
                                    c_widgets::dropdown("HUD Side", &::features->target_hud_anchor, target_hud_anchors, IM_ARRAYSIZE(target_hud_anchors));
                                    c_widgets::slider_float("HUD Offset", &::features->target_hud_offset, 16.0f, 120.0f, "%.0f px");
                                }
                                if (::features->aimbot_draw_fov)
                                {
                                    static const char* fov_modes[] = { "Center", "Mouse" };
                                    c_widgets::dropdown("FOV Position", &::features->aimbot_fov_mode, fov_modes, IM_ARRAYSIZE(fov_modes));
                                }
                                c_widgets::checkbox("Limit FOV", &::features->aimbot_limit_fov);
                                if (::features->aimbot_limit_fov || ::features->aimbot_draw_fov)
                                {
                                    c_widgets::slider_float("FOV Radius", &::features->aimbot_fov_radius, 10.0f, 1000.0f, "%.0f");
                                }
                            }
                        }, false, true, true);

                    render_panel("##panel_top_right", "Triggerbot", top_right, panel_size_half, [&]()
                        {
                            render_subtabs("##triggerbot_subtabs", triggerbot_subtab, triggerbot_tabs, IM_ARRAYSIZE(triggerbot_tabs), 0.0f, 0.0f, 0.0f, 0.0f);
                            if (triggerbot_subtab == 0)
                            {
                                c_widgets::checkbox("Enable", &::features->enable_triggerbot);
                                c_widgets::keybind("##triggerbot_key", ::features->triggerbot_keybind);

                                static const char* hitbox_items[] = { "Head", "Upper Torso", "Humanoid Root" };
                                c_widgets::dropdown("Hitbox", &::features->triggerbot_hitbox, hitbox_items, IM_ARRAYSIZE(hitbox_items));
                                c_widgets::checkbox("Nearest Part", &::features->triggerbot_nearest_part);
                                c_widgets::checkbox("Closest Point", &::features->enable_triggerbot_closest_point);
                                c_widgets::checkbox("Sticky Aim", &::features->triggerbot_sticky);
                                c_widgets::checkbox("Hold Fire (Auto)", &::features->triggerbot_hold_fire);
                                c_widgets::slider_float("Delay (ms)", &::features->triggerbot_delay_ms, 0.0f, 300.0f, "%.0f");
                            }
                            else
                            {
                                c_widgets::checkbox("Only Enemies", &::features->triggerbot_only_enemies);
                                c_widgets::checkbox("Occluded Check", &::features->triggerbot_visibility_check);

                                const char* check_items[] = { "Team", "Health", "K.O", "Grabbed", "Reloading", "Typing" };
                                bool check_values[] = {
                                    ::features->triggerbot_check_team,
                                    ::features->triggerbot_check_health,
                                    ::features->triggerbot_check_knocked,
                                    ::features->triggerbot_check_grabbed,
                                    ::features->triggerbot_check_reloading,
                                    ::features->triggerbot_check_typing
                                };
                                if (c_widgets::multi_dropdown("Checks", check_values, check_items, IM_ARRAYSIZE(check_items)))
                                {
                                    ::features->triggerbot_check_team = check_values[0];
                                    ::features->triggerbot_check_health = check_values[1];
                                    ::features->triggerbot_check_knocked = check_values[2];
                                    ::features->triggerbot_check_grabbed = check_values[3];
                                    ::features->triggerbot_check_reloading = check_values[4];
                                    ::features->triggerbot_check_typing = check_values[5];
                                }

                                c_widgets::checkbox("Draw FOV", &::features->triggerbot_draw_fov);
                                if (::features->triggerbot_draw_fov)
                                {
                                    static const char* fov_modes[] = { "Center", "Mouse" };
                                    c_widgets::dropdown("FOV Position", &::features->triggerbot_fov_mode, fov_modes, IM_ARRAYSIZE(fov_modes));
                                }
                                c_widgets::checkbox("Limit FOV", &::features->triggerbot_limit_fov);
                                if (::features->triggerbot_limit_fov || ::features->triggerbot_draw_fov)
                                {
                                    c_widgets::slider_float("FOV Radius", &::features->triggerbot_fov_radius, 10.0f, 1000.0f, "%.0f");
                                }

                                c_widgets::slider_float("Max Distance", &::features->triggerbot_max_distance, 0.0f, 5000.0f, "%.0f");
                            }
                        }, false, true, true);

                    render_panel("##panel_bottom_left", "Silent Aim", bottom_left, panel_size_half, [&]()
                        {
                            render_subtabs("##silent_subtabs", silent_subtab, silent_tabs, IM_ARRAYSIZE(silent_tabs), 1.0f, 1.0f, 1.0f, 1.0f);
                            if (silent_subtab == 0)
                            {
                                c_widgets::checkbox("Enable", &::features->enable_free_aim);
                                c_widgets::keybind("##free_aim_key", ::features->free_aim_keybind);

                                static const char* silent_mode_items[] = { "Free Aim", "Viewport" };
                                c_widgets::dropdown("Silent Mode", &::features->free_aim_silent_mode, silent_mode_items, IM_ARRAYSIZE(silent_mode_items));
                                if (globals->game_id == 292439477)
                                {
                                    const ImVec4 disabled = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
                                    c_widgets::text_colored(disabled, "Phantom Forces: uses camera aim (same as aimbot). Disable aimbot first. Invisible silent needs Lua hooks.");
                                }

                                static const char* hitbox_items[] = { "Head", "Upper Torso", "Humanoid Root" };
                                c_widgets::dropdown("Hitbox", &::features->free_aim_hitbox, hitbox_items, IM_ARRAYSIZE(hitbox_items));
                                c_widgets::checkbox("Nearest Part", &::features->free_aim_nearest_part);
                                c_widgets::checkbox("Closest Point", &::features->enable_free_aim_closest_point);
                                c_widgets::checkbox("Sticky Aim", &::features->free_aim_sticky);
                                c_widgets::checkbox("Mouse Spoof", &::features->free_aim_mouse_spoof);
                            }
                            else
                            {
                                c_widgets::checkbox("Only Enemies", &::features->free_aim_only_enemies);
                                c_widgets::checkbox("Occluded Check", &::features->free_aim_visibility_check);
                                const char* check_items[] = { "Team", "Health", "K.O", "Grabbed", "Typing" };
                                bool check_values[] = {
                                    ::features->free_aim_check_team,
                                    ::features->free_aim_check_health,
                                    ::features->free_aim_check_knocked,
                                    ::features->free_aim_check_grabbed,
                                    ::features->free_aim_check_typing
                                };
                                if (c_widgets::multi_dropdown("Checks", check_values, check_items, IM_ARRAYSIZE(check_items)))
                                {
                                    ::features->free_aim_check_team = check_values[0];
                                    ::features->free_aim_check_health = check_values[1];
                                    ::features->free_aim_check_knocked = check_values[2];
                                    ::features->free_aim_check_grabbed = check_values[3];
                                    ::features->free_aim_check_typing = check_values[4];
                                }

                                c_widgets::checkbox("Prediction", &::features->free_aim_enable_prediction);
                                if (::features->free_aim_enable_prediction)
                                {
                                    static const char* prediction_modes[] = { "Default", "Ballistic" };
                                    c_widgets::dropdown("Prediction Mode", &::features->free_aim_prediction_mode, prediction_modes, IM_ARRAYSIZE(prediction_modes));
                                    if (::features->free_aim_prediction_mode == 0)
                                    {
                                        c_widgets::slider_float("Prediction X", &::features->free_aim_prediction_x, 0.0f, 30.0f, "%.2f");
                                        c_widgets::slider_float("Prediction Y", &::features->free_aim_prediction_y, 0.0f, 30.0f, "%.2f");
                                    }
                                }
                                c_widgets::slider_float("Max Distance", &::features->free_aim_max_distance, 0.0f, 5000.0f, "%.0f");
                                c_widgets::checkbox("Draw FOV", &::features->free_aim_draw_fov);
                                if (::features->free_aim_draw_fov)
                                {
                                    static const char* fov_modes[] = { "Center", "Mouse" };
                                    c_widgets::dropdown("FOV Position", &::features->free_aim_fov_mode, fov_modes, IM_ARRAYSIZE(fov_modes));
                                }
                                c_widgets::checkbox("Limit FOV", &::features->free_aim_limit_fov);
                                if (::features->free_aim_limit_fov)
                                {
                                    c_widgets::slider_float("FOV Radius", &::features->free_aim_fov_radius, 10.0f, 1000.0f, "%.0f");
                                }
                            }
                        }, false, true, true);

                    render_panel("##panel_bottom_right", "Extra", bottom_right, panel_size_half, []() {}, false);
                }
                else if (tab_index == 1)
                {
                    render_panel("##panel_top_left", "Player ESP", top_left, panel_size_half, []()
                        {
                            menu::render_visuals();
                        });
                    render_panel("##panel_top_right", "Effects", top_right, panel_size_half, [&]()
                        {
                            menu::render_effects();
                        });
                    render_panel("##panel_bottom_left", "World", bottom_left, panel_size_half, []()
                        {
                            bool lighting_changed = false;

                            lighting_changed |= c_widgets::checkbox("Brightness", &features->lighting_enable_brightness);

                            if (features->lighting_enable_brightness)
                            {
                                lighting_changed |= c_widgets::slider_float("Brightness", &features->lighting_brightness_value, 0.0f, 20.0f, "%.1f");
                            }

                            lighting_changed |= c_widgets::checkbox("Ambient", &features->lighting_enable_ambient);

                            ImVec4 ambient_color(features->lighting_ambient_color.x, features->lighting_ambient_color.y, features->lighting_ambient_color.z, 1.0f);
                            if (c_widgets::colorpicker("##ambient_color", ambient_color, ImGui::GetFrameHeight() * 0.825f))
                            {
                                features->lighting_ambient_color = { ambient_color.x, ambient_color.y, ambient_color.z };
                                lighting_changed = true;
                            }

                            lighting_changed |= c_widgets::checkbox("Outdoor Ambient", &features->lighting_enable_outdoor_ambient);

                            ImVec4 outdoor_color(features->lighting_outdoor_ambient_color.x, features->lighting_outdoor_ambient_color.y, features->lighting_outdoor_ambient_color.z, 1.0f);
                            if (c_widgets::colorpicker("##outdoor_ambient_color", outdoor_color, ImGui::GetFrameHeight() * 0.825f))
                            {
                                features->lighting_outdoor_ambient_color = { outdoor_color.x, outdoor_color.y, outdoor_color.z };
                                lighting_changed = true;
                            }

                            lighting_changed |= c_widgets::checkbox("ColorShift Top", &features->lighting_enable_colorshift_top);

                            ImVec4 colorshift_top(features->lighting_colorshift_top.x, features->lighting_colorshift_top.y, features->lighting_colorshift_top.z, 1.0f);
                            if (c_widgets::colorpicker("##colorshift_top_color", colorshift_top, ImGui::GetFrameHeight() * 0.825f))
                            {
                                features->lighting_colorshift_top = { colorshift_top.x, colorshift_top.y, colorshift_top.z };
                                lighting_changed = true;
                            }

                            lighting_changed |= c_widgets::checkbox("ColorShift Bottom", &features->lighting_enable_colorshift_bottom);

                            ImVec4 colorshift_bottom(features->lighting_colorshift_bottom.x, features->lighting_colorshift_bottom.y, features->lighting_colorshift_bottom.z, 1.0f);
                            if (c_widgets::colorpicker("##colorshift_bottom_color", colorshift_bottom, ImGui::GetFrameHeight() * 0.825f))
                            {
                                features->lighting_colorshift_bottom = { colorshift_bottom.x, colorshift_bottom.y, colorshift_bottom.z };
                                lighting_changed = true;
                            }

                            lighting_changed |= c_widgets::checkbox("Exposure Compensation", &features->lighting_enable_exposure_compensation);

                            if (features->lighting_enable_exposure_compensation)
                            {
                                lighting_changed |= c_widgets::slider_float("Exposure Compensation", &features->lighting_exposure_compensation_value, -10.0f, 10.0f, "%.1f");
                            }

                            lighting_changed |= c_widgets::checkbox("Fog Color", &features->lighting_enable_fog_color);

                            ImVec4 fog_color(features->lighting_fog_color.x, features->lighting_fog_color.y, features->lighting_fog_color.z, 1.0f);
                            if (c_widgets::colorpicker("##fog_color", fog_color, ImGui::GetFrameHeight() * 0.825f))
                            {
                                features->lighting_fog_color = { fog_color.x, fog_color.y, fog_color.z };
                                lighting_changed = true;
                            }

                            lighting_changed |= c_widgets::checkbox("Fog Start", &features->lighting_enable_fog_start);

                            if (features->lighting_enable_fog_start)
                            {
                                lighting_changed |= c_widgets::slider_float("Fog Start", &features->lighting_fog_start, 0.0f, 20000.0f, "%.0f");
                            }

                            lighting_changed |= c_widgets::checkbox("Fog End", &features->lighting_enable_fog_end);

                            if (features->lighting_enable_fog_end)
                            {
                                lighting_changed |= c_widgets::slider_float("Fog End", &features->lighting_fog_end, 0.0f, 20000.0f, "%.0f");
                            }

                            lighting_changed |= c_widgets::checkbox("Skybox", &features->lighting_enable_skybox);

                            static const char* skybox_items[] = {
                                "Default",
                                "Aurora Blue",
                                "Nebula",
                                "Realistic",
                                "Dark"
                            };

                            int preset = features->lighting_skybox_preset;
                            if (c_widgets::dropdown("Skybox Preset", &preset, skybox_items, IM_ARRAYSIZE(skybox_items)))
                            {
                                preset = std::clamp(preset, 0, static_cast<int>(IM_ARRAYSIZE(skybox_items)) - 1);
                                features->lighting_skybox_preset = preset;
                                lighting_changed = true;
                            }
                            float star_count = static_cast<float>(features->lighting_star_count);
                            if (c_widgets::slider_float("Star Count", &star_count, 0.0f, 6000.0f, "%.0f"))
                            {
                                features->lighting_star_count = static_cast<int>(star_count);
                                lighting_changed = true;
                            }
                            lighting_changed |= c_widgets::checkbox("Sun Texture", &features->lighting_enable_sun_texture);
                            lighting_changed |= c_widgets::checkbox("Moon Texture", &features->lighting_enable_moon_texture);
                            c_widgets::slider_float("Reapply Every (s)", &features->lighting_reapply_interval_seconds, 0.1f, 10.0f, "%.1f");

                            if (lighting_changed)
                            {
                                lighting::apply();
                                lighting::force_renderview_flag();
                            }
                        });
                    render_panel("##panel_bottom_right", "Other", bottom_right, panel_size_half, []()
                        {
                            menu::render_additional_esp_settings();
                        });
                }
                else if (tab_index == 2)
                {
                    render_panel("##panel_top_left", "Settings", top_left, panel_size_half, [&]()
                        {
                            c_keybind& menu_key = c_ui::menu_key_bind();

                            if (c_widgets::button("Exit"))
                            {
                                exit_requested = true;
                                done = true;
                                ::PostQuitMessage(0);
                            }
                            c_widgets::checkbox("Hide Console", &features->hide_console);
                            c_widgets::checkbox("Stream-Proof", &features->stream_proof);
                            c_widgets::checkbox("V-Sync", &features->enable_vsync);
                            c_widgets::checkbox("Watermark", &features->show_watermark);
                            c_widgets::checkbox("Spotify Player", &features->show_spotify_player);
                            c_widgets::checkbox("Keybind List", &features->show_keybinds_list);
                            c_widgets::checkbox("Player List", &features->show_player_list);
                            c_widgets::checkbox("AI Chat Window", &features->show_ai_chat_window);
                            c_widgets::checkbox("Appearance Window", &features->show_appearance_window);
                            c_widgets::checkbox("Lua Editor Window", &features->show_lua_editor_window);
                            c_widgets::checkbox("Console Window", &features->show_console_window);
                            c_widgets::checkbox("ESP Preview Window", &features->show_esp_preview_window);
                            c_widgets::checkbox("Configs Window", &features->show_configs_window);
                        });
                    render_panel("##panel_top_right", "Other", top_right, panel_size_half, []()
                        {
                            c_widgets::checkbox("Explorer", &features->show_testing_explorer_window);
                            c_widgets::checkbox("Primitive Wireframes", &features->show_visibility_debug_primitives);
                            c_widgets::checkbox("Raycast Engine", &features->enable_raycast_engine);
                            
                        });
                    render_panel("##panel_bottom_left", "Movement", bottom_left, panel_size_half, []()
                        {
                            static const char* walkspeed_modes[] = { "Position", "Humanoid" };

                            c_widgets::checkbox("Enable Fly", &features->enable_fly);
                            c_widgets::keybind("##fly_key", features->fly_keybind);

                            c_widgets::checkbox("Check Typing", &features->fly_check_typing);
                            c_widgets::slider_float("Fly Speed", &features->fly_speed, 5.0f, 1000.0f, "%.0f");
                            c_widgets::slider_float("Vertical Multiplier", &features->fly_vertical_boost, 0.1f, 3.0f, "%.2f");
                            c_widgets::slider_float("Damping", &features->fly_damping, 0.0f, 50.0f, "%.1f");
                            c_widgets::checkbox("Enable Walkspeed", &features->enable_walkspeed);
                            c_widgets::keybind("##walkspeed_key", features->walkspeed_keybind);
                            c_widgets::dropdown("Walkspeed Mode", &features->walkspeed_mode, walkspeed_modes, IM_ARRAYSIZE(walkspeed_modes));
                            c_widgets::slider_float("Walkspeed", &features->walkspeed_value, 1.0f, 500.0f, "%.0f");
                            c_widgets::checkbox("Enable Bhop", &features->enable_bhop);
                            c_widgets::keybind("##bhop_key", features->bhop_keybind);
                            c_widgets::slider_float("Bhop Speed", &features->bhop_speed, 1.0f, 500.0f, "%.0f");
                            c_widgets::checkbox("Enable Noclip", &features->enable_noclip);
                            c_widgets::keybind("##noclip_key", features->noclip_keybind);
                        });
                    render_panel("##panel_bottom_right", "Client", bottom_right, panel_size_half, []()
                        {
                            const ImVec4 desync_notice_color(1.0f, 0.55f, 0.12f, 1.0f);
                            c_widgets::checkbox("Desync", &features->desync);
                            if (ImGui::IsItemHovered())
                            {
                                ImGui::BeginTooltip();
                                ImGui::TextColored(desync_notice_color, "To enable server sided desync, reset ur character");
                                ImGui::EndTooltip();
                            }
                            c_widgets::keybind("##desync_key", features->desync_keybind);

                            c_widgets::checkbox("Auto Shooter", &features->enable_auto_shooter);
                            if (features->enable_auto_shooter)
                            {
                                c_widgets::colorpicker("##host_color", features->host_color, ImGui::GetFrameHeight() * 0.825f);
                                c_widgets::slider_float("Delay (ms)", &features->host_click_delay_ms, 0.0f, 300.0f, "%.0f");
                                const auto local_state = cache::localplayer->snapshot();
                                float bullets_max = local_state.tool_max_ammo > 0 ? static_cast<float>(local_state.tool_max_ammo) : 1.0f;
                                bullets_max = (std::max)(bullets_max, 1.0f);
                                features->bullets_sent = std::clamp(features->bullets_sent, 1.0f, bullets_max);
                                c_widgets::slider_float("Bullets Sent", &features->bullets_sent, 1.0f, bullets_max, "%.0f");
                            }
                            c_widgets::checkbox("Freeze Players", &features->freeze_players);
                            c_widgets::keybind("##freeze_players_key", features->freeze_players_keybind);
                            c_widgets::checkbox("Tickrate Modifier", &features->enable_tickrate_modifier);
                            if (features->enable_tickrate_modifier)
                            {
                                c_widgets::slider_float("Tickrate Value", &features->tickrate_modifier_value, 1.0f, 660.0f, "%.0f");
                            }
                        });
                }
                else
                {
                    const int lua_tab_offset = tab_index - 3;
                    if (lua_tab_offset >= 0 && lua_tab_offset < static_cast<int>(lua_tabs.size()))
                    {
                        const std::string panel_title = lua_tabs[lua_tab_offset].name.empty()
                            ? std::string("Lua Tab")
                            : lua_tabs[lua_tab_offset].name;
                        render_panel("##panel_lua_tab", panel_title.c_str(), grid_origin, ImVec2(grid_avail.x, panel_vertical_space), [&]()
                            {
                                lua_ui_bridge::render_tab_widgets(lua_tabs[lua_tab_offset].id);
                            }, true, false, true);
                    }
                }

                float panel_block_height = panel_size_half.y > 0.0f ? (panel_size_half.y * 2.0f + grid_spacing) : 0.0f;
                if (tab_index >= 3)
                {
                    panel_block_height = panel_vertical_space;
                }
                ImVec2 footer_pos = ImVec2(grid_origin.x, grid_origin.y + panel_block_height + footer_spacing);
                ImGui::SetCursorPos(footer_pos);

                if (c_widgets::begin_padded_child("##footer", 0, ImGuiWindowFlags_NoScrollbar, ImVec2(-1.0f, footer_height), true, false, false))
                {
                    const auto local_footer = cache::localplayer->snapshot();
                    const std::string footer_name = !local_footer.display_name.empty()
                        ? local_footer.display_name
                        : (!local_footer.name.empty() ? local_footer.name : "player");

                    ImVec2 text_line_pos = ImGui::GetCursorPos();
                    const float footer_right_edge = text_line_pos.x + ImGui::GetContentRegionAvail().x;

                    ImGui::SetCursorPos(ImVec2(text_line_pos.x, text_line_pos.y - 1.0f));
                    ImGui::TextColored(c_colors::text_muted, "welcome back, ");
                    ImGui::SameLine(0.0f, 2.0f);
                    ImGui::PushFont(c_fonts::verdana_bold);
                    ImGui::TextColored(c_colors::top_accent_color, "%s", footer_name.c_str());
                    ImGui::PopFont();

                    const char* left_bracket = "[ ";
                    const char* text = "roblox";
                    const char* right_bracket = " ]";

                    char fps_text[32];
                    ImFormatString(fps_text, IM_ARRAYSIZE(fps_text), "%.0f fps", io.Framerate);

                    ImVec2 left_size = ImGui::CalcTextSize(left_bracket);
                    ImVec2 text_size = ImGui::CalcTextSize(text);
                    ImVec2 right_size = ImGui::CalcTextSize(right_bracket);
                    ImVec2 fps_size = ImGui::CalcTextSize(fps_text);

                    const float label_width = left_size.x + text_size.x + right_size.x;
                    const float total_width = label_width + ImGui::GetStyle().ItemSpacing.x ;
                    ImVec2 right_pos(footer_right_edge - total_width, text_line_pos.y - 1.0f);

                    ImGui::SetCursorPos(right_pos);
                    ImGui::TextColored(c_colors::text_muted, "%s", left_bracket);

                    ImGui::SameLine(0, 0);
                    ImGui::PushFont(c_fonts::verdana_bold);
                    ImGui::TextColored(c_colors::top_accent_color, "%s", text);
                    ImGui::PopFont();

                    ImGui::SameLine(0, 0);
                    ImGui::TextColored(c_colors::text_muted, "%s", right_bracket);

                    
                    
                    
                }
                c_widgets::end_padded_child();

            }
            c_widgets::end_padded_child();
            ImGui::End();

            ImGui::PopStyleVar();
        }

        lua_vm::begin_aux_window_hittest_frame();

        if (menu_showing)
        {
            const float aux_alpha = ImGui::GetStyle().Alpha * eased_alpha;
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, aux_alpha);

            if (::features->show_configs_window)
            {
                render_configs_window();
            }
            if (::features->show_ai_chat_window)
            {
                render_ai_chat_window();
            }
            if (::features->show_appearance_window)
            {
                draw_appearance_window();
            }
            if (::features->show_esp_preview_window)
            {
                draw_esp_preview_window();
            }
            if (::features->show_testing_explorer_window)
            {
                draw_testing_explorer_window();
            }
            if (::features->show_player_list)
            {
                update_player_list_from_cache();
                ImVec2 player_list_pos;
                ImVec2 player_list_size(0.0f, 0.0f);
                const bool has_configs_frame = ::features->show_configs_window
                    && g_configs_has_frame
                    && g_configs_last_size.x > 0.0f
                    && g_configs_last_size.y > 0.0f;
                const bool has_appearance_frame = ::features->show_appearance_window
                    && g_appearance_has_frame
                    && g_appearance_last_size.x > 0.0f
                    && g_appearance_last_size.y > 0.0f;
                const bool has_menu_frame = g_menu_has_frame
                    && g_menu_last_size.x > 0.0f
                    && g_menu_last_size.y > 0.0f;
                if (has_configs_frame || has_appearance_frame)
                {
                    const float gap = 12.0f;
                    float anchor_x = has_configs_frame ? g_configs_last_pos.x : g_appearance_last_pos.x;
                    float anchor_bottom = has_configs_frame
                        ? (g_configs_last_pos.y + g_configs_last_size.y)
                        : (g_appearance_last_pos.y + g_appearance_last_size.y);
                    if (has_configs_frame && has_appearance_frame)
                    {
                        anchor_x = ImMin(g_configs_last_pos.x, g_appearance_last_pos.x);
                        const float configs_bottom = g_configs_last_pos.y + g_configs_last_size.y;
                        const float appearance_bottom = g_appearance_last_pos.y + g_appearance_last_size.y;
                        anchor_bottom = ImMax(configs_bottom, appearance_bottom);
                    }
                    player_list_pos = ImVec2(anchor_x - 10.0f, anchor_bottom + gap);
                }
                else if (has_menu_frame)
                {
                    const float gap = 12.0f;
                    player_list_pos = ImVec2(g_menu_last_pos.x + g_menu_last_size.x + gap, g_menu_last_pos.y);
                }
                else
                {
                    player_list_pos = ImVec2(60.0f, 68.0f);
                }
                draw_player_list(player_list_pos, player_list_size, eased_alpha);
            }

            render_lua_scripts_window();

            ImGui::PopStyleVar();
        }

        refresh_overlay_input_for_floating_widgets(hwnd);

        if (::features->show_keybinds_list)
            render_keybinds_window();
        if (::features->show_watermark)
            render_watermark_window();
        if (::features->show_spotify_player)
            render_media_player_widget();
        else
        {
            g_media_island_hittest_active = false;
            g_media_lyrics_panel_hittest_active = false;
            g_media_lyrics_open = false;
        }

        }
        else
        {
            g_menu_showing_state.store(false, std::memory_order_relaxed);
            blurred_window(hwnd, false, 0.0f);
        }

        if (menu_showing && !splash_active)
            draw_custom_cursor(ImGui::GetIO());

        draw_loading_splash(splash_elapsed);

        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        esp::render_highlight_mesh_material_pass(g_pd3dDevice, g_pd3dDeviceContext, g_mainRenderTargetView);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        UINT sync_interval = features->enable_vsync ? 1u : 0u;
        HRESULT hr = g_pSwapChain->Present(sync_interval, 0);

        if (!features->enable_vsync)
        {
            constexpr auto k_min_yield = std::chrono::milliseconds(1);
            const auto frame_end = std::chrono::steady_clock::now();
            const auto elapsed = frame_end - frame_start;
            if (elapsed < k_min_yield)
            {
                std::this_thread::sleep_for(k_min_yield - elapsed);
            }
        }
    }

    lua_vm::shutdown();
    vanille::media::stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    DestroyGrenadeTexture();
    DestroyDeathImageTexture();
    destroy_media_art_texture();
    DestroyLogoTexture();
    DestroyCustomCursorTexture();
    cleanup_device_d3d();
    ::DestroyWindow(hwnd);
    ::UnregisterClass(wc.lpszClassName, wc.hInstance);

    return;
}

bool vanille::overlay::create_device_d3d(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    create_render_target();
    return true;
}

void vanille::overlay::cleanup_device_d3d()
{
    cleanup_render_target();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void vanille::overlay::create_render_target()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void vanille::overlay::cleanup_render_target()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_SETCURSOR)
    {
        if (g_menu_showing_state.load(std::memory_order_relaxed) && LOWORD(lParam) == HTCLIENT)
        {
            ::SetCursor(nullptr);
            return TRUE;
        }
    }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_NCHITTEST:
    {
        if (g_overlay_accepts_input.load(std::memory_order_relaxed))
            return HTCLIENT;

        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (::ScreenToClient(hWnd, &pt))
        {
            if (g_watermark_island_hittest_active && ::PtInRect(&g_watermark_island_client_rect, pt))
                return HTCLIENT;
            if (g_media_island_hittest_active && ::PtInRect(&g_media_island_client_rect, pt))
                return HTCLIENT;
            if (g_media_lyrics_panel_hittest_active && ::PtInRect(&g_media_lyrics_panel_client_rect, pt))
                return HTCLIENT;
            if (lua_vm::client_point_over_aux_windows(hWnd, pt.x, pt.y))
                return HTCLIENT;
        }
        return HTTRANSPARENT;
    }
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool vanille::overlay::is_window_focus(const HWND hWnd)
{
    char lpCurrentWindowUsedClass[125];
    char lpCurrentWindowClass[125];
    char lpOverlayWindowClass[125];

    const HWND ForegroundWindow = GetForegroundWindow();
    if (GetClassName(ForegroundWindow, lpCurrentWindowUsedClass, sizeof(lpCurrentWindowUsedClass)) == 0)
        return false;

    if (GetClassName(g_rbx_window, lpCurrentWindowClass, sizeof(lpCurrentWindowClass)) == 0)
        return false;

    if (GetClassName(hWnd, lpOverlayWindowClass, sizeof(lpOverlayWindowClass)) == 0)
        return false;

    if (strcmp(lpCurrentWindowUsedClass, lpCurrentWindowClass) != 0 && strcmp(lpCurrentWindowUsedClass, lpOverlayWindowClass) != 0)
    {
        g_overlay_accepts_input.store(false, std::memory_order_relaxed);
        apply_overlay_window_style(hWnd, false);
        return false;
    }

    return true;
}

void vanille::overlay::move_window(const HWND hWnd)
{
    if (g_rbx_window == nullptr)
        return;

    if (!is_window_focus(hWnd))
    {
        SetWindowPos(hWnd, nullptr, 0, 0, 0, 0, SWP_HIDEWINDOW);
        return;
    }

    RECT clientRect;
    GetClientRect(g_rbx_window, &clientRect);

    POINT topLeft = { clientRect.left, clientRect.top };
    ClientToScreen(g_rbx_window, &topLeft);

    int lWindowWidth = clientRect.right - clientRect.left;
    int lWindowHeight = clientRect.bottom - clientRect.top;

    SetWindowPos(
        hWnd,
        nullptr,
        topLeft.x,
        topLeft.y,
        lWindowWidth,
        lWindowHeight,
        SWP_SHOWWINDOW
    );
}
