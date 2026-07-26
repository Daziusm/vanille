#pragma once

#include <memory>
#include <string>

namespace lua_drawing
{
    struct vector2_value
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct color_value
    {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    enum class object_type
    {
        unknown,
        square,
        text,
        line
    };

    struct drawing_object
    {
        int id = 0;
        object_type type = object_type::unknown;
        bool removed = false;
        bool visible = true;
        color_value color;
        float transparency = 1.0f;
        float thickness = 1.0f;
        int z_index = 1;
        vector2_value position;
        vector2_value size{ 50.0f, 50.0f };
        bool filled = false;
        std::string text = "text";
        float text_size = 13.0f;
        bool center = false;
        vector2_value from;
        vector2_value to;
    };

    std::shared_ptr<drawing_object> create_object(const std::string& type_name);
    std::shared_ptr<drawing_object> find_object(int object_id);
    bool remove_object(int object_id);
    void clear_all();
    void render_all();
}
