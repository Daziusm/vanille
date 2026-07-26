
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "assistant.h"

#include "overlay.hpp"
#include "../globals/globals.h"
#include "configs/config_manager.h"
#include "colors/colors.h"
#include "gui/widgets/keybind/keybind.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <tinygltf/json.hpp>
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace vanille::assistant
{
    namespace
    {
        using ::features;

        struct ai_chat_response
        {
            std::string text;
            std::string error;
        };

        struct ai_url_parts
        {
            bool secure = false;
            INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
            std::wstring host;
            std::wstring path;
        };

        std::vector<chat_message> g_messages;
        std::future<ai_chat_response> g_future;
        bool g_pending = false;
        bool g_scroll_to_bottom = false;
        std::string g_error;

        enum class pending_appearance
        {
            none,
            shadow_size,
            shadow_offset
        };
        pending_appearance g_pending_appearance = pending_appearance::none;

        enum class ai_feature_target
        {
            none,
            vsync,
            fly,
            noclip,
            auto_shooter,
            esp,
            boxes,
            names,
            skeleton,
            distance,
            health,
            armor,
            body_status,
            highlight,
            aimbot,
            silent_aim
        };
        ai_feature_target g_last_feature_toggle = ai_feature_target::none;

        std::string to_lower_ascii(std::string text)
        {
            for (char& c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        bool contains_theme_keywords(const std::string& lowered)
        {
            return lowered.find("theme") != std::string::npos
                || lowered.find("preset") != std::string::npos
                || lowered.find("appearance") != std::string::npos
                || lowered.find("menu") != std::string::npos
                || lowered.find("ui") != std::string::npos
                || lowered.find("color") != std::string::npos
                || lowered.find("skin") != std::string::npos
                || lowered.find("style") != std::string::npos;
        }

        bool has_recent_theme_context()
        {
            if (g_messages.empty())
                return false;
            for (auto it = g_messages.rbegin(); it != g_messages.rend(); ++it)
            {
                if (it->is_user)
                    continue;
                const std::string lowered = to_lower_ascii(it->text);
                return contains_theme_keywords(lowered);
            }
            return false;
        }

        int pick_random_theme_index()
        {
            static std::mt19937 rng{ std::random_device{}() };
            std::uniform_int_distribution<int> dist(0, 7);
            return dist(rng);
        }

        int pick_random_theme_index_excluding(int current)
        {
            if (current < 0 || current > 7)
                return pick_random_theme_index();
            int next = pick_random_theme_index();
            if (next == current)
                next = (current + 1) % 8;
            return next;
        }

        std::string pick_theme_flavor_line()
        {
            static std::mt19937 rng{ std::random_device{}() };
            static const char* k_lines[] = {
                "It looks clean and balanced.",
                "Nice contrast and a smooth vibe.",
                "Great choice; it feels crisp.",
                "Looks sharp with solid contrast.",
                "Sleek and readable with good depth.",
                "That palette feels fresh and calm."
            };
            std::uniform_int_distribution<int> dist(0, static_cast<int>(IM_ARRAYSIZE(k_lines)) - 1);
            return k_lines[dist(rng)];
        }
        bool try_extract_number_after(const std::string& lowered, const char* keyword, float& out_value)
        {
            const auto pos = lowered.find(keyword);
            if (pos == std::string::npos)
                return false;
            const char* start = lowered.c_str() + pos + std::strlen(keyword);
            while (*start && !std::isdigit(static_cast<unsigned char>(*start)) && *start != '-' && *start != '.')
                ++start;
            if (!*start)
                return false;
            char* end = nullptr;
            const float value = std::strtof(start, &end);
            if (end == start)
                return false;
            out_value = value;
            return std::isfinite(out_value);
        }

        bool try_parse_max_keyword(const std::string& lowered, float max_value, float& out_value)
        {
            if (lowered.find("max") == std::string::npos)
                return false;
            out_value = max_value;
            return true;
        }

        bool try_extract_first_number(const std::string& lowered, float& out_value)
        {
            const char* start = lowered.c_str();
            while (*start && !std::isdigit(static_cast<unsigned char>(*start)) && *start != '-' && *start != '.')
                ++start;
            if (!*start)
                return false;
            char* end = nullptr;
            const float value = std::strtof(start, &end);
            if (end == start)
                return false;
            out_value = value;
            return std::isfinite(out_value);
        }

        bool try_match_existing_config_name(const std::string& lowered, std::string& out_name)
        {
            config_manager::refresh();
            const auto& configs = config_manager::get_configs();
            for (const auto& name : configs)
            {
                if (name.empty())
                    continue;
                const std::string lowered_name = to_lower_ascii(name);
                if (!lowered_name.empty() && lowered.find(lowered_name) != std::string::npos)
                {
                    out_name = name;
                    return true;
                }
            }
            return false;
        }

        std::string join_items(const std::vector<std::string>& items)
        {
            if (items.empty())
                return {};
            std::string result = items.front();
            for (size_t i = 1; i < items.size(); ++i)
            {
                result += ", ";
                result += items[i];
            }
            return result;
        }

        std::string format_number(float value)
        {
            std::ostringstream oss;
            oss.setf(std::ios::fixed);
            oss.precision(1);
            oss << value;
            std::string out = oss.str();
            if (out.size() >= 2 && out.substr(out.size() - 2) == ".0")
                out.resize(out.size() - 2);
            return out;
        }

        bool try_parse_enable_intent(const std::string& lowered, bool& out_enable)
        {
            if (lowered.find("disable") != std::string::npos
                || lowered.find("turn off") != std::string::npos
                || lowered.find("off") != std::string::npos
                || lowered.find("remove") != std::string::npos)
            {
                out_enable = false;
                return true;
            }
            if (lowered.find("enable") != std::string::npos
                || lowered.find("turn on") != std::string::npos
                || lowered.find("on") != std::string::npos
                || lowered.find("add") != std::string::npos)
            {
                out_enable = true;
                return true;
            }
            return false;
        }

        std::string generate_config_name()
        {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
            return "assistant_" + std::to_string(seconds);
        }

        enum class ai_config_preset
        {
            none,
            legit,
            semi_blatant,
            blatant
        };

        ai_config_preset detect_config_preset(const std::string& lowered)
        {
            const bool has_semi = lowered.find("semi") != std::string::npos;
            const bool has_blatant = lowered.find("blatant") != std::string::npos
                || lowered.find("rage") != std::string::npos;
            const bool has_legit = lowered.find("legit") != std::string::npos;

            if (has_semi && has_blatant)
                return ai_config_preset::semi_blatant;
            if (has_legit)
                return ai_config_preset::legit;
            if (has_blatant)
                return ai_config_preset::blatant;
            return ai_config_preset::none;
        }

        void set_keybind_value(c_keybind& bind, int key, c_keybind::keybind_mode mode)
        {
            bind.key = key;
            bind.type = mode;
            bind.awaiting_release = false;
            bind.was_down = false;
            bind.enabled = (mode == c_keybind::ALWAYS);
            bind.waiting_for_input = false;
            bind.menu_opened = false;
        }

        void apply_preset_config(ai_config_preset preset, std::vector<std::string>& applied)
        {
            if (preset == ai_config_preset::none)
                return;

            if (preset == ai_config_preset::legit)
            {
                features->enable_aimbot = true;
                features->enable_aimbot_smooth = true;
                features->aimbot_smooth_x = 12.0f;
                features->aimbot_smooth_y = 12.0f;
                features->enable_aimbot_prediction = true;
                features->aimbot_prediction_x = 3.0f;
                features->aimbot_prediction_y = 4.0f;
                features->aimbot_nearest_part = true;
                features->enable_aimbot_closest_point = true;

                features->enable_free_aim = false;
                features->free_aim_nearest_part = false;
                features->enable_free_aim_closest_point = false;

                set_keybind_value(features->aimbot_keybind, VK_RBUTTON, c_keybind::HOLD);

                applied = { "legit preset", "aimbot", "smooth", "prediction", "closest point", "silent aim off", "aimbot key RMB hold" };
                return;
            }

            if (preset == ai_config_preset::semi_blatant)
            {
                features->enable_aimbot = true;
                features->enable_aimbot_smooth = true;
                features->aimbot_smooth_x = 4.5f;
                features->aimbot_smooth_y = 4.5f;
                features->enable_aimbot_prediction = true;
                features->aimbot_prediction_x = 12.0f;
                features->aimbot_prediction_y = 12.0f;
                features->aimbot_nearest_part = false;
                features->enable_aimbot_closest_point = false;

                features->enable_free_aim = true;
                features->free_aim_mouse_spoof = true;

                features->enable_esp = true;
                features->enable_bounding_box = true;
                features->enable_name_esp = true;
                features->enable_distance = true;
                features->enable_healthbar = true;
                features->enable_body_status = true;
                features->show_status_movement = true;
                features->show_status_reload = true;
                features->show_status_grabbed = true;
                features->show_status_gun_firing = true;
                features->show_status_knocked = true;

                set_keybind_value(features->aimbot_keybind, 'E', c_keybind::TOGGLE);
                set_keybind_value(features->free_aim_keybind, 'X', c_keybind::TOGGLE);

                applied = { "semi-blatant preset", "esp", "names", "distance", "health", "body status", "aimbot", "smooth", "prediction", "nearest part", "silent aim on", "aimbot key E toggle", "silent aim key X toggle" };
                return;
            }

            features->enable_aimbot = true;
            features->enable_aimbot_smooth = false;
            features->enable_aimbot_prediction = false;
            features->aimbot_prediction_x = 12.0f;
            features->aimbot_prediction_y = 12.0f;
            features->aimbot_nearest_part = false;
            features->enable_aimbot_closest_point = false;
            features->aimbot_limit_fov = false;

            features->enable_free_aim = true;
            features->free_aim_nearest_part = false;
            features->enable_free_aim_closest_point = false;
            features->free_aim_enable_prediction = false;
            features->free_aim_prediction_x = 12.0f;
            features->free_aim_prediction_y = 12.0f;
            features->free_aim_limit_fov = false;

            features->enable_esp = true;
            features->enable_bounding_box = true;
            features->enable_name_esp = true;
            features->enable_distance = true;
            features->enable_healthbar = true;
            features->enable_body_status = true;
            features->show_status_movement = true;
            features->show_status_reload = true;
            features->show_status_grabbed = true;
            features->show_status_gun_firing = true;
            features->show_status_knocked = true;

            set_keybind_value(features->aimbot_keybind, 'E', c_keybind::TOGGLE);
            set_keybind_value(features->free_aim_keybind, 'E', c_keybind::TOGGLE);

            applied = { "blatant preset", "esp", "names", "distance", "health", "body status", "aimbot", "prediction", "nearest part", "silent aim on", "aimbot key E toggle", "silent aim key X toggle" };
        }
        std::vector<std::string> tokenize_words(const std::string& text)
        {
            std::vector<std::string> tokens;
            std::string current;
            for (char c : text)
            {
                if (std::isalnum(static_cast<unsigned char>(c)))
                {
                    current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                }
                else if (!current.empty())
                {
                    tokens.push_back(current);
                    current.clear();
                }
            }
            if (!current.empty())
                tokens.push_back(current);
            return tokens;
        }

        bool try_parse_key_token(const std::vector<std::string>& tokens, std::string& out_token)
        {
            for (size_t i = 0; i + 1 < tokens.size(); ++i)
            {
                if (tokens[i] == "to" || tokens[i] == "key" || tokens[i] == "bind" || tokens[i] == "keybind")
                {
                    if (tokens[i + 1] == "to" && i + 2 < tokens.size())
                    {
                        if (i + 3 < tokens.size())
                        {
                            const std::string& word = tokens[i + 2];
                            const std::string& next = tokens[i + 3];
                            if ((next == "button" || next == "mouse"))
                            {
                                if (word == "right")
                                {
                                    out_token = "rbutton";
                                    return true;
                                }
                                if (word == "left")
                                {
                                    out_token = "lbutton";
                                    return true;
                                }
                                if (word == "middle" || word == "mid")
                                {
                                    out_token = "mbutton";
                                    return true;
                                }
                            }
                        }

                        out_token = tokens[i + 2];
                        return true;
                    }
                    if (i + 2 < tokens.size())
                    {
                        const std::string& word = tokens[i + 1];
                        const std::string& next = tokens[i + 2];
                        if ((next == "button" || next == "mouse"))
                        {
                            if (word == "right")
                            {
                                out_token = "rbutton";
                                return true;
                            }
                            if (word == "left")
                            {
                                out_token = "lbutton";
                                return true;
                            }
                            if (word == "middle" || word == "mid")
                            {
                                out_token = "mbutton";
                                return true;
                            }
                        }
                    }
                    out_token = tokens[i + 1];
                    return true;
                }
            }
            return false;
        }

        bool try_key_from_token(const std::string& token, int& out_vk)
        {
            if (token.size() == 1)
            {
                const char c = token[0];
                if (c >= 'a' && c <= 'z')
                {
                    out_vk = static_cast<int>(std::toupper(static_cast<unsigned char>(c)));
                    return true;
                }
                if (c >= '0' && c <= '9')
                {
                    out_vk = static_cast<int>(c);
                    return true;
                }
            }

            if (token[0] == 'f' && token.size() >= 2 && token.size() <= 3)
            {
                const int num = std::atoi(token.c_str() + 1);
                if (num >= 1 && num <= 24)
                {
                    out_vk = VK_F1 + (num - 1);
                    return true;
                }
            }

            if (token == "del" || token == "delete")
            {
                out_vk = VK_DELETE;
                return true;
            }
            if (token == "ins" || token == "insert")
            {
                out_vk = VK_INSERT;
                return true;
            }
            if (token == "tab")
            {
                out_vk = VK_TAB;
                return true;
            }
            if (token == "shift")
            {
                out_vk = VK_SHIFT;
                return true;
            }
            if (token == "ctrl" || token == "control")
            {
                out_vk = VK_CONTROL;
                return true;
            }
            if (token == "alt")
            {
                out_vk = VK_MENU;
                return true;
            }
            if (token == "space")
            {
                out_vk = VK_SPACE;
                return true;
            }
            if (token == "capslock")
            {
                out_vk = VK_CAPITAL;
                return true;
            }
            if (token == "home")
            {
                out_vk = VK_HOME;
                return true;
            }
            if (token == "end")
            {
                out_vk = VK_END;
                return true;
            }
            if (token == "pgup" || token == "pageup")
            {
                out_vk = VK_PRIOR;
                return true;
            }
            if (token == "pgdn" || token == "pagedown")
            {
                out_vk = VK_NEXT;
                return true;
            }
            if (token == "up")
            {
                out_vk = VK_UP;
                return true;
            }
            if (token == "down")
            {
                out_vk = VK_DOWN;
                return true;
            }
            if (token == "left")
            {
                out_vk = VK_LEFT;
                return true;
            }
            if (token == "right")
            {
                out_vk = VK_RIGHT;
                return true;
            }
            if (token == "lmb" || token == "mouse1" || token == "lbutton" || token == "leftbutton")
            {
                out_vk = VK_LBUTTON;
                return true;
            }
            if (token == "rmb" || token == "mouse2" || token == "rbutton" || token == "rightbutton")
            {
                out_vk = VK_RBUTTON;
                return true;
            }
            if (token == "mmb" || token == "mouse3" || token == "mbutton" || token == "middlebutton")
            {
                out_vk = VK_MBUTTON;
                return true;
            }

            return false;
        }

        bool try_parse_keybind_mode(const std::string& lowered, c_keybind::keybind_mode& out_mode)
        {
            if (lowered.find("toggle") != std::string::npos)
            {
                out_mode = c_keybind::TOGGLE;
                return true;
            }
            if (lowered.find("hold") != std::string::npos)
            {
                out_mode = c_keybind::HOLD;
                return true;
            }
            if (lowered.find("always") != std::string::npos)
            {
                out_mode = c_keybind::ALWAYS;
                return true;
            }
            return false;
        }

        bool wants_random_key(const std::string& lowered)
        {
            return lowered.find("whatever") != std::string::npos
                || lowered.find("anything") != std::string::npos
                || lowered.find("any key") != std::string::npos
                || lowered.find("random") != std::string::npos
                || lowered.find("pick one") != std::string::npos
                || lowered.find("choose one") != std::string::npos
                || lowered.find("surprise") != std::string::npos;
        }

        int pick_random_key_vk()
        {
            static std::mt19937 rng{ std::random_device{}() };
            static std::vector<int> keys;
            if (keys.empty())
            {
                for (int c = 'A'; c <= 'Z'; ++c)
                    keys.push_back(c);
                for (int c = '0'; c <= '9'; ++c)
                    keys.push_back(c);
                for (int i = 0; i < 12; ++i)
                    keys.push_back(VK_F1 + i);
            }
            std::uniform_int_distribution<size_t> dist(0, keys.size() - 1);
            return keys[dist(rng)];
        }

        std::string key_name_from_vk(int vk)
        {
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
                return std::string(1, static_cast<char>(vk));
            if (vk >= VK_F1 && vk <= VK_F24)
                return "F" + std::to_string((vk - VK_F1) + 1);
            if (vk == VK_LBUTTON)
                return "LMB";
            if (vk == VK_RBUTTON)
                return "RMB";
            if (vk == VK_MBUTTON)
                return "MMB";
            return "key";
        }
        bool try_parse_hitbox_from_text(const std::string& lowered, int& out_index)
        {
            if (lowered.find("upper torso") != std::string::npos
                || lowered.find("uppertorso") != std::string::npos
                || lowered.find("torso") != std::string::npos)
            {
                out_index = 1;
                return true;
            }
            if (lowered.find("humanoid root") != std::string::npos
                || lowered.find("humanoidroot") != std::string::npos
                || lowered.find("root part") != std::string::npos
                || lowered.find("hrp") != std::string::npos
                || lowered.find("root") != std::string::npos)
            {
                out_index = 2;
                return true;
            }
            if (lowered.find("head") != std::string::npos)
            {
                out_index = 0;
                return true;
            }
            return false;
        }

        bool try_parse_name_font_from_text(const std::string& lowered, int& out_index)
        {
            if (lowered.find("proggy") != std::string::npos)
            {
                out_index = 4;
                return true;
            }
            if (lowered.find("verdana bold") != std::string::npos
                || (lowered.find("verdana") != std::string::npos && lowered.find("bold") != std::string::npos))
            {
                out_index = 2;
                return true;
            }
            if (lowered.find("verdana") != std::string::npos)
            {
                out_index = 1;
                return true;
            }
            if (lowered.find("tahoma") != std::string::npos)
            {
                out_index = 0;
                return true;
            }
            if (lowered.find("smallest pixel") != std::string::npos
                || lowered.find("pixel") != std::string::npos)
            {
                out_index = 3;
                return true;
            }
            return false;
        }

        bool try_parse_name_mode_from_text(const std::string& lowered, int& out_index)
        {
            if (lowered.find("display name") != std::string::npos
                || lowered.find("displayname") != std::string::npos
                || lowered.find("display") != std::string::npos)
            {
                out_index = 1;
                return true;
            }
            if (lowered.find("username") != std::string::npos
                || lowered.find("user name") != std::string::npos)
            {
                out_index = 0;
                return true;
            }
            return false;
        }

        bool try_parse_box_style_from_text(const std::string& lowered, int& out_index)
        {
            if (lowered.find("corner") != std::string::npos)
            {
                out_index = 1;
                return true;
            }
            if (lowered.find("full") != std::string::npos)
            {
                out_index = 0;
                return true;
            }
            return false;
        }

        bool try_parse_healthbar_mode_from_text(const std::string& lowered, int& out_index)
        {
            if (lowered.find("health-based") != std::string::npos
                || lowered.find("health based") != std::string::npos)
            {
                out_index = 1;
                return true;
            }
            if (lowered.find("gradient") != std::string::npos)
            {
                out_index = 0;
                return true;
            }
            return false;
        }

        bool try_parse_highlight_mode_from_text(const std::string& lowered, int& out_index)
        {
            if (lowered.find("wireframe") != std::string::npos
                || lowered.find("wire frame") != std::string::npos)
            {
                out_index = 2;
                return true;
            }
            if (lowered.find("mesh") != std::string::npos
                || lowered.find("meshes") != std::string::npos)
            {
                out_index = 1;
                return true;
            }
            if (lowered.find("default") != std::string::npos)
            {
                out_index = 0;
                return true;
            }
            return false;
        }

        bool try_parse_highlight_material_from_text(const std::string& lowered, int& out_index)
        {
            if (lowered.find("glowing") != std::string::npos
                || lowered.find("glow") != std::string::npos
                || lowered.find("emissive") != std::string::npos
                || lowered.find("neon") != std::string::npos)
            {
                out_index = 4;
                return true;
            }
            if (lowered.find("acrylic") != std::string::npos
                || lowered.find("glass") != std::string::npos
                || lowered.find("frosted") != std::string::npos)
            {
                out_index = 3;
                return true;
            }
            if (lowered.find("monochrome") != std::string::npos
                || lowered.find("mono chrome") != std::string::npos
                || lowered.find("monochrom") != std::string::npos
                || lowered.find("grayscale") != std::string::npos
                || lowered.find("greyscale") != std::string::npos)
            {
                out_index = 2;
                return true;
            }
            if (lowered.find("metallic") != std::string::npos
                || lowered.find("metalic") != std::string::npos
                || lowered.find("metal") != std::string::npos)
            {
                out_index = 1;
                return true;
            }
            if (lowered.find("flat") != std::string::npos
                || lowered.find("default") != std::string::npos)
            {
                out_index = 0;
                return true;
            }
            return false;
        }

        bool try_parse_aimbot_mode_from_text(const std::string& lowered, int& out_index)
        {
            if (lowered.find("mouse") != std::string::npos)
            {
                out_index = 1;
                return true;
            }
            if (lowered.find("camera") != std::string::npos)
            {
                out_index = 0;
                return true;
            }
            return false;
        }

        bool try_parse_silent_mode_from_text(const std::string& lowered, int& out_index)
        {
            if (lowered.find("viewport") != std::string::npos)
            {
                out_index = 1;
                return true;
            }
            if (lowered.find("free aim") != std::string::npos
                || lowered.find("free-aim") != std::string::npos)
            {
                out_index = 0;
                return true;
            }
            return false;
        }

        bool try_parse_fov_position_from_text(const std::string& lowered, int& out_index)
        {
            if (lowered.find("mouse") != std::string::npos)
            {
                out_index = 1;
                return true;
            }
            if (lowered.find("center") != std::string::npos
                || lowered.find("centre") != std::string::npos)
            {
                out_index = 0;
                return true;
            }
            return false;
        }

        bool try_parse_hex_color(const std::string& text, ImVec4& out_color)
        {
            const auto hash = text.find('#');
            if (hash == std::string::npos)
                return false;

            const size_t hex_start = hash + 1;
            size_t hex_len = 0;
            while (hex_start + hex_len < text.size() && std::isxdigit(static_cast<unsigned char>(text[hex_start + hex_len])))
                ++hex_len;
            if (hex_len != 6 && hex_len != 8)
                return false;

            auto hex_to_int = [](char c) -> int
            {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F')
                    return 10 + (c - 'A');
                return 0;
            };

            auto read_byte = [&](size_t offset) -> int
            {
                return (hex_to_int(text[hex_start + offset]) << 4) | hex_to_int(text[hex_start + offset + 1]);
            };

            const int r = read_byte(0);
            const int g = read_byte(2);
            const int b = read_byte(4);
            const int a = (hex_len == 8) ? read_byte(6) : 255;

            out_color = ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
            return true;
        }

        bool try_parse_color_from_text(const std::string& lowered, ImVec4& out_color)
        {
            if (try_parse_hex_color(lowered, out_color))
                return true;

            struct named_color
            {
                const char* name;
                ImVec4 color;
            };

            static const named_color k_colors[] = {
                { "red", ImVec4(1.0f, 0.0f, 0.0f, 1.0f) },
                { "green", ImVec4(0.0f, 1.0f, 0.0f, 1.0f) },
                { "blue", ImVec4(0.0f, 0.45f, 1.0f, 1.0f) },
                { "yellow", ImVec4(1.0f, 0.9f, 0.0f, 1.0f) },
                { "orange", ImVec4(1.0f, 0.55f, 0.0f, 1.0f) },
                { "purple", ImVec4(0.6f, 0.2f, 1.0f, 1.0f) },
                { "violet", ImVec4(0.55f, 0.3f, 0.9f, 1.0f) },
                { "magenta", ImVec4(1.0f, 0.0f, 1.0f, 1.0f) },
                { "pink", ImVec4(1.0f, 0.4f, 0.7f, 1.0f) },
                { "cyan", ImVec4(0.0f, 0.85f, 1.0f, 1.0f) },
                { "teal", ImVec4(0.0f, 0.7f, 0.7f, 1.0f) },
                { "white", ImVec4(1.0f, 1.0f, 1.0f, 1.0f) },
                { "black", ImVec4(0.0f, 0.0f, 0.0f, 1.0f) },
                { "gray", ImVec4(0.5f, 0.5f, 0.5f, 1.0f) },
                { "grey", ImVec4(0.5f, 0.5f, 0.5f, 1.0f) },
                { "brown", ImVec4(0.6f, 0.4f, 0.2f, 1.0f) },
                { "lime", ImVec4(0.4f, 1.0f, 0.4f, 1.0f) }
            };

            for (const auto& entry : k_colors)
            {
                if (lowered.find(entry.name) != std::string::npos)
                {
                    out_color = entry.color;
                    const bool light = lowered.find("light") != std::string::npos;
                    const bool dark = lowered.find("dark") != std::string::npos;
                    if (light && !dark)
                        out_color = ImLerp(out_color, ImVec4(1.0f, 1.0f, 1.0f, out_color.w), 0.35f);
                    else if (dark)
                        out_color = ImLerp(out_color, ImVec4(0.0f, 0.0f, 0.0f, out_color.w), 0.35f);
                    return true;
                }
            }

            return false;
        }

        ImVec4 with_alpha(const ImVec4& color, float alpha)
        {
            return ImVec4(color.x, color.y, color.z, alpha);
        }
        bool try_handle_ai_keybind_command(const std::string& input, std::string& out_response)
        {
            const std::string lowered = to_lower_ascii(input);
            const bool mentions_key = lowered.find("key") != std::string::npos
                || lowered.find("bind") != std::string::npos
                || lowered.find("keybind") != std::string::npos;
            if (!mentions_key)
                return false;

            c_keybind* target_bind = nullptr;
            std::string target_name;
            if (lowered.find("aimbot") != std::string::npos)
            {
                target_bind = &features->aimbot_keybind;
                target_name = "aimbot";
            }
            else if (lowered.find("silent") != std::string::npos || lowered.find("free aim") != std::string::npos || lowered.find("free-aim") != std::string::npos)
            {
                target_bind = &features->free_aim_keybind;
                target_name = "silent aim";
            }
            else if (lowered.find("fly") != std::string::npos)
            {
                target_bind = &features->fly_keybind;
                target_name = "fly";
            }
            else if (lowered.find("noclip") != std::string::npos)
            {
                target_bind = &features->noclip_keybind;
                target_name = "noclip";
            }
            else if (lowered.find("desync") != std::string::npos)
            {
                target_bind = &features->desync_keybind;
                target_name = "desync";
            }
            else if (lowered.find("freeze") != std::string::npos)
            {
                target_bind = &features->freeze_players_keybind;
                target_name = "freeze";
            }
            else if (lowered.find("menu") != std::string::npos)
            {
                target_bind = &vanille::overlay::menu_key_bind();
                target_name = "menu";
            }

            if (!target_bind)
                return false;

            const auto tokens = tokenize_words(input);
            std::string key_token;
            if (!try_parse_key_token(tokens, key_token))
            {
                if (wants_random_key(lowered))
                {
                    const int vk = pick_random_key_vk();
                    c_keybind::keybind_mode mode = target_bind->type;
                    try_parse_keybind_mode(lowered, mode);

                    target_bind->key = vk;
                    target_bind->type = mode;
                    target_bind->waiting_for_input = false;
                    target_bind->awaiting_release = false;
                    target_bind->menu_opened = false;
                    target_bind->was_down = false;
                    target_bind->enabled = (mode == c_keybind::ALWAYS);

                    out_response = "Set " + target_name + " keybind to " + key_name_from_vk(vk) + ".";
                    return true;
                }

                c_keybind::keybind_mode mode = target_bind->type;
                const bool mode_explicit = try_parse_keybind_mode(lowered, mode);
                if (!mode_explicit)
                {
                    out_response = "Tell me the key to bind.";
                    return true;
                }

                target_bind->type = mode;
                target_bind->waiting_for_input = false;
                target_bind->awaiting_release = false;
                target_bind->menu_opened = false;
                target_bind->was_down = false;
                target_bind->enabled = (mode == c_keybind::ALWAYS);

                const char* mode_name = mode == c_keybind::TOGGLE ? "toggle"
                    : (mode == c_keybind::HOLD ? "hold" : "always");
                out_response = "Set " + target_name + " keybind mode to " + mode_name + ".";
                return true;
            }

            auto is_mode_token = [](const std::string& token)
            {
                return token == "mode" || token == "toggle" || token == "hold" || token == "always";
            };

            int vk = 0;
            if (!try_key_from_token(key_token, vk))
            {
                if (wants_random_key(lowered))
                {
                    vk = pick_random_key_vk();
                    c_keybind::keybind_mode mode = target_bind->type;
                    try_parse_keybind_mode(lowered, mode);

                    target_bind->key = vk;
                    target_bind->type = mode;
                    target_bind->waiting_for_input = false;
                    target_bind->awaiting_release = false;
                    target_bind->menu_opened = false;
                    target_bind->was_down = false;
                    target_bind->enabled = (mode == c_keybind::ALWAYS);

                    out_response = "Set " + target_name + " keybind to " + key_name_from_vk(vk) + ".";
                    return true;
                }

                if (is_mode_token(key_token))
                {
                    c_keybind::keybind_mode mode = target_bind->type;
                    const bool mode_explicit = try_parse_keybind_mode(lowered, mode);
                    if (!mode_explicit)
                    {
                        out_response = "Tell me the key to bind.";
                        return true;
                    }

                    target_bind->type = mode;
                    target_bind->waiting_for_input = false;
                    target_bind->awaiting_release = false;
                    target_bind->menu_opened = false;
                    target_bind->was_down = false;
                    target_bind->enabled = (mode == c_keybind::ALWAYS);

                    const char* mode_name = mode == c_keybind::TOGGLE ? "toggle"
                        : (mode == c_keybind::HOLD ? "hold" : "always");
                    out_response = "Set " + target_name + " keybind mode to " + mode_name + ".";
                    return true;
                }

                out_response = "Unsupported key.";
                return true;
            }

            c_keybind::keybind_mode mode = target_bind->type;
            try_parse_keybind_mode(lowered, mode);

            target_bind->key = vk;
            target_bind->type = mode;
            target_bind->waiting_for_input = false;
            target_bind->awaiting_release = false;
            target_bind->menu_opened = false;
            target_bind->was_down = false;
            target_bind->enabled = (mode == c_keybind::ALWAYS);

            out_response = "Set " + target_name + " keybind to " + key_name_from_vk(vk) + ".";
            return true;
        }

        bool handle_pending_appearance_value(const std::string& input, std::string& out_response)
        {
            if (g_pending_appearance == pending_appearance::none)
                return false;

            const std::string lowered = to_lower_ascii(input);
            if (lowered.find("cancel") != std::string::npos
                || lowered.find("never mind") != std::string::npos
                || lowered.find("nevermind") != std::string::npos)
            {
                g_pending_appearance = pending_appearance::none;
                out_response = "Cancelled.";
                return true;
            }

            float value = 0.0f;
            if (!try_extract_first_number(lowered, value))
            {
                out_response = "Tell me the value to use.";
                return true;
            }

            ImGuiStyle& style = ImGui::GetStyle();
            if (g_pending_appearance == pending_appearance::shadow_size)
            {
                style.WindowShadowSize = std::clamp(value, 0.0f, 128.0f);
                out_response = "Set shadow size to " + format_number(style.WindowShadowSize) + ".";
            }
            else
            {
                style.WindowShadowOffsetDist = std::clamp(value, 0.0f, 64.0f);
                out_response = "Set shadow offset distance to " + format_number(style.WindowShadowOffsetDist) + ".";
            }

            g_pending_appearance = pending_appearance::none;
            return true;
        }

        bool try_handle_ai_appearance_command(const std::string& input, std::string& out_response)
        {
            const std::string lowered = to_lower_ascii(input);
            ImGuiStyle& style = ImGui::GetStyle();

            if (lowered.find("shadow size") != std::string::npos
                || lowered.find("shadow strength") != std::string::npos)
            {
                float value = 0.0f;
                if (try_extract_first_number(lowered, value))
                {
                    style.WindowShadowSize = std::clamp(value, 0.0f, 128.0f);
                    out_response = "Set shadow size to " + format_number(style.WindowShadowSize) + ".";
                }
                else
                {
                    g_pending_appearance = pending_appearance::shadow_size;
                    out_response = "Tell me the shadow size.";
                }
                return true;
            }

            if (lowered.find("shadow offset") != std::string::npos
                || lowered.find("shadow distance") != std::string::npos)
            {
                float value = 0.0f;
                if (try_extract_first_number(lowered, value))
                {
                    style.WindowShadowOffsetDist = std::clamp(value, 0.0f, 64.0f);
                    out_response = "Set shadow offset distance to " + format_number(style.WindowShadowOffsetDist) + ".";
                }
                else
                {
                    g_pending_appearance = pending_appearance::shadow_offset;
                    out_response = "Tell me the shadow offset distance.";
                }
                return true;
            }

            if (lowered.find("shadow color") != std::string::npos
                || lowered.find("shadow colour") != std::string::npos)
            {
                ImVec4 color;
                if (try_parse_color_from_text(lowered, color))
                {
                    style.Colors[ImGuiCol_WindowShadow] = with_alpha(color, style.Colors[ImGuiCol_WindowShadow].w);
                    out_response = "Updated shadow color.";
                    return true;
                }
            }

            if ((lowered.find("accent") != std::string::npos)
                && (lowered.find("color") != std::string::npos || lowered.find("colour") != std::string::npos))
            {
                ImVec4 color;
                if (try_parse_color_from_text(lowered, color))
                {
                    c_colors::top_accent_color = with_alpha(color, 1.0f);
                    c_colors::bottom_accent_color = c_colors::derive_bottom_accent(c_colors::top_accent_color);
                    out_response = "Updated accent color.";
                    return true;
                }
            }

            if (lowered.find("blur") != std::string::npos)
            {
                bool enable = false;
                if (try_parse_enable_intent(lowered, enable))
                {
                    vanille::overlay::overlay_blur_enabled() = enable;
                    out_response = enable ? "Menu blur on." : "Menu blur off.";
                    return true;
                }
            }

            if (contains_theme_keywords(lowered) || has_recent_theme_context())
            {
                static const char* k_theme_names[] = {
                    "Default", "Cherry", "Blue", "Purplish", "Gamesense", "Onetap", "Assembly", "Dracula"
                };

                int theme_index = -1;
                if (lowered.find("default") != std::string::npos)
                    theme_index = 0;
                else if (lowered.find("cherry") != std::string::npos)
                    theme_index = 1;
                else if (lowered.find("blue") != std::string::npos)
                    theme_index = 2;
                else if (lowered.find("purplish") != std::string::npos || lowered.find("purple") != std::string::npos)
                    theme_index = 3;
                else if (lowered.find("gamesense") != std::string::npos)
                    theme_index = 4;
                else if (lowered.find("onetap") != std::string::npos || lowered.find("one tap") != std::string::npos)
                    theme_index = 5;
                else if (lowered.find("assembly") != std::string::npos)
                    theme_index = 6;
                else if (lowered.find("dracula") != std::string::npos)
                    theme_index = 7;

                const bool wants_random = lowered.find("random") != std::string::npos
                    || lowered.find("surprise") != std::string::npos
                    || lowered.find("shuffle") != std::string::npos;
                const bool wants_next = lowered.find("next") != std::string::npos
                    || lowered.find("cycle") != std::string::npos
                    || lowered.find("different") != std::string::npos
                    || lowered.find("change") != std::string::npos
                    || lowered.find("switch") != std::string::npos;

                if (theme_index < 0 && (wants_random || wants_next))
                    theme_index = pick_random_theme_index_excluding(features->menu_theme);

                if (theme_index >= 0)
                {
                    features->menu_theme = theme_index;
                    vanille::overlay::apply_theme_preset(theme_index);
                    const char* name = (theme_index >= 0 && theme_index < static_cast<int>(IM_ARRAYSIZE(k_theme_names)))
                        ? k_theme_names[theme_index]
                        : "theme";
                    out_response = std::string("Applied the ") + name + " theme. " + pick_theme_flavor_line();
                    return true;
                }

                out_response = "Tell me the theme: Default, Cherry, Blue, Purplish, Gamesense, Onetap, Assembly, or Dracula.";
                return true;
            }

            return false;
        }

        const char* feature_name(ai_feature_target target)
        {
            switch (target)
            {
            case ai_feature_target::vsync:
                return "v-sync";
            case ai_feature_target::fly:
                return "fly";
            case ai_feature_target::noclip:
                return "noclip";
            case ai_feature_target::auto_shooter:
                return "auto shooter";
            case ai_feature_target::esp:
                return "esp";
            case ai_feature_target::boxes:
                return "boxes";
            case ai_feature_target::names:
                return "name esp";
            case ai_feature_target::skeleton:
                return "skeleton";
            case ai_feature_target::distance:
                return "distance";
            case ai_feature_target::health:
                return "healthbar";
            case ai_feature_target::armor:
                return "armor bar";
            case ai_feature_target::body_status:
                return "body status";
            case ai_feature_target::highlight:
                return "highlight";
            case ai_feature_target::aimbot:
                return "aimbot";
            case ai_feature_target::silent_aim:
                return "silent aim";
            default:
                return "feature";
            }
        }

        void apply_feature_toggle(ai_feature_target target, bool enable)
        {
            switch (target)
            {
            case ai_feature_target::vsync:
                features->enable_vsync = enable;
                break;
            case ai_feature_target::fly:
                features->enable_fly = enable;
                break;
            case ai_feature_target::noclip:
                features->enable_noclip = enable;
                break;
            case ai_feature_target::auto_shooter:
                features->enable_auto_shooter = enable;
                break;
            case ai_feature_target::esp:
                features->enable_esp = enable;
                break;
            case ai_feature_target::boxes:
                features->enable_bounding_box = enable;
                if (enable)
                    features->enable_esp = true;
                break;
            case ai_feature_target::names:
                features->enable_name_esp = enable;
                if (enable)
                    features->enable_esp = true;
                break;
            case ai_feature_target::skeleton:
                features->enable_skeleton = enable;
                if (enable)
                    features->enable_esp = true;
                break;
            case ai_feature_target::distance:
                features->enable_distance = enable;
                if (enable)
                    features->enable_esp = true;
                break;
            case ai_feature_target::health:
                features->enable_healthbar = enable;
                if (enable)
                    features->enable_esp = true;
                break;
            case ai_feature_target::armor:
                features->enable_armor_bar = enable;
                if (enable)
                    features->enable_esp = true;
                break;
            case ai_feature_target::body_status:
                features->enable_body_status = enable;
                if (enable)
                {
                    features->enable_esp = true;
                    features->show_status_movement = true;
                    features->show_status_reload = true;
                    features->show_status_grabbed = true;
                    features->show_status_gun_firing = true;
                    features->show_status_knocked = true;
                }
                break;
            case ai_feature_target::highlight:
                features->enable_highlight = enable;
                if (enable)
                    features->enable_esp = true;
                break;
            case ai_feature_target::aimbot:
                features->enable_aimbot = enable;
                break;
            case ai_feature_target::silent_aim:
                features->enable_free_aim = enable;
                break;
            default:
                break;
            }
        }

        bool try_handle_ai_feature_toggle_command(const std::string& input, std::string& out_response)
        {
            const std::string lowered = to_lower_ascii(input);
            bool enable = false;
            if (!try_parse_enable_intent(lowered, enable))
                return false;

            ai_feature_target target = ai_feature_target::none;
            if (lowered.find("v-sync") != std::string::npos || lowered.find("vsync") != std::string::npos)
                target = ai_feature_target::vsync;
            else if (lowered.find("fly") != std::string::npos)
                target = ai_feature_target::fly;
            else if (lowered.find("noclip") != std::string::npos)
                target = ai_feature_target::noclip;
            else if (lowered.find("auto shooter") != std::string::npos || lowered.find("auto-shooter") != std::string::npos)
                target = ai_feature_target::auto_shooter;
            else if (lowered.find("esp") != std::string::npos && lowered.find("name") == std::string::npos)
                target = ai_feature_target::esp;
            else if (lowered.find("box") != std::string::npos)
                target = ai_feature_target::boxes;
            else if (lowered.find("name esp") != std::string::npos || lowered.find("names") != std::string::npos)
                target = ai_feature_target::names;
            else if (lowered.find("skeleton") != std::string::npos)
                target = ai_feature_target::skeleton;
            else if (lowered.find("distance") != std::string::npos)
                target = ai_feature_target::distance;
            else if (lowered.find("healthbar") != std::string::npos || lowered.find("health bar") != std::string::npos)
                target = ai_feature_target::health;
            else if (lowered.find("armor") != std::string::npos)
                target = ai_feature_target::armor;
            else if (lowered.find("body status") != std::string::npos || lowered.find("status") != std::string::npos)
                target = ai_feature_target::body_status;
            else if (lowered.find("highlight") != std::string::npos)
                target = ai_feature_target::highlight;
            else if (lowered.find("aimbot") != std::string::npos)
                target = ai_feature_target::aimbot;
            else if (lowered.find("silent") != std::string::npos || lowered.find("free aim") != std::string::npos || lowered.find("free-aim") != std::string::npos)
                target = ai_feature_target::silent_aim;

            if (target == ai_feature_target::none)
            {
                if ((lowered.find("it") != std::string::npos || lowered.find("that") != std::string::npos || lowered.find("this") != std::string::npos)
                    && g_last_feature_toggle != ai_feature_target::none)
                {
                    target = g_last_feature_toggle;
                }
            }

            if (target == ai_feature_target::none)
                return false;

            apply_feature_toggle(target, enable);
            g_last_feature_toggle = target;

            out_response = std::string("Set ") + feature_name(target) + (enable ? " on." : " off.");
            return true;
        }

        bool try_extract_quoted_text(const std::string& input, std::string& out_text)
        {
            const auto first_quote = input.find('"');
            if (first_quote != std::string::npos)
            {
                const auto second_quote = input.find('"', first_quote + 1);
                if (second_quote != std::string::npos && second_quote > first_quote + 1)
                {
                    out_text = input.substr(first_quote + 1, second_quote - first_quote - 1);
                    return true;
                }
            }

            const auto first_tick = input.find('\'');
            if (first_tick != std::string::npos)
            {
                const auto second_tick = input.find('\'', first_tick + 1);
                if (second_tick != std::string::npos && second_tick > first_tick + 1)
                {
                    out_text = input.substr(first_tick + 1, second_tick - first_tick - 1);
                    return true;
                }
            }

            return false;
        }

        bool try_extract_config_name(const std::string& input, const std::string& lowered, std::string& out_name)
        {
            if (try_extract_quoted_text(input, out_name))
                return true;

            if (try_match_existing_config_name(lowered, out_name))
                return true;

            static const char* k_keywords[] = { "config", "preset", "profile" };
            for (const char* keyword : k_keywords)
            {
                const auto pos = lowered.find(keyword);
                if (pos == std::string::npos)
                    continue;

                size_t start = pos + std::strlen(keyword);
                while (start < input.size())
                {
                    const char c = input[start];
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                        break;
                    ++start;
                }

                size_t end = start;
                while (end < input.size())
                {
                    const char c = input[end];
                    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
                        break;
                    ++end;
                }

                if (end > start)
                {
                    out_name = input.substr(start, end - start);
                    return true;
                }
            }

            return false;
        }

        bool try_extract_axis_value(const std::string& lowered, const char* axis, const char* primary, float& out_value)
        {
            const std::string key1 = std::string(primary) + " " + axis;
            const std::string key2 = std::string(primary) + axis;
            const std::string key3 = std::string(axis) + " " + primary;
            return try_extract_number_after(lowered, key1.c_str(), out_value)
                || try_extract_number_after(lowered, key2.c_str(), out_value)
                || try_extract_number_after(lowered, key3.c_str(), out_value);
        }

        bool try_handle_ai_config_command(const std::string& input, std::string& out_response)
        {
            const std::string lowered = to_lower_ascii(input);
            const auto tokens = tokenize_words(lowered);
            auto has_token = [&](const char* token)
            {
                return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
            };

            const bool mentions_config = has_token("config") || has_token("configs") || has_token("cfg")
                || has_token("preset") || has_token("profile") || has_token("configuration");

            ai_config_preset preset = detect_config_preset(lowered);
            if (preset != ai_config_preset::none)
            {
                const bool apply_preset = mentions_config
                    || lowered.find("setup") != std::string::npos
                    || lowered.find("settings") != std::string::npos
                    || lowered.find("apply") != std::string::npos
                    || lowered.find("use") != std::string::npos
                    || lowered.find("set") != std::string::npos
                    || lowered.find("load") != std::string::npos;

                if (apply_preset)
                {
                    std::vector<std::string> applied;
                    apply_preset_config(preset, applied);
                    if (!applied.empty())
                    {
                        out_response = "Applied config: " + join_items(applied) + ".";
                        return true;
                    }
                }
            }

            if (mentions_config)
            {
                if (has_token("list") || has_token("show"))
                {
                    config_manager::refresh();
                    const auto& configs = config_manager::get_configs();
                    if (configs.empty())
                        out_response = "No configs saved yet.";
                    else
                        out_response = "Saved configs: " + join_items(configs) + ".";
                    return true;
                }

                if ((has_token("open") || has_token("folder") || has_token("directory") || has_token("dir"))
                    && (has_token("config") || has_token("configs")))
                {
                    config_manager::open_directory();
                    out_response = "Opened config folder.";
                    return true;
                }

                std::string config_name;
                const bool has_name = try_extract_config_name(input, lowered, config_name);

                if (has_token("save") || has_token("store"))
                {
                    if (!has_name)
                        config_name = generate_config_name();
                    config_name = config_manager::sanitize(config_name);
                    const bool ok = config_manager::save(config_name);
                    if (ok)
                        out_response = "Saved config " + config_name + ".";
                    else
                        out_response = config_manager::get_status().empty() ? "Failed to save config." : config_manager::get_status();
                    return true;
                }

                if (has_token("create") || has_token("new"))
                {
                    if (!has_name)
                        config_name = generate_config_name();
                    config_name = config_manager::sanitize(config_name);
                    const bool ok = config_manager::create(config_name);
                    if (ok)
                        out_response = "Created config " + config_name + ".";
                    else
                        out_response = config_manager::get_status().empty() ? "Failed to create config." : config_manager::get_status();
                    return true;
                }

                if (has_token("load") || has_token("apply") || has_token("use"))
                {
                    if (!has_name)
                    {
                        out_response = "Tell me which config to load.";
                        return true;
                    }
                    config_name = config_manager::sanitize(config_name);
                    const bool ok = config_manager::load(config_name);
                    if (ok)
                        out_response = "Loaded config " + config_name + ".";
                    else
                        out_response = config_manager::get_status().empty() ? "Failed to load config." : config_manager::get_status();
                    return true;
                }

                if (has_token("delete") || has_token("remove"))
                {
                    if (!has_name)
                    {
                        out_response = "Tell me which config to delete.";
                        return true;
                    }
                    config_name = config_manager::sanitize(config_name);
                    const bool ok = config_manager::remove(config_name);
                    if (ok)
                        out_response = "Deleted config " + config_name + ".";
                    else
                        out_response = config_manager::get_status().empty() ? "Failed to delete config." : config_manager::get_status();
                    return true;
                }
            }

            std::vector<std::string> applied;

            const bool is_silent = lowered.find("silent") != std::string::npos
                || lowered.find("free aim") != std::string::npos
                || lowered.find("free-aim") != std::string::npos;
            const bool is_aimbot = lowered.find("aimbot") != std::string::npos;

            if (lowered.find("nearest part") != std::string::npos || lowered.find("closest part") != std::string::npos)
            {
                bool enable = true;
                try_parse_enable_intent(lowered, enable);

                if (!is_silent && !is_aimbot)
                {
                    out_response = "Say aimbot nearest part or silent aim nearest part.";
                    return true;
                }

                if (is_aimbot)
                {
                    features->aimbot_nearest_part = enable;
                    applied.push_back("aimbot nearest part");
                }
                if (is_silent)
                {
                    features->free_aim_nearest_part = enable;
                    applied.push_back("silent aim nearest part");
                }
            }

            if (lowered.find("closest point") != std::string::npos)
            {
                bool enable = true;
                try_parse_enable_intent(lowered, enable);

                if (!is_silent && !is_aimbot)
                {
                    out_response = "Say aimbot closest point or silent aim closest point.";
                    return true;
                }

                if (is_aimbot)
                {
                    features->enable_aimbot_closest_point = enable;
                    applied.push_back("aimbot closest point");
                }
                if (is_silent)
                {
                    features->enable_free_aim_closest_point = enable;
                    applied.push_back("silent aim closest point");
                }
            }

            if (lowered.find("mouse spoof") != std::string::npos)
            {
                bool enable = true;
                try_parse_enable_intent(lowered, enable);
                features->free_aim_mouse_spoof = enable;
                if (enable)
                    features->enable_free_aim = true;
                applied.push_back("mouse spoof");
            }

            if (lowered.find("prediction") != std::string::npos)
            {
                float pred_x = 0.0f;
                float pred_y = 0.0f;
                const bool has_pred_x = try_extract_axis_value(lowered, "x", "prediction", pred_x)
                    || try_extract_axis_value(lowered, "x", "pred", pred_x);
                const bool has_pred_y = try_extract_axis_value(lowered, "y", "prediction", pred_y)
                    || try_extract_axis_value(lowered, "y", "pred", pred_y);

                float pred_value = 0.0f;
                const bool has_pred_value = (!has_pred_x && !has_pred_y)
                    && (try_extract_number_after(lowered, "prediction", pred_value) || try_extract_first_number(lowered, pred_value));

                bool pred_enable = true;
                const bool has_toggle = try_parse_enable_intent(lowered, pred_enable);

                const bool target_aimbot = is_aimbot || (!is_aimbot && !is_silent);
                const bool target_free = is_silent;

                if (has_pred_x || has_pred_y || has_pred_value)
                {
                    if (target_aimbot)
                    {
                        if (has_pred_x)
                            features->aimbot_prediction_x = pred_x;
                        if (has_pred_y)
                            features->aimbot_prediction_y = pred_y;
                        if (has_pred_value)
                        {
                            features->aimbot_prediction_x = pred_value;
                            features->aimbot_prediction_y = pred_value;
                        }
                        features->enable_aimbot_prediction = true;
                        applied.push_back("aimbot prediction");
                    }
                    if (target_free)
                    {
                        if (has_pred_x)
                            features->free_aim_prediction_x = pred_x;
                        if (has_pred_y)
                            features->free_aim_prediction_y = pred_y;
                        if (has_pred_value)
                        {
                            features->free_aim_prediction_x = pred_value;
                            features->free_aim_prediction_y = pred_value;
                        }
                        features->free_aim_enable_prediction = true;
                        features->enable_free_aim = true;
                        applied.push_back("silent aim prediction");
                    }
                }
                else if (has_toggle)
                {
                    if (target_aimbot)
                    {
                        features->enable_aimbot_prediction = pred_enable;
                        applied.push_back("aimbot prediction");
                    }
                    if (target_free)
                    {
                        features->free_aim_enable_prediction = pred_enable;
                        if (pred_enable)
                            features->enable_free_aim = true;
                        applied.push_back("silent aim prediction");
                    }
                }
            }

            if (lowered.find("smooth") != std::string::npos)
            {
                float smooth_x = 0.0f;
                float smooth_y = 0.0f;
                const bool has_smooth_x = try_extract_axis_value(lowered, "x", "smoothness", smooth_x)
                    || try_extract_axis_value(lowered, "x", "smooth", smooth_x);
                const bool has_smooth_y = try_extract_axis_value(lowered, "y", "smoothness", smooth_y)
                    || try_extract_axis_value(lowered, "y", "smooth", smooth_y);

                float smooth_value = 0.0f;
                const bool has_smooth_value = (!has_smooth_x && !has_smooth_y)
                    && (try_extract_number_after(lowered, "smoothness", smooth_value) || try_extract_first_number(lowered, smooth_value));

                bool smooth_enable = true;
                const bool has_toggle = try_parse_enable_intent(lowered, smooth_enable);

                if (has_smooth_x || has_smooth_y || has_smooth_value)
                {
                    if (has_smooth_x)
                        features->aimbot_smooth_x = smooth_x;
                    if (has_smooth_y)
                        features->aimbot_smooth_y = smooth_y;
                    if (has_smooth_value)
                    {
                        features->aimbot_smooth_x = smooth_value;
                        features->aimbot_smooth_y = smooth_value;
                    }
                    features->enable_aimbot_smooth = true;
                    applied.push_back("smooth");
                }
                else if (has_toggle)
                {
                    features->enable_aimbot_smooth = smooth_enable;
                    applied.push_back("smooth");
                }
            }

            if (lowered.find("hitbox") != std::string::npos
                || lowered.find("aimpart") != std::string::npos
                || lowered.find("aim part") != std::string::npos)
            {
                int hitbox = 0;
                if (try_parse_hitbox_from_text(lowered, hitbox))
                {
                    if (is_silent)
                    {
                        features->free_aim_hitbox = hitbox;
                        applied.push_back("silent aim hitbox");
                    }
                    if (is_aimbot || !is_silent)
                    {
                        features->aimbot_hitbox = hitbox;
                        applied.push_back("aimbot hitbox");
                    }
                }
            }

            if (lowered.find("aimbot mode") != std::string::npos || (lowered.find("aim mode") != std::string::npos && is_aimbot))
            {
                int mode = 0;
                if (try_parse_aimbot_mode_from_text(lowered, mode))
                {
                    features->aimbot_mode = mode;
                    applied.push_back("aimbot mode");
                }
            }

            if (lowered.find("silent mode") != std::string::npos
                || lowered.find("silent aim mode") != std::string::npos
                || lowered.find("free aim mode") != std::string::npos)
            {
                int mode = 0;
                if (try_parse_silent_mode_from_text(lowered, mode))
                {
                    features->free_aim_silent_mode = mode;
                    applied.push_back("silent aim mode");
                }
            }

            if (lowered.find("fov position") != std::string::npos || lowered.find("fov mode") != std::string::npos)
            {
                int mode = 0;
                if (try_parse_fov_position_from_text(lowered, mode))
                {
                    if (is_silent)
                    {
                        features->free_aim_fov_mode = mode;
                        applied.push_back("silent aim fov position");
                    }
                    else
                    {
                        features->aimbot_fov_mode = mode;
                        applied.push_back("aimbot fov position");
                    }
                }
            }

            if (lowered.find("limit fov") != std::string::npos || lowered.find("fov limit") != std::string::npos)
            {
                bool enable = true;
                try_parse_enable_intent(lowered, enable);
                if (is_silent)
                {
                    features->free_aim_limit_fov = enable;
                    applied.push_back("silent aim limit fov");
                }
                else
                {
                    features->aimbot_limit_fov = enable;
                    applied.push_back("aimbot limit fov");
                }
            }

            if (lowered.find("draw fov") != std::string::npos)
            {
                bool enable = true;
                try_parse_enable_intent(lowered, enable);
                if (is_silent)
                {
                    features->free_aim_draw_fov = enable;
                    applied.push_back("silent aim draw fov");
                }
                else
                {
                    features->aimbot_draw_fov = enable;
                    applied.push_back("aimbot draw fov");
                }
            }

            if (lowered.find("fov") != std::string::npos)
            {
                float radius = 0.0f;
                const bool has_fov_radius = try_extract_number_after(lowered, "fov radius", radius)
                    || try_extract_number_after(lowered, "fov size", radius)
                    || try_extract_number_after(lowered, "fov", radius);

                if (has_fov_radius)
                {
                    if (is_silent)
                    {
                        features->free_aim_fov_radius = radius;
                        applied.push_back("silent aim fov radius");
                    }
                    else if (is_aimbot || !is_silent)
                    {
                        features->aimbot_fov_radius = radius;
                        applied.push_back("aimbot fov radius");
                    }
                }
            }

            if (lowered.find("max distance") != std::string::npos || lowered.find("distance max") != std::string::npos)
            {
                float distance = 0.0f;
                if (try_extract_number_after(lowered, "max distance", distance) || try_extract_number_after(lowered, "distance max", distance))
                {
                    if (is_silent)
                    {
                        features->free_aim_max_distance = distance;
                        applied.push_back("silent aim max distance");
                    }
                    else if (is_aimbot)
                    {
                        features->aimbot_max_distance = distance;
                        applied.push_back("aimbot max distance");
                    }
                    else
                    {
                        features->max_distance = distance;
                        applied.push_back("esp max distance");
                    }
                }
            }

            if (lowered.find("name font") != std::string::npos || (lowered.find("font") != std::string::npos && lowered.find("name") != std::string::npos))
            {
                int font = 0;
                if (try_parse_name_font_from_text(lowered, font))
                {
                    features->name_esp_font = font;
                    applied.push_back("name font");
                }
            }

            if (lowered.find("name mode") != std::string::npos
                || lowered.find("display name") != std::string::npos
                || lowered.find("username") != std::string::npos)
            {
                int mode = 0;
                if (try_parse_name_mode_from_text(lowered, mode))
                {
                    features->name_esp_mode = mode;
                    applied.push_back("name mode");
                }
            }

            if (lowered.find("box style") != std::string::npos
                || lowered.find("box type") != std::string::npos
                || lowered.find("corner box") != std::string::npos
                || lowered.find("full box") != std::string::npos)
            {
                int style = 0;
                if (try_parse_box_style_from_text(lowered, style))
                {
                    features->bounding_box_style = style;
                    applied.push_back("box style");
                }
            }

            if (lowered.find("healthbar mode") != std::string::npos || lowered.find("health bar mode") != std::string::npos)
            {
                int mode = 0;
                if (try_parse_healthbar_mode_from_text(lowered, mode))
                {
                    features->healthbar_color_mode = mode;
                    applied.push_back("healthbar mode");
                }
            }

            if (lowered.find("highlight mode") != std::string::npos)
            {
                int mode = 0;
                if (try_parse_highlight_mode_from_text(lowered, mode))
                {
                    features->highlight_mode = mode;
                    applied.push_back("highlight mode");
                }
            }

            if (lowered.find("mesh material") != std::string::npos
                || lowered.find("highlight material") != std::string::npos
                || ((lowered.find("highlight") != std::string::npos || lowered.find("mesh") != std::string::npos)
                    && (lowered.find("metallic") != std::string::npos
                        || lowered.find("metalic") != std::string::npos
                        || lowered.find("flat") != std::string::npos
                        || lowered.find("monochrome") != std::string::npos
                        || lowered.find("mono chrome") != std::string::npos
                        || lowered.find("grayscale") != std::string::npos
                        || lowered.find("greyscale") != std::string::npos
                        || lowered.find("acrylic") != std::string::npos
                        || lowered.find("glass") != std::string::npos
                        || lowered.find("frosted") != std::string::npos
                        || lowered.find("glowing") != std::string::npos
                        || lowered.find("glow") != std::string::npos
                        || lowered.find("emissive") != std::string::npos
                        || lowered.find("neon") != std::string::npos)))
            {
                int material = 0;
                if (try_parse_highlight_material_from_text(lowered, material))
                {
                    features->highlight_mesh_material = material;
                    applied.push_back("mesh material");
                }
            }

            if (lowered.find("color") != std::string::npos || lowered.find("colour") != std::string::npos)
            {
                ImVec4 color;
                if (try_parse_color_from_text(lowered, color))
                {
                    bool color_applied = false;

                    if (lowered.find("box") != std::string::npos || lowered.find("bounding") != std::string::npos)
                    {
                        features->bounding_box_color = color;
                        applied.push_back("box color");
                        color_applied = true;
                    }
                    else if (lowered.find("name") != std::string::npos)
                    {
                        features->name_esp_color = color;
                        applied.push_back("name color");
                        color_applied = true;
                    }
                    else if (lowered.find("distance") != std::string::npos)
                    {
                        features->distance_color = color;
                        applied.push_back("distance color");
                        color_applied = true;
                    }
                    else if (lowered.find("status") != std::string::npos)
                    {
                        features->status_color = color;
                        applied.push_back("status color");
                        color_applied = true;
                    }
                    else if (lowered.find("skeleton") != std::string::npos)
                    {
                        if (lowered.find("outline") != std::string::npos)
                        {
                            features->skeleton_outline_color = with_alpha(color, features->skeleton_outline_color.w);
                            applied.push_back("skeleton outline color");
                        }
                        else
                        {
                            features->skeleton_color = with_alpha(color, features->skeleton_color.w);
                            applied.push_back("skeleton color");
                        }
                        color_applied = true;
                    }
                    else if (lowered.find("highlight") != std::string::npos)
                    {
                        if (lowered.find("outline") != std::string::npos)
                        {
                            features->highlight_outline_color = with_alpha(color, features->highlight_outline_color.w);
                            applied.push_back("highlight outline color");
                        }
                        else if (lowered.find("fill") != std::string::npos || lowered.find("inside") != std::string::npos)
                        {
                            features->highlight_fill_color = with_alpha(color, features->highlight_fill_color.w);
                            applied.push_back("highlight fill color");
                        }
                        else
                        {
                            features->highlight_fill_color = with_alpha(color, features->highlight_fill_color.w);
                            features->highlight_outline_color = with_alpha(color, features->highlight_outline_color.w);
                            applied.push_back("highlight color");
                        }
                        color_applied = true;
                    }
                    else if (lowered.find("health") != std::string::npos)
                    {
                        if (lowered.find("top") != std::string::npos || lowered.find("upper") != std::string::npos)
                        {
                            features->healthbar_top_color = with_alpha(color, features->healthbar_top_color.w);
                            applied.push_back("healthbar top color");
                        }
                        else if (lowered.find("bottom") != std::string::npos || lowered.find("lower") != std::string::npos)
                        {
                            features->healthbar_bottom_color = with_alpha(color, features->healthbar_bottom_color.w);
                            applied.push_back("healthbar bottom color");
                        }
                        else
                        {
                            features->healthbar_top_color = with_alpha(color, features->healthbar_top_color.w);
                            features->healthbar_bottom_color = with_alpha(color, features->healthbar_bottom_color.w);
                            applied.push_back("healthbar color");
                        }
                        color_applied = true;
                    }
                    else if (lowered.find("dormant") != std::string::npos)
                    {
                        features->dormant_color = with_alpha(color, features->dormant_color.w);
                        features->override_dormant_color = true;
                        applied.push_back("dormant color");
                        color_applied = true;
                    }
                    else if (lowered.find("host") != std::string::npos || lowered.find("auto shooter") != std::string::npos)
                    {
                        features->host_color = color;
                        applied.push_back("auto shooter color");
                        color_applied = true;
                    }

                    if (!color_applied)
                    {
                        out_response = "Tell me which feature color to change.";
                        return true;
                    }
                }
            }

            if (!applied.empty())
            {
                out_response = "Applied config: " + join_items(applied) + ".";
                return true;
            }

            return false;
        }

        bool try_handle_ai_identity_command(const std::string& input, std::string& out_response)
        {
            const std::string lowered = to_lower_ascii(input);
            const bool asks_owner = lowered.find("who") != std::string::npos
                || lowered.find("owner") != std::string::npos
                || lowered.find("developer") != std::string::npos
                || lowered.find("made") != std::string::npos
                || lowered.find("created") != std::string::npos;

            if (asks_owner && lowered.find("vanille") != std::string::npos)
            {
                out_response = "Vanille is owned and developed by Aori, a Roblox developer.";
                return true;
            }

            if (lowered.find("aori") == std::string::npos)
                return false;

            if (lowered.find("like") != std::string::npos || lowered.find("love") != std::string::npos)
            {
                out_response = "Aori is the owner and developer of Vanille. I think his work is impressive.";
                return true;
            }

            out_response = "Vanille is owned and developed by Aori, a Roblox developer.";
            return true;
        }

        bool try_handle_ai_program_info_command(const std::string& input, std::string& out_response)
        {
            const std::string lowered = to_lower_ascii(input);
            const bool asks_what = lowered.find("what") != std::string::npos || lowered.find("whats") != std::string::npos;
            const bool mentions_vanille = lowered.find("vanille") != std::string::npos;
            const bool mentions_program = lowered.find("program") != std::string::npos || lowered.find("tool") != std::string::npos;
            const bool mentions_this = lowered.find("this") != std::string::npos;

            if (asks_what && (mentions_vanille || mentions_program || mentions_this))
            {
                out_response = "Vanille is an external tool by Aori that lets you customize Roblox visuals and gameplay settings like ESP, aiming, and movement.";
                return true;
            }

            return false;
        }

        bool try_handle_ai_chat_command(const std::string& input, std::string& out_response)
        {
            const std::string lowered = to_lower_ascii(input);
            if (lowered.find("help") != std::string::npos || lowered.find("commands") != std::string::npos)
            {
                out_response = "I can enable/disable features, change keybinds, adjust colors, and tweak aimbot/silent aim settings. Try: \"enable aimbot\", \"change aimbot keybind to RMB\", or \"set box color to red\".";
                return true;
            }

            if (lowered.find("hello") != std::string::npos || lowered.find("hi") != std::string::npos)
            {
                out_response = "Hey! Tell me what to change in Vanille.";
                return true;
            }

            return false;
        }

        std::string trim_copy(const std::string& text)
        {
            const size_t start = text.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                return {};
            const size_t end = text.find_last_not_of(" \t\r\n");
            return text.substr(start, end - start + 1);
        }

    } // namespace

    void update()
    {
        if (g_pending && g_future.valid())
        {
            if (g_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                const ai_chat_response response = g_future.get();
                g_pending = false;

                if (!response.error.empty())
                {
                    g_error = response.error;
                }
                else if (!response.text.empty())
                {
                    g_messages.push_back({ false, response.text });
                    g_scroll_to_bottom = true;
                }
            }
        }
    }

    bool send_message(const std::string& input)
    {
        const std::string trimmed = trim_copy(input);
        if (trimmed.empty())
            return false;

        if (g_pending)
            return false;

        g_messages.push_back({ true, trimmed });
        g_scroll_to_bottom = true;

        std::string response;
        bool handled = false;

        if (handle_pending_appearance_value(trimmed, response))
            handled = true;
        else if (try_handle_ai_keybind_command(trimmed, response))
            handled = true;
        else if (try_handle_ai_config_command(trimmed, response))
            handled = true;
        else if (try_handle_ai_appearance_command(trimmed, response))
            handled = true;
        else if (try_handle_ai_feature_toggle_command(trimmed, response))
            handled = true;
        else if (try_handle_ai_identity_command(trimmed, response))
            handled = true;
        else if (try_handle_ai_program_info_command(trimmed, response))
            handled = true;
        else if (try_handle_ai_chat_command(trimmed, response))
            handled = true;

        if (!handled)
        {
            response = "I can help with Vanille settings. Try: \"enable ESP\", \"set box color to red\", or \"change aimbot keybind to RMB\".";
        }

        if (!response.empty())
        {
            g_messages.push_back({ false, response });
            g_scroll_to_bottom = true;
        }

        g_error.clear();
        return true;
    }

    const std::vector<chat_message>& messages()
    {
        return g_messages;
    }

    bool pending()
    {
        return g_pending;
    }

    const std::string& error()
    {
        return g_error;
    }

    bool consume_scroll_to_bottom()
    {
        const bool value = g_scroll_to_bottom;
        g_scroll_to_bottom = false;
        return value;
    }

    void clear_messages()
    {
        g_messages.clear();
    }

    void clear_error()
    {
        g_error.clear();
    }
}
