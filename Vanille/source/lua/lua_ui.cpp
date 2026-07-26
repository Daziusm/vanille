#include "lua/lua_ui.h"

#include "gui/widgets/widgets.h"
#include "gui/widgets/keybind/keybind.h"
#include <imgui/imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace lua_ui
{
    namespace
    {
        struct widget_definition
        {
            widget_type type = widget_type::checkbox;
            std::string key;
            std::string label;
            float min_value = 0.0f;
            float max_value = 1.0f;
            std::vector<std::string> options;
            int callback_ref = -1;
        };

        struct tab_state
        {
            int id = 0;
            std::string name;
            std::vector<widget_definition> widgets;
            std::unordered_map<std::string, std::size_t> widget_index_by_key;
            std::unordered_map<std::string, widget_value> values;
            std::unordered_map<std::string, c_keybind> runtime_keybinds;
        };

        std::mutex g_mutex;
        std::unordered_map<int, tab_state> g_tabs_by_id;
        std::vector<int> g_tab_order;
        int g_next_tab_id = 1;
        std::vector<int> g_pending_button_callbacks;
        std::vector<int> g_released_callback_refs;

        std::string trim_copy(const std::string& value)
        {
            const std::size_t begin = value.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
            {
                return std::string();
            }
            const std::size_t end = value.find_last_not_of(" \t\r\n");
            return value.substr(begin, end - begin + 1);
        }

        std::string lower_copy(const std::string& value)
        {
            std::string lowered = value;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
                });
            return lowered;
        }

        std::string normalize_name(const std::string& value)
        {
            const std::string trimmed = trim_copy(value);
            if (trimmed.empty())
            {
                return std::string();
            }
            return trimmed;
        }

        tab_state* find_tab_locked(int tab_id)
        {
            const auto iterator = g_tabs_by_id.find(tab_id);
            if (iterator == g_tabs_by_id.end())
            {
                return nullptr;
            }
            return &iterator->second;
        }

        void collect_tab_callback_refs(const tab_state& tab)
        {
            for (const widget_definition& widget : tab.widgets)
            {
                if (widget.type == widget_type::button && widget.callback_ref >= 0)
                {
                    g_released_callback_refs.push_back(widget.callback_ref);
                }
            }
        }

        std::string label_or_key(const widget_definition& widget)
        {
            if (!widget.label.empty())
            {
                return widget.label;
            }
            return widget.key;
        }

        widget_definition* upsert_widget(tab_state& tab, widget_type type, const std::string& key, const std::string& label)
        {
            if (key.empty())
            {
                return nullptr;
            }

            if (const auto iterator = tab.widget_index_by_key.find(key); iterator != tab.widget_index_by_key.end())
            {
                widget_definition& existing = tab.widgets[iterator->second];
                if (existing.type == widget_type::button && type != widget_type::button && existing.callback_ref >= 0)
                {
                    g_released_callback_refs.push_back(existing.callback_ref);
                    existing.callback_ref = -1;
                }
                existing.type = type;
                if (!label.empty())
                {
                    existing.label = label;
                }
                return &existing;
            }

            widget_definition widget{};
            widget.type = type;
            widget.key = key;
            widget.label = label.empty() ? key : label;
            const std::size_t index = tab.widgets.size();
            tab.widgets.push_back(widget);
            tab.widget_index_by_key[key] = index;
            return &tab.widgets[index];
        }

        widget_value& ensure_value(tab_state& tab, const std::string& key)
        {
            return tab.values[key];
        }

        bool ensure_type_or_default(widget_value& value, value_type type)
        {
            if (value.type == type)
            {
                return false;
            }
            value = widget_value{};
            value.type = type;
            return true;
        }

        int clamp_mode(int mode)
        {
            return std::clamp(mode, 0, 2);
        }
    }

    void clear_all()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_tabs_by_id.clear();
        g_tab_order.clear();
        g_pending_button_callbacks.clear();
        g_released_callback_refs.clear();
        g_next_tab_id = 1;
    }

    int create_tab(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        const std::string normalized = normalize_name(name);
        if (!normalized.empty())
        {
            const std::string normalized_lower = lower_copy(normalized);
            for (int tab_id : g_tab_order)
            {
                tab_state* existing = find_tab_locked(tab_id);
                if (existing && lower_copy(existing->name) == normalized_lower)
                {
                    return existing->id;
                }
            }
        }

        tab_state tab{};
        tab.id = g_next_tab_id++;
        tab.name = normalized.empty() ? ("lua_tab_" + std::to_string(tab.id)) : normalized;

        g_tab_order.push_back(tab.id);
        g_tabs_by_id.emplace(tab.id, std::move(tab));
        return tab.id;
    }

    bool remove_tab(int tab_id)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        auto iterator = g_tabs_by_id.find(tab_id);
        if (iterator == g_tabs_by_id.end())
        {
            return false;
        }

        collect_tab_callback_refs(iterator->second);
        g_tabs_by_id.erase(iterator);
        g_tab_order.erase(std::remove(g_tab_order.begin(), g_tab_order.end(), tab_id), g_tab_order.end());
        return true;
    }

    bool set_tab_name(int tab_id, const std::string& name)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return false;
        }

        const std::string normalized = normalize_name(name);
        if (normalized.empty())
        {
            return false;
        }

        tab->name = normalized;
        return true;
    }

    bool find_tab_id_by_name(const std::string& name, int& out_tab_id)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        const std::string target = lower_copy(normalize_name(name));
        if (target.empty())
        {
            return false;
        }

        for (int tab_id : g_tab_order)
        {
            tab_state* tab = find_tab_locked(tab_id);
            if (!tab)
            {
                continue;
            }

            if (lower_copy(tab->name) == target)
            {
                out_tab_id = tab_id;
                return true;
            }
        }

        return false;
    }

    std::vector<tab_descriptor> get_tabs_snapshot()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        std::vector<tab_descriptor> snapshot;
        snapshot.reserve(g_tab_order.size());
        for (int tab_id : g_tab_order)
        {
            tab_state* tab = find_tab_locked(tab_id);
            if (!tab)
            {
                continue;
            }
            snapshot.push_back(tab_descriptor{ tab->id, tab->name });
        }

        return snapshot;
    }

    void clear_tab_widgets(int tab_id)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return;
        }

        collect_tab_callback_refs(*tab);
        tab->widgets.clear();
        tab->widget_index_by_key.clear();
        tab->runtime_keybinds.clear();
    }

    bool add_checkbox_widget(int tab_id, const std::string& key, const std::string& label, bool default_value)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return false;
        }

        widget_definition* widget = upsert_widget(*tab, widget_type::checkbox, key, label);
        if (!widget)
        {
            return false;
        }

        widget_value& value = ensure_value(*tab, key);
        if (ensure_type_or_default(value, value_type::boolean))
        {
            value.bool_value = default_value;
        }

        return true;
    }

    bool add_slider_widget(int tab_id, const std::string& key, const std::string& label, float min_value, float max_value, float default_value)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return false;
        }

        if (max_value < min_value)
        {
            std::swap(min_value, max_value);
        }

        widget_definition* widget = upsert_widget(*tab, widget_type::slider, key, label);
        if (!widget)
        {
            return false;
        }

        widget->min_value = min_value;
        widget->max_value = max_value;

        widget_value& value = ensure_value(*tab, key);
        if (ensure_type_or_default(value, value_type::number))
        {
            value.number_value = std::clamp(default_value, min_value, max_value);
        }
        else
        {
            value.number_value = std::clamp(value.number_value, min_value, max_value);
        }

        return true;
    }

    bool add_dropdown_widget(int tab_id, const std::string& key, const std::string& label, const std::vector<std::string>& options, int default_index)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return false;
        }

        widget_definition* widget = upsert_widget(*tab, widget_type::dropdown, key, label);
        if (!widget)
        {
            return false;
        }

        widget->options = options;
        if (widget->options.empty())
        {
            widget->options.push_back("item");
        }

        widget_value& value = ensure_value(*tab, key);
        if (ensure_type_or_default(value, value_type::integer))
        {
            value.int_value = default_index;
        }

        value.int_value = std::clamp(value.int_value, 0, static_cast<int>(widget->options.size()) - 1);
        return true;
    }

    bool add_multi_dropdown_widget(int tab_id, const std::string& key, const std::string& label, const std::vector<std::string>& options, const std::vector<bool>& default_values)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return false;
        }

        widget_definition* widget = upsert_widget(*tab, widget_type::multi_dropdown, key, label);
        if (!widget)
        {
            return false;
        }

        widget->options = options;
        if (widget->options.empty())
        {
            widget->options.push_back("item");
        }

        widget_value& value = ensure_value(*tab, key);
        if (ensure_type_or_default(value, value_type::boolean_list))
        {
            value.bool_list_value.assign(widget->options.size(), false);
            for (std::size_t index = 0; index < value.bool_list_value.size() && index < default_values.size(); ++index)
            {
                value.bool_list_value[index] = default_values[index];
            }
        }

        value.bool_list_value.resize(widget->options.size(), false);
        return true;
    }

    bool add_keybind_widget(int tab_id, const std::string& key, const std::string& label, int default_key, int default_mode)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return false;
        }

        widget_definition* widget = upsert_widget(*tab, widget_type::keybind, key, label);
        if (!widget)
        {
            return false;
        }

        widget_value& value = ensure_value(*tab, key);
        if (ensure_type_or_default(value, value_type::keybind))
        {
            value.keybind.key = std::clamp(default_key, 0, 0xFF);
            value.keybind.mode = clamp_mode(default_mode);
            value.keybind.enabled = false;
        }

        auto runtime_iterator = tab->runtime_keybinds.find(key);
        if (runtime_iterator == tab->runtime_keybinds.end())
        {
            c_keybind runtime_bind("lua_keybind");
            runtime_bind.key = value.keybind.key;
            runtime_bind.type = static_cast<c_keybind::keybind_mode>(value.keybind.mode);
            runtime_bind.enabled = value.keybind.enabled;
            runtime_iterator = tab->runtime_keybinds.emplace(key, runtime_bind).first;
        }
        else
        {
            runtime_iterator->second.key = value.keybind.key;
            runtime_iterator->second.type = static_cast<c_keybind::keybind_mode>(value.keybind.mode);
            runtime_iterator->second.enabled = value.keybind.enabled;
        }

        return true;
    }

    bool add_colorpicker_widget(int tab_id, const std::string& key, const std::string& label, const color_value& default_color)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return false;
        }

        widget_definition* widget = upsert_widget(*tab, widget_type::colorpicker, key, label);
        if (!widget)
        {
            return false;
        }

        widget_value& value = ensure_value(*tab, key);
        if (ensure_type_or_default(value, value_type::color))
        {
            value.color = default_color;
        }

        value.color.r = std::clamp(value.color.r, 0.0f, 1.0f);
        value.color.g = std::clamp(value.color.g, 0.0f, 1.0f);
        value.color.b = std::clamp(value.color.b, 0.0f, 1.0f);
        value.color.a = std::clamp(value.color.a, 0.0f, 1.0f);
        return true;
    }

    bool add_button_widget(int tab_id, const std::string& key, const std::string& label, int callback_ref)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return false;
        }

        widget_definition* widget = upsert_widget(*tab, widget_type::button, key, label);
        if (!widget)
        {
            return false;
        }

        if (widget->callback_ref >= 0 && widget->callback_ref != callback_ref)
        {
            g_released_callback_refs.push_back(widget->callback_ref);
        }

        widget->callback_ref = callback_ref;
        return true;
    }

    bool add_input_text_widget(int tab_id, const std::string& key, const std::string& label, const std::string& default_value)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return false;
        }

        widget_definition* widget = upsert_widget(*tab, widget_type::input_text, key, label);
        if (!widget)
        {
            return false;
        }

        widget_value& value = ensure_value(*tab, key);
        if (ensure_type_or_default(value, value_type::string))
        {
            value.string_value = default_value;
        }

        if (value.string_value.size() > 4096)
        {
            value.string_value.resize(4096);
        }

        return true;
    }

    bool get_widget_value(int tab_id, const std::string& key, widget_value& out_value)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return false;
        }

        const auto iterator = tab->values.find(key);
        if (iterator == tab->values.end())
        {
            return false;
        }

        out_value = iterator->second;
        return true;
    }

    bool set_widget_value(int tab_id, const std::string& key, const widget_value& in_value)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return false;
        }

        if (key.empty())
        {
            return false;
        }

        tab->values[key] = in_value;

        if (in_value.type == value_type::keybind)
        {
            auto runtime_iterator = tab->runtime_keybinds.find(key);
            if (runtime_iterator == tab->runtime_keybinds.end())
            {
                c_keybind runtime_bind("lua_keybind");
                runtime_bind.key = std::clamp(in_value.keybind.key, 0, 0xFF);
                runtime_bind.type = static_cast<c_keybind::keybind_mode>(clamp_mode(in_value.keybind.mode));
                runtime_bind.enabled = in_value.keybind.enabled;
                tab->runtime_keybinds.emplace(key, runtime_bind);
            }
            else
            {
                runtime_iterator->second.key = std::clamp(in_value.keybind.key, 0, 0xFF);
                runtime_iterator->second.type = static_cast<c_keybind::keybind_mode>(clamp_mode(in_value.keybind.mode));
                runtime_iterator->second.enabled = in_value.keybind.enabled;
            }
        }

        return true;
    }

    void render_tab_widgets(int tab_id)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        tab_state* tab = find_tab_locked(tab_id);
        if (!tab)
        {
            return;
        }

        for (std::size_t widget_index = 0; widget_index < tab->widgets.size(); ++widget_index)
        {
            widget_definition& widget = tab->widgets[widget_index];
            widget_value& value = ensure_value(*tab, widget.key);
            const std::string display_label = label_or_key(widget);
            const bool previous_widget_is_checkbox = widget_index > 0 && tab->widgets[widget_index - 1].type == widget_type::checkbox;

            ImGui::PushID(tab->id);
            ImGui::PushID(widget.key.c_str());

            switch (widget.type)
            {
            case widget_type::checkbox:
            {
                if (ensure_type_or_default(value, value_type::boolean))
                {
                    value.bool_value = false;
                }
                c_widgets::checkbox(display_label.c_str(), &value.bool_value);
                break;
            }
            case widget_type::slider:
            {
                if (widget.max_value < widget.min_value)
                {
                    std::swap(widget.max_value, widget.min_value);
                }
                if (ensure_type_or_default(value, value_type::number))
                {
                    value.number_value = widget.min_value;
                }
                value.number_value = std::clamp(value.number_value, widget.min_value, widget.max_value);
                c_widgets::slider_float(display_label.c_str(), &value.number_value, widget.min_value, widget.max_value, "%.2f");
                break;
            }
            case widget_type::dropdown:
            {
                if (widget.options.empty())
                {
                    widget.options.push_back("item");
                }
                if (ensure_type_or_default(value, value_type::integer))
                {
                    value.int_value = 0;
                }

                value.int_value = std::clamp(value.int_value, 0, static_cast<int>(widget.options.size()) - 1);

                std::vector<const char*> option_ptrs;
                option_ptrs.reserve(widget.options.size());
                for (const std::string& option : widget.options)
                {
                    option_ptrs.push_back(option.c_str());
                }

                c_widgets::dropdown(display_label.c_str(), &value.int_value, option_ptrs.data(), static_cast<int>(option_ptrs.size()));
                break;
            }
            case widget_type::multi_dropdown:
            {
                if (widget.options.empty())
                {
                    widget.options.push_back("item");
                }
                if (ensure_type_or_default(value, value_type::boolean_list))
                {
                    value.bool_list_value.assign(widget.options.size(), false);
                }
                value.bool_list_value.resize(widget.options.size(), false);

                std::vector<const char*> option_ptrs;
                option_ptrs.reserve(widget.options.size());
                for (const std::string& option : widget.options)
                {
                    option_ptrs.push_back(option.c_str());
                }

                std::unique_ptr<bool[]> states = std::make_unique<bool[]>(widget.options.size());
                for (std::size_t index = 0; index < widget.options.size(); ++index)
                {
                    states[index] = value.bool_list_value[index];
                }

                c_widgets::multi_dropdown(display_label.c_str(), states.get(), option_ptrs.data(), static_cast<int>(widget.options.size()));
                for (std::size_t index = 0; index < widget.options.size(); ++index)
                {
                    value.bool_list_value[index] = states[index];
                }
                break;
            }
            case widget_type::keybind:
            {
                if (ensure_type_or_default(value, value_type::keybind))
                {
                    value.keybind.key = 0;
                    value.keybind.mode = 0;
                    value.keybind.enabled = false;
                }

                auto runtime_iterator = tab->runtime_keybinds.find(widget.key);
                if (runtime_iterator == tab->runtime_keybinds.end())
                {
                    c_keybind runtime_bind("lua_keybind");
                    runtime_bind.key = std::clamp(value.keybind.key, 0, 0xFF);
                    runtime_bind.type = static_cast<c_keybind::keybind_mode>(clamp_mode(value.keybind.mode));
                    runtime_bind.enabled = value.keybind.enabled;
                    runtime_iterator = tab->runtime_keybinds.emplace(widget.key, runtime_bind).first;
                }

                if (!previous_widget_is_checkbox)
                {
                    bool keybind_anchor_value = false;
                    ImGui::BeginDisabled();
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
                    c_widgets::checkbox("##lua_keybind_anchor_checkbox", &keybind_anchor_value);
                    ImGui::PopStyleVar();
                    ImGui::EndDisabled();
                }
                c_widgets::keybind(display_label.c_str(), runtime_iterator->second);

                value.keybind.key = runtime_iterator->second.key;
                value.keybind.mode = clamp_mode(static_cast<int>(runtime_iterator->second.type));
                value.keybind.enabled = runtime_iterator->second.enabled;
                break;
            }
            case widget_type::colorpicker:
            {
                if (ensure_type_or_default(value, value_type::color))
                {
                    value.color = color_value{};
                }

                ImVec4 color(value.color.r, value.color.g, value.color.b, value.color.a);
                c_widgets::colorpicker(display_label.c_str(), color, ImGui::GetFrameHeight() * 0.825f);
                value.color.r = std::clamp(color.x, 0.0f, 1.0f);
                value.color.g = std::clamp(color.y, 0.0f, 1.0f);
                value.color.b = std::clamp(color.z, 0.0f, 1.0f);
                value.color.a = std::clamp(color.w, 0.0f, 1.0f);
                break;
            }
            case widget_type::button:
            {
                if (c_widgets::button(display_label.c_str()))
                {
                    if (widget.callback_ref >= 0)
                    {
                        g_pending_button_callbacks.push_back(widget.callback_ref);
                    }
                }
                break;
            }
            case widget_type::input_text:
            {
                if (ensure_type_or_default(value, value_type::string))
                {
                    value.string_value.clear();
                }

                if (value.string_value.size() > 4096)
                {
                    value.string_value.resize(4096);
                }

                const std::size_t capacity = std::max<std::size_t>(256, value.string_value.size() + 128);
                std::vector<char> buffer(capacity, '\0');
                std::copy(value.string_value.begin(), value.string_value.end(), buffer.begin());

                if (c_widgets::input_text(display_label.c_str(), buffer.data(), buffer.size()))
                {
                    value.string_value.assign(buffer.data());
                }
                break;
            }
            }

            ImGui::PopID();
            ImGui::PopID();
        }
    }

    std::vector<int> take_pending_button_callbacks()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<int> callbacks;
        callbacks.swap(g_pending_button_callbacks);
        return callbacks;
    }

    std::vector<int> take_released_callback_refs()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<int> refs;
        refs.swap(g_released_callback_refs);
        return refs;
    }

    std::vector<int> take_all_callback_refs()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        std::unordered_set<int> unique_refs;
        for (const auto& pair : g_tabs_by_id)
        {
            for (const widget_definition& widget : pair.second.widgets)
            {
                if (widget.type == widget_type::button && widget.callback_ref >= 0)
                {
                    unique_refs.insert(widget.callback_ref);
                }
            }
        }

        for (int callback_ref : g_pending_button_callbacks)
        {
            if (callback_ref >= 0)
            {
                unique_refs.insert(callback_ref);
            }
        }

        for (int callback_ref : g_released_callback_refs)
        {
            if (callback_ref >= 0)
            {
                unique_refs.insert(callback_ref);
            }
        }

        std::vector<int> refs;
        refs.reserve(unique_refs.size());
        for (int callback_ref : unique_refs)
        {
            refs.push_back(callback_ref);
        }

        g_pending_button_callbacks.clear();
        g_released_callback_refs.clear();
        return refs;
    }
}
