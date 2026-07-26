#include "keybind.h"
#include "../../colors/colors.h"
#include <imgui/imgui_internal.h>
#include <cctype>
#include <string>
#include <algorithm>
#include <windows.h>
#include <cstddef>
#include "../widgets.h"

namespace
{
    constexpr int kMinVirtualKey = VK_BACK;
    constexpr int kMaxVirtualKey = VK_RMENU;

    const char* const key_names[] = {
        "Unknown", "LMB", "RMB", "CANCEL", "MMB", "XB1", "XB2", "Unknown",
        "BACK", "TAB", "Unknown", "Unknown", "CLEAR", "RETURN", "Unknown", "Unknown",
        "SHIFT", "CTRL", "MENU", "PAUSE", "CAPITAL", "KANA", "Unknown", "JUNJA",
        "FINAL", "KANJI", "Unknown", "ESC", "CONVERT", "NONCONVERT", "ACCEPT", "MODECHANGE",
        "SPACE", "PRIOR", "NEXT", "END", "HOME", "LEFT", "UP", "RIGHT", "DOWN", "SEL",
        "PRINT", "EXE", "SNAPSHOT", "INS", "DEL", "HELP", "0", "1", "2", "3",
        "4", "5", "6", "7", "8", "9", "Unknown", "Unknown", "Unknown", "Unknown",
        "Unknown", "Unknown", "Unknown", "A", "B", "C", "D", "E", "F", "G",
        "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q",
        "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "LWIN",
        "RWIN", "APPS", "Unknown", "SLEEP", "NUMPAD0", "NUMPAD1", "NUMPAD2", "NUMPAD3", "NUMPAD4", "NUMPAD5",
        "NUMPAD6", "NUMPAD7", "NUMPAD8", "NUMPAD9", "MULTIPLY", "ADD", "SEPARATOR", "SUBTRACT", "DECIMAL", "DIVIDE",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10",
        "F11", "F12", "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20",
        "F21", "F22", "F23", "F24", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown",
        "Unknown", "Unknown", "NUMLOCK", "SCROLL", "OEM_NEC_EQUAL", "OEM_FJ_MASSHOU", "OEM_FJ_TOUROKU", "OEM_FJ_LOYA", "OEM_FJ_ROYA", "Unknown",
        "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "Unknown", "LSHIFT", "RSHIFT", "LCONTROL",
        "RCONTROL", "LMENU", "RMENU"
    };

    inline ImVec4 scale_color(const ImVec4& color, float factor)
    {
        ImVec4 res = color;
        res.x = ImClamp(res.x * factor, 0.0f, 1.0f);
        res.y = ImClamp(res.y * factor, 0.0f, 1.0f);
        res.z = ImClamp(res.z * factor, 0.0f, 1.0f);
        return res;
    }

    inline ImRect shrink_rect(const ImRect& rect, float amount)
    {
        return ImRect(
            ImVec2(rect.Min.x + amount, rect.Min.y + amount),
            ImVec2(rect.Max.x - amount, rect.Max.y - amount)
        );
    }

    inline void draw_gradient_rect(ImDrawList* draw_list, const ImRect& rect, const ImVec4& top_col, const ImVec4& bottom_col)
    {
        draw_list->AddRectFilledMultiColor(
            rect.Min,
            rect.Max,
            ImGui::GetColorU32(bottom_col),
            ImGui::GetColorU32(bottom_col),
            ImGui::GetColorU32(top_col),
            ImGui::GetColorU32(top_col)
        );
    }
}

