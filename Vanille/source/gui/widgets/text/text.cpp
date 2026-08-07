#include "../widgets.h"
#include "globals/globals_fixed.h"
#include <imgui/imgui_internal.h>
#include <cstdarg>
#include <cctype>

namespace
{
    void render_text(const ImVec4& color, const char* fmt, va_list args)
    {
        if (!fmt)
            return;

        char buffer[1024];
        ImFormatStringV(buffer, IM_ARRAYSIZE(buffer), fmt, args);

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(buffer);
        ImGui::PopStyleColor();
    }
}

namespace c_widgets
{
    void text(const char* fmt, ...)
    {
        if (!fmt)
            return;

        va_list args;
        va_start(args, fmt);
        render_text(ImGui::GetStyle().Colors[ImGuiCol_Text], fmt, args);
        va_end(args);
    }

    void text_accent(const char* fmt, ...)
    {
        if (!fmt)
            return;

        va_list args;
        va_start(args, fmt);
        render_text(c_colors::top_accent_color, fmt, args);
        va_end(args);
    }

    void text_colored(const ImVec4& color, const char* fmt, ...)
    {
        if (!fmt)
            return;

        va_list args;
        va_start(args, fmt);
        render_text(color, fmt, args);
        va_end(args);
    }

    void section_label(const char* fmt, ...)
    {
        if (!fmt)
            return;

        char buffer[256];
        va_list args;
        va_start(args, fmt);
        ImFormatStringV(buffer, IM_ARRAYSIZE(buffer), fmt, args);
        va_end(args);

        for (char* p = buffer; *p; ++p)
            *p = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));

        ImFont* font = c_fonts::ui_section ? c_fonts::ui_section : ImGui::GetFont();
        ImGui::PushFont(font);
        ImGui::PushStyleColor(ImGuiCol_Text, c_colors::text_muted);
        ImGui::TextUnformatted(buffer);
        ImGui::PopStyleColor();
        ImGui::PopFont();

        const ImVec2 text_min = ImGui::GetItemRectMin();
        const ImVec2 text_max = ImGui::GetItemRectMax();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        const float line_y = ImGui::GetCursorScreenPos().y;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(text_min.x, line_y),
            ImVec2(text_max.x, line_y),
            ImGui::GetColorU32(c_colors::main_border),
            1.0f);
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
    }
}
