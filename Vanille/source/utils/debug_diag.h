#pragma once

#include <cstddef>
#include <cstdint>

namespace debug_diag
{
    constexpr double k_report_interval_seconds = 2.0;

    void initialize();
    const char* log_file_path();

    bool logging_active();
    bool should_emit_reports();

    void log_always(const char* message);
    void log_status(const char* message);

    struct esp_frame_report
    {
        const char* phase = "idle";
        bool esp_enabled = false;
        bool bbox_enabled = false;
        bool name_enabled = false;
        bool skeleton_enabled = false;
        bool highlight_enabled = false;
        bool draw_list_ok = false;
        bool camera_ok = false;
        bool local_ok = false;
        std::size_t cached_players = 0;
        std::size_t rendered_players = 0;
        std::size_t players_with_bounds = 0;
        std::size_t draw_primitives = 0;
        float viewport_w = 0.0f;
        float viewport_h = 0.0f;
        std::uintptr_t datamodel = 0;
        std::uintptr_t players_service = 0;
        std::uintptr_t workspace = 0;
        std::uintptr_t visualengine = 0;
    };

    void report_esp_frame(const esp_frame_report& report);

    struct engine_report
    {
        bool engine_ready = false;
        bool datamodel_valid = false;
        bool players_valid = false;
        bool workspace_valid = false;
        bool visualengine_valid = false;
        std::uintptr_t datamodel = 0;
        std::uintptr_t players = 0;
        std::uintptr_t workspace = 0;
        std::uintptr_t visualengine = 0;
        std::int64_t place_id = 0;
    };

    void report_engine(const engine_report& report);
}
