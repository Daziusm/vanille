#include "mcp/mcp_bridge.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <tinygltf/json.hpp>
#include <Windows.h>

#include "explorer/explorer_service.h"
#include "globals/globals_fixed.h"
#include "lua/lua_console.h"
#include "lua/lua_vm.h"

namespace vanille::mcp_bridge
{
    namespace
    {
        std::filesystem::path g_root_dir;
        bool g_initialized = false;
        double g_last_status_write = 0.0;

        double now_seconds()
        {
            using clock = std::chrono::steady_clock;
            return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
        }

        std::int64_t now_unix_ms()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::filesystem::path resolve_root_dir()
        {
            char* local_app_data = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&local_app_data, &length, "LOCALAPPDATA") != 0 || !local_app_data || !*local_app_data)
            {
                if (local_app_data)
                {
                    free(local_app_data);
                }
                return std::filesystem::path("Vanille") / "mcp";
            }

            const std::filesystem::path path = std::filesystem::path(local_app_data) / "Vanille" / "mcp";
            free(local_app_data);
            return path;
        }

        void ensure_directories()
        {
            std::error_code ec;
            std::filesystem::create_directories(requests_dir(), ec);
            std::filesystem::create_directories(responses_dir(), ec);
        }

        bool write_json_file(const std::filesystem::path& path, const nlohmann::json& value)
        {
            try
            {
                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    return false;
                }
                output << value.dump(2);
                return output.good();
            }
            catch (...)
            {
                return false;
            }
        }

        std::optional<nlohmann::json> read_json_file(const std::filesystem::path& path)
        {
            try
            {
                std::ifstream input(path, std::ios::binary);
                if (!input)
                {
                    return std::nullopt;
                }
                std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
                nlohmann::json parsed = nlohmann::json::parse(contents, nullptr, false);
                if (parsed.is_discarded())
                {
                    return std::nullopt;
                }
                return parsed;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        nlohmann::json make_status_json()
        {
            nlohmann::json status;
            status["alive"] = true;
            status["updated_at_unix_ms"] = now_unix_ms();
            status["game_id"] = globals ? globals->game_id : 0;
            status["lua_ready"] = lua_vm::is_ready();
            status["mcp_root"] = g_root_dir.string();
            status["explorer_snapshot_path"] = explorer_snapshot_path().string();
            status["explorer_snapshot_exists"] = std::filesystem::exists(explorer_snapshot_path());
            return status;
        }

        void write_status()
        {
            write_json_file(status_path(), make_status_json());
        }

        nlohmann::json handle_ping(const nlohmann::json&)
        {
            return make_status_json();
        }

        nlohmann::json handle_status(const nlohmann::json&)
        {
            return make_status_json();
        }

        nlohmann::json handle_explorer_refresh(const nlohmann::json& args)
        {
            explorer::capture_options options{};
            if (args.is_object())
            {
                if (args.contains("max_depth") && args["max_depth"].is_number_unsigned())
                {
                    options.max_depth = args["max_depth"].get<std::size_t>();
                }
                if (args.contains("max_nodes") && args["max_nodes"].is_number_unsigned())
                {
                    options.max_nodes = args["max_nodes"].get<std::size_t>();
                }
            }

            const bool exported = explorer::export_mcp_snapshot(options);
            const auto snap = explorer::capture(options);
            nlohmann::json result;
            result["exported"] = exported;
            result["path"] = explorer_snapshot_path().string();
            result["node_count"] = snap.node_count;
            result["truncated"] = snap.truncated;
            return result;
        }

        nlohmann::json handle_lua_execute(const nlohmann::json& args)
        {
            nlohmann::json result;
            if (!args.is_object() || !args.contains("source") || !args["source"].is_string())
            {
                result["success"] = false;
                result["error"] = "missing_source";
                return result;
            }

            if (!lua_vm::is_ready())
            {
                result["success"] = false;
                result["error"] = "lua_vm_not_ready";
                return result;
            }

            const std::string source = args["source"].get<std::string>();
            const std::string chunk_name = args.value("chunk_name", "mcp_lua");
            const std::size_t console_before = lua_console::get_lines_snapshot().size();
            const bool success = lua_vm::execute_string(source, chunk_name);
            const auto lines = lua_console::get_lines_snapshot();
            std::vector<std::string> new_lines;
            if (lines.size() > console_before)
            {
                new_lines.reserve(lines.size() - console_before);
                for (std::size_t i = console_before; i < lines.size(); ++i)
                {
                    const auto& line = lines[i];
                    std::string prefix;
                    switch (line.level)
                    {
                    case lua_console::log_level::warning: prefix = "[warn] "; break;
                    case lua_console::log_level::error: prefix = "[error] "; break;
                    default: prefix = "[info] "; break;
                    }
                    new_lines.push_back(prefix + line.text);
                }
            }

            result["success"] = success;
            result["console"] = new_lines;
            return result;
        }

        nlohmann::json handle_lua_console(const nlohmann::json& args)
        {
            const std::size_t max_lines = args.is_object() && args.contains("max_lines") && args["max_lines"].is_number_unsigned()
                ? args["max_lines"].get<std::size_t>()
                : 200;

            const auto lines = lua_console::get_lines_snapshot();
            const std::size_t start = lines.size() > max_lines ? lines.size() - max_lines : 0;
            nlohmann::json result = nlohmann::json::array();
            for (std::size_t i = start; i < lines.size(); ++i)
            {
                nlohmann::json entry;
                switch (lines[i].level)
                {
                case lua_console::log_level::warning: entry["level"] = "warning"; break;
                case lua_console::log_level::error: entry["level"] = "error"; break;
                default: entry["level"] = "info"; break;
                }
                entry["text"] = lines[i].text;
                result.push_back(std::move(entry));
            }
            return result;
        }

        std::optional<nlohmann::json> dispatch_command(const std::string& command, const nlohmann::json& args)
        {
            if (command == "ping" || command == "status")
            {
                return handle_status(args);
            }
            if (command == "explorer_refresh")
            {
                return handle_explorer_refresh(args);
            }
            if (command == "lua_execute")
            {
                return handle_lua_execute(args);
            }
            if (command == "lua_console")
            {
                return handle_lua_console(args);
            }
            return std::nullopt;
        }

        void process_request_file(const std::filesystem::path& request_path)
        {
            const auto request = read_json_file(request_path);
            if (!request || !request->is_object())
            {
                std::error_code ec;
                std::filesystem::remove(request_path, ec);
                return;
            }

            const std::string id = request->value("id", request_path.stem().string());
            const std::string command = request->value("command", "");
            const nlohmann::json args = request->contains("args") ? (*request)["args"] : nlohmann::json::object();

            nlohmann::json response;
            response["id"] = id;
            response["command"] = command;
            response["completed_at_unix_ms"] = now_unix_ms();

            if (const auto result = dispatch_command(command, args))
            {
                response["ok"] = true;
                response["result"] = *result;
            }
            else
            {
                response["ok"] = false;
                response["error"] = command.empty() ? "missing_command" : "unknown_command";
            }

            write_json_file(responses_dir() / (id + ".json"), response);

            std::error_code ec;
            std::filesystem::remove(request_path, ec);
        }

        void process_pending_requests()
        {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(requests_dir(), ec))
            {
                if (ec || !entry.is_regular_file())
                {
                    continue;
                }
                if (entry.path().extension() == ".json")
                {
                    process_request_file(entry.path());
                }
            }
        }
    }

    std::filesystem::path root_dir()
    {
        if (g_root_dir.empty())
        {
            g_root_dir = resolve_root_dir();
        }
        return g_root_dir;
    }

    std::filesystem::path requests_dir()
    {
        return root_dir() / "requests";
    }

    std::filesystem::path responses_dir()
    {
        return root_dir() / "responses";
    }

    std::filesystem::path status_path()
    {
        return root_dir() / "status.json";
    }

    std::filesystem::path explorer_snapshot_path()
    {
        return explorer::default_mcp_snapshot_path();
    }

    void initialize()
    {
        if (g_initialized)
        {
            return;
        }

        g_root_dir = resolve_root_dir();
        ensure_directories();
        write_status();
        g_last_status_write = now_seconds();
        g_initialized = true;
    }

    void tick()
    {
        if (!g_initialized)
        {
            initialize();
        }

        process_pending_requests();

        const double now = now_seconds();
        if ((now - g_last_status_write) >= 1.0)
        {
            write_status();
            g_last_status_write = now;
        }
    }
}
