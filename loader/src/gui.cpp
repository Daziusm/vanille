#include "gui.h"

#include "downloader.h"
#include "payload.h"
#include "roblox.h"
#include "settings.h"

#include <dwmapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
    constexpr UINT WM_LOADER_STATUS = WM_APP + 1;
    constexpr UINT WM_LOADER_TASK_DONE = WM_APP + 2;
    constexpr UINT TIMER_REFRESH = 1;
    constexpr UINT TIMER_AUTOLOAD = 2;

    constexpr int IDC_INSTALL_PATH = 1005;
    constexpr int IDC_REPO_URL = 1008;
    constexpr int IDC_BRANCH_EDIT = 1009;
    constexpr int IDC_SOURCE_PATH = 1011;

    constexpr int kRainbowHeight = 3;
    constexpr size_t kMaxLogLines = 8;

    constexpr COLORREF kBg = RGB(18, 18, 18);
    constexpr COLORREF kBgGrid = RGB(26, 26, 28);
    constexpr COLORREF kPanel = RGB(25, 25, 28);
    constexpr COLORREF kPanelInset = RGB(20, 20, 22);
    constexpr COLORREF kBorderOuter = RGB(72, 72, 78);
    constexpr COLORREF kBorderInner = RGB(42, 42, 48);
    constexpr COLORREF kText = RGB(210, 210, 215);
    constexpr COLORREF kMuted = RGB(108, 108, 114);
    constexpr COLORREF kGreen = RGB(126, 179, 86);
    constexpr COLORREF kWhite = RGB(240, 240, 245);
    constexpr COLORREF kButtonFill = RGB(35, 35, 38);
    constexpr COLORREF kButtonBorder = RGB(60, 60, 65);
    constexpr COLORREF kButtonHover = RGB(46, 46, 52);
    constexpr COLORREF kButtonPressed = RGB(30, 30, 34);
    constexpr COLORREF kEditFill = RGB(28, 28, 31);

    enum class HitTarget
    {
        None,
        Refresh,
        Load,
        Exit,
        Download,
        BrowseInstall,
        BrowseSource,
        AutoLoad,
        ProcessRow
    };

    struct Rect
    {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;

        RECT win32() const { return RECT{ x, y, x + w, y + h }; }
        bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    };

    struct StatusLine
    {
        std::wstring text;
        bool highlight = false;
    };

    struct Layout
    {
        Rect roblox_panel;
        Rect options_panel;
        Rect status_panel;
        Rect sources_panel;
        Rect roblox_icon;
        Rect refresh;
        Rect auto_load;
        Rect browse_install;
        Rect load;
        Rect exit_btn;
        Rect browse_source;
        Rect download;
        Rect install_edit;
        Rect repo_edit;
        Rect branch_edit;
        Rect source_edit;
        std::vector<Rect> process_rows;
    };

    HINSTANCE g_instance = nullptr;
    HWND g_window = nullptr;
    HFONT g_font = nullptr;
    HFONT g_font_semibold = nullptr;
    settings::loader_settings g_settings;
    std::atomic<bool> g_task_running{ false };

    HWND g_install_path = nullptr;
    HWND g_repo_url = nullptr;
    HWND g_branch_edit = nullptr;
    HWND g_source_path = nullptr;

    std::vector<roblox::instance> g_processes;
    int g_selected_process = -1;
    HitTarget g_hover = HitTarget::None;
    HitTarget g_pressed = HitTarget::None;
    int g_hovered_process = -1;
    std::vector<StatusLine> g_status_log;
    Layout g_layout;

    bool point_in_rect(const Rect& rect, int x, int y)
    {
        return rect.contains(x, y);
    }

    void append_status(const std::wstring& text, bool highlight = false)
    {
        g_status_log.push_back({ text, highlight });
        while (g_status_log.size() > kMaxLogLines)
            g_status_log.erase(g_status_log.begin());
        InvalidateRect(g_window, nullptr, FALSE);
    }

    void set_status(const std::wstring& text, bool highlight = false)
    {
        append_status(text, highlight);
    }

    std::wstring read_control_text(HWND control)
    {
        if (!control)
            return {};
        const int length = GetWindowTextLengthW(control);
        if (length <= 0)
            return {};
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(control, text.data(), length + 1);
        text.resize(length);
        return text;
    }

    void write_control_text(HWND control, const std::wstring& text)
    {
        if (control)
            SetWindowTextW(control, text.c_str());
    }

    void fill_rect(HDC dc, const Rect& rect, COLORREF color)
    {
        HBRUSH brush = CreateSolidBrush(color);
        const RECT r = rect.win32();
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    }

    void frame_rect(HDC dc, const Rect& rect, COLORREF color)
    {
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, rect.x, rect.y, rect.x + rect.w, rect.y + rect.h);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }

    void draw_text(HDC dc, const wchar_t* text, const Rect& rect, COLORREF color, HFONT font, DWORD format)
    {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        SelectObject(dc, font);
        RECT r = rect.win32();
        DrawTextW(dc, text, -1, &r, format);
    }

    COLORREF lerp_color(COLORREF a, COLORREF b, float t)
    {
        const int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
        const int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
        return RGB(
            ar + static_cast<int>((br - ar) * t),
            ag + static_cast<int>((bg - ag) * t),
            ab + static_cast<int>((bb - ab) * t));
    }

    void draw_grid_texture(HDC dc, int width, int height)
    {
        fill_rect(dc, { 0, 0, width, height }, kBg);

        HPEN pen = CreatePen(PS_SOLID, 1, kBgGrid);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        for (int x = 0; x < width; x += 4)
        {
            MoveToEx(dc, x, kRainbowHeight, nullptr);
            LineTo(dc, x, height);
        }
        for (int y = kRainbowHeight; y < height; y += 4)
        {
            MoveToEx(dc, 0, y, nullptr);
            LineTo(dc, width, y);
        }
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }

    void draw_rainbow_bar(HDC dc, int width)
    {
        static const COLORREF stops[] = {
            RGB(255, 0, 0),
            RGB(255, 127, 0),
            RGB(255, 255, 0),
            RGB(0, 200, 0),
            RGB(0, 120, 255),
            RGB(148, 0, 211)
        };

        for (int x = 0; x < width; ++x)
        {
            const float t = static_cast<float>(x) / static_cast<float>(width > 1 ? width - 1 : 1) * 5.0f;
            const int segment = static_cast<int>(t);
            const float local = t - static_cast<float>(segment);
            const COLORREF c = lerp_color(stops[segment], stops[segment + 1 > 5 ? 5 : segment + 1], local);
            fill_rect(dc, { x, 0, 1, kRainbowHeight }, c);
        }
    }

    void draw_group_box(HDC dc, const Rect& rect, const wchar_t* label)
    {
        fill_rect(dc, rect, kPanelInset);
        frame_rect(dc, rect, kBorderOuter);
        frame_rect(dc, { rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2 }, kBorderInner);

        SelectObject(dc, g_font);
        SIZE size{};
        GetTextExtentPoint32W(dc, label, static_cast<int>(wcslen(label)), &size);

        const int label_w = size.cx + 10;
        const int label_x = rect.x + (rect.w - label_w) / 2;
        const int label_y = rect.y - size.cy / 2;
        fill_rect(dc, { label_x, label_y, label_w, size.cy }, kBg);
        draw_text(dc, label, { label_x, label_y, label_w, size.cy }, kMuted, g_font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void draw_flat_button(HDC dc, const Rect& rect, const wchar_t* label, bool hovered, bool pressed, bool disabled)
    {
        COLORREF fill = kButtonFill;
        COLORREF text = kText;
        if (disabled)
        {
            fill = RGB(30, 30, 33);
            text = kMuted;
        }
        else if (pressed)
        {
            fill = kButtonPressed;
        }
        else if (hovered)
        {
            fill = kButtonHover;
        }

        fill_rect(dc, rect, fill);
        frame_rect(dc, rect, kButtonBorder);
        draw_text(dc, label, rect, text, g_font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void draw_edit_field_bg(HDC dc, const Rect& rect)
    {
        fill_rect(dc, rect, kEditFill);
        frame_rect(dc, rect, kBorderInner);
    }

    void draw_roblox_icon(HDC dc, const Rect& rect)
    {
        fill_rect(dc, rect, kPanel);
        frame_rect(dc, rect, kBorderInner);
        draw_text(dc, L"RBX", rect, kGreen, g_font_semibold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    bool browse_for_folder(const wchar_t* title, std::wstring& in_out_path)
    {
        wchar_t display_name[MAX_PATH] = L"";
        BROWSEINFOW info{};
        info.hwndOwner = g_window;
        info.pszDisplayName = display_name;
        info.lpszTitle = title;
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&info);
        if (!pidl)
            return false;

        wchar_t selected[MAX_PATH]{};
        const bool ok = SHGetPathFromIDListW(pidl, selected);
        CoTaskMemFree(pidl);
        if (!ok)
            return false;

        in_out_path = selected;
        return true;
    }

    void save_settings_from_ui()
    {
        g_settings.install_path = read_control_text(g_install_path);
        g_settings.source_path = read_control_text(g_source_path);
        g_settings.repo_url = read_control_text(g_repo_url);
        g_settings.branch = read_control_text(g_branch_edit);
        settings::save(g_settings);
    }

    void refresh_process_list()
    {
        g_processes = roblox::enumerate_instances();
        if (g_selected_process >= static_cast<int>(g_processes.size()))
            g_selected_process = static_cast<int>(g_processes.size()) - 1;
        if (g_selected_process < 0 && !g_processes.empty())
            g_selected_process = 0;

        if (g_processes.empty())
            set_status(L"No Roblox instances found.");
        else
            set_status(L"Found " + std::to_wstring(g_processes.size()) + L" Roblox instance(s).", true);

        InvalidateRect(g_window, nullptr, FALSE);
    }

    std::uint32_t selected_pid()
    {
        if (g_selected_process < 0 || g_selected_process >= static_cast<int>(g_processes.size()))
            return 0;
        return g_processes[static_cast<size_t>(g_selected_process)].pid;
    }

    void perform_load()
    {
        if (g_task_running.exchange(true))
        {
            set_status(L"Another task is already running...");
            return;
        }

        save_settings_from_ui();

        const std::uint32_t pid = selected_pid();
        if (pid == 0)
        {
            set_status(L"Select a Roblox process before loading.", true);
            g_task_running = false;
            return;
        }

        set_status(L"Preparing load for PID " + std::to_wstring(pid) + L"...", true);

        std::thread([install_path = g_settings.install_path, pid]() {
            std::wstring runtime_dir;
            const bool installed = payload::ensure_installed(runtime_dir, install_path);
            bool launched = false;
            if (installed)
                launched = payload::launch_vanille(runtime_dir);

            const std::wstring message = launched
                ? L"Loaded Vanille for Roblox PID " + std::to_wstring(pid) + L"."
                : L"Failed to load Vanille.";

            PostMessageW(g_window, WM_LOADER_TASK_DONE, launched ? 1 : 0, reinterpret_cast<LPARAM>(new std::wstring(message)));
        }).detach();
    }

    void perform_source_download()
    {
        if (g_task_running.exchange(true))
        {
            set_status(L"Another task is already running...");
            return;
        }

        save_settings_from_ui();
        set_status(L"Downloading source from GitHub...", true);

        std::thread([repo = g_settings.repo_url, branch = g_settings.branch, destination = g_settings.source_path]() {
            std::wstring error;
            const bool ok = downloader::install_sources(repo, branch, destination, error);
            auto* message = new std::wstring(ok ? L"Source installed to " + destination : error);
            PostMessageW(g_window, WM_LOADER_TASK_DONE, ok ? 1 : 0, reinterpret_cast<LPARAM>(message));
        }).detach();
    }

    void try_auto_load()
    {
        if (!g_settings.auto_load)
            return;
        if (roblox::is_vanille_running())
            return;
        if (g_processes.empty())
            return;
        if (g_task_running.load())
            return;

        perform_load();
    }

    void layout_ui(int width, int height)
    {
        const int pad = 10;
        const int gap = 8;
        const int top_y = kRainbowHeight + pad;
        const int panel_h = 158;
        const int left_w = (width - pad * 2 - gap) * 47 / 100;
        const int right_w = width - pad * 2 - gap - left_w;

        g_layout.roblox_panel = { pad, top_y, left_w, panel_h };
        g_layout.options_panel = { pad + left_w + gap, top_y, right_w, panel_h };

        const int status_y = top_y + panel_h + gap;
        const int status_h = 70;
        g_layout.status_panel = { pad, status_y, width - pad * 2, status_h };

        const int sources_y = status_y + status_h + gap;
        g_layout.sources_panel = { pad, sources_y, width - pad * 2, height - sources_y - pad };

        const Rect& rbx = g_layout.roblox_panel;
        g_layout.roblox_icon = { rbx.x + 10, rbx.y + 22, 42, 42 };

        const Rect& opt = g_layout.options_panel;
        int ox = opt.x + 10;
        int oy = opt.y + 20;
        const int btn_w = opt.w - 20;
        g_layout.load = { ox, oy, btn_w, 28 };
        oy += 32;
        g_layout.refresh = { ox, oy, btn_w, 28 };
        oy += 32;
        g_layout.exit_btn = { ox, oy, btn_w, 28 };
        oy += 34;
        g_layout.auto_load = { ox, oy, btn_w, 16 };
        oy += 20;
        g_layout.install_edit = { ox, oy, btn_w - 32, 22 };
        g_layout.browse_install = { ox + btn_w - 28, oy, 28, 22 };

        const Rect& src = g_layout.sources_panel;
        int sx = src.x + 10;
        int sy = src.y + 16;
        const int sw = src.w - 20;
        g_layout.repo_edit = { sx, sy, sw, 22 };
        sy += 26;
        g_layout.branch_edit = { sx, sy, 72, 22 };
        g_layout.download = { sx + 78, sy, sw - 78, 22 };
        sy += 26;
        g_layout.source_edit = { sx, sy, sw - 32, 22 };
        g_layout.browse_source = { sx + sw - 28, sy, 28, 22 };

        if (g_install_path)
        {
            MoveWindow(g_install_path, g_layout.install_edit.x + 4, g_layout.install_edit.y + 3, g_layout.install_edit.w - 8, 16, TRUE);
            MoveWindow(g_repo_url, g_layout.repo_edit.x + 4, g_layout.repo_edit.y + 3, g_layout.repo_edit.w - 8, 16, TRUE);
            MoveWindow(g_branch_edit, g_layout.branch_edit.x + 4, g_layout.branch_edit.y + 3, g_layout.branch_edit.w - 8, 16, TRUE);
            MoveWindow(g_source_path, g_layout.source_edit.x + 4, g_layout.source_edit.y + 3, g_layout.source_edit.w - 8, 16, TRUE);
        }

        (void)height;
    }

    HWND create_dark_edit(int id, const std::wstring& text)
    {
        HWND edit = CreateWindowExW(
            0,
            L"EDIT",
            text.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0,
            0,
            100,
            20,
            g_window,
            reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
            g_instance,
            nullptr);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        return edit;
    }

    void paint_ui(HDC dc, const RECT& client)
    {
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        layout_ui(width, height);

        draw_grid_texture(dc, width, height);
        draw_rainbow_bar(dc, width);

        frame_rect(dc, { 8, kRainbowHeight + 7, width - 16, height - kRainbowHeight - 14 }, kBorderOuter);
        frame_rect(dc, { 9, kRainbowHeight + 8, width - 18, height - kRainbowHeight - 16 }, kBorderInner);

        draw_group_box(dc, g_layout.roblox_panel, L"Roblox");
        draw_group_box(dc, g_layout.options_panel, L"Options");
        draw_group_box(dc, g_layout.status_panel, L"Status");
        draw_group_box(dc, g_layout.sources_panel, L"Sources");

        draw_roblox_icon(dc, g_layout.roblox_icon);

        g_layout.process_rows.clear();
        const int list_x = g_layout.roblox_icon.x + g_layout.roblox_icon.w + 8;
        const int list_w = g_layout.roblox_panel.x + g_layout.roblox_panel.w - list_x - 8;
        int row_y = g_layout.roblox_panel.y + 20;

        if (g_processes.empty())
        {
            draw_text(
                dc,
                L"Waiting for RobloxPlayerBeta.exe...",
                { list_x, row_y, list_w, g_layout.roblox_panel.h - 28 },
                kMuted,
                g_font,
                DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
        else
        {
            for (int i = 0; i < static_cast<int>(g_processes.size()); ++i)
            {
                Rect row{ list_x, row_y, list_w, 34 };
                g_layout.process_rows.push_back(row);

                const bool selected = (i == g_selected_process);
                const bool hovered = (i == g_hovered_process);
                if (selected)
                    fill_rect(dc, row, RGB(32, 36, 30));
                else if (hovered)
                    fill_rect(dc, row, RGB(28, 28, 32));

                if (selected)
                    fill_rect(dc, { row.x, row.y + 5, 3, row.h - 10 }, kGreen);

                wchar_t pid_text[32]{};
                swprintf_s(pid_text, L"PID %lu", g_processes[static_cast<size_t>(i)].pid);
                draw_text(dc, pid_text, { row.x + 10, row.y, row.w - 10, 16 }, kWhite, g_font_semibold, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                draw_text(
                    dc,
                    g_processes[static_cast<size_t>(i)].title.c_str(),
                    { row.x + 10, row.y + 16, row.w - 10, 16 },
                    selected ? kGreen : kMuted,
                    g_font,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                row_y += 36;
            }
        }

        const Rect toggle{ g_layout.auto_load.x, g_layout.auto_load.y, 14, 14 };
        fill_rect(dc, toggle, g_settings.auto_load ? kGreen : kPanelInset);
        frame_rect(dc, toggle, g_settings.auto_load ? kGreen : kBorderInner);
        if (g_settings.auto_load)
            draw_text(dc, L"x", toggle, kBg, g_font_semibold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        draw_text(
            dc,
            L"Auto-load when Roblox opens",
            { toggle.x + 20, toggle.y - 1, g_layout.auto_load.w - 20, 16 },
            kText,
            g_font,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        draw_edit_field_bg(dc, g_layout.install_edit);
        draw_flat_button(dc, g_layout.browse_install, L"...", g_hover == HitTarget::BrowseInstall, g_pressed == HitTarget::BrowseInstall, false);

        draw_flat_button(dc, g_layout.load, L"Load", g_hover == HitTarget::Load, g_pressed == HitTarget::Load, g_task_running.load());
        draw_flat_button(dc, g_layout.refresh, L"Refresh", g_hover == HitTarget::Refresh, g_pressed == HitTarget::Refresh, false);
        draw_flat_button(dc, g_layout.exit_btn, L"Exit", g_hover == HitTarget::Exit, g_pressed == HitTarget::Exit, false);

        draw_text(dc, L"Repository", { g_layout.repo_edit.x, g_layout.repo_edit.y - 14, 80, 12 }, kMuted, g_font, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_edit_field_bg(dc, g_layout.repo_edit);

        draw_text(dc, L"Branch", { g_layout.branch_edit.x, g_layout.branch_edit.y - 14, 50, 12 }, kMuted, g_font, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_edit_field_bg(dc, g_layout.branch_edit);
        draw_flat_button(dc, g_layout.download, L"Download", g_hover == HitTarget::Download, g_pressed == HitTarget::Download, g_task_running.load());

        draw_text(dc, L"Source path", { g_layout.source_edit.x, g_layout.source_edit.y - 14, 80, 12 }, kMuted, g_font, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_edit_field_bg(dc, g_layout.source_edit);
        draw_flat_button(dc, g_layout.browse_source, L"...", g_hover == HitTarget::BrowseSource, g_pressed == HitTarget::BrowseSource, false);

        int line_y = g_layout.status_panel.y + 18;
        for (const StatusLine& line : g_status_log)
        {
            draw_text(
                dc,
                line.text.c_str(),
                { g_layout.status_panel.x + 10, line_y, g_layout.status_panel.w - 20, 15 },
                line.highlight ? kGreen : kText,
                g_font,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            line_y += 15;
        }
    }

    HitTarget hit_test(int x, int y)
    {
        if (point_in_rect(g_layout.refresh, x, y))
            return HitTarget::Refresh;
        if (point_in_rect(g_layout.load, x, y))
            return HitTarget::Load;
        if (point_in_rect(g_layout.exit_btn, x, y))
            return HitTarget::Exit;
        if (point_in_rect(g_layout.download, x, y))
            return HitTarget::Download;
        if (point_in_rect(g_layout.browse_install, x, y))
            return HitTarget::BrowseInstall;
        if (point_in_rect(g_layout.browse_source, x, y))
            return HitTarget::BrowseSource;
        if (point_in_rect(g_layout.auto_load, x, y))
            return HitTarget::AutoLoad;

        for (int i = 0; i < static_cast<int>(g_layout.process_rows.size()); ++i)
        {
            if (point_in_rect(g_layout.process_rows[static_cast<size_t>(i)], x, y))
                return HitTarget::ProcessRow;
        }
        return HitTarget::None;
    }

    void create_ui()
    {
        g_install_path = create_dark_edit(IDC_INSTALL_PATH, g_settings.install_path);
        g_repo_url = create_dark_edit(IDC_REPO_URL, g_settings.repo_url);
        g_branch_edit = create_dark_edit(IDC_BRANCH_EDIT, g_settings.branch);
        g_source_path = create_dark_edit(IDC_SOURCE_PATH, g_settings.source_path);
    }

    LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_CREATE:
            g_window = hwnd;
            create_ui();
            append_status(L"Ready.");
            refresh_process_list();
            SetTimer(hwnd, TIMER_REFRESH, 3000, nullptr);
            SetTimer(hwnd, TIMER_AUTOLOAD, 1500, nullptr);
            payload::set_status_sink([](const std::wstring& text) {
                PostMessageW(g_window, WM_LOADER_STATUS, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
            });
            return 0;

        case WM_SIZE:
            layout_ui(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);

            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);

            paint_ui(mem, client);
            BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);

            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLOREDIT:
        {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, kText);
            SetBkColor(dc, kEditFill);
            static HBRUSH edit_brush = CreateSolidBrush(kEditFill);
            return reinterpret_cast<LRESULT>(edit_brush);
        }

        case WM_LOADER_STATUS:
        {
            auto* text = reinterpret_cast<std::wstring*>(lparam);
            if (text)
            {
                set_status(*text, true);
                delete text;
            }
            return 0;
        }

        case WM_LOADER_TASK_DONE:
        {
            auto* text = reinterpret_cast<std::wstring*>(lparam);
            if (text)
            {
                set_status(*text, wparam != 0);
                delete text;
            }
            g_task_running = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_TIMER:
            if (wparam == TIMER_REFRESH)
                refresh_process_list();
            else if (wparam == TIMER_AUTOLOAD)
                try_auto_load();
            return 0;

        case WM_MOUSEMOVE:
        {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            const HitTarget hit = hit_test(x, y);

            int hovered_process = -1;
            if (hit == HitTarget::ProcessRow)
            {
                for (int i = 0; i < static_cast<int>(g_layout.process_rows.size()); ++i)
                {
                    if (point_in_rect(g_layout.process_rows[static_cast<size_t>(i)], x, y))
                    {
                        hovered_process = i;
                        break;
                    }
                }
            }

            if (hit != g_hover || hovered_process != g_hovered_process)
            {
                g_hover = hit;
                g_hovered_process = hovered_process;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            g_pressed = hit_test(x, y);
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP:
        {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            const HitTarget released = hit_test(x, y);
            const HitTarget pressed = g_pressed;
            g_pressed = HitTarget::None;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);

            if (released != pressed)
                return 0;

            switch (released)
            {
            case HitTarget::Refresh:
                refresh_process_list();
                break;
            case HitTarget::Load:
                if (!g_task_running.load())
                    perform_load();
                break;
            case HitTarget::Exit:
                save_settings_from_ui();
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                break;
            case HitTarget::Download:
                if (!g_task_running.load())
                    perform_source_download();
                break;
            case HitTarget::BrowseInstall:
            {
                std::wstring path = read_control_text(g_install_path);
                if (browse_for_folder(L"Choose Vanille install folder", path))
                    write_control_text(g_install_path, path);
                break;
            }
            case HitTarget::BrowseSource:
            {
                std::wstring path = read_control_text(g_source_path);
                if (browse_for_folder(L"Choose source download folder", path))
                    write_control_text(g_source_path, path);
                break;
            }
            case HitTarget::AutoLoad:
                g_settings.auto_load = !g_settings.auto_load;
                settings::save(g_settings);
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            case HitTarget::ProcessRow:
                for (int i = 0; i < static_cast<int>(g_layout.process_rows.size()); ++i)
                {
                    if (point_in_rect(g_layout.process_rows[static_cast<size_t>(i)], x, y))
                    {
                        g_selected_process = i;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                    }
                }
                break;
            default:
                break;
            }
            return 0;
        }

        case WM_CLOSE:
            save_settings_from_ui();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_REFRESH);
            KillTimer(hwnd, TIMER_AUTOLOAD);
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }

        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void enable_dark_title_bar(HWND hwnd)
    {
        const BOOL use_dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark, sizeof(use_dark));
        COLORREF caption = kBg;
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    }
}

namespace gui
{
    int run(HINSTANCE instance)
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        g_instance = instance;
        g_settings = settings::load();

        g_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_font_semibold = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Semibold");

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = window_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"ChocolaLoaderWindow";
        RegisterClassExW(&wc);

        g_window = CreateWindowExW(
            0,
            wc.lpszClassName,
            L"Chocola",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            536,
            420,
            nullptr,
            nullptr,
            instance,
            nullptr);

        if (!g_window)
            return 1;

        enable_dark_title_bar(g_window);
        ShowWindow(g_window, SW_SHOW);
        UpdateWindow(g_window);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0))
        {
            if (!IsDialogMessageW(g_window, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        DeleteObject(g_font);
        DeleteObject(g_font_semibold);
        CoUninitialize();
        return static_cast<int>(msg.wParam);
    }
}
