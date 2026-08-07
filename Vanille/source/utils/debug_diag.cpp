#include "utils/debug_diag.h"

#include "utils/logger.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <windows.h>

namespace
{
    using clock = std::chrono::steady_clock;

    std::mutex g_mutex;
    std::string g_log_path;
    const clock::time_point g_started = clock::now();
    double g_last_esp_report = -1000.0;
    double g_last_engine_report = -1000.0;

    double elapsed_seconds()
    {
        return std::chrono::duration<double>(clock::now() - g_started).count();
    }

    std::filesystem::path resolve_log_path()
    {
        char module_path[MAX_PATH]{};
        if (GetModuleFileNameA(nullptr, module_path, MAX_PATH) != 0)
        {
            return std::filesystem::path(module_path).parent_path() / "vanille-debug.log";
        }

        return std::filesystem::current_path() / "vanille-debug.log";
    }

    void append_log_line(const char* message)
    {
        std::lock_guard lock(g_mutex);
        if (g_log_path.empty())
        {
            return;
        }

        FILE* file = nullptr;
        if (fopen_s(&file, g_log_path.c_str(), "a") != 0 || !file)
        {
            return;
        }

        SYSTEMTIME system_time{};
        GetLocalTime(&system_time);
        std::fprintf(
            file,
            "[%02u:%02u:%02u] %s\n",
            system_time.wHour,
            system_time.wMinute,
            system_time.wSecond,
            message);
        std::fflush(file);
        std::fclose(file);
    }
}

namespace debug_diag
{
    void initialize()
    {
        const auto path = resolve_log_path();
        g_log_path = path.string();

        FILE* file = nullptr;
        if (fopen_s(&file, g_log_path.c_str(), "w") == 0 && file)
        {
            SYSTEMTIME system_time{};
            GetLocalTime(&system_time);
            std::fprintf(
                file,
                "=== vanille debug log started %04u-%02u-%02u %02u:%02u:%02u ===\n",
                system_time.wYear,
                system_time.wMonth,
                system_time.wDay,
                system_time.wHour,
                system_time.wMinute,
                system_time.wSecond);
            std::fflush(file);
            std::fclose(file);
        }

        logger_core::log_info("debug log -> {}", g_log_path);
    }

    const char* log_file_path()
    {
        return g_log_path.c_str();
    }

    bool logging_active()
    {
        return true;
    }

    bool should_emit_reports()
    {
        return true;
    }

    void log_always(const char* message)
    {
        append_log_line(message);
        logger_core::log_info("[diag] {}", message);
    }

    void log_status(const char* message)
    {
        append_log_line(message);
        if (logging_active())
        {
            logger_core::log_info("[diag] {}", message);
        }
    }

    void report_esp_frame(const esp_frame_report& report)
    {
        const double now = elapsed_seconds();
        if ((now - g_last_esp_report) < k_report_interval_seconds)
        {
            return;
        }

        g_last_esp_report = now;

        const auto line = std::format(
            "esp phase={} enabled={} bbox={} name={} skel={} hl={} draw={} camera={} local={} cached={} rendered={} bounds={} primitives={} "
            "viewport={:.0f}x{:.0f} dm=0x{:X} players=0x{:X} workspace=0x{:X} ve=0x{:X}",
            report.phase,
            report.esp_enabled ? 1 : 0,
            report.bbox_enabled ? 1 : 0,
            report.name_enabled ? 1 : 0,
            report.skeleton_enabled ? 1 : 0,
            report.highlight_enabled ? 1 : 0,
            report.draw_list_ok ? 1 : 0,
            report.camera_ok ? 1 : 0,
            report.local_ok ? 1 : 0,
            report.cached_players,
            report.rendered_players,
            report.players_with_bounds,
            report.draw_primitives,
            report.viewport_w,
            report.viewport_h,
            report.datamodel,
            report.players_service,
            report.workspace,
            report.visualengine);

        append_log_line(line.c_str());
        logger_core::log_info("[esp] {}", line);
    }

    void report_engine(const engine_report& report)
    {
        const double now = elapsed_seconds();
        if ((now - g_last_engine_report) < k_report_interval_seconds)
        {
            return;
        }

        g_last_engine_report = now;

        const auto line = std::format(
            "engine ready={} dm={} players={} workspace={} ve={} "
            "dm=0x{:X} players=0x{:X} workspace=0x{:X} ve=0x{:X} place={}",
            report.engine_ready ? 1 : 0,
            report.datamodel_valid ? 1 : 0,
            report.players_valid ? 1 : 0,
            report.workspace_valid ? 1 : 0,
            report.visualengine_valid ? 1 : 0,
            report.datamodel,
            report.players,
            report.workspace,
            report.visualengine,
            report.place_id);

        append_log_line(line.c_str());
        logger_core::log_info("[engine] {}", line);
    }
}
