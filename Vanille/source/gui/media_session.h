#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace vanille::media
{
    struct snapshot
    {
        bool active = false;
        bool is_playing = false;
        bool is_spotify = false;
        std::string title;
        std::string artist;
        std::string album;
        std::string app_name;
        std::vector<unsigned char> art_bytes;
        std::uint64_t art_revision = 0;
        double position_seconds = 0.0;
        double duration_seconds = 0.0;
        std::chrono::steady_clock::time_point position_sample_time{};
    };

    struct lyrics_line
    {
        double time_seconds = 0.0;
        std::string text;
    };

    enum class lyrics_state
    {
        idle = 0,
        loading,
        ready,
        not_found,
        failed
    };

    struct lyrics_snapshot
    {
        lyrics_state state = lyrics_state::idle;
        std::vector<lyrics_line> synced_lines;
        std::vector<std::string> plain_lines;
        bool has_synced = false;
        std::string track_key;
    };

    void start();
    void stop();
    snapshot get_snapshot();
    lyrics_snapshot get_lyrics();

    void toggle_play_pause();
    void skip_next();
    void skip_previous();
}
