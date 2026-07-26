#include "media_session.h"

#include <Windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <winhttp.h>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winhttp.lib")

namespace vanille::media
{
    namespace
    {
        using namespace winrt::Windows::Media::Control;
        using namespace winrt::Windows::Storage::Streams;

        std::mutex g_mutex;
        snapshot g_snapshot;
        std::mutex g_lyrics_mutex;
        lyrics_snapshot g_lyrics;
        std::atomic<bool> g_running{ false };
        std::thread g_worker;
        std::atomic<bool> g_winrt_ready{ false };
        std::string g_last_track_key;
        std::atomic<bool> g_lyrics_fetch_running{ false };
        std::string g_lyrics_fetch_key;

        enum class media_command : int
        {
            none = 0,
            toggle,
            next,
            previous
        };

        std::mutex g_command_mutex;
        std::deque<media_command> g_command_queue;

        std::wstring utf8_to_wide(const std::string& text)
        {
            if (text.empty())
                return {};
            const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
            if (needed <= 0)
                return {};
            std::wstring out(static_cast<size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed);
            return out;
        }

        std::string url_encode(const std::string& value)
        {
            static const char hex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(value.size() * 3);
            for (unsigned char c : value)
            {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                    out.push_back(static_cast<char>(c));
                else if (c == ' ')
                    out.push_back('+');
                else
                {
                    out.push_back('%');
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0x0F]);
                }
            }
            return out;
        }

        std::vector<unsigned char> download_https_bytes(
            const std::wstring& host,
            const std::wstring& path,
            const wchar_t* extra_headers = nullptr)
        {
            std::vector<unsigned char> result;
            HINTERNET session = WinHttpOpen(
                L"vanille/1.0 (LRCLIB client)",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0);
            if (!session)
                return result;

            HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (!connect)
            {
                WinHttpCloseHandle(session);
                return result;
            }

            HINTERNET request = WinHttpOpenRequest(
                connect,
                L"GET",
                path.c_str(),
                nullptr,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                WINHTTP_FLAG_SECURE);
            if (!request)
            {
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                return result;
            }

            if (extra_headers)
                WinHttpAddRequestHeaders(request, extra_headers, static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

            if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                || !WinHttpReceiveResponse(request, nullptr))
            {
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                return result;
            }

            DWORD status_code = 0;
            DWORD status_size = sizeof(status_code);
            if (WinHttpQueryHeaders(
                    request,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &status_code,
                    &status_size,
                    WINHTTP_NO_HEADER_INDEX)
                && status_code >= 400)
            {
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                return result;
            }

            DWORD available = 0;
            while (WinHttpQueryDataAvailable(request, &available) && available > 0)
            {
                const size_t offset = result.size();
                result.resize(offset + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, result.data() + offset, available, &read) || read == 0)
                {
                    result.resize(offset);
                    break;
                }
                result.resize(offset + read);
            }

            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return result;
        }

        std::string extract_json_string(const std::string& json, const char* key)
        {
            const std::string needle = std::string("\"") + key + "\":\"";
            const size_t pos = json.find(needle);
            if (pos == std::string::npos)
                return {};

            std::string out;
            out.reserve(256);
            for (size_t i = pos + needle.size(); i < json.size(); ++i)
            {
                const char c = json[i];
                if (c == '"')
                    break;
                if (c == '\\' && i + 1 < json.size())
                {
                    const char next = json[++i];
                    switch (next)
                    {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    default: out.push_back(next); break;
                    }
                }
                else
                {
                    out.push_back(c);
                }
            }
            return out;
        }

        bool json_value_is_null(const std::string& json, const char* key)
        {
            const std::string needle = std::string("\"") + key + "\":null";
            return json.find(needle) != std::string::npos;
        }

        std::string upgrade_artwork_url(std::string url)
        {
            static const char* patterns[] = { "100x100bb", "60x60bb", "30x30bb" };
            for (const char* pattern : patterns)
            {
                const size_t pos = url.find(pattern);
                if (pos != std::string::npos)
                {
                    url.replace(pos, std::strlen(pattern), "600x600bb");
                    break;
                }
            }
            return url;
        }

        std::vector<unsigned char> fetch_hi_res_artwork(const std::string& artist, const std::string& title, const std::string& album)
        {
            std::string query = artist;
            if (!title.empty())
            {
                if (!query.empty())
                    query.push_back(' ');
                query += title;
            }
            if (query.empty() && !album.empty())
                query = album;
            if (query.empty())
                return {};

            const std::wstring path = L"/search?term=" + utf8_to_wide(url_encode(query)) + L"&entity=song&limit=1";
            const auto json_bytes = download_https_bytes(L"itunes.apple.com", path);
            if (json_bytes.empty())
                return {};

            const std::string json(json_bytes.begin(), json_bytes.end());
            std::string art_url = extract_json_string(json, "artworkUrl100");
            if (art_url.empty())
                art_url = extract_json_string(json, "artworkUrl60");
            if (art_url.empty())
                return {};

            art_url = upgrade_artwork_url(std::move(art_url));
            const size_t scheme_pos = art_url.find("://");
            if (scheme_pos == std::string::npos)
                return {};
            const size_t host_start = scheme_pos + 3;
            const size_t path_start = art_url.find('/', host_start);
            if (path_start == std::string::npos)
                return {};

            const std::string host = art_url.substr(host_start, path_start - host_start);
            const std::string art_path = art_url.substr(path_start);
            return download_https_bytes(utf8_to_wide(host), utf8_to_wide(art_path));
        }

        std::string wide_to_utf8(std::wstring_view wide)
        {
            if (wide.empty())
                return {};
            const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            if (needed <= 0)
                return {};
            std::string out(static_cast<size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
            if (!out.empty() && out.back() == '\0')
                out.pop_back();
            return out;
        }

        std::wstring app_display_name(std::wstring_view app_id)
        {
            const size_t bang = app_id.find(L'!');
            const std::wstring_view tail = (bang == std::wstring_view::npos) ? app_id : app_id.substr(bang + 1);
            if (tail.find(L"Spotify") != std::wstring_view::npos)
                return L"Spotify";
            return std::wstring(tail);
        }

        std::vector<unsigned char> read_stream_reference(const IRandomAccessStreamReference& ref)
        {
            if (!ref)
                return {};
            const auto stream = ref.OpenReadAsync().get();
            const uint64_t size64 = stream.Size();
            if (size64 == 0 || size64 > 4 * 1024 * 1024)
                return {};
            const uint32_t size = static_cast<uint32_t>(size64);
            DataReader reader(stream);
            reader.LoadAsync(size).get();
            std::vector<unsigned char> bytes(size);
            reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), static_cast<uint32_t>(bytes.size())));
            return bytes;
        }

        double time_span_to_seconds(const winrt::Windows::Foundation::TimeSpan& span)
        {
            return static_cast<double>(span.count()) / 10000000.0;
        }

        std::optional<GlobalSystemMediaTransportControlsSession> pick_session(
            const GlobalSystemMediaTransportControlsSessionManager& manager)
        {
            GlobalSystemMediaTransportControlsSession spotify{ nullptr };
            const auto sessions = manager.GetSessions();
            for (uint32_t i = 0; i < sessions.Size(); ++i)
            {
                const auto session = sessions.GetAt(i);
                const std::wstring_view app_id{ session.SourceAppUserModelId() };
                if (app_id.find(L"Spotify") != std::wstring_view::npos)
                {
                    spotify = session;
                    break;
                }
            }
            if (spotify)
                return spotify;

            const auto current = manager.GetCurrentSession();
            if (current)
                return current;
            return std::nullopt;
        }

        void with_session(const std::function<void(GlobalSystemMediaTransportControlsSession)>& fn)
        {
            if (!g_winrt_ready.load(std::memory_order_acquire))
                return;
            try
            {
                const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                const auto session = pick_session(manager);
                if (!session)
                    return;
                fn(*session);
            }
            catch (...)
            {
            }
        }

        void enqueue_command(media_command cmd)
        {
            std::lock_guard lock(g_command_mutex);
            g_command_queue.push_back(cmd);
        }

        void process_pending_commands()
        {
            std::deque<media_command> pending;
            {
                std::lock_guard lock(g_command_mutex);
                pending.swap(g_command_queue);
            }

            if (pending.empty())
                return;

            with_session([&](GlobalSystemMediaTransportControlsSession session) {
                for (const media_command cmd : pending)
                {
                    switch (cmd)
                    {
                    case media_command::toggle:
                        session.TryTogglePlayPauseAsync().get();
                        break;
                    case media_command::next:
                        session.TrySkipNextAsync().get();
                        break;
                    case media_command::previous:
                        session.TrySkipPreviousAsync().get();
                        break;
                    default:
                        break;
                    }
                }
            });
        }

        double parse_lrc_timestamp(const std::string& stamp)
        {
            const size_t colon = stamp.find(':');
            if (colon == std::string::npos)
                return 0.0;
            try
            {
                const int minutes = std::stoi(stamp.substr(0, colon));
                const double seconds = std::stod(stamp.substr(colon + 1));
                return static_cast<double>(minutes) * 60.0 + seconds;
            }
            catch (...)
            {
                return 0.0;
            }
        }

        std::vector<lyrics_line> parse_lrc(const std::string& lrc)
        {
            std::vector<lyrics_line> lines;
            size_t line_start = 0;
            while (line_start < lrc.size())
            {
                size_t line_end = lrc.find('\n', line_start);
                if (line_end == std::string::npos)
                    line_end = lrc.size();

                const std::string line = lrc.substr(line_start, line_end - line_start);
                line_start = line_end + 1;

                size_t cursor = 0;
                while (cursor < line.size() && line[cursor] == '[')
                {
                    const size_t close = line.find(']', cursor);
                    if (close == std::string::npos)
                        break;

                    const std::string stamp = line.substr(cursor + 1, close - cursor - 1);
                    std::string text = line.substr(close + 1);
                    while (!text.empty() && (text.front() == ' ' || text.front() == '\r'))
                        text.erase(text.begin());

                    lyrics_line entry;
                    entry.time_seconds = parse_lrc_timestamp(stamp);
                    entry.text = std::move(text);
                    if (!entry.text.empty())
                        lines.push_back(std::move(entry));

                    cursor = close + 1;
                    while (cursor < line.size() && line[cursor] != '[')
                        ++cursor;
                }
            }

            std::sort(lines.begin(), lines.end(), [](const lyrics_line& a, const lyrics_line& b) {
                return a.time_seconds < b.time_seconds;
            });
            return lines;
        }

        std::vector<std::string> split_plain_lyrics(const std::string& plain)
        {
            std::vector<std::string> lines;
            size_t line_start = 0;
            while (line_start <= plain.size())
            {
                size_t line_end = plain.find('\n', line_start);
                if (line_end == std::string::npos)
                    line_end = plain.size();

                std::string line = plain.substr(line_start, line_end - line_start);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (!line.empty())
                    lines.push_back(std::move(line));

                if (line_end == plain.size())
                    break;
                line_start = line_end + 1;
            }
            return lines;
        }

        lyrics_snapshot fetch_lyrics_from_lrclib(const std::string& title, const std::string& artist, const std::string& album)
        {
            lyrics_snapshot result;
            result.track_key = title + '\x1f' + artist;

            if (title.empty())
            {
                result.state = lyrics_state::not_found;
                return result;
            }

            std::wstring path = L"/api/search?track_name=" + utf8_to_wide(url_encode(title));
            if (!artist.empty())
                path += L"&artist_name=" + utf8_to_wide(url_encode(artist));
            if (!album.empty())
                path += L"&album_name=" + utf8_to_wide(url_encode(album));

            const auto json_bytes = download_https_bytes(L"lrclib.net", path);
            if (json_bytes.empty())
            {
                result.state = lyrics_state::failed;
                return result;
            }

            const std::string json(json_bytes.begin(), json_bytes.end());
            if (json.empty() || json.front() != '[' || json.find(']') == std::string::npos)
            {
                result.state = lyrics_state::not_found;
                return result;
            }

            const size_t object_start = json.find('{');
            if (object_start == std::string::npos)
            {
                result.state = lyrics_state::not_found;
                return result;
            }

            const size_t object_end = json.find('}', object_start);
            if (object_end == std::string::npos)
            {
                result.state = lyrics_state::not_found;
                return result;
            }

            const std::string object = json.substr(object_start, object_end - object_start + 1);
            const std::string synced = extract_json_string(object, "syncedLyrics");
            const std::string plain = extract_json_string(object, "plainLyrics");
            const bool synced_null = json_value_is_null(object, "syncedLyrics");
            const bool plain_null = json_value_is_null(object, "plainLyrics");

            if (!synced.empty())
            {
                result.synced_lines = parse_lrc(synced);
                result.has_synced = !result.synced_lines.empty();
            }
            else if (!synced_null)
            {
                result.has_synced = false;
            }

            if (!plain.empty())
                result.plain_lines = split_plain_lyrics(plain);
            else if (!plain_null && result.synced_lines.empty())
            {
                result.state = lyrics_state::not_found;
                return result;
            }

            if (result.synced_lines.empty() && result.plain_lines.empty())
            {
                result.state = lyrics_state::not_found;
                return result;
            }

            result.state = lyrics_state::ready;
            return result;
        }

        void start_lyrics_fetch(const std::string& title, const std::string& artist, const std::string& album, const std::string& track_key)
        {
            if (track_key.empty())
                return;

            if (g_lyrics_fetch_running.load(std::memory_order_acquire) && g_lyrics_fetch_key == track_key)
                return;

            g_lyrics_fetch_key = track_key;
            g_lyrics_fetch_running.store(true, std::memory_order_release);

            {
                std::lock_guard lock(g_lyrics_mutex);
                g_lyrics = {};
                g_lyrics.state = lyrics_state::loading;
                g_lyrics.track_key = track_key;
            }

            std::thread([title, artist, album, track_key]() {
                const lyrics_snapshot fetched = fetch_lyrics_from_lrclib(title, artist, album);
                std::lock_guard lock(g_lyrics_mutex);
                if (g_lyrics_fetch_key == track_key)
                    g_lyrics = fetched;
                g_lyrics_fetch_running.store(false, std::memory_order_release);
            }).detach();
        }

        void poll_once()
        {
            if (!g_winrt_ready.load(std::memory_order_acquire))
                return;

            snapshot next;
            try
            {
                const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                const auto session_opt = pick_session(manager);
                if (!session_opt)
                {
                    std::lock_guard lock(g_mutex);
                    g_snapshot = {};
                    return;
                }

                const auto& session = *session_opt;
                const std::wstring_view app_id{ session.SourceAppUserModelId() };
                next.is_spotify = app_id.find(L"Spotify") != std::wstring_view::npos;
                next.app_name = wide_to_utf8(app_display_name(app_id));

                const auto playback = session.GetPlaybackInfo();
                const auto status = playback.PlaybackStatus();
                next.is_playing = status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                next.active = status != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed
                    && status != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped;

                const auto props = session.TryGetMediaPropertiesAsync().get();
                next.title = wide_to_utf8(props.Title().c_str());
                next.artist = wide_to_utf8(props.Artist().c_str());
                next.album = wide_to_utf8(props.AlbumTitle().c_str());

                try
                {
                    const auto timeline = session.GetTimelineProperties();
                    next.position_seconds = time_span_to_seconds(timeline.Position());
                    next.duration_seconds = time_span_to_seconds(timeline.EndTime());
                    next.position_sample_time = std::chrono::steady_clock::now();
                }
                catch (...)
                {
                    next.position_seconds = 0.0;
                    next.duration_seconds = 0.0;
                    next.position_sample_time = {};
                }

                if (next.title.empty() && next.artist.empty())
                    next.active = false;

                const std::string track_key = next.title + '\x1f' + next.artist + '\x1f' + next.album;
                const bool track_changed = track_key != g_last_track_key;
                if (track_changed)
                {
                    g_last_track_key = track_key;
                    if (!next.title.empty())
                        start_lyrics_fetch(next.title, next.artist, next.album, next.title + '\x1f' + next.artist);
                    else
                    {
                        std::lock_guard lyrics_lock(g_lyrics_mutex);
                        g_lyrics = {};
                        g_lyrics_fetch_key.clear();
                    }
                }

                std::vector<unsigned char> art_bytes;
                if (track_changed)
                {
                    art_bytes = fetch_hi_res_artwork(next.artist, next.title, next.album);
                    if (art_bytes.empty())
                        art_bytes = read_stream_reference(props.Thumbnail());
                }

                std::lock_guard lock(g_mutex);
                if (track_changed)
                {
                    if (!art_bytes.empty())
                    {
                        next.art_bytes = std::move(art_bytes);
                        next.art_revision = g_snapshot.art_revision + 1;
                    }
                    else
                    {
                        next.art_bytes.clear();
                        next.art_revision = g_snapshot.art_revision + 1;
                    }
                }
                else
                {
                    next.art_bytes = g_snapshot.art_bytes;
                    next.art_revision = g_snapshot.art_revision;
                }
                g_snapshot = std::move(next);
            }
            catch (...)
            {
                std::lock_guard lock(g_mutex);
                g_snapshot = {};
            }
        }

        void worker_main()
        {
            try
            {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                g_winrt_ready.store(true, std::memory_order_release);
            }
            catch (...)
            {
                return;
            }

            auto last_poll = std::chrono::steady_clock::now();
            while (g_running.load(std::memory_order_acquire))
            {
                process_pending_commands();

                const auto now = std::chrono::steady_clock::now();
                if (now - last_poll >= std::chrono::milliseconds(150))
                {
                    poll_once();
                    last_poll = now;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    void start()
    {
        if (g_running.exchange(true))
            return;
        g_worker = std::thread(worker_main);
    }

    void stop()
    {
        if (!g_running.exchange(false))
            return;
        if (g_worker.joinable())
            g_worker.join();
        g_winrt_ready.store(false, std::memory_order_release);
        g_last_track_key.clear();
        g_lyrics_fetch_key.clear();
        g_lyrics_fetch_running.store(false, std::memory_order_release);
        {
            std::lock_guard lock(g_mutex);
            g_snapshot = {};
        }
        {
            std::lock_guard lock(g_lyrics_mutex);
            g_lyrics = {};
        }
        {
            std::lock_guard lock(g_command_mutex);
            g_command_queue.clear();
        }
    }

    snapshot get_snapshot()
    {
        std::lock_guard lock(g_mutex);
        snapshot snap = g_snapshot;
        if (snap.is_playing && snap.position_sample_time.time_since_epoch().count() != 0)
        {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - snap.position_sample_time).count();
            snap.position_seconds += elapsed;
            if (snap.duration_seconds > 0.0)
                snap.position_seconds = (std::min)(snap.position_seconds, snap.duration_seconds);
        }
        return snap;
    }

    lyrics_snapshot get_lyrics()
    {
        std::lock_guard lock(g_lyrics_mutex);
        return g_lyrics;
    }

    void toggle_play_pause()
    {
        enqueue_command(media_command::toggle);
    }

    void skip_next()
    {
        enqueue_command(media_command::next);
    }

    void skip_previous()
    {
        enqueue_command(media_command::previous);
    }
}
