#pragma once
#include <memory>
#include <Windows.h>
#include <string>
#include <array>
#include <cmath>
#include <cstdint>
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>
#include "gui/globals/globals.h"

struct ImGuiIO;
struct ImVec2;
class c_keybind;

namespace vanille
{
	namespace overlay
	{
		extern HWND g_rbx_window;
		extern HWND g_overlay_window;

		struct player_list_entry
		{
			std::string name;
			std::string display_name;
			std::string username;
			std::string role;
			std::uint64_t user_id = 0;
			std::uintptr_t address = 0;
			std::uintptr_t character = 0;
			std::uintptr_t humanoid = 0;
			std::uintptr_t root_part_primitive = 0;
			std::uintptr_t humanoid_root_part = 0;
			float distance = 0.0f;
			bool selectable = true;
			bool is_local = false;
		};

		void run_overlay();

		bool create_device_d3d(HWND hWnd);
		void cleanup_device_d3d();
		void create_render_target();
		void cleanup_render_target();

		void move_window(const HWND hWnd);
		bool is_window_focus(const HWND hWnd);

		void apply_theme_preset(int index);
		bool& overlay_blur_enabled();
		bool is_menu_open();

		c_keybind& menu_key_bind();
		void clear_player_list();
		void add_player_to_list(const player_list_entry& entry);
		void set_player_list_width(float width);
		void draw_player_list(const ImVec2& pos, const ImVec2& size, float alpha);
		int get_player_status(std::uint64_t user_id, const std::string& name);
		bool is_host(std::uint64_t user_id, const std::string& name);

		void request_avatar_texture(std::uint64_t user_id);
		ImTextureID get_avatar_texture(std::uint64_t user_id, int* out_width = nullptr, int* out_height = nullptr);

	}
}
