#include "utils/console.h"

#include <cwchar>
#include <windows.h>

namespace console_core
{
    void console_service::initialize()
    {
        if (!GetConsoleWindow())
        {
            AllocConsole();

            FILE* stream = nullptr;
            freopen_s(&stream, "CONOUT$", "w", stdout);
            freopen_s(&stream, "CONOUT$", "w", stderr);
            freopen_s(&stream, "CONIN$", "r", stdin);
            SetConsoleTitleW(L"Vanille");
        }

        const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output == INVALID_HANDLE_VALUE)
        {
            return;
        }

        CONSOLE_FONT_INFOEX font_info{};
        font_info.cbSize = sizeof(font_info);
        font_info.dwFontSize.X = 6;
        font_info.dwFontSize.Y = 13;
        wcscpy_s(font_info.FaceName, L"Consolas");
        SetCurrentConsoleFontEx(output, FALSE, &font_info);

        SMALL_RECT window_rect{ 0, 0, 1, 1 };
        SetConsoleWindowInfo(output, TRUE, &window_rect);

        const COORD buffer_size{ 80, 30 };
        SetConsoleScreenBufferSize(output, buffer_size);

        window_rect = { 0, 0, static_cast<SHORT>(buffer_size.X - 1), static_cast<SHORT>(buffer_size.Y - 1) };
        SetConsoleWindowInfo(output, TRUE, &window_rect);

        const HWND window = GetConsoleWindow();
        if (window)
        {
            SetWindowPos(window, nullptr, 60, 60, 520, 320, SWP_SHOWWINDOW);
        }
    }
}
