#pragma once

#include <string>
#include <vector>

namespace lua_ui_bridge
{
    enum class widget_type
    {
        checkbox,
        slider,
        dropdown,
        multi_dropdown,
        keybind,
        colorpicker,
        button,
        input_text
    };

    enum class value_type
    {
        none,
        boolean,
        number,
        integer,
        string,
        color,
        keybind,
        boolean_list
    };

    struct color_value
    {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    struct keybind_value
    {
        int key = 0;
        int mode = 0;
        bool enabled = false;
    };

    struct widget_value
    {
        value_type type = value_type::none;
        bool bool_value = false;
        float number_value = 0.0f;
        int int_value = 0;
        std::string string_value;
        color_value color;
        keybind_value keybind;
        std::vector<bool> bool_list_value;
    };

    struct tab_descriptor
    {
        int id = 0;
        std::string name;
    };

    void clear_all();
    int create_tab(const std::string& name);
    bool remove_tab(int tab_id);
    bool set_tab_name(int tab_id, const std::string& name);
    bool find_tab_id_by_name(const std::string& name, int& out_tab_id);
    std::vector<tab_descriptor> get_tabs_snapshot();
    void clear_tab_widgets(int tab_id);

    bool add_checkbox_widget(int tab_id, const std::string& key, const std::string& label, bool default_value);
    bool add_slider_widget(int tab_id, const std::string& key, const std::string& label, float min_value, float max_value, float default_value);
    bool add_dropdown_widget(int tab_id, const std::string& key, const std::string& label, const std::vector<std::string>& options, int default_index);
    bool add_multi_dropdown_widget(int tab_id, const std::string& key, const std::string& label, const std::vector<std::string>& options, const std::vector<bool>& default_values);
    bool add_keybind_widget(int tab_id, const std::string& key, const std::string& label, int default_key, int default_mode);
    bool add_colorpicker_widget(int tab_id, const std::string& key, const std::string& label, const color_value& default_color);
    bool add_button_widget(int tab_id, const std::string& key, const std::string& label, int callback_ref);
    bool add_input_text_widget(int tab_id, const std::string& key, const std::string& label, const std::string& default_value);

    bool get_widget_value(int tab_id, const std::string& key, widget_value& out_value);
    bool set_widget_value(int tab_id, const std::string& key, const widget_value& in_value);

    void render_tab_widgets(int tab_id);

    std::vector<int> take_pending_button_callbacks();
    std::vector<int> take_released_callback_refs();
    std::vector<int> take_all_callback_refs();
}
