#include "roblox.h"

#include <windows.h>
#include <tlhelp32.h>

#include <unordered_set>

namespace
{
    BOOL CALLBACK collect_window_title(HWND hwnd, LPARAM param)
    {
        auto* ctx = reinterpret_cast<std::pair<DWORD, std::wstring*>*>(param);
        if (!ctx || !ctx->second)
            return TRUE;

        DWORD window_pid = 0;
        GetWindowThreadProcessId(hwnd, &window_pid);
        if (window_pid != ctx->first)
            return TRUE;
        if (!IsWindowVisible(hwnd))
            return TRUE;

        wchar_t title[512]{};
        const int length = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        if (length <= 0)
            return TRUE;

        if (ctx->second->empty() || ctx->second->size() < static_cast<size_t>(length))
            *ctx->second = title;

        return TRUE;
    }

    std::wstring query_main_window_title(DWORD pid)
    {
        std::wstring title;
        std::pair<DWORD, std::wstring*> ctx{ pid, &title };
        EnumWindows(collect_window_title, reinterpret_cast<LPARAM>(&ctx));
        if (title.empty())
            title = L"(no window title)";
        return title;
    }
}

namespace roblox
{
    std::vector<instance> enumerate_instances()
    {
        std::vector<instance> results;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return results;

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, L"RobloxPlayerBeta.exe") != 0)
                    continue;

                instance item;
                item.pid = entry.th32ProcessID;
                item.title = query_main_window_title(item.pid);
                results.push_back(std::move(item));
            } while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return results;
    }

    bool is_vanille_running()
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;

        bool found = false;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, L"vanille.exe") == 0)
                {
                    found = true;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return found;
    }
}
