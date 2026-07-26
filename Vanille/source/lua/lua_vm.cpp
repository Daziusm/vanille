#include "lua/lua_vm.h"

#include "lua/datamodel.h"
#include "lua/instance.h"
#include "lua/lua_console.h"
#include "lua/lua_drawing.h"
#include "lua/lua_ui_bridge.h"
#include "lua/script_storage.h"
#include "cache/local_player_cache.h"
#include "cache/player_cache.h"
#include "globals/globals.h"
#include "sdk/camera.h"
#include "sdk/player.h"
#include "gui/colors/colors.h"
#include "gui/globals/globals.h"
#include "gui/overlay.hpp"
#include "gui/widgets/widgets.h"
#include <text_editor/TextEditor.h>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C"
{
    struct lua_state;
    using lua_c_function = int(__cdecl*)(lua_state* state);
    using lua_number = double;
    using lua_integer = long long;
    using lua_k_context = intptr_t;
    using lua_k_function = int(__cdecl*)(lua_state* state, int status, lua_k_context context);
}

namespace lua_vm
{
    static std::vector<RECT> g_aux_window_hittest_rects;

    namespace
    {
        constexpr int lua_registry_index = -1001000;
        constexpr int lua_mult_ret = -1;
        constexpr int lua_type_nil = 0;
        constexpr int lua_type_boolean = 1;
        constexpr int lua_type_number = 3;
        constexpr int lua_type_string = 4;
        constexpr int lua_type_table = 5;
        constexpr int lua_type_function = 6;
        constexpr int lua_type_userdata = 7;
        constexpr const char* instance_metatable_name = "vanille_instance_meta";
        constexpr const char* drawing_metatable_name = "vanille_drawing_meta";

        struct lua_api
        {
            lua_state* (__cdecl* lua_l_newstate)() = nullptr;
            void (__cdecl* lua_close)(lua_state* state) = nullptr;
            void (__cdecl* lua_l_openlibs)(lua_state* state) = nullptr;
            int (__cdecl* lua_gettop)(lua_state* state) = nullptr;
            void (__cdecl* lua_settop)(lua_state* state, int index) = nullptr;
            void (__cdecl* lua_pushvalue)(lua_state* state, int index) = nullptr;
            int (__cdecl* lua_type)(lua_state* state, int index) = nullptr;
            int (__cdecl* lua_toboolean)(lua_state* state, int index) = nullptr;
            const char* (__cdecl* lua_tolstring)(lua_state* state, int index, std::size_t* length) = nullptr;
            lua_number (__cdecl* lua_tonumberx)(lua_state* state, int index, int* valid) = nullptr;
            lua_integer (__cdecl* lua_tointegerx)(lua_state* state, int index, int* valid) = nullptr;
            void* (__cdecl* lua_touserdata)(lua_state* state, int index) = nullptr;
            void (__cdecl* lua_pushnil)(lua_state* state) = nullptr;
            void (__cdecl* lua_pushboolean)(lua_state* state, int value) = nullptr;
            void (__cdecl* lua_pushnumber)(lua_state* state, lua_number value) = nullptr;
            void (__cdecl* lua_pushinteger)(lua_state* state, lua_integer value) = nullptr;
            const char* (__cdecl* lua_pushlstring)(lua_state* state, const char* string, std::size_t length) = nullptr;
            void (__cdecl* lua_pushcclosure)(lua_state* state, lua_c_function function, int upvalues) = nullptr;
            void (__cdecl* lua_createtable)(lua_state* state, int array_count, int record_count) = nullptr;
            void (__cdecl* lua_setfield)(lua_state* state, int index, const char* key) = nullptr;
            int (__cdecl* lua_getfield)(lua_state* state, int index, const char* key) = nullptr;
            void (__cdecl* lua_setglobal)(lua_state* state, const char* name) = nullptr;
            int (__cdecl* lua_getglobal)(lua_state* state, const char* name) = nullptr;
            int (__cdecl* lua_rawgeti)(lua_state* state, int index, lua_integer item_index) = nullptr;
            void (__cdecl* lua_rawseti)(lua_state* state, int index, lua_integer item_index) = nullptr;
            std::size_t (__cdecl* lua_rawlen)(lua_state* state, int index) = nullptr;
            int (__cdecl* lua_l_loadbufferx)(lua_state* state, const char* buffer, std::size_t size, const char* name, const char* mode) = nullptr;
            int (__cdecl* lua_pcallk)(lua_state* state, int argument_count, int result_count, int error_function, lua_k_context context, lua_k_function continuation) = nullptr;
            int (__cdecl* lua_l_ref)(lua_state* state, int index) = nullptr;
            void (__cdecl* lua_l_unref)(lua_state* state, int index, int reference) = nullptr;
            const char* (__cdecl* lua_l_tolstring)(lua_state* state, int index, std::size_t* length) = nullptr;
            void (__cdecl* lua_l_traceback)(lua_state* state, lua_state* source, const char* message, int level) = nullptr;
            void* (__cdecl* lua_newuserdata)(lua_state* state, std::size_t size) = nullptr;
            void* (__cdecl* lua_newuserdatauv)(lua_state* state, std::size_t size, int nuvalue) = nullptr;
            int (__cdecl* lua_setmetatable)(lua_state* state, int index) = nullptr;
            int (__cdecl* lua_l_newmetatable)(lua_state* state, const char* name) = nullptr;
        };

        struct scheduled_callback
        {
            double due_time_seconds = 0.0;
            int callback_ref = -1;
        };

        struct lua_instance_ref
        {
            sandbox::instance* instance_ptr = nullptr;
        };

        struct lua_drawing_ref
        {
            int object_id = 0;
        };

        struct pending_instance_callback
        {
            int callback_ref = -1;
            std::shared_ptr<sandbox::instance> argument;
            std::string context;
        };

        struct lua_runtime_state
        {
            HMODULE module = nullptr;
            lua_api api{};
            lua_state* state = nullptr;
            bool ready = false;
            double current_time_seconds = 0.0;
            std::vector<scheduled_callback> scheduled_callbacks;
            std::shared_ptr<sandbox::data_model> root_model;
            std::unordered_map<sandbox::instance*, std::shared_ptr<sandbox::instance>> instance_lookup;
            std::vector<std::shared_ptr<sandbox::instance>> detached_instances;
            std::unordered_map<std::uintptr_t, std::shared_ptr<sandbox::instance>> player_instances_by_address;
            std::unordered_map<std::uintptr_t, std::shared_ptr<sandbox::instance>> workspace_instances_by_address;
            std::vector<int> players_player_added_callbacks;
            std::unordered_map<sandbox::instance*, std::vector<int>> player_character_added_callbacks;
            std::vector<pending_instance_callback> pending_instance_callbacks;
        };

        struct editor_state
        {
            int selected_script_index = -1;
            std::string selected_script_name;
            std::string script_source;
            std::string saved_script_source;
            char script_name[128]{};
            std::unordered_set<int> selected_console_lines;
            bool initialized = false;
            bool focus_console_next_frame = false;
            std::string vm_init_error;
        };

        lua_runtime_state g_runtime;
        editor_state g_editor;
        TextEditor g_script_text_editor;
        bool g_script_text_editor_initialized = false;
        std::mutex g_runtime_mutex;

        ImVec4 lighten_color(const ImVec4& color, float amount)
        {
            ImVec4 output = color;
            output.x = ImLerp(color.x, 1.0f, amount);
            output.y = ImLerp(color.y, 1.0f, amount);
            output.z = ImLerp(color.z, 1.0f, amount);
            return output;
        }

        ImVec4 darken_color(const ImVec4& color, float amount)
        {
            ImVec4 output = color;
            output.x = ImLerp(color.x, 0.0f, amount);
            output.y = ImLerp(color.y, 0.0f, amount);
            output.z = ImLerp(color.z, 0.0f, amount);
            return output;
        }

        void draw_gradient_text(const char* text, const ImVec4& left_color, const ImVec4& right_color)
        {
            if (!text || !*text)
            {
                return;
            }

            ImFont* font = ImGui::GetFont();
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            const ImVec2 text_size = ImGui::CalcTextSize(text);

            if (!font || !draw_list || text_size.x <= 0.0f || text_size.y <= 0.0f)
            {
                ImGui::TextUnformatted(text ? text : "");
                return;
            }

            const float font_size = ImGui::GetFontSize();
            const ImVec2 text_position = ImGui::GetCursorScreenPos();
            float cursor_offset = 0.0f;

            for (const char* current = text; *current; ++current)
            {
                const char character[2] = { *current, 0 };
                const ImVec2 character_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, character);
                const float factor = (cursor_offset + character_size.x * 0.5f) / text_size.x;
                const ImVec4 color = ImLerp(left_color, right_color, factor);
                draw_list->AddText(font, font_size, ImVec2(text_position.x + cursor_offset, text_position.y), ImGui::ColorConvertFloat4ToU32(color), character);
                cursor_offset += character_size.x;
            }

            ImGui::Dummy(text_size);
        }

        void draw_accent_gradient_text(const char* text)
        {
            const ImVec4 left_color = lighten_color(c_colors::top_accent_color, 0.35f);
            const ImVec4 right_color = darken_color(c_colors::top_accent_color, 0.25f);
            draw_gradient_text(text, left_color, right_color);
        }

        void draw_window_background()
        {
            if (ImGui::IsWindowCollapsed())
            {
                return;
            }

            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            const ImVec2 window_position = ImGui::GetWindowPos();
            const ImVec2 window_size = ImGui::GetWindowSize();
            const ImVec2 window_bottom_right(window_position.x + window_size.x, window_position.y + window_size.y);

            const float accent_padding = 3.0f;
            const ImVec2 accent_min(window_position.x - accent_padding, window_position.y - accent_padding);
            const ImVec2 accent_max(window_bottom_right.x + accent_padding, window_bottom_right.y + accent_padding);
            draw_list->AddRectFilled(accent_min, accent_max, ImGui::GetColorU32(c_colors::top_accent_color));

            const ImU32 top_color = ImGui::GetColorU32(c_colors::top_window_background);
            const ImU32 bottom_color = ImGui::GetColorU32(c_colors::bottom_window_background);
            const ImU32 border_color = ImGui::GetColorU32(c_colors::main_border);

            draw_list->AddRectFilledMultiColor(window_position, window_bottom_right, top_color, top_color, bottom_color, bottom_color);
            const float border_thickness = 1.0f;
            const ImVec2 inner_min(window_position.x + border_thickness, window_position.y + border_thickness);
            const ImVec2 inner_max(window_bottom_right.x - border_thickness, window_bottom_right.y - border_thickness);
            draw_list->AddRect(inner_min, inner_max, border_color, 0.0f, 0, border_thickness);
        }

        void draw_draggable_window_header(const char* drag_id, const char* title)
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            if (!window)
            {
                return;
            }

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
            {
                ImGui::SetWindowPos(window->Pos + ImGui::GetIO().MouseDelta);
            }

            draw_list->AddText(
                font,
                font_size,
                ImVec2(window_pos.x + pad_x, window_pos.y + pad_y),
                ImGui::GetColorU32(c_colors::white),
                title);

