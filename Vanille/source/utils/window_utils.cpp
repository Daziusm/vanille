#include "utils/window_utils.h"

namespace
{
    struct window_query_data
    {
        DWORD process_id;
        HWND window;
    };

    BOOL CALLBACK enum_windows_callback(HWND hwnd, LPARAM lparam)
    {
        auto* data = reinterpret_cast<window_query_data*>(lparam);
        DWORD window_process_id = 0;
        GetWindowThreadProcessId(hwnd, &window_process_id);
        const bool is_target_process = window_process_id == data->process_id;
        const bool has_owner = GetWindow(hwnd, GW_OWNER) != nullptr;
        const bool visible = IsWindowVisible(hwnd) != 0;
        if (is_target_process && !has_owner && visible)
        {
            data->window = hwnd;
            return FALSE;
        }
        return TRUE;
    }
}

namespace window_utils
{
    HWND find_main_window(DWORD process_id)
    {
        window_query_data data{ process_id, nullptr };
        EnumWindows(enum_windows_callback, reinterpret_cast<LPARAM>(&data));
        return data.window;
    }
}
