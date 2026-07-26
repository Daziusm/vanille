#include "lua/lua_drawing.h"

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui.h>

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lua_drawing
{
    namespace
    {
        std::mutex g_mutex;
        std::unordered_map<int, std::shared_ptr<drawing_object>> g_objects;
        int g_next_id = 1;

        std::string normalize_name(const std::string& value)
        {
            std::string normalized;
            normalized.reserve(value.size());
            for (unsigned char character : value)
            {
                if (std::isalnum(character))
                {
                    normalized.push_back(static_cast<char>(std::tolower(character)));
                }
            }
            return normalized;
        }

        object_type parse_type(const std::string& type_name)
        {
            const std::string normalized = normalize_name(type_name);
            if (normalized == "square")
            {
                return object_type::square;
            }
            if (normalized == "text")
            {
                return object_type::text;
            }
            if (normalized == "line")
            {
                return object_type::line;
            }
            return object_type::unknown;
        }

        ImU32 to_color_u32(const color_value& color, float transparency)
        {
            const float alpha = std::clamp(color.a * std::clamp(transparency, 0.0f, 1.0f), 0.0f, 1.0f);
            const ImVec4 output(
                std::clamp(color.r, 0.0f, 1.0f),
                std::clamp(color.g, 0.0f, 1.0f),
                std::clamp(color.b, 0.0f, 1.0f),
                alpha);
            return ImGui::ColorConvertFloat4ToU32(output);
        }
    }

    std::shared_ptr<drawing_object> create_object(const std::string& type_name)
    {
        const object_type type = parse_type(type_name);
        if (type == object_type::unknown)
        {
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        const int object_id = g_next_id++;

        std::shared_ptr<drawing_object> object = std::make_shared<drawing_object>();
        object->id = object_id;
        object->type = type;

        if (type == object_type::text)
        {
            object->size = vector2_value{ 0.0f, 0.0f };
        }

        g_objects[object_id] = object;
        return object;
    }

    std::shared_ptr<drawing_object> find_object(int object_id)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto iterator = g_objects.find(object_id);
        if (iterator == g_objects.end())
        {
            return nullptr;
        }
        return iterator->second;
    }

    bool remove_object(int object_id)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto iterator = g_objects.find(object_id);
        if (iterator == g_objects.end())
        {
            return false;
        }

        iterator->second->removed = true;
        g_objects.erase(iterator);
        return true;
    }

    void clear_all()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_objects.clear();
        g_next_id = 1;
    }

    void render_all()
    {
        std::vector<std::shared_ptr<drawing_object>> snapshot;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            snapshot.reserve(g_objects.size());
            for (const auto& pair : g_objects)
            {
                if (pair.second)
                {
                    snapshot.push_back(pair.second);
                }
            }
        }

        if (snapshot.empty())
        {
            return;
        }

        std::sort(snapshot.begin(), snapshot.end(), [](const std::shared_ptr<drawing_object>& left, const std::shared_ptr<drawing_object>& right)
            {
                if (left->z_index == right->z_index)
                {
                    return left->id < right->id;
                }
                return left->z_index < right->z_index;
            });

        ImDrawList* draw_list = ImGui::GetForegroundDrawList();
        if (!draw_list)
        {
            return;
        }

        for (const std::shared_ptr<drawing_object>& object : snapshot)
        {
            if (!object || object->removed || !object->visible)
            {
                continue;
            }

            const ImU32 color = to_color_u32(object->color, object->transparency);
            const float thickness = std::max(1.0f, object->thickness);

            if (object->type == object_type::square)
            {
                const ImVec2 min(object->position.x, object->position.y);
                const ImVec2 max(object->position.x + object->size.x, object->position.y + object->size.y);
                if (object->filled)
                {
                    draw_list->AddRectFilled(min, max, color);
                }
                else
                {
                    draw_list->AddRect(min, max, color, 0.0f, 0, thickness);
                }
            }
            else if (object->type == object_type::text)
            {
                ImFont* font = ImGui::GetFont();
                float font_size = object->text_size > 0.0f ? object->text_size : ImGui::GetFontSize();
                ImVec2 position(object->position.x, object->position.y);
                if (object->center)
                {
                    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, object->text.c_str());
                    position.x -= text_size.x * 0.5f;
                    position.y -= text_size.y * 0.5f;
                }
                draw_list->AddText(font, font_size, position, color, object->text.c_str());
            }
            else if (object->type == object_type::line)
            {
                draw_list->AddLine(ImVec2(object->from.x, object->from.y), ImVec2(object->to.x, object->to.y), color, thickness);
            }
        }
    }
}