            ImGui::SetCursorPos(ImVec2(style.WindowPadding.x, header_height - style.WindowPadding.y));
        }

        template <std::size_t buffer_size>
        void copy_text_to_buffer(char (&buffer)[buffer_size], const std::string& text)
        {
            const std::size_t max_length = buffer_size - 1;
            const std::size_t copy_length = text.size() < max_length ? text.size() : max_length;
            if (copy_length > 0)
            {
                std::memcpy(buffer, text.data(), copy_length);
            }
            buffer[copy_length] = '\0';
        }

        int absolute_index(lua_state* state, int index)
        {
            if (index > 0 || index <= lua_registry_index)
            {
                return index;
            }
            return g_runtime.api.lua_gettop(state) + index + 1;
        }

        void pop_values(lua_state* state, int count)
        {
            if (count > 0)
            {
                g_runtime.api.lua_settop(state, -count - 1);
            }
        }

        void push_c_function(lua_state* state, lua_c_function function)
        {
            g_runtime.api.lua_pushcclosure(state, function, 0);
        }

        void* new_userdata(lua_state* state, std::size_t size)
        {
            if (g_runtime.api.lua_newuserdata)
            {
                return g_runtime.api.lua_newuserdata(state, size);
            }
            if (g_runtime.api.lua_newuserdatauv)
            {
                return g_runtime.api.lua_newuserdatauv(state, size, 0);
            }
            return nullptr;
        }

        std::string stack_to_string(lua_state* state, int index, const std::string& fallback = std::string())
        {
            std::size_t length = 0;
            const char* text = g_runtime.api.lua_tolstring(state, index, &length);
            if (!text)
            {
                return fallback;
            }
            return std::string(text, length);
        }

        std::string stack_value_to_string(lua_state* state, int index)
        {
            const int absolute = absolute_index(state, index);
            std::size_t length = 0;
            const char* text = g_runtime.api.lua_l_tolstring(state, absolute, &length);
            std::string output = text ? std::string(text, length) : std::string();
            pop_values(state, 1);
            return output;
        }

        int to_integer(lua_state* state, int index, int fallback)
        {
            int valid = 0;
            const lua_integer value = g_runtime.api.lua_tointegerx(state, index, &valid);
            if (!valid)
            {
                return fallback;
            }
            if (value < static_cast<lua_integer>(INT_MIN))
            {
                return INT_MIN;
            }
            if (value > static_cast<lua_integer>(INT_MAX))
            {
                return INT_MAX;
            }
            return static_cast<int>(value);
        }

        float to_number(lua_state* state, int index, float fallback)
        {
            int valid = 0;
            const lua_number value = g_runtime.api.lua_tonumberx(state, index, &valid);
            return valid ? static_cast<float>(value) : fallback;
        }

        bool to_boolean(lua_state* state, int index, bool fallback)
        {
            return g_runtime.api.lua_type(state, index) == lua_type_nil ? fallback : (g_runtime.api.lua_toboolean(state, index) != 0);
        }

        std::filesystem::path module_directory()
        {
            wchar_t module_path[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
            return length == 0 ? std::filesystem::current_path() : std::filesystem::path(module_path).parent_path();
        }

        template <typename function_type>
        bool load_proc_address(HMODULE module, function_type& target, const char* name)
        {
            target = reinterpret_cast<function_type>(GetProcAddress(module, name));
            return target != nullptr;
        }

        void unload_runtime();
        void release_lua_ref(int callback_ref);
        void track_instance_recursive(const std::shared_ptr<sandbox::instance>& instance_ptr);
        bool push_instance_userdata(lua_state* state, const std::shared_ptr<sandbox::instance>& instance_ptr);
        bool push_drawing_userdata(lua_state* state, const std::shared_ptr<lua_drawing::drawing_object>& object_ptr);
        void push_signal_table(lua_state* state, const std::string& signal_name, const std::shared_ptr<sandbox::instance>& owner);
        int lua_instance_index(lua_state* state);
        int lua_instance_newindex(lua_state* state);
        int lua_instance_tostring(lua_state* state);
        int lua_signal_connect(lua_state* state);
        int lua_instance_get_players(lua_state* state);
        int lua_drawing_index(lua_state* state);
        int lua_drawing_newindex(lua_state* state);
        int lua_drawing_tostring(lua_state* state);

        std::string resolve_player_name(const std::string& name, const std::string& display_name, std::uint64_t user_id, std::uintptr_t address)
        {
            if (!name.empty())
            {
                return name;
            }
            if (!display_name.empty())
            {
                return display_name;
            }
            if (user_id != 0)
            {
                return "player_" + std::to_string(user_id);
            }
            if (address != 0)
            {
                return "player_" + std::to_string(address);
            }
            return "player";
        }

        void sync_players_service_from_cache_locked()
        {
            if (!g_runtime.root_model)
            {
                return;
            }

            const std::shared_ptr<sandbox::instance> players_service = g_runtime.root_model->get_service("Players");
            if (!players_service)
            {
                return;
            }

            const std::shared_ptr<const std::vector<cache::player_state>> snapshot = cache::players_cache ? cache::players_cache->snapshot() : nullptr;
            const cache::local_player_state local_snapshot = cache::localplayer ? cache::localplayer->snapshot() : cache::local_player_state{};
            std::optional<rbx::Matrix> view_matrix;
            std::optional<rbx::Vector2> view_dimensions;
            if (globals && globals->visualengine.is_valid())
            {
                const rbx::visualengine_t visual_engine(globals->visualengine.get_address());
                view_matrix = visual_engine.get_view_matrix();
                view_dimensions = visual_engine.get_dimensions();
            }

            std::unordered_map<std::uintptr_t, std::shared_ptr<sandbox::instance>> next_instances;
            next_instances.reserve((snapshot ? snapshot->size() : 0) + 4);
            std::vector<std::shared_ptr<sandbox::instance>> player_added_events;
            std::vector<std::pair<std::shared_ptr<sandbox::instance>, std::shared_ptr<sandbox::instance>>> character_added_events;

            auto update_head_attributes = [&](const std::shared_ptr<sandbox::instance>& head_instance, const cache::primitive_part& head_part)
            {
                if (!head_instance)
                {
                    return;
                }

                if (!head_part.instance.is_valid())
                {
                    head_instance->set_attribute("address", std::monostate{});
                    head_instance->set_attribute("position_x", std::monostate{});
                    head_instance->set_attribute("position_y", std::monostate{});
                    head_instance->set_attribute("position_z", std::monostate{});
                    head_instance->set_attribute("screen_x", std::monostate{});
                    head_instance->set_attribute("screen_y", std::monostate{});
                    head_instance->set_attribute("on_screen", false);
                    return;
                }

                head_instance->set_attribute("address", static_cast<double>(head_part.instance.get_address()));
                const std::optional<rbx::Vector3> position = head_part.instance.get_position(head_part.primitive);
                if (!position)
                {
                    head_instance->set_attribute("position_x", std::monostate{});
                    head_instance->set_attribute("position_y", std::monostate{});
                    head_instance->set_attribute("position_z", std::monostate{});
                    head_instance->set_attribute("screen_x", std::monostate{});
                    head_instance->set_attribute("screen_y", std::monostate{});
                    head_instance->set_attribute("on_screen", false);
                    return;
                }

                head_instance->set_attribute("position_x", static_cast<double>(position->x));
                head_instance->set_attribute("position_y", static_cast<double>(position->y));
                head_instance->set_attribute("position_z", static_cast<double>(position->z));

                bool on_screen = false;
                if (view_matrix && view_dimensions)
                {
                    const std::optional<rbx::Vector2> screen = rbx::camera::world_to_screen(*position, *view_matrix, *view_dimensions);
                    if (screen)
                    {
                        head_instance->set_attribute("screen_x", static_cast<double>(screen->x));
                        head_instance->set_attribute("screen_y", static_cast<double>(screen->y));
                        on_screen = true;
                    }
                    else
                    {
                        head_instance->set_attribute("screen_x", std::monostate{});
                        head_instance->set_attribute("screen_y", std::monostate{});
                    }
                }
                else
                {
                    head_instance->set_attribute("screen_x", std::monostate{});
                    head_instance->set_attribute("screen_y", std::monostate{});
                }
                head_instance->set_attribute("on_screen", on_screen);
            };

            auto ensure_player_character = [&](const std::shared_ptr<sandbox::instance>& player_instance, const cache::character_parts* character_parts, const rbx::instance_t& character_instance)
            {
                if (!player_instance)
                {
                    return;
                }

                std::shared_ptr<sandbox::instance> character_node = player_instance->find_first_child("Character");
                if (!character_node)
                {
                    character_node = std::make_shared<sandbox::instance>("Model", "Character");
                    track_instance_recursive(character_node);
                }
                character_node->set_parent(player_instance);

                if (character_instance.is_valid())
                {
                    character_node->set_attribute("address", static_cast<double>(character_instance.get_address()));
                    const std::string model_name = character_instance.get_name();
                    if (!model_name.empty())
                    {
                        character_node->set_attribute("model_name", model_name);
                    }
                    else
                    {
                        character_node->set_attribute("model_name", std::monostate{});
                    }
                }
                else
                {
                    character_node->set_attribute("address", std::monostate{});
                    character_node->set_attribute("model_name", std::monostate{});
                }

                std::shared_ptr<sandbox::instance> head_node = character_node->find_first_child("Head");
                if (!head_node)
                {
                    head_node = std::make_shared<sandbox::instance>("Part", "Head");
                    track_instance_recursive(head_node);
                }
                head_node->set_parent(character_node);

                if (character_parts)
                {
                    update_head_attributes(head_node, character_parts->head);
                }
                else
                {
                    head_node->set_attribute("address", std::monostate{});
                    head_node->set_attribute("position_x", std::monostate{});
                    head_node->set_attribute("position_y", std::monostate{});
                    head_node->set_attribute("position_z", std::monostate{});
                    head_node->set_attribute("screen_x", std::monostate{});
                    head_node->set_attribute("screen_y", std::monostate{});
                    head_node->set_attribute("on_screen", false);
                }
            };

            auto upsert_player = [&](std::uintptr_t address, const std::string& name, const std::string& display_name, std::uint64_t user_id, bool is_local, const cache::character_parts* character_parts, const rbx::instance_t& character_instance)
            {
                if (address == 0)
                {
                    return;
                }

                std::shared_ptr<sandbox::instance> player_instance;
                bool is_existing_player = false;
                const auto existing = g_runtime.player_instances_by_address.find(address);
                if (existing != g_runtime.player_instances_by_address.end())
                {
                    player_instance = existing->second;
                    is_existing_player = true;
                }

                if (!player_instance)
                {
                    player_instance = std::make_shared<sandbox::instance>("Player", resolve_player_name(name, display_name, user_id, address));
                    track_instance_recursive(player_instance);
                }

                std::uintptr_t previous_character_address = 0;
                if (const std::optional<sandbox::attribute_value> previous_value = player_instance->get_attribute("character_address"))
                {
                    if (std::holds_alternative<double>(*previous_value))
                    {
                        const double raw_value = std::get<double>(*previous_value);
                        if (raw_value > 0.0)
                        {
                            previous_character_address = static_cast<std::uintptr_t>(raw_value);
                        }
                    }
                }
                bool existing_is_local = false;
                if (const std::optional<sandbox::attribute_value> previous_local = player_instance->get_attribute("is_local"))
                {
                    if (std::holds_alternative<bool>(*previous_local))
                    {
                        existing_is_local = std::get<bool>(*previous_local);
                    }
                }

                player_instance->set_name(resolve_player_name(name, display_name, user_id, address));
                player_instance->set_attribute("address", static_cast<double>(address));
                player_instance->set_attribute("user_id", static_cast<double>(user_id));
                if (!name.empty())
                {
                    player_instance->set_attribute("name", name);
                    player_instance->set_attribute("username", name);
                }
                else
                {
                    player_instance->set_attribute("name", std::monostate{});
                    player_instance->set_attribute("username", std::monostate{});
                }
                if (!display_name.empty())
                {
                    player_instance->set_attribute("display_name", display_name);
                }
                else
                {
                    player_instance->set_attribute("display_name", std::monostate{});
                }
                player_instance->set_attribute("is_local", is_local || existing_is_local);
                player_instance->set_parent(players_service);

                std::uintptr_t character_address = previous_character_address;
                bool has_character_payload = false;
                if (character_instance.is_valid())
                {
                    character_address = character_instance.get_address();
                    has_character_payload = true;
                }
                else if (character_parts && character_parts->head.instance.is_valid())
                {
                    character_address = character_parts->head.instance.get_address();
                    has_character_payload = true;
                }

                if (has_character_payload)
                {
                    if (character_address != 0)
                    {
                        player_instance->set_attribute("character_address", static_cast<double>(character_address));
                    }
                    else
                    {
                        player_instance->set_attribute("character_address", std::monostate{});
                    }
                }

                if (has_character_payload)
                {
                    ensure_player_character(player_instance, character_parts, character_instance);
                }

                if (!is_existing_player)
                {
                    player_added_events.push_back(player_instance);
                }

                if (has_character_payload && character_address != 0 && character_address != previous_character_address)
                {
                    if (const std::shared_ptr<sandbox::instance> character_node = player_instance->find_first_child("Character"))
                    {
                        character_added_events.push_back({ player_instance, character_node });
                    }
                }

                next_instances[address] = player_instance;
            };

            if (local_snapshot.address != 0)
            {
                players_service->set_attribute("local_player_address", static_cast<double>(local_snapshot.address));
            }
            else
            {
                players_service->set_attribute("local_player_address", std::monostate{});
            }
            upsert_player(
                local_snapshot.address,
                local_snapshot.name,
                local_snapshot.display_name,
                local_snapshot.user_id,
                true,
                (local_snapshot.character.is_valid() || local_snapshot.parts.head.instance.is_valid()) ? &local_snapshot.parts : nullptr,
                local_snapshot.character);

            if (snapshot)
            {
                for (const cache::player_state& player_state : *snapshot)
                {
                    upsert_player(
                        player_state.address,
                        player_state.name,
                        player_state.display_name,
                        player_state.user_id,
                        false,
                        (player_state.character.is_valid() || player_state.parts.head.instance.is_valid()) ? &player_state.parts : nullptr,
                        player_state.character);
                }
            }

            if (globals)
            {
                rbx::instance_t players_root = globals->players;
                if (!players_root.is_valid() && globals->datamodel.is_valid())
                {
                    players_root = globals->datamodel.find_first_child_by_class("Players");
                }

                if (players_root.is_valid())
                {
                    std::uintptr_t local_address = 0;
                    if (const auto local_player = rbx::player::get_local_player(players_root))
                    {
                        if (local_player->is_valid())
                        {
                            local_address = local_player->get_address();
                        }
                    }

                    const std::vector<rbx::instance_t> children = players_root.get_children();
                    for (const rbx::instance_t& child : children)
                    {
                        if (!child.is_valid() || child.get_class_name() != "Player")
                        {
                            continue;
                        }

                        std::uint64_t user_id = 0;
                        if (const auto id = rbx::player::get_user_id(child))
                        {
                            user_id = *id;
                        }

                        std::string display_name;
                        if (const auto name = rbx::player::get_display_name(child))
                        {
                            display_name = *name;
                        }

                        upsert_player(child.get_address(), child.get_name(), display_name, user_id, local_address != 0 && child.get_address() == local_address, nullptr, rbx::instance_t{});
                    }
                }

                if (next_instances.empty() && globals->workspace.is_valid())
                {
                    const std::vector<rbx::instance_t> workspace_children = globals->workspace.get_children();
                    for (const rbx::instance_t& child : workspace_children)
                    {
                        if (!child.is_valid() || child.get_class_name() != "Model")
                        {
                            continue;
                        }

                        if (!child.find_first_child_by_class("Humanoid").is_valid())
                        {
                            continue;
                        }

                        const std::string model_name = child.get_name();
                        upsert_player(child.get_address(), model_name, model_name, 0, false, nullptr, rbx::instance_t{});
                    }
                }
            }

            for (const auto& pair : g_runtime.player_instances_by_address)
            {
                if (next_instances.find(pair.first) == next_instances.end() && pair.second)
                {
                    pair.second->set_parent(nullptr);
                    g_runtime.detached_instances.push_back(pair.second);
                    const auto callbacks_iterator = g_runtime.player_character_added_callbacks.find(pair.second.get());
                    if (callbacks_iterator != g_runtime.player_character_added_callbacks.end())
                    {
                        for (int callback_ref : callbacks_iterator->second)
                        {
                            release_lua_ref(callback_ref);
                        }
                        g_runtime.player_character_added_callbacks.erase(callbacks_iterator);
                    }
                }
            }

            g_runtime.player_instances_by_address.swap(next_instances);
            for (const std::shared_ptr<sandbox::instance>& player_instance : player_added_events)
            {
                if (!player_instance)
                {
                    continue;
                }
                for (int callback_ref : g_runtime.players_player_added_callbacks)
                {
                    g_runtime.pending_instance_callbacks.push_back({ callback_ref, player_instance, "players_player_added_callback" });
                }
            }
            for (const auto& event_item : character_added_events)
            {
                const std::shared_ptr<sandbox::instance>& player_instance = event_item.first;
                const std::shared_ptr<sandbox::instance>& character_instance_ptr = event_item.second;
                if (!player_instance || !character_instance_ptr)
                {
                    continue;
                }
                const auto callbacks_iterator = g_runtime.player_character_added_callbacks.find(player_instance.get());
                if (callbacks_iterator == g_runtime.player_character_added_callbacks.end())
                {
                    continue;
                }
                for (int callback_ref : callbacks_iterator->second)
                {
                    g_runtime.pending_instance_callbacks.push_back({ callback_ref, character_instance_ptr, "player_character_added_callback" });
                }
            }
        }

        void sync_workspace_service_from_globals_locked()
        {
            if (!g_runtime.root_model)
            {
                return;
            }

            const std::shared_ptr<sandbox::instance> workspace_service = g_runtime.root_model->get_service("Workspace");
            if (!workspace_service)
            {
                return;
            }

            std::unordered_map<std::uintptr_t, std::shared_ptr<sandbox::instance>> next_instances;

            if (globals && globals->workspace.is_valid())
            {
                const std::vector<rbx::instance_t> workspace_children = globals->workspace.get_children();
                next_instances.reserve(workspace_children.size());

                for (const rbx::instance_t& child : workspace_children)
                {
                    if (!child.is_valid())
                    {
                        continue;
                    }

                    const std::uintptr_t address = child.get_address();
                    if (address == 0)
                    {
                        continue;
                    }

                    const std::string class_name = child.get_class_name();
                    const std::string name = child.get_name();

                    std::shared_ptr<sandbox::instance> child_instance;
                    const auto existing = g_runtime.workspace_instances_by_address.find(address);
                    if (existing != g_runtime.workspace_instances_by_address.end() &&
                        existing->second &&
                        existing->second->get_class_name() == class_name)
                    {
                        child_instance = existing->second;
                    }
                    else if (existing != g_runtime.workspace_instances_by_address.end() && existing->second)
                    {
                        existing->second->set_parent(nullptr);
                        g_runtime.detached_instances.push_back(existing->second);
                    }

                    if (!child_instance)
                    {
                        child_instance = std::make_shared<sandbox::instance>(class_name, name);
                        track_instance_recursive(child_instance);
                    }

                    child_instance->set_name(name);
                    child_instance->set_attribute("address", static_cast<double>(address));
                    child_instance->set_parent(workspace_service);
                    next_instances[address] = child_instance;
                }
            }

            for (const auto& pair : g_runtime.workspace_instances_by_address)
            {
                if (next_instances.find(pair.first) == next_instances.end() && pair.second)
                {
                    pair.second->set_parent(nullptr);
                    g_runtime.detached_instances.push_back(pair.second);
                }
            }

            g_runtime.workspace_instances_by_address.swap(next_instances);
        }

        bool load_library_and_api()
        {
            if (g_runtime.module)
            {
                return true;
            }

            const std::filesystem::path module_dir = module_directory();
            const std::vector<std::wstring> candidates = {
                (module_dir / L"lua53-64.dll").wstring(),
                (module_dir / L"lua54.dll").wstring(),
                (module_dir / L"lua53.dll").wstring(),
                (module_dir / L"lua5.4.dll").wstring(),
                (module_dir / L"lua5.3.dll").wstring(),
                L"lua53-64.dll",
                L"lua54.dll",
                L"lua53.dll",
                L"lua5.4.dll",
                L"lua5.3.dll"
            };

            for (const std::wstring& candidate : candidates)
            {
                HMODULE module = LoadLibraryW(candidate.c_str());
                if (module)
                {
                    g_runtime.module = module;
                    break;
                }
            }

            if (!g_runtime.module)
            {
                g_editor.vm_init_error = "Lua DLL not found. Place lua53-64.dll next to vanille.exe.";
                lua_console::push_error("lua_runtime_load_failed");
                return false;
            }

            bool loaded = true;
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_l_newstate, "luaL_newstate");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_close, "lua_close");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_l_openlibs, "luaL_openlibs");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_gettop, "lua_gettop");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_settop, "lua_settop");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_pushvalue, "lua_pushvalue");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_type, "lua_type");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_toboolean, "lua_toboolean");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_tolstring, "lua_tolstring");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_tonumberx, "lua_tonumberx");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_tointegerx, "lua_tointegerx");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_touserdata, "lua_touserdata");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_pushnil, "lua_pushnil");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_pushboolean, "lua_pushboolean");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_pushnumber, "lua_pushnumber");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_pushinteger, "lua_pushinteger");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_pushlstring, "lua_pushlstring");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_pushcclosure, "lua_pushcclosure");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_createtable, "lua_createtable");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_setfield, "lua_setfield");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_getfield, "lua_getfield");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_setglobal, "lua_setglobal");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_getglobal, "lua_getglobal");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_rawgeti, "lua_rawgeti");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_rawseti, "lua_rawseti");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_rawlen, "lua_rawlen");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_l_loadbufferx, "luaL_loadbufferx");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_pcallk, "lua_pcallk");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_l_ref, "luaL_ref");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_l_unref, "luaL_unref");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_l_tolstring, "luaL_tolstring");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_l_traceback, "luaL_traceback");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_setmetatable, "lua_setmetatable");
            loaded = loaded && load_proc_address(g_runtime.module, g_runtime.api.lua_l_newmetatable, "luaL_newmetatable");

            load_proc_address(g_runtime.module, g_runtime.api.lua_newuserdata, "lua_newuserdata");
            load_proc_address(g_runtime.module, g_runtime.api.lua_newuserdatauv, "lua_newuserdatauv");

            if (!g_runtime.api.lua_newuserdata && !g_runtime.api.lua_newuserdatauv)
            {
                loaded = false;
            }

            if (!loaded)
            {
                lua_console::push_error("lua_runtime_symbol_load_failed");
                unload_runtime();
                return false;
            }

            return true;
        }

        void unload_runtime()
        {
            if (g_runtime.state)
            {
                g_runtime.api.lua_close(g_runtime.state);
                g_runtime.state = nullptr;
            }
            if (g_runtime.module)
            {
                FreeLibrary(g_runtime.module);
                g_runtime.module = nullptr;
            }
            g_runtime.api = lua_api{};
            g_runtime.ready = false;
            g_runtime.current_time_seconds = 0.0;
            g_runtime.scheduled_callbacks.clear();
            g_runtime.root_model.reset();
            g_runtime.instance_lookup.clear();
            g_runtime.detached_instances.clear();
            g_runtime.player_instances_by_address.clear();
            g_runtime.workspace_instances_by_address.clear();
            g_runtime.players_player_added_callbacks.clear();
            g_runtime.player_character_added_callbacks.clear();
            g_runtime.pending_instance_callbacks.clear();
        }

        void set_global_nil(lua_state* state, const char* name)
        {
            g_runtime.api.lua_pushnil(state);
            g_runtime.api.lua_setglobal(state, name);
        }

        int traceback_handler(lua_state* state)
        {
            const std::string message = stack_to_string(state, 1, "lua_runtime_error");
            g_runtime.api.lua_l_traceback(state, state, message.c_str(), 1);
            return 1;
        }

        void release_lua_ref(int callback_ref)
        {
            if (g_runtime.state && callback_ref >= 0)
            {
                g_runtime.api.lua_l_unref(g_runtime.state, lua_registry_index, callback_ref);
            }
        }

        void release_lua_refs(const std::vector<int>& callback_refs)
        {
            for (int callback_ref : callback_refs)
            {
                release_lua_ref(callback_ref);
            }
        }

        int lua_print(lua_state* state)
        {
            const int top = g_runtime.api.lua_gettop(state);
            std::ostringstream stream;
            for (int index = 1; index <= top; ++index)
            {
                if (index > 1)
                {
                    stream << '\t';
                }
                stream << stack_value_to_string(state, index);
            }
            lua_console::push_info(stream.str());
            return 0;
        }

        int lua_warn(lua_state* state)
        {
            const int top = g_runtime.api.lua_gettop(state);
            std::ostringstream stream;
            for (int index = 1; index <= top; ++index)
            {
                if (index > 1)
                {
                    stream << '\t';
                }
                stream << stack_value_to_string(state, index);
            }
            lua_console::push_warning(stream.str());
            return 0;
        }

        int lua_loadstring(lua_state* state)
        {
            std::size_t length = 0;
            const char* source = g_runtime.api.lua_tolstring(state, 1, &length);
            if (!source)
            {
                g_runtime.api.lua_pushnil(state);
                g_runtime.api.lua_pushlstring(state, "loadstring_expected_string", std::strlen("loadstring_expected_string"));
                return 2;
            }

            const std::string chunk_name = stack_to_string(state, 2, "=loadstring");
            const int status = g_runtime.api.lua_l_loadbufferx(state, source, length, chunk_name.c_str(), nullptr);
            if (status != 0)
            {
                const std::string error_text = stack_to_string(state, -1, "loadstring_compile_error");
                pop_values(state, 1);
                g_runtime.api.lua_pushnil(state);
                g_runtime.api.lua_pushlstring(state, error_text.c_str(), error_text.size());
                return 2;
            }

            return 1;
        }

        int lua_load(lua_state* state)
        {
            return lua_loadstring(state);
        }

        int lua_getgenv(lua_state* state)
        {
            g_runtime.api.lua_getglobal(state, "_G");
            return 1;
        }

        void sanitize_globals(lua_state* state)
        {
            set_global_nil(state, "io");
            set_global_nil(state, "os");
            set_global_nil(state, "package");
            set_global_nil(state, "debug");
            set_global_nil(state, "dofile");
            set_global_nil(state, "loadfile");
            set_global_nil(state, "require");
        }

        void register_global_function(lua_state* state, const char* name, lua_c_function function)
        {
            push_c_function(state, function);
            g_runtime.api.lua_setglobal(state, name);
        }

        std::shared_ptr<sandbox::instance> lookup_instance(sandbox::instance* instance_ptr)
        {
            if (!instance_ptr)
            {
                return nullptr;
            }
            const auto iterator = g_runtime.instance_lookup.find(instance_ptr);
            if (iterator == g_runtime.instance_lookup.end())
            {
                return nullptr;
            }
            return iterator->second;
        }

        void track_instance_recursive(const std::shared_ptr<sandbox::instance>& instance_ptr)
        {
            if (!instance_ptr)
            {
                return;
            }
            g_runtime.instance_lookup[instance_ptr.get()] = instance_ptr;
            const std::vector<std::shared_ptr<sandbox::instance>> children = instance_ptr->get_children();
            for (const std::shared_ptr<sandbox::instance>& child : children)
            {
                track_instance_recursive(child);
            }
        }

        lua_instance_ref* get_instance_ref(lua_state* state, int index)
        {
            if (g_runtime.api.lua_type(state, index) != lua_type_userdata)
            {
                return nullptr;
            }
            return reinterpret_cast<lua_instance_ref*>(g_runtime.api.lua_touserdata(state, index));
        }

        std::shared_ptr<sandbox::instance> get_instance_shared(lua_state* state, int index)
        {
            lua_instance_ref* ref = get_instance_ref(state, index);
            if (!ref)
            {
                return nullptr;
            }
            return lookup_instance(ref->instance_ptr);
        }

        int lua_instance_find_first_child(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            if (!self)
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            push_instance_userdata(state, self->find_first_child(stack_to_string(state, 2, std::string())));
            return 1;
        }

        int lua_instance_find_first_descendant(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            if (!self)
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            push_instance_userdata(state, self->find_first_descendant(stack_to_string(state, 2, std::string())));
            return 1;
        }

        int lua_instance_get_children(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            g_runtime.api.lua_createtable(state, 0, 0);
            if (!self)
            {
                return 1;
            }

            const std::vector<std::shared_ptr<sandbox::instance>> children = self->get_children();
            for (std::size_t index = 0; index < children.size(); ++index)
            {
                push_instance_userdata(state, children[index]);
                g_runtime.api.lua_rawseti(state, -2, static_cast<lua_integer>(index + 1));
            }
            return 1;
        }

        int lua_instance_get_descendants(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            g_runtime.api.lua_createtable(state, 0, 0);
            if (!self)
            {
                return 1;
            }

            const std::vector<std::shared_ptr<sandbox::instance>> descendants = self->get_descendants();
            for (std::size_t index = 0; index < descendants.size(); ++index)
            {
                push_instance_userdata(state, descendants[index]);
                g_runtime.api.lua_rawseti(state, -2, static_cast<lua_integer>(index + 1));
            }
            return 1;
        }

        int lua_instance_is_descendant_of(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            std::shared_ptr<sandbox::instance> parent = get_instance_shared(state, 2);
            g_runtime.api.lua_pushboolean(state, self && parent && self->is_descendant_of(parent));
            return 1;
        }

        int lua_instance_is_ancestor_of(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            std::shared_ptr<sandbox::instance> child = get_instance_shared(state, 2);
            g_runtime.api.lua_pushboolean(state, self && child && self->is_ancestor_of(child));
            return 1;
        }

        int lua_instance_wait_for_child(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            if (!self)
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            const std::string name = stack_to_string(state, 2, std::string());
            const float timeout_seconds = to_number(state, 3, -1.0f);
            push_instance_userdata(state, self->wait_for_child(name, timeout_seconds));
            return 1;
        }

        bool parse_attribute_value(lua_state* state, int index, sandbox::attribute_value& out_value)
        {
            const int type = g_runtime.api.lua_type(state, index);
            if (type == lua_type_nil)
            {
                out_value = std::monostate{};
                return true;
            }
            if (type == lua_type_boolean)
            {
                out_value = g_runtime.api.lua_toboolean(state, index) != 0;
                return true;
            }
            if (type == lua_type_number)
            {
                out_value = static_cast<double>(to_number(state, index, 0.0f));
                return true;
            }
            if (type == lua_type_string)
            {
                out_value = stack_to_string(state, index);
                return true;
            }
            return false;
        }

        void push_attribute_value(lua_state* state, const sandbox::attribute_value& value)
        {
            if (std::holds_alternative<std::monostate>(value))
            {
                g_runtime.api.lua_pushnil(state);
            }
            else if (std::holds_alternative<bool>(value))
            {
                g_runtime.api.lua_pushboolean(state, std::get<bool>(value));
            }
            else if (std::holds_alternative<double>(value))
            {
                g_runtime.api.lua_pushnumber(state, std::get<double>(value));
            }
            else
            {
                const std::string& text = std::get<std::string>(value);
                g_runtime.api.lua_pushlstring(state, text.c_str(), text.size());
            }
        }

        int lua_instance_get_attribute(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            if (!self)
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            const std::optional<sandbox::attribute_value> value = self->get_attribute(stack_to_string(state, 2, std::string()));
            if (!value.has_value())
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            push_attribute_value(state, value.value());
            return 1;
        }

        int lua_instance_set_attribute(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            if (!self)
            {
                g_runtime.api.lua_pushboolean(state, 0);
                return 1;
            }
            sandbox::attribute_value value;
            if (!parse_attribute_value(state, 3, value))
            {
                g_runtime.api.lua_pushboolean(state, 0);
                return 1;
            }
            self->set_attribute(stack_to_string(state, 2, std::string()), value);
            g_runtime.api.lua_pushboolean(state, 1);
            return 1;
        }

        int lua_instance_get_attributes(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            g_runtime.api.lua_createtable(state, 0, 0);
            if (!self)
            {
                return 1;
            }

            const auto attributes = self->get_attributes();
            for (const auto& pair : attributes)
            {
                push_attribute_value(state, pair.second);
                g_runtime.api.lua_setfield(state, -2, pair.first.c_str());
            }
            return 1;
        }

        int lua_instance_get_service(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            std::shared_ptr<sandbox::data_model> model = std::dynamic_pointer_cast<sandbox::data_model>(self);
            if (!model)
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            push_instance_userdata(state, model->get_service(stack_to_string(state, 2, std::string())));
            return 1;
        }

        int lua_instance_get_players(lua_state* state)
        {
            return lua_instance_get_children(state);
        }

        int lua_signal_connect(lua_state* state)
        {
            if (g_runtime.api.lua_type(state, 1) != lua_type_table || g_runtime.api.lua_type(state, 2) != lua_type_function)
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }

            g_runtime.api.lua_pushvalue(state, 2);
            const int callback_ref = g_runtime.api.lua_l_ref(state, lua_registry_index);

            g_runtime.api.lua_getfield(state, 1, "__signal_name");
            const std::string signal_name = stack_to_string(state, -1, std::string());
            pop_values(state, 1);

            if (signal_name == "player_added")
            {
                g_runtime.players_player_added_callbacks.push_back(callback_ref);
                g_runtime.api.lua_pushboolean(state, 1);
                return 1;
            }

            if (signal_name == "character_added")
            {
                g_runtime.api.lua_getfield(state, 1, "__signal_owner");
                std::shared_ptr<sandbox::instance> owner = get_instance_shared(state, -1);
                pop_values(state, 1);
                if (!owner)
                {
                    release_lua_ref(callback_ref);
                    g_runtime.api.lua_pushnil(state);
                    return 1;
                }
                g_runtime.player_character_added_callbacks[owner.get()].push_back(callback_ref);
                g_runtime.api.lua_pushboolean(state, 1);
                return 1;
            }

            release_lua_ref(callback_ref);
            g_runtime.api.lua_pushnil(state);
            return 1;
        }

        void push_signal_table(lua_state* state, const std::string& signal_name, const std::shared_ptr<sandbox::instance>& owner)
        {
            g_runtime.api.lua_createtable(state, 0, 3);
            g_runtime.api.lua_pushlstring(state, signal_name.c_str(), signal_name.size());
            g_runtime.api.lua_setfield(state, -2, "__signal_name");
            if (owner)
            {
                push_instance_userdata(state, owner);
            }
            else
            {
                g_runtime.api.lua_pushnil(state);
            }
            g_runtime.api.lua_setfield(state, -2, "__signal_owner");
            push_c_function(state, lua_signal_connect);
            g_runtime.api.lua_setfield(state, -2, "Connect");
        }

        int lua_instance_index(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            if (!self)
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }

            const std::string key = stack_to_string(state, 2, std::string());
            const std::string class_name = self->get_class_name();
            if (key == "name" || key == "Name")
            {
                const std::string text = self->get_name();
                g_runtime.api.lua_pushlstring(state, text.c_str(), text.size());
                return 1;
            }
            if (key == "class_name" || key == "ClassName")
            {
                const std::string text = self->get_class_name();
                g_runtime.api.lua_pushlstring(state, text.c_str(), text.size());
                return 1;
            }
            if (key == "parent" || key == "Parent")
            {
                push_instance_userdata(state, self->get_parent());
                return 1;
            }
            if (key == "children" || key == "Children")
            {
                return lua_instance_get_children(state);
            }
            if ((key == "Character" || key == "character") && class_name == "Player")
            {
                push_instance_userdata(state, self->find_first_child("Character"));
                return 1;
            }
            if ((key == "LocalPlayer" || key == "local_player") && class_name == "Players")
            {
                for (const std::shared_ptr<sandbox::instance>& child : self->get_children())
                {
                    if (!child)
                    {
                        continue;
                    }
                    const std::optional<sandbox::attribute_value> is_local = child->get_attribute("is_local");
                    if (is_local && std::holds_alternative<bool>(*is_local) && std::get<bool>(*is_local))
                    {
                        push_instance_userdata(state, child);
                        return 1;
                    }
                }
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            if (key == "PlayerAdded" && class_name == "Players")
            {
                push_signal_table(state, "player_added", self);
                return 1;
            }
            if (key == "CharacterAdded" && class_name == "Player")
            {
                push_signal_table(state, "character_added", self);
                return 1;
            }

            if (key == "find_first_child" || key == "FindFirstChild")
            {
                push_c_function(state, lua_instance_find_first_child);
                return 1;
            }
            if (key == "find_first_descendant" || key == "FindFirstDescendant")
            {
                push_c_function(state, lua_instance_find_first_descendant);
                return 1;
            }
            if (key == "get_children" || key == "GetChildren")
            {
                push_c_function(state, lua_instance_get_children);
                return 1;
            }
            if (key == "get_descendants" || key == "GetDescendants")
            {
                push_c_function(state, lua_instance_get_descendants);
                return 1;
            }
            if (key == "is_descendant_of" || key == "IsDescendantOf")
            {
                push_c_function(state, lua_instance_is_descendant_of);
                return 1;
            }
            if (key == "is_ancestor_of" || key == "IsAncestorOf")
            {
                push_c_function(state, lua_instance_is_ancestor_of);
                return 1;
            }
            if (key == "wait_for_child" || key == "WaitForChild")
            {
                push_c_function(state, lua_instance_wait_for_child);
                return 1;
            }
            if (key == "get_attribute" || key == "GetAttribute")
            {
                push_c_function(state, lua_instance_get_attribute);
                return 1;
            }
            if (key == "set_attribute" || key == "SetAttribute")
            {
                push_c_function(state, lua_instance_set_attribute);
                return 1;
            }
            if (key == "get_attributes" || key == "GetAttributes")
            {
                push_c_function(state, lua_instance_get_attributes);
                return 1;
            }
            if (key == "get_service" || key == "GetService")
            {
                push_c_function(state, lua_instance_get_service);
                return 1;
            }
            if (key == "get_players" || key == "GetPlayers")
            {
                push_c_function(state, lua_instance_get_players);
                return 1;
            }

            if (const std::shared_ptr<sandbox::instance> child = self->find_first_child(key))
            {
                push_instance_userdata(state, child);
                return 1;
            }

            g_runtime.api.lua_pushnil(state);
            return 1;
        }

        int lua_instance_newindex(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            if (!self)
            {
                return 0;
            }

            const std::string key = stack_to_string(state, 2, std::string());
            if (key == "name" || key == "Name")
            {
                self->set_name(stack_to_string(state, 3, self->get_name()));
            }
            else if (key == "parent" || key == "Parent")
            {
                if (g_runtime.api.lua_type(state, 3) == lua_type_nil)
                {
                    self->set_parent(nullptr);
                }
                else
                {
                    std::shared_ptr<sandbox::instance> parent = get_instance_shared(state, 3);
                    if (parent)
                    {
                        self->set_parent(parent);
                    }
                }
            }
            return 0;
        }

        int lua_instance_tostring(lua_state* state)
        {
            std::shared_ptr<sandbox::instance> self = get_instance_shared(state, 1);
            if (!self)
            {
                g_runtime.api.lua_pushlstring(state, "Instance", std::strlen("Instance"));
                return 1;
            }
            const std::string text = self->get_class_name() + " " + self->get_name();
            g_runtime.api.lua_pushlstring(state, text.c_str(), text.size());
            return 1;
        }

        bool ensure_instance_metatable(lua_state* state)
        {
            const int created = g_runtime.api.lua_l_newmetatable(state, instance_metatable_name);
            if (created != 0)
            {
                push_c_function(state, lua_instance_index);
                g_runtime.api.lua_setfield(state, -2, "__index");
                push_c_function(state, lua_instance_newindex);
                g_runtime.api.lua_setfield(state, -2, "__newindex");
                push_c_function(state, lua_instance_tostring);
                g_runtime.api.lua_setfield(state, -2, "__tostring");
            }
            return true;
        }

        bool push_instance_userdata(lua_state* state, const std::shared_ptr<sandbox::instance>& instance_ptr)
        {
            if (!instance_ptr)
            {
                g_runtime.api.lua_pushnil(state);
                return false;
            }
            track_instance_recursive(instance_ptr);
            void* memory = new_userdata(state, sizeof(lua_instance_ref));
            if (!memory)
            {
                g_runtime.api.lua_pushnil(state);
                return false;
            }
            lua_instance_ref* ref = new (memory) lua_instance_ref();
            ref->instance_ptr = instance_ptr.get();
            if (!ensure_instance_metatable(state))
            {
                g_runtime.api.lua_pushnil(state);
                return false;
            }
            g_runtime.api.lua_setmetatable(state, -2);
            return true;
        }

        bool read_table_number_index(lua_state* state, int table_index, int index, float& out_value)
        {
            const int absolute = absolute_index(state, table_index);
            g_runtime.api.lua_rawgeti(state, absolute, index);
            if (g_runtime.api.lua_type(state, -1) != lua_type_number)
            {
                pop_values(state, 1);
                return false;
            }
            out_value = to_number(state, -1, out_value);
            pop_values(state, 1);
            return true;
        }

        bool read_vector2(lua_state* state, int table_index, lua_drawing::vector2_value& out_value)
        {
            if (g_runtime.api.lua_type(state, table_index) != lua_type_table)
            {
                return false;
            }
            float x = out_value.x;
            float y = out_value.y;
            bool ok = false;
            ok = read_table_number_index(state, table_index, 1, x) || ok;
            ok = read_table_number_index(state, table_index, 2, y) || ok;
            if (ok)
            {
                out_value.x = x;
                out_value.y = y;
            }
            return ok;
        }

        bool read_color(lua_state* state, int table_index, lua_drawing::color_value& out_value)
        {
            if (g_runtime.api.lua_type(state, table_index) != lua_type_table)
            {
                return false;
            }
            float r = out_value.r;
            float g = out_value.g;
            float b = out_value.b;
            float a = out_value.a;
            bool ok = false;
            ok = read_table_number_index(state, table_index, 1, r) || ok;
            ok = read_table_number_index(state, table_index, 2, g) || ok;
            ok = read_table_number_index(state, table_index, 3, b) || ok;
            ok = read_table_number_index(state, table_index, 4, a) || ok;
            if (ok)
            {
                out_value.r = std::clamp(r, 0.0f, 1.0f);
                out_value.g = std::clamp(g, 0.0f, 1.0f);
                out_value.b = std::clamp(b, 0.0f, 1.0f);
                out_value.a = std::clamp(a, 0.0f, 1.0f);
            }
            return ok;
        }

        void push_vector2(lua_state* state, const lua_drawing::vector2_value& value)
        {
            g_runtime.api.lua_createtable(state, 2, 0);
            g_runtime.api.lua_pushnumber(state, value.x);
            g_runtime.api.lua_rawseti(state, -2, 1);
            g_runtime.api.lua_pushnumber(state, value.y);
            g_runtime.api.lua_rawseti(state, -2, 2);
        }

        void push_color(lua_state* state, const lua_drawing::color_value& value)
        {
            g_runtime.api.lua_createtable(state, 4, 0);
            g_runtime.api.lua_pushnumber(state, value.r);
            g_runtime.api.lua_rawseti(state, -2, 1);
            g_runtime.api.lua_pushnumber(state, value.g);
            g_runtime.api.lua_rawseti(state, -2, 2);
            g_runtime.api.lua_pushnumber(state, value.b);
            g_runtime.api.lua_rawseti(state, -2, 3);
            g_runtime.api.lua_pushnumber(state, value.a);
            g_runtime.api.lua_rawseti(state, -2, 4);
        }

        lua_drawing_ref* get_drawing_ref(lua_state* state, int index)
        {
            if (g_runtime.api.lua_type(state, index) != lua_type_userdata)
            {
                return nullptr;
            }
            return reinterpret_cast<lua_drawing_ref*>(g_runtime.api.lua_touserdata(state, index));
        }

        std::shared_ptr<lua_drawing::drawing_object> get_drawing_object(lua_state* state, int index)
        {
            lua_drawing_ref* ref = get_drawing_ref(state, index);
            if (!ref)
            {
                return nullptr;
            }
            return lua_drawing::find_object(ref->object_id);
        }

        int lua_drawing_remove(lua_state* state)
        {
            lua_drawing_ref* ref = get_drawing_ref(state, 1);
            g_runtime.api.lua_pushboolean(state, ref && lua_drawing::remove_object(ref->object_id));
            return 1;
        }

        int lua_drawing_index(lua_state* state)
        {
            std::shared_ptr<lua_drawing::drawing_object> object_ptr = get_drawing_object(state, 1);
            if (!object_ptr)
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            const std::string key = stack_to_string(state, 2, std::string());
            if (key == "Remove")
            {
                push_c_function(state, lua_drawing_remove);
                return 1;
            }
            if (key == "Visible") { g_runtime.api.lua_pushboolean(state, object_ptr->visible); return 1; }
            if (key == "Position") { push_vector2(state, object_ptr->position); return 1; }
            if (key == "Size") { push_vector2(state, object_ptr->size); return 1; }
            if (key == "Color") { push_color(state, object_ptr->color); return 1; }
            if (key == "Filled") { g_runtime.api.lua_pushboolean(state, object_ptr->filled); return 1; }
            if (key == "Thickness") { g_runtime.api.lua_pushnumber(state, object_ptr->thickness); return 1; }
            if (key == "Text") { g_runtime.api.lua_pushlstring(state, object_ptr->text.c_str(), object_ptr->text.size()); return 1; }
            if (key == "From") { push_vector2(state, object_ptr->from); return 1; }
            if (key == "To") { push_vector2(state, object_ptr->to); return 1; }
            if (key == "Center") { g_runtime.api.lua_pushboolean(state, object_ptr->center); return 1; }
            if (key == "Transparency") { g_runtime.api.lua_pushnumber(state, object_ptr->transparency); return 1; }
            if (key == "ZIndex") { g_runtime.api.lua_pushinteger(state, object_ptr->z_index); return 1; }
            g_runtime.api.lua_pushnil(state);
            return 1;
        }

        int lua_drawing_newindex(lua_state* state)
        {
            std::shared_ptr<lua_drawing::drawing_object> object_ptr = get_drawing_object(state, 1);
            if (!object_ptr)
            {
                return 0;
            }
            const std::string key = stack_to_string(state, 2, std::string());
            if (key == "Visible") { object_ptr->visible = to_boolean(state, 3, object_ptr->visible); return 0; }
            if (key == "Position") { auto value = object_ptr->position; if (read_vector2(state, 3, value)) object_ptr->position = value; return 0; }
            if (key == "Size") { auto value = object_ptr->size; if (read_vector2(state, 3, value)) object_ptr->size = value; return 0; }
            if (key == "Color") { auto value = object_ptr->color; if (read_color(state, 3, value)) object_ptr->color = value; return 0; }
            if (key == "Filled") { object_ptr->filled = to_boolean(state, 3, object_ptr->filled); return 0; }
            if (key == "Thickness") { object_ptr->thickness = (std::max)(1.0f, to_number(state, 3, object_ptr->thickness)); return 0; }
            if (key == "Text") { object_ptr->text = stack_to_string(state, 3, object_ptr->text); return 0; }
            if (key == "From") { auto value = object_ptr->from; if (read_vector2(state, 3, value)) object_ptr->from = value; return 0; }
            if (key == "To") { auto value = object_ptr->to; if (read_vector2(state, 3, value)) object_ptr->to = value; return 0; }
            if (key == "Center") { object_ptr->center = to_boolean(state, 3, object_ptr->center); return 0; }
            if (key == "Transparency") { object_ptr->transparency = std::clamp(to_number(state, 3, object_ptr->transparency), 0.0f, 1.0f); return 0; }
            if (key == "ZIndex") { object_ptr->z_index = to_integer(state, 3, object_ptr->z_index); return 0; }
            return 0;
        }

        int lua_drawing_tostring(lua_state* state)
        {
            g_runtime.api.lua_pushlstring(state, "DrawingObject", std::strlen("DrawingObject"));
            return 1;
        }

        bool ensure_drawing_metatable(lua_state* state)
        {
            const int created = g_runtime.api.lua_l_newmetatable(state, drawing_metatable_name);
            if (created != 0)
            {
                push_c_function(state, lua_drawing_index);
                g_runtime.api.lua_setfield(state, -2, "__index");
                push_c_function(state, lua_drawing_newindex);
                g_runtime.api.lua_setfield(state, -2, "__newindex");
                push_c_function(state, lua_drawing_tostring);
                g_runtime.api.lua_setfield(state, -2, "__tostring");
            }
            return true;
        }

        bool push_drawing_userdata(lua_state* state, const std::shared_ptr<lua_drawing::drawing_object>& object_ptr)
        {
            if (!object_ptr)
            {
                g_runtime.api.lua_pushnil(state);
                return false;
            }
            void* memory = new_userdata(state, sizeof(lua_drawing_ref));
            if (!memory)
            {
                g_runtime.api.lua_pushnil(state);
                return false;
            }
            lua_drawing_ref* ref = new (memory) lua_drawing_ref();
            ref->object_id = object_ptr->id;
            if (!ensure_drawing_metatable(state))
            {
                g_runtime.api.lua_pushnil(state);
                return false;
            }
            g_runtime.api.lua_setmetatable(state, -2);
            return true;
        }

        int lua_drawing_new(lua_state* state)
        {
            const std::shared_ptr<lua_drawing::drawing_object> object_ptr = lua_drawing::create_object(stack_to_string(state, 1, std::string()));
            if (!object_ptr)
            {
                lua_console::push_warning("Drawing.new: not_supported");
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            push_drawing_userdata(state, object_ptr);
            return 1;
        }

        int lua_instance_new(lua_state* state)
        {
            const std::string class_name = stack_to_string(state, 1, "Instance");
            const std::shared_ptr<sandbox::instance> object_ptr = std::make_shared<sandbox::instance>(class_name, class_name);
            g_runtime.detached_instances.push_back(object_ptr);
            track_instance_recursive(object_ptr);
            if (g_runtime.api.lua_type(state, 2) == lua_type_userdata)
            {
                std::shared_ptr<sandbox::instance> parent = get_instance_shared(state, 2);
                if (parent)
                {
                    object_ptr->set_parent(parent);
                }
            }
            push_instance_userdata(state, object_ptr);
            return 1;
        }

        std::vector<std::string> read_string_array(lua_state* state, int table_index)
        {
            std::vector<std::string> values;
            if (g_runtime.api.lua_type(state, table_index) != lua_type_table)
            {
                return values;
            }
            const int absolute = absolute_index(state, table_index);
            const std::size_t count = g_runtime.api.lua_rawlen(state, absolute);
            values.reserve(count);
            for (std::size_t index = 1; index <= count; ++index)
            {
                g_runtime.api.lua_rawgeti(state, absolute, static_cast<lua_integer>(index));
                values.push_back(stack_to_string(state, -1, std::string()));
                pop_values(state, 1);
            }
            return values;
        }

        std::vector<bool> read_bool_array(lua_state* state, int table_index)
        {
            std::vector<bool> values;
            if (g_runtime.api.lua_type(state, table_index) != lua_type_table)
            {
                return values;
            }
            const int absolute = absolute_index(state, table_index);
            const std::size_t count = g_runtime.api.lua_rawlen(state, absolute);
            values.resize(count, false);
            for (std::size_t index = 1; index <= count; ++index)
            {
                g_runtime.api.lua_rawgeti(state, absolute, static_cast<lua_integer>(index));
                values[index - 1] = g_runtime.api.lua_toboolean(state, -1) != 0;
                pop_values(state, 1);
            }
            return values;
        }

        int lua_ui_create_tab(lua_state* state)
        {
            const int tab_id = lua_ui_bridge::create_tab(stack_to_string(state, 1, "lua_tab"));
            g_runtime.api.lua_pushinteger(state, tab_id);
            return 1;
        }

        bool resolve_tab_id(lua_state* state, int index, int& out_tab_id)
        {
            if (g_runtime.api.lua_type(state, index) == lua_type_number)
            {
                out_tab_id = to_integer(state, index, 0);
                return out_tab_id > 0;
            }
            if (g_runtime.api.lua_type(state, index) == lua_type_string)
            {
                const std::string name = stack_to_string(state, index, std::string());
                if (name.empty())
                {
                    return false;
                }
                if (!lua_ui_bridge::find_tab_id_by_name(name, out_tab_id))
                {
                    out_tab_id = lua_ui_bridge::create_tab(name);
                }
                return out_tab_id > 0;
            }
            return false;
        }

        int lua_ui_remove_tab(lua_state* state)
        {
            int tab_id = 0;
            g_runtime.api.lua_pushboolean(state, resolve_tab_id(state, 1, tab_id) && lua_ui_bridge::remove_tab(tab_id));
            return 1;
        }

        int lua_ui_set_tab_name(lua_state* state)
        {
            int tab_id = 0;
            g_runtime.api.lua_pushboolean(state, resolve_tab_id(state, 1, tab_id) && lua_ui_bridge::set_tab_name(tab_id, stack_to_string(state, 2, std::string())));
            return 1;
        }

        int lua_ui_clear_tab(lua_state* state)
        {
            int tab_id = 0;
            if (!resolve_tab_id(state, 1, tab_id))
            {
                g_runtime.api.lua_pushboolean(state, 0);
                return 1;
            }
            lua_ui_bridge::clear_tab_widgets(tab_id);
            g_runtime.api.lua_pushboolean(state, 1);
            return 1;
        }

        template <typename add_fn>
        int bind_widget(lua_state* state, const char* fallback_key, add_fn&& add)
        {
            int tab_id = 0;
            if (!resolve_tab_id(state, 1, tab_id))
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            std::string key = stack_to_string(state, 2, fallback_key);
            if (key.empty())
            {
                key = fallback_key;
            }
            const std::string label = stack_to_string(state, 3, key);
            if (!add(tab_id, key, label))
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            lua_ui_bridge::widget_value value{};
            if (!lua_ui_bridge::get_widget_value(tab_id, key, value))
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            if (value.type == lua_ui_bridge::value_type::boolean)
            {
                g_runtime.api.lua_pushboolean(state, value.bool_value);
            }
            else if (value.type == lua_ui_bridge::value_type::number)
            {
                g_runtime.api.lua_pushnumber(state, value.number_value);
            }
            else if (value.type == lua_ui_bridge::value_type::integer)
            {
                g_runtime.api.lua_pushinteger(state, value.int_value);
            }
            else if (value.type == lua_ui_bridge::value_type::string)
            {
                g_runtime.api.lua_pushlstring(state, value.string_value.c_str(), value.string_value.size());
            }
            else if (value.type == lua_ui_bridge::value_type::keybind)
            {
                g_runtime.api.lua_createtable(state, 0, 3);
                g_runtime.api.lua_pushinteger(state, value.keybind.key);
                g_runtime.api.lua_setfield(state, -2, "key");
                g_runtime.api.lua_pushinteger(state, value.keybind.mode);
                g_runtime.api.lua_setfield(state, -2, "mode");
                g_runtime.api.lua_pushboolean(state, value.keybind.enabled);
                g_runtime.api.lua_setfield(state, -2, "enabled");
            }
            else if (value.type == lua_ui_bridge::value_type::color)
            {
                g_runtime.api.lua_createtable(state, 4, 0);
                g_runtime.api.lua_pushnumber(state, value.color.r);
                g_runtime.api.lua_rawseti(state, -2, 1);
                g_runtime.api.lua_pushnumber(state, value.color.g);
                g_runtime.api.lua_rawseti(state, -2, 2);
                g_runtime.api.lua_pushnumber(state, value.color.b);
                g_runtime.api.lua_rawseti(state, -2, 3);
                g_runtime.api.lua_pushnumber(state, value.color.a);
                g_runtime.api.lua_rawseti(state, -2, 4);
            }
            else
            {
                g_runtime.api.lua_pushnil(state);
            }
            return 1;
        }

        int lua_ui_checkbox(lua_state* state) { return bind_widget(state, "checkbox", [&](int tab_id, const std::string& key, const std::string& label) { return lua_ui_bridge::add_checkbox_widget(tab_id, key, label, to_boolean(state, 4, false)); }); }
        int lua_ui_slider(lua_state* state) { return bind_widget(state, "slider", [&](int tab_id, const std::string& key, const std::string& label) { return lua_ui_bridge::add_slider_widget(tab_id, key, label, to_number(state, 4, 0.0f), to_number(state, 5, 100.0f), to_number(state, 6, 0.0f)); }); }
        int lua_ui_dropdown(lua_state* state) { return bind_widget(state, "dropdown", [&](int tab_id, const std::string& key, const std::string& label) { return lua_ui_bridge::add_dropdown_widget(tab_id, key, label, read_string_array(state, 4), to_integer(state, 5, 0)); }); }
        int lua_ui_multi_dropdown(lua_state* state) { return bind_widget(state, "multi_dropdown", [&](int tab_id, const std::string& key, const std::string& label) { return lua_ui_bridge::add_multi_dropdown_widget(tab_id, key, label, read_string_array(state, 4), read_bool_array(state, 5)); }); }
        int lua_ui_keybind(lua_state* state) { return bind_widget(state, "keybind", [&](int tab_id, const std::string& key, const std::string& label) { return lua_ui_bridge::add_keybind_widget(tab_id, key, label, to_integer(state, 4, 0), to_integer(state, 5, 0)); }); }
        int lua_ui_input_text(lua_state* state) { return bind_widget(state, "input_text", [&](int tab_id, const std::string& key, const std::string& label) { return lua_ui_bridge::add_input_text_widget(tab_id, key, label, stack_to_string(state, 4, std::string())); }); }

        int lua_ui_colorpicker(lua_state* state)
        {
            return bind_widget(state, "colorpicker", [&](int tab_id, const std::string& key, const std::string& label)
                {
                    lua_ui_bridge::color_value color{};
                    color.r = color.g = color.b = color.a = 1.0f;
                    if (g_runtime.api.lua_type(state, 4) == lua_type_table)
                    {
                        float v = color.r;
                        if (read_table_number_index(state, 4, 1, v)) color.r = std::clamp(v, 0.0f, 1.0f);
                        v = color.g; if (read_table_number_index(state, 4, 2, v)) color.g = std::clamp(v, 0.0f, 1.0f);
                        v = color.b; if (read_table_number_index(state, 4, 3, v)) color.b = std::clamp(v, 0.0f, 1.0f);
                        v = color.a; if (read_table_number_index(state, 4, 4, v)) color.a = std::clamp(v, 0.0f, 1.0f);
                    }
                    return lua_ui_bridge::add_colorpicker_widget(tab_id, key, label, color);
                });
        }

        int lua_ui_button(lua_state* state)
        {
            int tab_id = 0;
            if (!resolve_tab_id(state, 1, tab_id))
            {
                g_runtime.api.lua_pushboolean(state, 0);
                return 1;
            }
            std::string key = stack_to_string(state, 2, "button");
            if (key.empty())
            {
                key = "button";
            }
            const std::string label = stack_to_string(state, 3, key);
            int callback_ref = -1;
            if (g_runtime.api.lua_type(state, 4) == lua_type_function)
            {
                g_runtime.api.lua_pushvalue(state, 4);
                callback_ref = g_runtime.api.lua_l_ref(state, lua_registry_index);
            }
            g_runtime.api.lua_pushboolean(state, lua_ui_bridge::add_button_widget(tab_id, key, label, callback_ref));
            return 1;
        }

        int lua_ui_get(lua_state* state)
        {
            int tab_id = 0;
            if (!resolve_tab_id(state, 1, tab_id))
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            lua_ui_bridge::widget_value value{};
            if (!lua_ui_bridge::get_widget_value(tab_id, stack_to_string(state, 2, std::string()), value))
            {
                g_runtime.api.lua_pushnil(state);
                return 1;
            }
            if (value.type == lua_ui_bridge::value_type::boolean) g_runtime.api.lua_pushboolean(state, value.bool_value);
            else if (value.type == lua_ui_bridge::value_type::number) g_runtime.api.lua_pushnumber(state, value.number_value);
            else if (value.type == lua_ui_bridge::value_type::integer) g_runtime.api.lua_pushinteger(state, value.int_value);
            else if (value.type == lua_ui_bridge::value_type::string) g_runtime.api.lua_pushlstring(state, value.string_value.c_str(), value.string_value.size());
            else g_runtime.api.lua_pushnil(state);
            return 1;
        }

        int lua_ui_set(lua_state* state)
        {
            int tab_id = 0;
            if (!resolve_tab_id(state, 1, tab_id))
            {
                g_runtime.api.lua_pushboolean(state, 0);
                return 1;
            }
            lua_ui_bridge::widget_value value{};
            const std::string key = stack_to_string(state, 2, std::string());
            const int type = g_runtime.api.lua_type(state, 3);
            if (type == lua_type_boolean)
            {
                value.type = lua_ui_bridge::value_type::boolean;
                value.bool_value = g_runtime.api.lua_toboolean(state, 3) != 0;
            }
            else if (type == lua_type_number)
            {
                value.type = lua_ui_bridge::value_type::number;
                value.number_value = to_number(state, 3, 0.0f);
            }
            else if (type == lua_type_string)
            {
                value.type = lua_ui_bridge::value_type::string;
                value.string_value = stack_to_string(state, 3, std::string());
            }
            else
            {
                value.type = lua_ui_bridge::value_type::none;
            }
            g_runtime.api.lua_pushboolean(state, !key.empty() && value.type != lua_ui_bridge::value_type::none && lua_ui_bridge::set_widget_value(tab_id, key, value));
            return 1;
        }

        int lua_task_delay(lua_state* state)
        {
            if (g_runtime.api.lua_type(state, 2) != lua_type_function)
            {
                g_runtime.api.lua_pushboolean(state, 0);
                return 1;
            }
            const float seconds = (std::max)(0.0f, to_number(state, 1, 0.0f));
            g_runtime.api.lua_pushvalue(state, 2);
            g_runtime.scheduled_callbacks.push_back({ g_runtime.current_time_seconds + seconds, g_runtime.api.lua_l_ref(state, lua_registry_index) });
            g_runtime.api.lua_pushboolean(state, 1);
            return 1;
        }

        int lua_task_wait(lua_state* state)
        {
            const float seconds = (std::max)(0.0f, to_number(state, 1, 0.0f));
            if (seconds > 0.0f)
            {
                std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(seconds)));
            }
            g_runtime.api.lua_pushnumber(state, seconds);
            return 1;
        }

        int lua_input_mouse_position(lua_state* state)
        {
            const ImVec2 pos = ImGui::GetIO().MousePos;
            g_runtime.api.lua_createtable(state, 2, 0);
            g_runtime.api.lua_pushnumber(state, pos.x);
            g_runtime.api.lua_rawseti(state, -2, 1);
            g_runtime.api.lua_pushnumber(state, pos.y);
            g_runtime.api.lua_rawseti(state, -2, 2);
            return 1;
        }

        int lua_input_is_key_down(lua_state* state)
        {
            const int key = std::clamp(to_integer(state, 1, 0), 0, 0xFF);
            g_runtime.api.lua_pushboolean(state, (GetAsyncKeyState(key) & 0x8000) != 0);
            return 1;
        }

        int lua_vector3_new(lua_state* state)
        {
            g_runtime.api.lua_createtable(state, 0, 3);
            g_runtime.api.lua_pushnumber(state, to_number(state, 1, 0.0f));
            g_runtime.api.lua_setfield(state, -2, "x");
            g_runtime.api.lua_pushnumber(state, to_number(state, 2, 0.0f));
            g_runtime.api.lua_setfield(state, -2, "y");
            g_runtime.api.lua_pushnumber(state, to_number(state, 3, 0.0f));
            g_runtime.api.lua_setfield(state, -2, "z");
            return 1;
        }

        int lua_color3_new(lua_state* state)
        {
            g_runtime.api.lua_createtable(state, 0, 3);
            g_runtime.api.lua_pushnumber(state, to_number(state, 1, 0.0f));
            g_runtime.api.lua_setfield(state, -2, "r");
            g_runtime.api.lua_pushnumber(state, to_number(state, 2, 0.0f));
            g_runtime.api.lua_setfield(state, -2, "g");
            g_runtime.api.lua_pushnumber(state, to_number(state, 3, 0.0f));
            g_runtime.api.lua_setfield(state, -2, "b");
            return 1;
        }

        int lua_udim_new(lua_state* state)
        {
            g_runtime.api.lua_createtable(state, 0, 2);
            g_runtime.api.lua_pushnumber(state, to_number(state, 1, 0.0f));
            g_runtime.api.lua_setfield(state, -2, "scale");
            g_runtime.api.lua_pushnumber(state, to_number(state, 2, 0.0f));
            g_runtime.api.lua_setfield(state, -2, "offset");
            return 1;
        }

        int lua_udim2_new(lua_state* state)
        {
            g_runtime.api.lua_createtable(state, 0, 4);
            g_runtime.api.lua_pushnumber(state, to_number(state, 1, 0.0f));
            g_runtime.api.lua_setfield(state, -2, "x_scale");
            g_runtime.api.lua_pushnumber(state, to_number(state, 2, 0.0f));
            g_runtime.api.lua_setfield(state, -2, "x_offset");
            g_runtime.api.lua_pushnumber(state, to_number(state, 3, 0.0f));
            g_runtime.api.lua_setfield(state, -2, "y_scale");
            g_runtime.api.lua_pushnumber(state, to_number(state, 4, 0.0f));
            g_runtime.api.lua_setfield(state, -2, "y_offset");
            return 1;
        }

        void set_table_function(lua_state* state, const char* name, lua_c_function function)
        {
            push_c_function(state, function);
            g_runtime.api.lua_setfield(state, -2, name);
        }

        void register_all_tables(lua_state* state)
        {
            g_runtime.api.lua_createtable(state, 0, 1);
            set_table_function(state, "new", lua_instance_new);
            g_runtime.api.lua_setglobal(state, "Instance");

            g_runtime.api.lua_createtable(state, 0, 1);
            set_table_function(state, "new", lua_drawing_new);
            g_runtime.api.lua_setglobal(state, "Drawing");

            g_runtime.api.lua_createtable(state, 0, 16);
            set_table_function(state, "create_tab", lua_ui_create_tab);
            set_table_function(state, "remove_tab", lua_ui_remove_tab);
            set_table_function(state, "set_tab_name", lua_ui_set_tab_name);
            set_table_function(state, "clear_tab", lua_ui_clear_tab);
            set_table_function(state, "checkbox", lua_ui_checkbox);
            set_table_function(state, "slider", lua_ui_slider);
            set_table_function(state, "dropdown", lua_ui_dropdown);
            set_table_function(state, "multi_dropdown", lua_ui_multi_dropdown);
            set_table_function(state, "keybind", lua_ui_keybind);
            set_table_function(state, "colorpicker", lua_ui_colorpicker);
            set_table_function(state, "button", lua_ui_button);
            set_table_function(state, "input_text", lua_ui_input_text);
            set_table_function(state, "get", lua_ui_get);
            set_table_function(state, "set", lua_ui_set);
            g_runtime.api.lua_setglobal(state, "ui");

            g_runtime.api.lua_getglobal(state, "ui");
            g_runtime.api.lua_setglobal(state, "vanille_ui");

            g_runtime.api.lua_createtable(state, 0, 2);
            set_table_function(state, "delay", lua_task_delay);
            set_table_function(state, "wait", lua_task_wait);
            g_runtime.api.lua_setglobal(state, "task");

            g_runtime.api.lua_createtable(state, 0, 2);
            set_table_function(state, "mouse_position", lua_input_mouse_position);
            set_table_function(state, "is_key_down", lua_input_is_key_down);
            g_runtime.api.lua_setglobal(state, "input");

            g_runtime.api.lua_createtable(state, 0, 1);
            set_table_function(state, "new", lua_vector3_new);
            g_runtime.api.lua_setglobal(state, "Vector3");

            g_runtime.api.lua_createtable(state, 0, 1);
            set_table_function(state, "new", lua_color3_new);
            g_runtime.api.lua_setglobal(state, "Color3");

            g_runtime.api.lua_createtable(state, 0, 1);
            set_table_function(state, "new", lua_udim_new);
            g_runtime.api.lua_setglobal(state, "UDim");

            g_runtime.api.lua_createtable(state, 0, 1);
            set_table_function(state, "new", lua_udim2_new);
            g_runtime.api.lua_setglobal(state, "UDim2");
        }

        void register_globals(lua_state* state)
        {
            register_global_function(state, "print", lua_print);
            register_global_function(state, "warn", lua_warn);
            register_global_function(state, "loadstring", lua_loadstring);
            register_global_function(state, "load", lua_load);
            register_global_function(state, "getgenv", lua_getgenv);
        }

        bool invoke_callback(int callback_ref, const std::string& context)
        {
            if (!g_runtime.state || callback_ref < 0)
            {
                return false;
            }
            lua_state* state = g_runtime.state;
            const int top_before = g_runtime.api.lua_gettop(state);
            push_c_function(state, traceback_handler);
            const int error_function = g_runtime.api.lua_gettop(state);
            g_runtime.api.lua_rawgeti(state, lua_registry_index, callback_ref);
            if (g_runtime.api.lua_type(state, -1) != lua_type_function)
            {
                g_runtime.api.lua_settop(state, top_before);
                return false;
            }
            const int status = g_runtime.api.lua_pcallk(state, 0, 0, error_function, 0, nullptr);
            if (status != 0)
            {
                lua_console::push_error(context + ": " + stack_to_string(state, -1, "lua_callback_error"));
                g_runtime.api.lua_settop(state, top_before);
                return false;
            }
            g_runtime.api.lua_settop(state, top_before);
            return true;
        }

        bool invoke_callback_with_instance(int callback_ref, const std::shared_ptr<sandbox::instance>& argument, const std::string& context)
        {
            if (!g_runtime.state || callback_ref < 0)
            {
                return false;
            }
            lua_state* state = g_runtime.state;
            const int top_before = g_runtime.api.lua_gettop(state);
            push_c_function(state, traceback_handler);
            const int error_function = g_runtime.api.lua_gettop(state);
            g_runtime.api.lua_rawgeti(state, lua_registry_index, callback_ref);
            if (g_runtime.api.lua_type(state, -1) != lua_type_function)
            {
                g_runtime.api.lua_settop(state, top_before);
                return false;
            }
            push_instance_userdata(state, argument);
            const int status = g_runtime.api.lua_pcallk(state, 1, 0, error_function, 0, nullptr);
            if (status != 0)
            {
                lua_console::push_error(context + ": " + stack_to_string(state, -1, "lua_callback_error"));
                g_runtime.api.lua_settop(state, top_before);
                return false;
            }
            g_runtime.api.lua_settop(state, top_before);
            return true;
        }

        void process_scheduled_callbacks_locked()
        {
            std::vector<scheduled_callback> queued;
            queued.swap(g_runtime.scheduled_callbacks);
            for (const scheduled_callback& item : queued)
            {
                if (item.due_time_seconds <= g_runtime.current_time_seconds)
                {
                    invoke_callback(item.callback_ref, "task_delay_callback");
                    release_lua_ref(item.callback_ref);
                }
                else
                {
                    g_runtime.scheduled_callbacks.push_back(item);
                }
            }
        }

        void process_pending_instance_callbacks_locked()
        {
            if (g_runtime.pending_instance_callbacks.empty())
            {
                return;
            }
            std::vector<pending_instance_callback> pending;
            pending.swap(g_runtime.pending_instance_callbacks);
            for (const pending_instance_callback& callback : pending)
            {
                invoke_callback_with_instance(callback.callback_ref, callback.argument, callback.context);
            }
        }

        void process_ui_callbacks_locked()
        {
            release_lua_refs(lua_ui_bridge::take_released_callback_refs());
            const std::vector<int> callbacks = lua_ui_bridge::take_pending_button_callbacks();
            for (int callback_ref : callbacks)
            {
                invoke_callback(callback_ref, "lua_ui_button_callback");
            }
        }

        void ensure_example_script_files()
        {
            script_storage::initialize();
            auto ensure_script = [](const char* script_name, const char* script_source)
            {
                std::string existing;
                if (!script_storage::load_script(script_name, existing) || existing.empty())
                {
                    script_storage::save_script(script_name, script_source, nullptr);
                }
            };

            const char* example_script = R"lua(local tab_id = ui.create_tab("lua tab example")
ui.clear_tab(tab_id)
ui.checkbox(tab_id, "enabled", "Enabled", true)
ui.colorpicker(tab_id, "accent", "Accent", { 0.3, 0.8, 1.0, 1.0 })
ui.checkbox(tab_id, "use_key", "Use Key", false)
ui.keybind(tab_id, "key", "Key", 0x46, 1)
ui.slider(tab_id, "amount", "Amount", 0, 100, 32)
ui.dropdown(tab_id, "mode", "Mode", { "A", "B", "C" }, 0)
)lua";

            const char* ui_example_script = R"lua(local tab_id = ui.create_tab("ui example")