std::string c_keybind::get_key_name() const
{
    if (waiting_for_input)
        return "...";

    if (key < 0 || key >= IM_ARRAYSIZE(key_names))
        return "-";

    std::string tmp = key_names[key];
    if (tmp.empty() || tmp == "Unknown")
        return "-";

    std::transform(tmp.begin(), tmp.end(), tmp.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return tmp;
}

std::string c_keybind::get_name() const
{
    return name ? std::string(name) : std::string();
}

std::string c_keybind::get_type() const
{
    switch (type)
    {
    case TOGGLE: return "Toggle";
    case HOLD:   return "Hold";
    case ALWAYS: return "Always";
    default:     return "-";
    }
}

void c_keybind::update()
{
    const bool now_down = key && (GetAsyncKeyState(key) & 0x8000);

    switch (type)
    {
    case TOGGLE:
        if (now_down && !was_down)
            enabled = !enabled;
        break;
    case HOLD:
        enabled = now_down;
        break;
    case ALWAYS:
        enabled = true;
        break;
    }

    was_down = now_down;
}

bool c_keybind::set_key()
{
    ImGuiIO& io = ImGui::GetIO();

    if (!awaiting_release)
    {
        for (int i = 0; i < IM_ARRAYSIZE(io.MouseDown); ++i)
            if (io.MouseDown[i])
                return false;

        for (int vk = kMinVirtualKey; vk <= kMaxVirtualKey; ++vk)
            if (GetAsyncKeyState(vk) & 0x8000)
                return false;

        awaiting_release = true;
        return false;
    }

    for (int i = 0; i < IM_ARRAYSIZE(io.MouseClicked); ++i)
    {
        if (io.MouseClicked[i])
        {
            switch (i)
            {
            case 0: key = VK_LBUTTON; break;
            case 1: key = VK_RBUTTON; break;
            case 2: key = VK_MBUTTON; break;
            case 3: key = VK_XBUTTON1; break;
            case 4: key = VK_XBUTTON2; break;
            }
            awaiting_release = false;
            return true;
        }
    }

    for (int vk = kMinVirtualKey; vk <= kMaxVirtualKey; ++vk)
    {
        if (vk == VK_ESCAPE)
            continue;
        if (GetAsyncKeyState(vk) & 1)
        {
            key = vk;
            awaiting_release = false;
            return true;
        }
    }

    if (GetAsyncKeyState(VK_ESCAPE) & 1)
    {
        key = 0;
        awaiting_release = false;
        return true;
    }

    return false;
}

static void draw_popup_background(ImDrawList* draw_list, const ImRect& rect)
{
    draw_list->AddRectFilledMultiColor(
        rect.Min,
        rect.Max,
        ImGui::GetColorU32(c_colors::bottom_child_background),
        ImGui::GetColorU32(c_colors::bottom_child_background),
        ImGui::GetColorU32(c_colors::top_child_background),
        ImGui::GetColorU32(c_colors::top_child_background));
    draw_list->AddRect(rect.Min, rect.Max, ImGui::GetColorU32(c_colors::outter_border), c_colors::widget_rounding);
    ImRect inner = shrink_rect(rect, 1.0f);
    draw_list->AddRect(inner.Min, inner.Max, ImGui::GetColorU32(c_colors::main_border),
                       ImMax(0.0f, c_colors::widget_rounding - 1.0f));
}

bool c_widgets::keybind(const char* id, c_keybind& keybind, const ImVec2& size_arg)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window || window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = ImGui::GetStyle();

    const char* widget_id = (id && id[0] != '\0') ? id : (keybind.name ? keybind.name : "##keybind");

    std::string display_name = keybind.get_key_name();
    ImVec2 text_size = ImGui::CalcTextSize(display_name.c_str());

    ImVec2 size = size_arg;
    const float checkbox_height = ImGui::GetFrameHeight() * 0.825f;
    if (size.y <= 0.0f)
        size.y = checkbox_height;
    if (size.x <= 0.0f)
        size.x = 52.0f;
    size.x = ImClamp(size.x, 52.0f, window->WorkRect.GetWidth());

    ImRect anchor_rect = (g.LastItemData.ID != 0) ? g.LastItemData.Rect
        : ImRect(window->DC.CursorPos, window->DC.CursorPos + size);

    ImVec2 pos(window->WorkRect.Max.x - size.x, anchor_rect.Min.y + (anchor_rect.GetHeight() - size.y) * 0.5f);
    ImRect bb(pos, pos + size);

    ImGui::PushID(widget_id);
    const ImGuiID btn_id = window->GetID("keybind_btn");
    const float cursor_y_before_item = window->DC.CursorPos.y;
    ImGui::ItemSize(bb);
    ImGui::ItemAdd(bb, btn_id);
    window->DC.CursorPos.y = cursor_y_before_item;

    bool hovered = false;
    bool held = false;
    ImGui::ButtonBehavior(bb, btn_id, &hovered, &held, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool left_released = hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    bool right_released = hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right);

    if (left_released)
    {
        keybind.waiting_for_input = true;
        keybind.awaiting_release = false;
    }

    if (right_released)
    {
        keybind.menu_opened = true;
        ImGui::OpenPopup("keybind_menu");
    }

    if (keybind.waiting_for_input && keybind.set_key())
        keybind.waiting_for_input = false;

    if (!ImGui::IsPopupOpen("keybind_menu"))
        keybind.menu_opened = false;

    ImDrawList* draw_list = window->DrawList;

    ImVec4 fill_col = c_colors::surface_raised;
    if (held)
        fill_col = scale_color(fill_col, 0.92f);

    ImVec4 border_col = c_colors::main_border;
    if (keybind.waiting_for_input)
        border_col = c_colors::top_accent_color;
    else if (hovered)
        border_col = c_colors::accent_dim;

    draw_list->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(fill_col), c_colors::widget_rounding);
    draw_list->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(border_col), c_colors::widget_rounding);

    ImVec4 text_col = c_colors::white;
    if (keybind.waiting_for_input || hovered)
        text_col = c_colors::top_accent_color;
    if (held)
        text_col = scale_color(text_col, 0.9f);

    ImVec2 text_pos(
        bb.Min.x + (bb.GetWidth() - text_size.x) * 0.5f,
        bb.Min.y + (bb.GetHeight() - text_size.y) * 0.5f
    );
    draw_list->AddText(text_pos, ImGui::GetColorU32(text_col), display_name.c_str());

    const float popup_width = ImMax(size.x, 52.0f);
    if (keybind.menu_opened)
    {
        ImGui::SetNextWindowPos(ImVec2(bb.Min.x, bb.Max.y + 4.0f));
        ImGui::SetNextWindowSize(ImVec2(popup_width, 0.0f));
    }

    const bool popup_open = ImGui::IsPopupOpen("keybind_menu");
    if (popup_open)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    }

    if (ImGui::BeginPopup("keybind_menu", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        keybind.menu_opened = true;

        ImVec2 popup_pos = ImGui::GetWindowPos();
        ImVec2 popup_size = ImGui::GetWindowSize();
        ImRect popup_rect(popup_pos, popup_pos + popup_size);
        ImDrawList* popup_draw_list = ImGui::GetWindowDrawList();
        draw_popup_background(popup_draw_list, popup_rect);

        const ImVec4 popup_base_text = style.Colors[ImGuiCol_Text];
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * 1.5f, style.ItemSpacing.y * 0.3f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));

        auto popup_text_col = [&](bool selected, bool hovered_item) -> ImVec4
        {
            if (selected)
            {
                ImVec4 col = c_colors::top_accent_color;
                return hovered_item ? scale_color(col, 1.15f) : col;
            }
            ImVec4 base = popup_base_text;
            return hovered_item ? scale_color(base, 1.05f) : scale_color(base, 0.75f);
        };

        const char* entries[] = { "Toggle", "Hold", "Always" };
        const ImGuiSelectableFlags selectable_flags = ImGuiSelectableFlags_SpanAvailWidth | ImGuiSelectableFlags_DontClosePopups;
        const float row_height = ImGui::GetFontSize() + style.FramePadding.y * 0.8f;
        for (int i = 0; i < IM_ARRAYSIZE(entries); ++i)
        {
            bool selected = static_cast<int>(keybind.type) == i;
            if (ImGui::Selectable(entries[i], selected, selectable_flags, ImVec2(0.0f, row_height)))
            {
                keybind.type = static_cast<c_keybind::keybind_mode>(i);
            }

            ImRect item_bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            bool item_hovered = ImGui::IsItemHovered();
            ImVec2 item_text_pos(
                item_bb.Min.x + style.FramePadding.x,
                item_bb.Min.y + (item_bb.GetHeight() - ImGui::GetFontSize()) * 0.5f
            );

            popup_draw_list->AddText(item_text_pos, ImGui::GetColorU32(popup_text_col(selected, item_hovered)), entries[i]);
        }

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        ImGui::EndPopup();
    }
    else
    {
        keybind.menu_opened = false;
    }

    if (popup_open)
    {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    ImGui::PopID();
    return left_released;
}