ui.clear_tab(tab_id)

ui.checkbox(tab_id, "enabled", "Enabled", true)
ui.colorpicker(tab_id, "accent", "Accent", { 0.15, 0.7, 1.0, 1.0 })

ui.checkbox(tab_id, "show_boxes", "Show Boxes", false)
ui.colorpicker(tab_id, "outline", "Outline", { 1.0, 1.0, 1.0, 1.0 })

ui.checkbox(tab_id, "hold_to_aim", "Hold To Aim", true)
ui.keybind(tab_id, "aim_key", "Aim Key", 0x02, 0)

ui.checkbox(tab_id, "toggle_menu", "Toggle Menu", true)
ui.keybind(tab_id, "menu_key", "Menu Key", 0x2D, 1)

ui.dropdown(tab_id, "mode", "Mode", { "Closest", "Fov", "Distance" }, 0)
ui.dropdown(tab_id, "hitpart", "Hitpart", { "Head", "Torso", "HumanoidRootPart" }, 0)

ui.slider(tab_id, "fov", "Fov", 25, 500, 120)
ui.slider(tab_id, "smoothness", "Smoothness", 1, 100, 20)
)lua";

            const char* lua_example_script = R"lua(local function print_instances(label, instances)
    print(label, #instances)
    for index, instance in ipairs(instances) do
        print(index, instance.name, instance.class_name)
    end
end

local workspace = game.Workspace
if workspace then
    print("workspace", workspace.name, workspace.class_name)
    print_instances("workspace instances", workspace:get_children())
end

local players = game.Players
if players then
    print("players", players.name, players.class_name)
    print_instances("game.Players", players:get_players())
end
)lua";

            ensure_script("example_lua_tab", example_script);
            ensure_script("ui_example", ui_example_script);
            ensure_script("lua_example", lua_example_script);
        }

        ImU32 color_to_u32(const ImVec4& color, float alpha = 1.0f)
        {
            ImVec4 tinted = color;
            tinted.w *= alpha;
            return ImGui::ColorConvertFloat4ToU32(tinted);
        }

        void initialize_script_text_editor()
        {
            if (g_script_text_editor_initialized)
            {
                return;
            }
            TextEditor::LanguageDefinition language_definition = TextEditor::LanguageDefinition::Lua();
            language_definition.mIdentifiers.clear();
            language_definition.mPreprocIdentifiers.clear();
            g_script_text_editor.SetLanguageDefinition(language_definition);
            g_script_text_editor.SetTabSize(4);
            g_script_text_editor.SetShowWhitespaces(false);

            TextEditor::Palette palette = TextEditor::GetDarkPalette();
            palette[(int)TextEditor::PaletteIndex::Background] = color_to_u32(c_colors::surface_inset);
            palette[(int)TextEditor::PaletteIndex::Default] = color_to_u32(c_colors::white, 0.92f);
            palette[(int)TextEditor::PaletteIndex::LineNumber] = color_to_u32(c_colors::text_muted, 0.45f);
            palette[(int)TextEditor::PaletteIndex::Selection] = color_to_u32(c_colors::top_accent_color, 0.28f);
            palette[(int)TextEditor::PaletteIndex::Cursor] = color_to_u32(c_colors::top_accent_color);
            palette[(int)TextEditor::PaletteIndex::CurrentLineFill] = color_to_u32(c_colors::accent_soft);
            palette[(int)TextEditor::PaletteIndex::CurrentLineFillInactive] = color_to_u32(c_colors::accent_soft, 0.5f);
            palette[(int)TextEditor::PaletteIndex::CurrentLineEdge] = color_to_u32(c_colors::accent_border);
            palette[(int)TextEditor::PaletteIndex::Comment] = color_to_u32(c_colors::text_muted, 0.72f);
            palette[(int)TextEditor::PaletteIndex::MultiLineComment] = color_to_u32(c_colors::text_muted, 0.72f);
            palette[(int)TextEditor::PaletteIndex::Keyword] = color_to_u32(c_colors::top_accent_color, 0.88f);
            palette[(int)TextEditor::PaletteIndex::String] = color_to_u32(c_colors::accent_dim);
            palette[(int)TextEditor::PaletteIndex::Number] = color_to_u32(c_colors::white, 0.78f);
            g_script_text_editor.SetPalette(palette);

            g_script_text_editor.SetText(g_editor.script_source);
            g_script_text_editor_initialized = true;
        }

        void set_script_name_buffer(const std::string& script_name)
        {
            copy_text_to_buffer(g_editor.script_name, script_name);
        }

        std::string get_script_name_from_buffer()
        {
            return script_storage::sanitize_script_name(g_editor.script_name);
        }

        bool load_script_into_editor(const std::string& script_name)
        {
            std::string script;
            if (!script_storage::load_script(script_name, script))
            {
                lua_console::push_warning("open_file_failed: " + script_name);
                return false;
            }
            g_editor.script_source = script;
            g_editor.saved_script_source = script;
            initialize_script_text_editor();
            g_script_text_editor.SetText(script);
            g_editor.selected_script_name = script_name;
            set_script_name_buffer(script_name);
            return true;
        }

        void show_console_on_execute_result(bool success)
        {
            features->show_console_window = true;
            if (!success)
            {
                g_editor.focus_console_next_frame = true;
            }
        }

        void register_visible_aux_window_hittest()
        {
            HWND overlay_hwnd = vanille::overlay::g_overlay_window;
            if (!overlay_hwnd || ImGui::IsWindowCollapsed())
            {
                return;
            }

            const ImVec2 window_pos = ImGui::GetWindowPos();
            const ImVec2 window_size = ImGui::GetWindowSize();
            register_aux_window_hittest_rect(window_pos, window_size, overlay_hwnd);
        }

        void sync_script_source_from_text_editor()
        {
            if (g_script_text_editor_initialized)
            {
                g_editor.script_source = g_script_text_editor.GetText();
            }
        }

        void mark_editor_source_saved()
        {
            sync_script_source_from_text_editor();
            g_editor.saved_script_source = g_editor.script_source;
        }

        bool editor_source_is_dirty()
        {
            sync_script_source_from_text_editor();
            return g_editor.script_source != g_editor.saved_script_source;
        }

        void render_scripts_panel(const std::vector<script_storage::script_entry>& scripts)
        {
            c_widgets::section_label("Scripts");
            const float row_width = ImGui::GetContentRegionAvail().x;
            const float button_spacing = ImGui::GetStyle().ItemSpacing.x;
            const float script_button_width = (row_width - button_spacing) * 0.5f;
            if (c_widgets::button("New Script", ImVec2(script_button_width, 0.0f)))
            {
                const std::string script_name = script_storage::make_unique_script_name("script");
                const std::string starter_source = "-- " + script_name + ".lua\n";
                if (script_storage::save_script(script_name, starter_source, nullptr))
                {
                    load_script_into_editor(script_name);
                    lua_console::push_info("created_script: " + script_name);
                }
                else
                {
                    lua_console::push_warning("create_script_failed: " + script_name);
                }
            }
            ImGui::SameLine();
            if (c_widgets::button("Delete", ImVec2(script_button_width, 0.0f)))
            {
                const std::string target = get_script_name_from_buffer();
                if (!target.empty() && script_storage::delete_script(target))
                {
                    lua_console::push_info("deleted_script: " + target);
                    g_editor.selected_script_name.clear();
                    g_editor.selected_script_index = -1;
                    g_editor.script_source.clear();
                    g_editor.saved_script_source.clear();
                    g_script_text_editor.SetText(std::string());
                    set_script_name_buffer("script");
                }
                else
                {
                    lua_console::push_warning("delete_script_failed: " + target);
                }
            }
            if (c_widgets::button("Rename", ImVec2(script_button_width, 0.0f)))
            {
                const std::string source_name = g_editor.selected_script_name.empty()
                    ? get_script_name_from_buffer()
                    : g_editor.selected_script_name;
                const std::string target_name = get_script_name_from_buffer();
                std::string renamed_name;
                if (!source_name.empty() && !target_name.empty()
                    && script_storage::rename_script(source_name, target_name, &renamed_name))
                {
                    g_editor.selected_script_name = renamed_name;
                    set_script_name_buffer(renamed_name);
                    lua_console::push_info("renamed_script: " + source_name + " -> " + renamed_name);
                }
                else
                {
                    lua_console::push_warning("rename_script_failed: " + source_name);
                }
            }
            ImGui::SameLine();
            if (c_widgets::button("Duplicate", ImVec2(script_button_width, 0.0f)))
            {
                const std::string source_name = g_editor.selected_script_name.empty()
                    ? get_script_name_from_buffer()
                    : g_editor.selected_script_name;
                std::string duplicate_name;
                const std::string requested_name = script_storage::make_unique_script_name(source_name + "_copy");
                if (!source_name.empty()
                    && script_storage::duplicate_script(source_name, requested_name, &duplicate_name))
                {
                    load_script_into_editor(duplicate_name);
                    lua_console::push_info("duplicated_script: " + duplicate_name);
                }
                else
                {
                    lua_console::push_warning("duplicate_script_failed: " + source_name);
                }
            }

            if (c_widgets::begin_padded_child("##lua_script_list", 0, ImGuiWindowFlags_NoScrollbar, ImVec2(-1.0f, -1.0f), true, true, false, true, false, false, false, true))
            {
                const ImGuiStyle& style = ImGui::GetStyle();
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                const ImVec4 base_text = style.Colors[ImGuiCol_Text];
                const float row_height = ImGui::GetTextLineHeight() + style.FramePadding.y * 2.0f;
                for (int index = 0; index < static_cast<int>(scripts.size()); ++index)
                {
                    const auto& script = scripts[index];
                    const bool selected = g_editor.selected_script_index == index;
                    ImGui::PushID(index);
                    const float row_width = (std::max)(ImGui::GetContentRegionAvail().x, 1.0f);
                    if (ImGui::InvisibleButton("##lua_script_entry", ImVec2(row_width, row_height)))
                    {
                        g_editor.selected_script_index = index;
                        load_script_into_editor(script.name);
                    }
                    const bool hovered = ImGui::IsItemHovered();
                    ImRect item_bounds(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                    const ImVec2 text_size = ImGui::CalcTextSize(script.name.c_str());
                    const ImVec2 text_position(
                        item_bounds.Min.x + style.FramePadding.x,
                        item_bounds.Min.y + (item_bounds.GetHeight() - text_size.y) * 0.5f);
                    ImVec4 text_color = c_colors::scale_color(base_text, hovered ? 1.08f : 0.78f);
                    if (selected)
                    {
                        text_color = c_colors::top_accent_color;
                        if (hovered)
                        {
                            text_color = c_colors::scale_color(text_color, 1.1f);
                        }
                    }
                    draw_list->AddText(text_position, ImGui::GetColorU32(text_color), script.name.c_str());
                    ImGui::PopID();
                }
            }
            c_widgets::end_padded_child();
        }

        void render_editor_content()
        {
            script_storage::initialize();
            script_storage::refresh();
            const auto& scripts = script_storage::get_scripts();

            if (!g_editor.selected_script_name.empty())
            {
                g_editor.selected_script_index = -1;
                for (int index = 0; index < static_cast<int>(scripts.size()); ++index)
                {
                    if (scripts[index].name == g_editor.selected_script_name)
                    {
                        g_editor.selected_script_index = index;
                        break;
                    }
                }
            }
            else
            {
                g_editor.selected_script_index = -1;
            }

            if (!g_editor.initialized)
            {
                g_editor.initialized = true;
                if (!scripts.empty())
                {
                    g_editor.selected_script_index = 0;
                    load_script_into_editor(scripts[0].name);
                }
                else
                {
                    set_script_name_buffer("script");
                }
            }

            initialize_script_text_editor();

            if (!is_ready())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                    g_editor.vm_init_error.empty() ? "Lua runtime is not ready." : g_editor.vm_init_error.c_str());
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
            }

            if (ImGui::BeginTable("##lua_editor_layout", 2))
            {
                ImGui::TableSetupColumn("editor", ImGuiTableColumnFlags_WidthStretch, 0.75f);
                ImGui::TableSetupColumn("scripts", ImGuiTableColumnFlags_WidthStretch, 0.25f);

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Script");
                if (editor_source_is_dirty())
                {
                    ImGui::SameLine();
                    ImGui::TextColored(c_colors::top_accent_color, "*");
                }
                ImGui::SameLine();
                c_widgets::input_text("##lua_script_name", g_editor.script_name, sizeof(g_editor.script_name));
                const float row_width = ImGui::GetContentRegionAvail().x;
                const float button_spacing = ImGui::GetStyle().ItemSpacing.x;
                const float button_width = (row_width - button_spacing * 3.0f) * 0.25f;

                if (c_widgets::button_primary("Execute", ImVec2(button_width, 0.0f)))
                {
                    sync_script_source_from_text_editor();
                    const std::string chunk_name = get_script_name_from_buffer().empty() ? "lua_editor" : get_script_name_from_buffer();
                    const bool success = execute_string(g_editor.script_source, chunk_name);
                    show_console_on_execute_result(success);
                }
                ImGui::SameLine();
                if (c_widgets::button("Clear", ImVec2(button_width, 0.0f)))
                {
                    g_editor.script_source.clear();
                    g_script_text_editor.SetText(std::string());
                }
                ImGui::SameLine();
                if (c_widgets::button("Open File", ImVec2(button_width, 0.0f)))
                {
                    std::string target = get_script_name_from_buffer();
                    if (!target.empty())
                    {
                        load_script_into_editor(target);
                    }
                    else
                    {
                        lua_console::push_warning("open_file_failed: empty script name");
                    }
                }
                ImGui::SameLine();
                if (c_widgets::button("Save File", ImVec2(button_width, 0.0f)))
                {
                    sync_script_source_from_text_editor();
                    std::string saved_name;
                    const std::string target = get_script_name_from_buffer().empty() ? "script" : get_script_name_from_buffer();
                    if (script_storage::save_script(target, g_editor.script_source, &saved_name))
                    {
                        g_editor.selected_script_name = saved_name;
                        set_script_name_buffer(saved_name);
                        mark_editor_source_saved();
                        script_storage::refresh();
                        lua_console::push_info("saved_script: " + saved_name);
                    }
                    else
                    {
                        lua_console::push_warning("save_script_failed: " + target);
                    }
                }

                g_script_text_editor.Render("##lua_script_editor", ImVec2(-1.0f, -1.0f), false);
                sync_script_source_from_text_editor();

                ImGui::TableNextColumn();
                render_scripts_panel(scripts);

                ImGui::EndTable();
            }
        }

        void render_console_panel()
        {
            const auto lines = lua_console::get_lines_snapshot();
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
            const float row_width = ImGui::GetContentRegionAvail().x;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float button_width = (row_width - spacing) * 0.5f;
            if (c_widgets::button("Clear Console", ImVec2(button_width, 0.0f)))
            {
                lua_console::clear();
            }
            ImGui::SameLine();
            if (c_widgets::button("Copy All", ImVec2(button_width, 0.0f)))
            {
                std::ostringstream output;
                for (const auto& line : lines)
                {
                    output << line.text << '\n';
                }
                const std::string text = output.str();
                ImGui::SetClipboardText(text.c_str());
            }
            if (c_widgets::begin_padded_child("##lua_console_lines", 0, ImGuiWindowFlags_NoScrollbar, ImVec2(-1.0f, -1.0f), true, true, false, true, false, false, false, true))
            {
                for (const auto& line : lines)
                {
                    ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                    if (line.level == lua_console::log_level::warning) color = ImVec4(1.0f, 0.76f, 0.23f, 1.0f);
                    else if (line.level == lua_console::log_level::error) color = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
                    ImGui::TextColored(color, "%s", line.text.c_str());
                }
            }
            c_widgets::end_padded_child();
        }

        void register_runtime_model(lua_state* state)
        {
            g_runtime.root_model = sandbox::create_default_data_model();
            track_instance_recursive(g_runtime.root_model);
            push_instance_userdata(state, g_runtime.root_model);
            g_runtime.api.lua_setglobal(state, "game");
            push_instance_userdata(state, g_runtime.root_model->get_service("Workspace"));
            g_runtime.api.lua_setglobal(state, "workspace");
        }
    }

    bool initialize()
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        if (g_runtime.ready)
        {
            g_editor.vm_init_error.clear();
            return true;
        }
        ensure_example_script_files();
        if (!load_library_and_api())
        {
            if (g_editor.vm_init_error.empty())
            {
                g_editor.vm_init_error = "Lua DLL not found. Place lua53-64.dll next to vanille.exe.";
            }
            return false;
        }
        g_runtime.state = g_runtime.api.lua_l_newstate();
        if (!g_runtime.state)
        {
            g_editor.vm_init_error = "Failed to create Lua state.";
            lua_console::push_error("lua_state_create_failed");
            unload_runtime();
            return false;
        }
        g_runtime.api.lua_l_openlibs(g_runtime.state);
        sanitize_globals(g_runtime.state);
        register_globals(g_runtime.state);
        register_all_tables(g_runtime.state);
        register_runtime_model(g_runtime.state);
        sync_workspace_service_from_globals_locked();
        sync_players_service_from_cache_locked();
        g_runtime.ready = true;
        g_editor.vm_init_error.clear();
        return true;
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        if (!g_runtime.module)
        {
            return;
        }
        std::vector<int> refs_to_release = lua_ui_bridge::take_all_callback_refs();
        for (const scheduled_callback& item : g_runtime.scheduled_callbacks)
        {
            refs_to_release.push_back(item.callback_ref);
        }
        for (int callback_ref : g_runtime.players_player_added_callbacks)
        {
            refs_to_release.push_back(callback_ref);
        }
        for (const auto& pair : g_runtime.player_character_added_callbacks)
        {
            for (int callback_ref : pair.second)
            {
                refs_to_release.push_back(callback_ref);
            }
        }
        release_lua_refs(refs_to_release);
        lua_ui_bridge::clear_all();
        lua_drawing::clear_all();
        unload_runtime();
        g_editor = editor_state{};
        if (g_script_text_editor_initialized)
        {
            g_script_text_editor.SetText(std::string());
        }
        g_script_text_editor_initialized = false;
    }

    bool is_ready()
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        return g_runtime.ready;
    }

    bool execute_string(const std::string& script, const std::string& chunk_name)
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        if (!g_runtime.ready || !g_runtime.state)
        {
            lua_console::push_error("lua_vm_not_ready");
            return false;
        }
        release_lua_refs(lua_ui_bridge::take_released_callback_refs());
        const int top_before = g_runtime.api.lua_gettop(g_runtime.state);
        push_c_function(g_runtime.state, traceback_handler);
        const int error_function = g_runtime.api.lua_gettop(g_runtime.state);
        const int load_status = g_runtime.api.lua_l_loadbufferx(g_runtime.state, script.c_str(), script.size(), chunk_name.c_str(), nullptr);
        if (load_status != 0)
        {
            lua_console::push_error(chunk_name + ": " + stack_to_string(g_runtime.state, -1, "lua_compile_error"));
            g_runtime.api.lua_settop(g_runtime.state, top_before);
            return false;
        }
        const int status = g_runtime.api.lua_pcallk(g_runtime.state, 0, lua_mult_ret, error_function, 0, nullptr);
        if (status != 0)
        {
            lua_console::push_error(chunk_name + ": " + stack_to_string(g_runtime.state, -1, "lua_runtime_execute_error"));
            g_runtime.api.lua_settop(g_runtime.state, top_before);
            return false;
        }
        g_runtime.api.lua_settop(g_runtime.state, top_before);
        release_lua_refs(lua_ui_bridge::take_released_callback_refs());
        return true;
    }

    bool execute_file(const std::filesystem::path& file_path)
    {
        std::ifstream input(file_path, std::ios::binary);
        if (!input.is_open())
        {
            lua_console::push_error("execute_file_open_failed: " + file_path.string());
            return false;
        }
        const std::string script((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return execute_string(script, file_path.filename().string());
    }

    void on_frame(float delta_time)
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        if (!g_runtime.ready || !g_runtime.state)
        {
            return;
        }
        sync_workspace_service_from_globals_locked();
        sync_players_service_from_cache_locked();
        process_pending_instance_callbacks_locked();
        g_runtime.current_time_seconds += static_cast<double>((std::max)(0.0f, delta_time));
        process_scheduled_callbacks_locked();
        process_ui_callbacks_locked();
        lua_drawing::render_all();
    }

    void begin_aux_window_hittest_frame()
    {
        g_aux_window_hittest_rects.clear();
    }

    void register_aux_window_hittest_rect(const ImVec2& screen_pos, const ImVec2& size, overlay_hwnd_t overlay_hwnd)
    {
        if (!overlay_hwnd || size.x <= 0.0f || size.y <= 0.0f)
        {
            return;
        }

        POINT top_left{ static_cast<LONG>(screen_pos.x), static_cast<LONG>(screen_pos.y) };
        POINT bottom_right{
            static_cast<LONG>(screen_pos.x + size.x),
            static_cast<LONG>(screen_pos.y + size.y),
        };
        ::ScreenToClient(reinterpret_cast<HWND>(overlay_hwnd), &top_left);
        ::ScreenToClient(reinterpret_cast<HWND>(overlay_hwnd), &bottom_right);

        RECT rect{
            top_left.x,
            top_left.y,
            bottom_right.x,
            bottom_right.y,
        };
        g_aux_window_hittest_rects.push_back(rect);
    }

    bool client_point_over_aux_windows(overlay_hwnd_t overlay_hwnd, int client_x, int client_y)
    {
        if (!overlay_hwnd || g_aux_window_hittest_rects.empty())
        {
            return false;
        }

        const POINT point{ client_x, client_y };
        for (const RECT& rect : g_aux_window_hittest_rects)
        {
            if (::PtInRect(&rect, point))
            {
                return true;
            }
        }
        return false;
    }

    bool cursor_over_aux_windows(overlay_hwnd_t overlay_hwnd)
    {
        if (!overlay_hwnd)
        {
            return false;
        }

        POINT cursor{};
        if (!::GetCursorPos(&cursor))
        {
            return false;
        }
        if (!::ScreenToClient(reinterpret_cast<HWND>(overlay_hwnd), &cursor))
        {
            return false;
        }
        return client_point_over_aux_windows(overlay_hwnd, cursor.x, cursor.y);
    }

    void render_editor_window(bool menu_has_frame, const ImVec2& menu_pos, const ImVec2& menu_size)
    {
        initialize();
        ImGui::SetNextWindowSize(ImVec2(580.0f, 420.0f), ImGuiCond_FirstUseEver);
        if (menu_has_frame && menu_size.x > 0.0f && menu_size.y > 0.0f)
        {
            ImGui::SetNextWindowPos(ImVec2(menu_pos.x, menu_pos.y + menu_size.y + 12.0f), ImGuiCond_FirstUseEver);
        }
        else
        {
            ImGui::SetNextWindowPos(ImVec2(60.0f, 620.0f), ImGuiCond_FirstUseEver);
        }
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        const bool opened = ImGui::Begin("Lua Editor##lua_editor_window", nullptr, ImGuiWindowFlags_NoTitleBar);
        ImGui::PopStyleColor();
        if (opened)
        {
            draw_window_background();
            draw_draggable_window_header("##lua_editor_drag", "Lua Editor");
            ImGui::Dummy(ImVec2(0.0f, 0.0f));

            ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
            if (c_widgets::begin_padded_child("##lua_editor_main_child", 0, 0, ImVec2(-1.0f, -1.0f), true, true, false, true, false))
            {
                render_editor_content();
            }
            c_widgets::end_padded_child();
            ImGui::PopStyleVar(2);
            register_visible_aux_window_hittest();
        }
        ImGui::End();
    }

    void render_console_window(bool menu_has_frame, const ImVec2& menu_pos, const ImVec2& menu_size)
    {
        initialize();
        if (g_editor.focus_console_next_frame)
        {
            ImGui::SetNextWindowFocus();
            g_editor.focus_console_next_frame = false;
        }
        ImGui::SetNextWindowSize(ImVec2(460.0f, 300.0f), ImGuiCond_FirstUseEver);
        if (menu_has_frame && menu_size.x > 0.0f && menu_size.y > 0.0f)
        {
            ImGui::SetNextWindowPos(ImVec2(menu_pos.x + 592.0f, menu_pos.y + menu_size.y + 12.0f), ImGuiCond_FirstUseEver);
        }
        else
        {
            ImGui::SetNextWindowPos(ImVec2(652.0f, 620.0f), ImGuiCond_FirstUseEver);
        }
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        const bool opened = ImGui::Begin("Lua Console##lua_console_window", nullptr, ImGuiWindowFlags_NoTitleBar);
        ImGui::PopStyleColor();
        if (opened)
        {
            draw_window_background();
            draw_draggable_window_header("##lua_console_drag", "Lua Console");

            ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
            if (c_widgets::begin_padded_child("##lua_console_main_child", 0, 0, ImVec2(-1.0f, -1.0f), true, true, false, true, false))
            {
                render_console_panel();
            }
            c_widgets::end_padded_child();
            ImGui::PopStyleVar(2);
            register_visible_aux_window_hittest();
        }
        ImGui::End();
    }
}
