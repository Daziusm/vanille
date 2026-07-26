#include "cache/player_cache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <mutex>
#include <format>
#include <vector>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <thread>
#include <cctype>

#include "cache/local_player_cache.h"
#include "globals/globals.h"
#include "memory/memory.h"
#include "sdk/engine.h"
#include "sdk/humanoid.h"
#include "sdk/part.h"
#include "sdk/offsets.h"
#include "sdk/mesh_part.h"
#include "sdk/value.h"
#include "sdk/player.h"
#include "sdk/math_types.h"
#include "utils/logger.h"
#include <winhttp.h>
#include <urlmon.h>
#include <objbase.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "urlmon.lib")

namespace
{
    constexpr std::int64_t arsenal_place_id = 286090429;
    constexpr std::int64_t phantom_forces_place_id = 292439477;
    constexpr std::int64_t img_place_id = 5269129530;
    constexpr std::int64_t lostfront_place_id = 102871156420149;
    constexpr std::size_t k_max_children = 2048;
    constexpr double k_parts_refresh_interval = 0.5;
    struct pf_model_entry
    {
        rbx::instance_t model;
        std::uintptr_t team = 0;
        bool enemy_folder = false;
    };

    std::mutex g_mesh_refresh_mutex;
    std::vector<std::uint64_t> g_last_mesh_ids;
    bool g_mesh_refresh_running = false;
    std::unordered_map<std::string, std::uint64_t> g_pf_cached_user_ids_by_name;

    std::vector<rbx::instance_t> get_children_safely(const rbx::instance_t& instance, std::string_view context);

    bool model_has_part_child(const rbx::instance_t& model)
    {
        if (!model.is_valid() || model.get_class_name() != "Model")
        {
            return false;
        }

        auto model_children = get_children_safely(model, "workspace.players.team.model");
        for (const auto& child : model_children)
        {
            const std::string cls = child.get_class_name();
            if (cls == "Part" || cls == "MeshPart")
            {
                return true;
            }
        }
        return false;
    }

    rbx::instance_t find_first_descendant_by_class_limited(const rbx::instance_t& root, std::string_view class_name, std::size_t max_nodes = 512)
    {
        if (!root.is_valid() || class_name.empty())
        {
            return {};
        }

        auto stack = get_children_safely(root, "descendant_search");
        std::size_t visited = 0;
        while (!stack.empty() && visited < max_nodes)
        {
            const auto current = stack.back();
            stack.pop_back();
            ++visited;

            if (!current.is_valid())
            {
                continue;
            }

            if (current.get_class_name() == class_name)
            {
                return current;
            }

            const auto children = get_children_safely(current, "descendant_walk");
            stack.insert(stack.end(), children.begin(), children.end());
        }

        return {};
    }

    std::optional<std::string> read_instance_string_field(const rbx::instance_t& instance, std::uintptr_t offset)
    {
        if (!instance.is_valid() || offset == 0)
        {
            return std::nullopt;
        }

        auto read_c_string = [](std::uintptr_t ptr) -> std::string
        {
            if (ptr == 0)
            {
                return {};
            }

            char buffer[256] = {};
            if (!memory->read_raw(buffer, ptr, sizeof(buffer) - 1))
            {
                return {};
            }
            buffer[sizeof(buffer) - 1] = '\0';
            return std::string(buffer);
        };

        const std::uintptr_t base = instance.get_address();
        const std::uintptr_t value_ptr = memory->read<std::uintptr_t>(base + offset);
        if (value_ptr != 0)
        {
            const auto pointer_value = memory->read_string(value_ptr);
            if (!pointer_value.empty() && pointer_value != "Unknown")
            {
                return pointer_value;
            }

            const auto raw_pointer_value = read_c_string(value_ptr);
            if (!raw_pointer_value.empty())
            {
                return raw_pointer_value;
            }
        }

        const auto inline_value = memory->read_string(base + offset);
        if (!inline_value.empty() && inline_value != "Unknown")
        {
            return inline_value;
        }

        const auto raw_inline_value = read_c_string(base + offset);
        if (!raw_inline_value.empty())
        {
            return raw_inline_value;
        }

        return std::nullopt;
    }

    std::string sanitize_pf_name_text(std::string text)
    {
        if (text.empty())
        {
            return {};
        }

        const auto line_end = text.find_first_of("\r\n");
        if (line_end != std::string::npos)
        {
            text.resize(line_end);
        }

        const auto begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
        {
            return {};
        }

        const auto end = text.find_last_not_of(" \t\r\n");
        if (end == std::string::npos || end < begin)
        {
            return {};
        }

        text = text.substr(begin, end - begin + 1);
        if (text == "Unknown")
        {
            return {};
        }

        return text;
    }

    std::string normalize_player_name_key(const std::string& value)
    {
        if (value.empty())
        {
            return {};
        }

        std::string out;
        out.reserve(value.size());
        for (unsigned char c : value)
        {
            if (c == '\r' || c == '\n' || c == '\t')
            {
                continue;
            }
            out.push_back(static_cast<char>(std::tolower(c)));
        }

        const auto begin = out.find_first_not_of(' ');
        if (begin == std::string::npos)
        {
            return {};
        }

        const auto end = out.find_last_not_of(' ');
        if (end == std::string::npos || end < begin)
        {
            return {};
        }

        return out.substr(begin, end - begin + 1);
    }

    rbx::instance_t find_pf_name_text_label(const rbx::instance_t& model, const cache::character_parts* parts = nullptr)
    {
        if (!model.is_valid())
        {
            return {};
        }

        rbx::instance_t head;
        if (parts && parts->head.instance.is_valid())
        {
            head = parts->head.instance;
        }

        if (!head.is_valid())
        {
            head = model.find_first_child("Head");
        }

        if (!head.is_valid())
        {
            const auto model_children = get_children_safely(model, "pf.model.children");
            for (const auto& child : model_children)
            {
                if (!child.is_valid())
                {
                    continue;
                }

                const std::string cls = child.get_class_name();
                if (cls != "Part" && cls != "MeshPart")
                {
                    continue;
                }

                const auto billboard = child.find_first_child_by_class("BillboardGui");
                if (billboard.is_valid())
                {
                    head = child;
                    break;
                }
            }
        }

        if (!head.is_valid())
        {
            return {};
        }

        auto billboard = head.find_first_child_by_class("BillboardGui");
        if (!billboard.is_valid())
        {
            billboard = find_first_descendant_by_class_limited(head, "BillboardGui", 256);
        }
        if (!billboard.is_valid())
        {
            return {};
        }

        auto text_label = billboard.find_first_child_by_class("TextLabel");
        if (!text_label.is_valid())
        {
            text_label = find_first_descendant_by_class_limited(billboard, "TextLabel");
        }
        return text_label;
    }

    std::optional<std::string> read_pf_name_from_billboard(const rbx::instance_t& model, const cache::character_parts* parts = nullptr)
    {
        const auto text_label = find_pf_name_text_label(model, parts);
        if (!text_label.is_valid())
        {
            return std::nullopt;
        }

        std::optional<std::string> raw_text = read_instance_string_field(text_label, roblox::offsets::gui_object::text);
        if (!raw_text)
        {
            return std::nullopt;
        }

        const std::string cleaned = sanitize_pf_name_text(*raw_text);
        if (cleaned.empty())
        {
            return std::nullopt;
        }

        return cleaned;
    }

    std::optional<sdk::math::color3> read_pf_textlabel_color3(const rbx::instance_t& model)
    {
        if (!model.is_valid())
        {
            return std::nullopt;
        }

        const auto text_label = find_pf_name_text_label(model);
        if (!text_label.is_valid())
        {
            return std::nullopt;
        }

        const std::uintptr_t offset = roblox::offsets::gui_object::text_color3
            ? roblox::offsets::gui_object::text_color3
            : roblox::offsets::gui_object::text_color3_fallback;
        if (!offset)
        {
            return std::nullopt;
        }

        sdk::math::color3 color = memory->read<sdk::math::color3>(text_label.get_address() + offset);
        if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b))
        {
            return std::nullopt;
        }

        color.r = std::clamp(color.r, 0.0f, 1.0f);
        color.g = std::clamp(color.g, 0.0f, 1.0f);
        color.b = std::clamp(color.b, 0.0f, 1.0f);
        return color;
    }

    bool pf_is_enemy_label_color(const sdk::math::color3& color)
    {
        const int r8 = static_cast<int>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f + 0.5f);
        const int g8 = static_cast<int>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f + 0.5f);
        const int b8 = static_cast<int>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f + 0.5f);

        constexpr int target_r = 255;
        constexpr int target_g = 10;
        constexpr int target_b = 20;
        constexpr int tolerance = 2;

        return std::abs(r8 - target_r) <= tolerance &&
            std::abs(g8 - target_g) <= tolerance &&
            std::abs(b8 - target_b) <= tolerance;
    }

    std::vector<rbx::instance_t> get_children_safely(const rbx::instance_t& instance, std::string_view context)
    {
        if (!instance.is_valid())
        {
            return {};
        }

        try
        {
            auto children = instance.get_children();
            if (children.size() > k_max_children)
            {
                children.resize(k_max_children);
            }
            return children;
        }
        catch (const std::exception& ex)
        {
            return {};
        }
        catch (...)
        {
            return {};
        }
    }

    std::vector<std::uint64_t> collect_mesh_ids_from_states(const std::vector<cache::player_state>& players, const cache::dummy_state& dummy_state)
    {
        std::unordered_set<std::uint64_t> unique_ids;
        std::vector<std::uint64_t> result;

        auto try_add_from_part = [&](const cache::primitive_part& part)
        {
            if (!part.instance.is_valid())
            {
                return;
            }

            const auto mesh_id = rbx::mesh_part::get_mesh_asset_id(part.instance);
            if (mesh_id && unique_ids.insert(*mesh_id).second)
            {
                result.push_back(*mesh_id);
            }
        };

        auto collect_from_parts = [&](const cache::character_parts& parts)
        {
            const cache::primitive_part* part_list[] = {
                &parts.head, &parts.torso, &parts.upper_torso, &parts.lower_torso, &parts.humanoid_root_part,
                &parts.left_arm, &parts.right_arm, &parts.left_leg, &parts.right_leg,
                &parts.left_upper_arm, &parts.left_lower_arm, &parts.left_hand,
                &parts.right_upper_arm, &parts.right_lower_arm, &parts.right_hand,
                &parts.left_upper_leg, &parts.left_lower_leg, &parts.left_foot,
                &parts.right_upper_leg, &parts.right_lower_leg, &parts.right_foot
            };

            for (const auto* part : part_list)
            {
                if (!part)
                {
                    continue;
                }
                try_add_from_part(*part);
            }
        };

        for (const auto& player : players)
        {
            collect_from_parts(player.parts);
        }
        collect_from_parts(dummy_state.parts);

        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    void schedule_mesh_refresh_if_needed(const std::vector<std::uint64_t>& ids)
    {
        std::lock_guard<std::mutex> lock(g_mesh_refresh_mutex);
        if (g_mesh_refresh_running)
        {
            return;
        }
        if (ids == g_last_mesh_ids)
        {
            return;
        }

        g_last_mesh_ids = ids;
        std::thread([]()
            {
                cache::download_mesh_assets_to_temp();
            }).detach();
    }

    void fill_part(cache::primitive_part& target, const rbx::instance_t& part)
    {
        target.instance = part;
        target.primitive = rbx::part::get_primitive(part);
        target.size = {};

        if (const auto size = rbx::part::get_size(target.primitive))
        {
            target.size = *size;
        }
    }

    std::optional<float> read_number_child(const rbx::instance_t& parent, std::string_view name)
    {
        if (!parent.is_valid())
        {
            return std::nullopt;
        }
        const auto child = parent.find_first_child(name);
        if (!child.is_valid())
        {
            return std::nullopt;
        }
        if (const auto val = rbx::value::get_number(child))
        {
            return static_cast<float>(*val);
        }
        return std::nullopt;
    }

    void apply_arsenal_health(const rbx::instance_t& player, float& health_out, float& max_health_out)
    {
        const auto nprbs = player.find_first_child("NRPBS");
        if (!nprbs.is_valid())
        {
            return;
        }

        if (const auto health_val = read_number_child(nprbs, "Health"))
        {
            health_out = *health_val;
        }

        if (const auto max_val = read_number_child(nprbs, "MaxHealth"))
        {
            max_health_out = *max_val;
        }
        else if (const auto alt_max = read_number_child(nprbs, "OMaxHealth"))
        {
            max_health_out = *alt_max;
        }
    }

    rbx::instance_t resolve_character(const rbx::instance_t& player, const std::string& name)
    {
        if (!player.is_valid())
        {
            return {};
        }

        if (const auto character = rbx::player::get_character(player))
        {
            return *character;
        }

        const auto workspace = globals->workspace;
        if (workspace.is_valid())
        {
            const auto child = workspace.find_first_child(name);
            if (child.is_valid())
            {
                return child;
            }
        }

        return {};
    }

    double now_seconds()
    {
        using clock = std::chrono::steady_clock;
        return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    int compute_cache_sleep_ms(int min_ms, int max_ms, int fallback_ms)
    {
        const float dt = g_overlay_dt.load(std::memory_order_relaxed);
        if (!std::isfinite(dt) || dt <= 0.0f)
        {
            return min_ms;
        }

        const int target_ms = static_cast<int>(std::lround(dt * 1000.0f));
        return std::clamp(target_ms, min_ms, max_ms);
    }

    bool head_has_billboard_gui(const cache::character_parts& parts)
    {
        const auto& head = parts.head.instance;
        if (!head.is_valid())
        {
            return false;
        }
        const auto billboard = head.find_first_child_by_class("BillboardGui");
        return billboard.is_valid();
    }
    
    std::filesystem::path mesh_temp_directory()
    {
        auto base = std::filesystem::temp_directory_path() / "vanille_meshes";
        std::error_code ec;
        std::filesystem::create_directories(base, ec);
        return base;
    }

    std::filesystem::path mesh_temp_path(std::uint64_t id)
    {
        return mesh_temp_directory() / (std::to_wstring(id) + L".rbxmesh");
    }

    bool download_mesh_to_file(std::uint64_t id, const std::filesystem::path& path)
    {
        const std::string url_primary = std::format("https://assetdelivery.roblox.com/v1/asset/?id={}", id);
        const std::string out_path = path.string();

        auto try_url = [&](const std::string& url) -> bool
        {
            const HRESULT hr = URLDownloadToFileA(nullptr, url.c_str(), out_path.c_str(), 0, nullptr);
            if (FAILED(hr))
            {
                return false;
            }
            std::error_code ec;
            auto size = std::filesystem::file_size(path, ec);
            return !ec && size > 0;
        };

        bool ok = try_url(url_primary);
        if (!ok)
        {
            //logger_core::log_warning("mesh download failed for {}", id);
            return false;
        }

        std::error_code ec;
        auto size = std::filesystem::file_size(path, ec);
        if (ec || size == 0)
        {
            logger_core::log_warning("mesh download produced empty file for {}", id);
            return false;
        }
        return true;
    }

    struct mesh_cache_entry
    {
        bool loaded = false;
        rbx::mesh_parse::mesh_data data{};
    };

    std::mutex g_mesh_cache_mutex;
    std::unordered_map<std::uint64_t, mesh_cache_entry> g_mesh_cache;

    cache::player_state make_npc_player_state(const cache::dummy_state& dummy)
    {
        cache::player_state state{};
        state.address = dummy.address;
        state.name = !dummy.name.empty() ? dummy.name : "Bot";
        state.display_name = state.name;
        state.health = dummy.health;
        state.max_health = dummy.max_health;
        state.character = dummy.character;
        state.humanoid = dummy.humanoid;
        state.parts = dummy.parts;
        state.body_effects = dummy.body_effects;
        state.team = static_cast<std::uintptr_t>(-1);
        return state;
    }
}

namespace cache
{
    player_cache::~player_cache()
    {
        stop();
    }

    bool player_cache::start()
    {
        if (running.load())
        {
            return true;
        }

        running = true;
        worker = std::thread(&player_cache::run, this);

        return true;
    }

    void player_cache::stop()
    {
        if (!running.exchange(false))
        {
            return;
        }

        if (worker.joinable())
        {
            worker.join();
        }
    }

    std::shared_ptr<const std::vector<player_state>> player_cache::snapshot()
    {
        std::scoped_lock lock(mutex);
        return players;
    }

    std::shared_ptr<const dummy_state> player_cache::dummy_snapshot()
    {
        std::scoped_lock lock(mutex);
        return dummy;
    }

    void player_cache::run()
    {
        while (running.load())
        {
            update_state();
            const int sleep_ms = compute_cache_sleep_ms(2, 75, 75);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }

    bool player_cache::refresh_core_instances()
    {
        const bool need_refresh = !globals->datamodel.is_valid() || !globals->players.is_valid() || !globals->workspace.is_valid();
        if (need_refresh)
        {
            if (!rbx::engine->refresh())
            {
                return false;
            }
            globals->datamodel = rbx::engine->get_datamodel();
        }

        if (!globals->players.is_valid() && globals->datamodel.is_valid())
        {
            globals->players = globals->datamodel.find_first_child_by_class("Players");
        }

        if (!globals->workspace.is_valid() && globals->datamodel.is_valid())
        {
            globals->workspace = globals->datamodel.find_first_child_by_class("Workspace");
        }

        return globals->datamodel.is_valid() && globals->players.is_valid() && globals->workspace.is_valid();
    }

    std::vector<pf_model_entry> collect_phantom_forces_models()
    {
        static double last_refresh = 0.0;
        static std::vector<pf_model_entry> cached;
        std::vector<pf_model_entry> models;
        if (!globals->workspace.is_valid())
        {
            cached.clear();
            return models;
        }

        const double now = now_seconds();
        constexpr double k_refresh_interval = 0.5;
        if (!cached.empty() && (now - last_refresh) < k_refresh_interval)
        {
            return cached;
        }

        const auto workspace_players = globals->workspace.find_first_child("Players");
        if (!workspace_players.is_valid())
            return models;

        const bool enemy_only = features->team_check;
        auto team_folders = get_children_safely(workspace_players, "workspace.players");
        for (std::size_t team_index = 0; team_index < team_folders.size(); ++team_index)
        {
            const auto& team_folder = team_folders[team_index];
            if (!team_folder.is_valid())
                continue;

            bool enemy_folder = false;
            if (enemy_only)
            {
                const auto team_children = get_children_safely(team_folder, "workspace.players.team");
                for (const auto& child : team_children)
                {
                    if (!child.is_valid() || !model_has_part_child(child))
                    {
                        continue;
                    }
                    const auto color = read_pf_textlabel_color3(child);
                    if (!color)
                    {
                        continue;
                    }

                    enemy_folder = pf_is_enemy_label_color(*color);
                    break;
                }
            }

            if (enemy_only && !enemy_folder)
            {
                continue;
            }

            auto team_children = get_children_safely(team_folder, "workspace.players.team");
            for (const auto& child : team_children)
            {
                if (!child.is_valid())
                    continue;
                if (!model_has_part_child(child))
                    continue;

                pf_model_entry entry{};
                entry.model = child;
                entry.team = static_cast<std::uintptr_t>(team_index + 1); // keep non-zero team ids
                entry.enemy_folder = enemy_folder;
                models.push_back(entry);
            }
        }

        cached = models;
        last_refresh = now;
        return models;
    }

    std::vector<rbx::instance_t> collect_workspace_player_models()
    {
        static double last_refresh = 0.0;
        static std::vector<rbx::instance_t> cached;
        std::vector<rbx::instance_t> models;
        if (!globals->workspace.is_valid())
        {
            cached.clear();
            return models;
        }

        const double now = now_seconds();
        constexpr double k_refresh_interval = 0.5;
        if (!cached.empty() && (now - last_refresh) < k_refresh_interval)
        {
            return cached;
        }

        const auto workspace_players = globals->workspace.find_first_child("Players");
        if (!workspace_players.is_valid())
            return models;

        const auto children = get_children_safely(workspace_players, "workspace.players.models");
        for (const auto& child : children)
        {
            if (!child.is_valid())
                continue;
            if (child.get_class_name() != "Model")
                continue;
            models.push_back(child);
        }

        cached = models;
        last_refresh = now;
        return models;
    }

    void player_cache::update_state()
    {
        if (!refresh_core_instances())
        {
            auto empty_players = std::make_shared<std::vector<player_state>>();
            auto empty_dummy = std::make_shared<dummy_state>();
            std::scoped_lock lock(mutex);
            players = std::move(empty_players);
            dummy = std::move(empty_dummy);
            return;
        }

        const auto local_snapshot = cache::localplayer->snapshot();
        std::shared_ptr<const std::vector<player_state>> previous_players_snapshot;
        {
            std::scoped_lock lock(mutex);
            previous_players_snapshot = players;
        }
        std::unordered_map<std::uintptr_t, const player_state*> previous_by_address;
        if (previous_players_snapshot && !previous_players_snapshot->empty())
        {
            previous_by_address.reserve(previous_players_snapshot->size());
            for (const auto& previous : *previous_players_snapshot)
            {
                if (previous.address != 0)
                {
                    previous_by_address.emplace(previous.address, &previous);
                }
            }
        }
        const auto children = get_children_safely(globals->players, "players");

        std::vector<player_state> next_players;
        next_players.reserve(children.size());
        dummy_state next_dummy{};
        const bool is_phantom_forces = (globals->game_id == phantom_forces_place_id);
        const bool is_img = (globals->game_id == img_place_id);
        const bool pf_enemy_only = is_phantom_forces && features->team_check;
        std::vector<pf_model_entry> pf_models;
        std::unordered_map<std::uintptr_t, std::uintptr_t> pf_team_by_model;
        std::unordered_map<std::uintptr_t, std::uint64_t> pf_user_id_by_character;
        std::unordered_map<std::string, std::uint64_t> pf_user_id_by_name;
        if (is_phantom_forces)
        {
            pf_models = collect_phantom_forces_models();
            pf_team_by_model.reserve(pf_models.size());
            pf_user_id_by_character.reserve(children.size());
            pf_user_id_by_name.reserve(children.size() * 2);
            for (const auto& entry : pf_models)
            {
                if (!entry.model.is_valid())
                    continue;
                const std::uintptr_t addr = entry.model.get_address();
                if (addr == 0)
                    continue;
                pf_team_by_model[addr] = entry.team;
            }

            for (const auto& child : children)
            {
                if (!child.is_valid() || child.get_class_name() != "Player")
                {
                    continue;
                }

                const auto user_id_value = rbx::player::get_user_id(child);
                if (!user_id_value || *user_id_value == 0)
                {
                    continue;
                }

                const std::uint64_t user_id = *user_id_value;
                const std::string player_name_key = normalize_player_name_key(child.get_name());
                if (!player_name_key.empty())
                {
                    pf_user_id_by_name[player_name_key] = user_id;
                }

                if (const auto display_name_value = rbx::player::get_display_name(child))
                {
                    const std::string display_name_key = normalize_player_name_key(*display_name_value);
                    if (!display_name_key.empty())
                    {
                        pf_user_id_by_name[display_name_key] = user_id;
                    }
                }

                if (const auto character_value = rbx::player::get_character(child))
                {
                    if (character_value->is_valid())
                    {
                        const std::uintptr_t character_address = character_value->get_address();
                        if (character_address != 0)
                        {
                            pf_user_id_by_character[character_address] = user_id;
                        }
                    }
                }
            }

            if (previous_players_snapshot)
            {
                for (const auto& previous : *previous_players_snapshot)
                {
                    if (previous.user_id == 0)
                    {
                        continue;
                    }

                    const std::string previous_name_key = normalize_player_name_key(previous.name);
                    if (!previous_name_key.empty())
                    {
                        pf_user_id_by_name[previous_name_key] = previous.user_id;
                    }

                    const std::string previous_display_key = normalize_player_name_key(previous.display_name);
                    if (!previous_display_key.empty())
                    {
                        pf_user_id_by_name[previous_display_key] = previous.user_id;
                    }
                }
            }

            for (const auto& [cached_key, cached_id] : g_pf_cached_user_ids_by_name)
            {
                if (!cached_key.empty() && cached_id != 0)
                {
                    pf_user_id_by_name[cached_key] = cached_id;
                }
            }
        }
        else
        {
            g_pf_cached_user_ids_by_name.clear();
        }

        for (const auto& child : children)
        {
             if (local_snapshot.address != 0 && child.get_address() == local_snapshot.address)
             {
                 continue;
             }

            if (child.get_class_name() != "Player")
            {
                continue;
            }

            const std::uintptr_t player_address = child.get_address();
            const player_state* previous_state = nullptr;
            if (player_address != 0)
            {
                auto it = previous_by_address.find(player_address);
                if (it != previous_by_address.end())
                {
                    previous_state = it->second;
                }
            }

            player_state state{};
            fill_player_state(state, child, previous_state);
            if (is_phantom_forces)
            {
                if (state.user_id != 0)
                {
                    const std::uintptr_t character_address = state.character.get_address();
                    if (character_address != 0)
                    {
                        pf_user_id_by_character[character_address] = state.user_id;
                    }

                    const std::string state_name_key = normalize_player_name_key(state.name);
                    if (!state_name_key.empty())
                    {
                        pf_user_id_by_name[state_name_key] = state.user_id;
                        g_pf_cached_user_ids_by_name[state_name_key] = state.user_id;
                    }

                    const std::string state_display_key = normalize_player_name_key(state.display_name);
                    if (!state_display_key.empty())
                    {
                        pf_user_id_by_name[state_display_key] = state.user_id;
                        g_pf_cached_user_ids_by_name[state_display_key] = state.user_id;
                    }
                }

                const std::uintptr_t character_address = state.character.get_address();
                auto it = pf_team_by_model.find(character_address);
                if (it == pf_team_by_model.end())
                {
                    continue;
                }
            }
            if (state.address != 0)
            {
                next_players.emplace_back(std::move(state));
            }
        }

        if (is_phantom_forces)
        {
            for (const auto& entry : pf_models)
            {
                if (pf_enemy_only && !entry.enemy_folder)
                    continue;

                const auto& model = entry.model;
                if (!model.is_valid())
                    continue;
                const std::uintptr_t model_address = model.get_address();
                if (model_address == 0)
                    continue;
                if (local_snapshot.character.is_valid() && model_address == local_snapshot.character.get_address())
                    continue;
                const std::string model_internal_name = model.get_name();
                const std::string model_internal_key = normalize_player_name_key(model_internal_name);

                player_state state{};
                state.clear();
                state.character = model;
                state.address = model_address;
                state.name = model_internal_name;
                state.display_name = state.name;
                state.team = entry.team;
                state.humanoid = rbx::humanoid::find_humanoid(model);
                update_character_parts(state, model);

                if (auto it = pf_user_id_by_character.find(model_address); it != pf_user_id_by_character.end())
                {
                    state.user_id = it->second;
                }
                else if (!model_internal_key.empty())
                {
                    if (auto it = pf_user_id_by_name.find(model_internal_key); it != pf_user_id_by_name.end())
                    {
                        state.user_id = it->second;
                    }
                }

                if (const auto pf_name = read_pf_name_from_billboard(model, &state.parts))
                {
                    state.name = *pf_name;
                    state.display_name = *pf_name;
                }

                if (state.user_id == 0)
                {
                    const std::string resolved_name_key = normalize_player_name_key(state.name);
                    if (!resolved_name_key.empty())
                    {
                        if (auto it = pf_user_id_by_name.find(resolved_name_key); it != pf_user_id_by_name.end())
                        {
                            state.user_id = it->second;
                        }
                    }
                }

                if (state.user_id != 0)
                {
                    pf_user_id_by_character[model_address] = state.user_id;
                    if (!model_internal_key.empty())
                    {
                        pf_user_id_by_name[model_internal_key] = state.user_id;
                    }

                    const std::string resolved_name_key = normalize_player_name_key(state.name);
                    if (!resolved_name_key.empty())
                    {
                        pf_user_id_by_name[resolved_name_key] = state.user_id;
                        g_pf_cached_user_ids_by_name[resolved_name_key] = state.user_id;
                    }

                    const std::string resolved_display_key = normalize_player_name_key(state.display_name);
                    if (!resolved_display_key.empty())
                    {
                        pf_user_id_by_name[resolved_display_key] = state.user_id;
                        g_pf_cached_user_ids_by_name[resolved_display_key] = state.user_id;
                    }
                }

                if (state.health <= 0.0f)
                {
                    state.health = 100.0f;
                    state.max_health = 100.0f;
                }
                if (state.address != 0)
                {
                    next_players.emplace_back(std::move(state));
                }
            }
        }

        if (is_img)
        {
            auto img_models = collect_workspace_player_models();
            for (const auto& model : img_models)
            {
                if (!model.is_valid())
                    continue;
                if (local_snapshot.character.is_valid() && model.get_address() == local_snapshot.character.get_address())
                    continue;

                player_state state{};
                state.clear();
                state.character = model;
                state.address = model.get_address();
                state.name = model.get_name();
                state.display_name = state.name;
                state.team = 0;
                state.humanoid = rbx::humanoid::find_humanoid(model);
                update_character_parts(state, model);
                if (state.humanoid.is_valid())
                {
                    if (const auto health_value = rbx::humanoid::get_health(state.humanoid))
                    {
                        state.health = *health_value;
                    }
                    if (const auto max_health_value = rbx::humanoid::get_max_health(state.humanoid))
                    {
                        state.max_health = *max_health_value;
                    }
                }
                if (state.health <= 0.0f)
                {
                    state.health = 100.0f;
                    state.max_health = 100.0f;
                }
                if (state.address != 0)
                {
                    next_players.emplace_back(std::move(state));
                }
            }
        }

        const auto dummy_model = globals->workspace.find_first_child("Dummy");
        if (dummy_model.is_valid())
        {
            fill_dummy_state(next_dummy, dummy_model);
        }

        const auto bots_folder = globals->workspace.find_first_child("Bots");
        if (bots_folder.is_valid())
        {
            const auto bot_children = get_children_safely(bots_folder, "workspace.bots");
            for (const auto& bot_child : bot_children)
            {
                if (!bot_child.is_valid())
                {
                    continue;
                }
                if (bot_child.get_class_name() != "Model")
                {
                    continue;
                }

                cache::dummy_state bot_state{};
                fill_dummy_state(bot_state, bot_child);
                if (bot_state.address == 0)
                {
                    continue;
                }
                if (next_dummy.address != 0 && bot_state.address == next_dummy.address)
                {
                    continue;
                }

                next_players.emplace_back(make_npc_player_state(bot_state));
            }
        }

        static double last_mesh_refresh = 0.0;
        static std::size_t last_mesh_count = 0;
        static std::vector<std::uint64_t> last_mesh_ids;
        const double mesh_now = now_seconds();
        const std::size_t mesh_count = next_players.size() + (next_dummy.address != 0 ? 1u : 0u);
        const bool refresh_mesh = (mesh_count != last_mesh_count) || (mesh_now - last_mesh_refresh >= 0.5);
        if (refresh_mesh)
        {
            last_mesh_ids = collect_mesh_ids_from_states(next_players, next_dummy);
            last_mesh_refresh = mesh_now;
            last_mesh_count = mesh_count;
        }

        auto next_players_ptr = std::make_shared<std::vector<player_state>>(std::move(next_players));
        auto next_dummy_ptr = std::make_shared<dummy_state>(std::move(next_dummy));

        {
            std::scoped_lock lock(mutex);
            players = std::move(next_players_ptr);
            dummy = std::move(next_dummy_ptr);
        }

        if (refresh_mesh)
        {
            schedule_mesh_refresh_if_needed(last_mesh_ids);
        }
    }

    void player_cache::fill_player_state(player_state& out_state, const rbx::instance_t& player, const player_state* previous_state)
    {
        out_state.clear();

        if (!player.is_valid())
        {
            return;
        }

        out_state.player = player;
        out_state.address = player.get_address();
        if (previous_state && previous_state->address == out_state.address)
        {
            out_state.name = previous_state->name;
            out_state.display_name = previous_state->display_name;
            out_state.user_id = previous_state->user_id;
        }

        if (out_state.name.empty())
        {
            out_state.name = player.get_name();
        }

        if (out_state.display_name.empty())
        {
            if (const auto display_name_value = rbx::player::get_display_name(player))
            {
                out_state.display_name = *display_name_value;
            }
        }

        if (out_state.display_name.empty())
        {
            out_state.display_name = out_state.name;
        }

        if (out_state.user_id == 0)
        {
            if (const auto user_id_value = rbx::player::get_user_id(player))
            {
                out_state.user_id = *user_id_value;
            }
        }

        if (const auto team_value = rbx::player::get_team(player))
        {
            out_state.team = *team_value;
        }

        const bool is_arsenal = (globals->game_id == arsenal_place_id);
        const bool is_phantom_forces = (globals->game_id == phantom_forces_place_id);
        const bool is_lostfront = (globals->game_id == lostfront_place_id);

        out_state.character = resolve_character(player, out_state.name);
        const std::uintptr_t current_character = out_state.character.get_address();
        const double now = now_seconds();
        bool reused_parts = false;
        if (previous_state && current_character != 0 &&
            previous_state->character.get_address() == current_character)
        {
            const auto& previous_parts = previous_state->parts;
            if (previous_parts.head.instance.is_valid() || previous_parts.humanoid_root_part.instance.is_valid())
            {
                out_state.parts = previous_parts;
                out_state.last_parts_refresh = previous_state->last_parts_refresh;
                reused_parts = true;
            }
        }

        const bool refresh_due = (out_state.last_parts_refresh <= 0.0)
            || ((now - out_state.last_parts_refresh) >= k_parts_refresh_interval);
        if (!reused_parts || refresh_due)
        {
            update_character_parts(out_state, out_state.character);
            if (out_state.character.is_valid())
            {
                out_state.last_parts_refresh = now;
            }
        }
        if (is_phantom_forces)
        {
            if (const auto pf_name = read_pf_name_from_billboard(out_state.character, &out_state.parts))
            {
                out_state.name = *pf_name;
                out_state.display_name = *pf_name;
            }
        }
        if (is_lostfront)
        {
            out_state.has_team_billboard = head_has_billboard_gui(out_state.parts);
        }

        out_state.humanoid = rbx::humanoid::find_humanoid(out_state.character);
        if (out_state.humanoid.is_valid())
        {
            if (const auto health_value = rbx::humanoid::get_health(out_state.humanoid))
            {
                out_state.health = *health_value;
            }

            if (const auto max_health_value = rbx::humanoid::get_max_health(out_state.humanoid))
            {
                out_state.max_health = *max_health_value;
            }

            if (const auto move_direction = rbx::humanoid::get_move_direction(out_state.humanoid))
            {
                out_state.movement.has_move_direction = true;
                out_state.movement.move_direction = *move_direction;
                const float magnitude_squared =
                    move_direction->x * move_direction->x +
                    move_direction->y * move_direction->y +
                    move_direction->z * move_direction->z;
                out_state.movement.moving = magnitude_squared > 1e-4f;
            }

            if (const auto jump_state = rbx::humanoid::get_jump(out_state.humanoid))
            {
                out_state.movement.has_jump_state = true;
                out_state.movement.jumping = *jump_state;
            }
        }

        if (is_arsenal)
        {
            apply_arsenal_health(player, out_state.health, out_state.max_health);
        }

        if (out_state.character.is_valid())
        {
            auto body_effects = out_state.character.find_first_child("BodyEffects");
            out_state.body_effects.container = body_effects;

            if (body_effects.is_valid())
            {
                auto read_bool = [&](std::string_view name, bool& target)
                {
                    const auto child = body_effects.find_first_child(name);
                    if (child.is_valid())
                    {
                        if (const auto value = rbx::value::get_bool(child))
                        {
                            target = *value;
                        }
                    }
                };

                auto read_armor_value = [&](int& target)
                {
                    auto try_set = [&](const rbx::instance_t& child, int& out_target)
                    {
                        if (const auto as_int = rbx::value::get_int(child))
                        {
                            out_target = *as_int;
                            return true;
                        }
                        if (const auto as_num = rbx::value::get_number(child))
                        {
                            out_target = static_cast<int>(std::round(*as_num));
                            return true;
                        }
                        return false;
                    };

                    const auto armor_child = body_effects.find_first_child("Armor");
                    const auto fire_armor_child = body_effects.find_first_child("FireArmor");
                    const auto defense_child = body_effects.find_first_child("Defense");

                    int new_armor = target;
                    if (armor_child.is_valid()) try_set(armor_child, new_armor);
                    if (new_armor <= 0 && fire_armor_child.is_valid()) try_set(fire_armor_child, new_armor);
                    if (new_armor <= 0 && defense_child.is_valid()) try_set(defense_child, new_armor);
                    target = new_armor;
                };

                auto read_int = [&](std::string_view name, int& target)
                {
                    const auto child = body_effects.find_first_child(name);
                    if (child.is_valid())
                    {
                        if (const auto value = rbx::value::get_int(child))
                        {
                            target = *value;
                        }
                    }
                };

                read_bool("Reload", out_state.body_effects.reload);
                read_armor_value(out_state.body_effects.armor);
                read_bool("Grabbed", out_state.body_effects.grabbed);
                read_bool("GunFiring", out_state.body_effects.gun_firing);
                read_bool("K.O", out_state.body_effects.knocked);
                read_bool("Dead", out_state.body_effects.dead);
                read_bool("Cuff", out_state.body_effects.cuffed);
            }
        }
    }

    void player_cache::fill_dummy_state(dummy_state& out_state, const rbx::instance_t& model)
    {
        out_state.clear();
        if (!model.is_valid())
        {
            return;
        }

        out_state.model = model;
        out_state.address = model.get_address();
        out_state.name = model.get_name();
        out_state.character = model;
        update_character_parts(out_state.parts, model);

        const bool is_arsenal = (globals->game_id == arsenal_place_id);

        if (out_state.character.is_valid())
        {
            auto body_effects = out_state.character.find_first_child("BodyEffects");
            out_state.body_effects.container = body_effects;

            if (body_effects.is_valid())
            {
                auto read_bool = [&](std::string_view name, bool& target)
                {
                    const auto child = body_effects.find_first_child(name);
                    if (child.is_valid())
                    {
                        if (const auto value = rbx::value::get_bool(child))
                        {
                            target = *value;
                        }
                    }
                };

                auto read_armor_value = [&](int& target)
                {
                    auto try_set = [&](const rbx::instance_t& child, int& out_target)
                    {
                        if (const auto as_int = rbx::value::get_int(child))
                        {
                            out_target = *as_int;
                            return true;
                        }
                        if (const auto as_num = rbx::value::get_number(child))
                        {
                            out_target = static_cast<int>(std::round(*as_num));
                            return true;
                        }
                        return false;
                    };

                    const auto armor_child = body_effects.find_first_child("Armor");
                    const auto fire_armor_child = body_effects.find_first_child("FireArmor");
                    const auto defense_child = body_effects.find_first_child("Defense");

                    int new_armor = target;
                    if (armor_child.is_valid()) try_set(armor_child, new_armor);
                    if (new_armor <= 0 && fire_armor_child.is_valid()) try_set(fire_armor_child, new_armor);
                    if (new_armor <= 0 && defense_child.is_valid()) try_set(defense_child, new_armor);
                    target = new_armor;
                };

                auto read_int = [&](std::string_view name, int& target)
                {
                    const auto child = body_effects.find_first_child(name);
                    if (child.is_valid())
                    {
                        if (const auto value = rbx::value::get_int(child))
                        {
                            target = *value;
                        }
                    }
                };

                read_bool("Reload", out_state.body_effects.reload);
                read_armor_value(out_state.body_effects.armor);
                read_bool("Grabbed", out_state.body_effects.grabbed);
                read_bool("GunFiring", out_state.body_effects.gun_firing);
                read_bool("K.O", out_state.body_effects.knocked);
                read_bool("Dead", out_state.body_effects.dead);
                read_bool("Cuff", out_state.body_effects.cuffed);
            }
        }

        out_state.humanoid = rbx::humanoid::find_humanoid(model);
        if (out_state.humanoid.is_valid())
        {
            out_state.humanoid_root_part = out_state.parts.humanoid_root_part.instance;
            if (const auto health_value = rbx::humanoid::get_health(out_state.humanoid))
            {
                out_state.health = *health_value;
            }
            if (const auto max_health_value = rbx::humanoid::get_max_health(out_state.humanoid))
            {
                out_state.max_health = *max_health_value;
            }
        }

        if (is_arsenal)
        {
            apply_arsenal_health(model, out_state.health, out_state.max_health);
        }
    }

    void player_cache::update_character_parts(player_state& out_state, const rbx::instance_t& character)
    {
        update_character_parts(out_state.parts, character);
    }

    void player_cache::update_character_parts(character_parts& out_parts, const rbx::instance_t& character)
    {
        out_parts.clear();

        if (!character.is_valid())
        {
            return;
        }

        const auto children = get_children_safely(character, "character");
        std::unordered_map<std::string, rbx::instance_t> part_map;
        part_map.reserve(children.size());
        for (const auto& child : children)
        {
            part_map.emplace(child.get_name(), child);
        }

        auto find_by_name = [&](std::string_view name) -> rbx::instance_t
        {
            auto it = part_map.find(std::string(name));
            if (it != part_map.end())
            {
                return it->second;
            }
            return {};
        };

        const bool has_upper_torso = part_map.find("UpperTorso") != part_map.end();
        const bool has_lower_torso = part_map.find("LowerTorso") != part_map.end();
        out_parts.is_r15 = has_upper_torso || has_lower_torso;
        const bool had_head_before = out_parts.head.instance.is_valid();
        const bool had_root_before = out_parts.humanoid_root_part.instance.is_valid();
        bool pf_fallback_used = false;
        if (out_parts.is_r15)
        {
            fill_part(out_parts.humanoid_root_part, find_by_name("HumanoidRootPart"));
            fill_part(out_parts.head, find_by_name("Head"));
            fill_part(out_parts.upper_torso, find_by_name("UpperTorso"));
            fill_part(out_parts.lower_torso, find_by_name("LowerTorso"));
            fill_part(out_parts.left_upper_arm, find_by_name("LeftUpperArm"));
            fill_part(out_parts.left_lower_arm, find_by_name("LeftLowerArm"));
            fill_part(out_parts.left_hand, find_by_name("LeftHand"));
            fill_part(out_parts.right_upper_arm, find_by_name("RightUpperArm"));
            fill_part(out_parts.right_lower_arm, find_by_name("RightLowerArm"));
            fill_part(out_parts.right_hand, find_by_name("RightHand"));
            fill_part(out_parts.left_upper_leg, find_by_name("LeftUpperLeg"));
            fill_part(out_parts.left_lower_leg, find_by_name("LeftLowerLeg"));
            fill_part(out_parts.left_foot, find_by_name("LeftFoot"));
            fill_part(out_parts.right_upper_leg, find_by_name("RightUpperLeg"));
            fill_part(out_parts.right_lower_leg, find_by_name("RightLowerLeg"));
            fill_part(out_parts.right_foot, find_by_name("RightFoot"));
        }
        else
        {
            fill_part(out_parts.humanoid_root_part, find_by_name("HumanoidRootPart"));
            fill_part(out_parts.head, find_by_name("Head"));
            fill_part(out_parts.torso, find_by_name("Torso"));
            fill_part(out_parts.left_arm, find_by_name("Left Arm"));
            fill_part(out_parts.right_arm, find_by_name("Right Arm"));
            fill_part(out_parts.left_leg, find_by_name("Left Leg"));
            fill_part(out_parts.right_leg, find_by_name("Right Leg"));
        }

        const bool is_phantom_forces = (globals->game_id == phantom_forces_place_id);
        if (is_phantom_forces)
        {
            const rbx::Vector3 pf_head_size(1.0f, 1.0f, 1.0f);
            const rbx::Vector3 pf_torso_size(2.0f, 2.0f, 1.0f);
            const rbx::Vector3 pf_limb_size(1.0f, 2.0f, 1.0f);

            std::vector<rbx::instance_t> part_children;
            part_children.reserve(children.size());
            for (const auto& child : children)
            {
                const std::string cls = child.get_class_name();
                if (cls == "Part" || cls == "MeshPart")
                {
                    part_children.push_back(child);
                }
            }

            rbx::instance_t detected_head;
            rbx::instance_t detected_torso;

            for (const auto& part : part_children)
            {
                auto subchildren = get_children_safely(part, "character.part.children");
                for (const auto& sub : subchildren)
                {
                    const std::string sub_class = sub.get_class_name();
                    if (!detected_head.is_valid() && sub_class == "BillboardGui")
                    {
                        detected_head = part;
                    }
                    else if (!detected_torso.is_valid() && sub_class == "SpotLight")
                    {
                        detected_torso = part;
                    }
                    if (detected_head.is_valid() && detected_torso.is_valid())
                        break;
                }
                if (detected_head.is_valid() && detected_torso.is_valid())
                    break;
            }

            auto try_fill_from_part = [&](cache::primitive_part& slot, const rbx::instance_t& part)
            {
                if (!slot.instance.is_valid() && part.is_valid())
                {
                    fill_part(slot, part);
                    return true;
                }
                return false;
            };

            auto try_fill_from_index = [&](cache::primitive_part& slot, std::size_t idx)
            {
                if (slot.instance.is_valid())
                    return;
                if (idx < part_children.size())
                {
                    fill_part(slot, part_children[idx]);
                }
            };

            bool head_from_hint = try_fill_from_part(out_parts.head, detected_head);
            bool torso_from_hint = try_fill_from_part(out_parts.torso, detected_torso);
            if (torso_from_hint)
            {
                try_fill_from_part(out_parts.humanoid_root_part, detected_torso);
            }
            try_fill_from_part(out_parts.humanoid_root_part, detected_head);

            if (!head_from_hint)
                try_fill_from_index(out_parts.head, 0);
            if (!out_parts.humanoid_root_part.instance.is_valid())
                try_fill_from_index(out_parts.humanoid_root_part, part_children.size() > 1 ? 1 : 0);
            if (!out_parts.is_r15)
            {
                if (!torso_from_hint)
                    try_fill_from_index(out_parts.torso, part_children.size() > 2 ? 2 : (part_children.size() > 1 ? 1 : 0));
            }

            pf_fallback_used = (!had_head_before && out_parts.head.instance.is_valid()) ||
                (!had_root_before && out_parts.humanoid_root_part.instance.is_valid());

            auto ensure_size = [&](cache::primitive_part& slot, const rbx::Vector3& fallback)
            {
                const bool invalid = slot.size.x <= 0.01f || slot.size.y <= 0.01f || slot.size.z <= 0.01f
                    || !std::isfinite(slot.size.x) || !std::isfinite(slot.size.y) || !std::isfinite(slot.size.z);
                if (invalid)
                {
                    slot.size = fallback;
                }
            };
            ensure_size(out_parts.head, pf_head_size);
            ensure_size(out_parts.torso, pf_torso_size);
            ensure_size(out_parts.humanoid_root_part, pf_torso_size);

            std::unordered_set<std::uintptr_t> used;
            auto mark_used = [&](const cache::primitive_part& slot)
            {
                if (slot.instance.is_valid())
                    used.insert(slot.instance.get_address());
            };
            mark_used(out_parts.head);
            mark_used(out_parts.torso);
            mark_used(out_parts.humanoid_root_part);

            auto assign_next = [&](cache::primitive_part& slot, const rbx::Vector3& fallback_size)
            {
                if (slot.instance.is_valid())
                    return;
                for (const auto& part : part_children)
                {
                    const auto addr = part.get_address();
                    if (addr == 0 || used.find(addr) != used.end())
                        continue;
                    fill_part(slot, part);
                    used.insert(addr);
                    ensure_size(slot, fallback_size);
                    break;
                }
            };

            assign_next(out_parts.left_arm, pf_limb_size);
            assign_next(out_parts.right_arm, pf_limb_size);
            assign_next(out_parts.left_leg, pf_limb_size);
            assign_next(out_parts.right_leg, pf_limb_size);

            assign_next(out_parts.left_upper_arm, pf_limb_size);
            assign_next(out_parts.right_upper_arm, pf_limb_size);
            assign_next(out_parts.left_lower_arm, pf_limb_size);
            assign_next(out_parts.right_lower_arm, pf_limb_size);
            assign_next(out_parts.left_upper_leg, pf_limb_size);
            assign_next(out_parts.right_upper_leg, pf_limb_size);
            assign_next(out_parts.left_lower_leg, pf_limb_size);
            assign_next(out_parts.right_lower_leg, pf_limb_size);
            assign_next(out_parts.left_foot, pf_limb_size);
            assign_next(out_parts.right_foot, pf_limb_size);
        }

        (void)pf_fallback_used;
    }

    std::vector<std::uint64_t> collect_player_mesh_asset_ids()
    {
        std::vector<std::uint64_t> result;
        std::unordered_set<std::uint64_t> unique_ids;

        auto try_add_from_part = [&](const cache::primitive_part& part)
        {
            if (!part.instance.is_valid())
            {
                return;
            }

            const auto mesh_id = rbx::mesh_part::get_mesh_asset_id(part.instance);

            if (mesh_id && unique_ids.insert(*mesh_id).second)
            {
                result.push_back(*mesh_id);
            }
        };

        auto collect_from_parts = [&](const cache::character_parts& parts)
        {
            const cache::primitive_part* part_list[] = {
                &parts.head, &parts.torso, &parts.upper_torso, &parts.lower_torso, &parts.humanoid_root_part,
                &parts.left_arm, &parts.right_arm, &parts.left_leg, &parts.right_leg,
                &parts.left_upper_arm, &parts.left_lower_arm, &parts.left_hand,
                &parts.right_upper_arm, &parts.right_lower_arm, &parts.right_hand,
                &parts.left_upper_leg, &parts.left_lower_leg, &parts.left_foot,
                &parts.right_upper_leg, &parts.right_lower_leg, &parts.right_foot
            };

            for (const auto* part : part_list)
            {
                if (!part)
                    continue;
                try_add_from_part(*part);
            }
        };

        const auto players_snapshot = players_cache->snapshot();
        if (players_snapshot)
        {
            for (const auto& player : *players_snapshot)
            {
                collect_from_parts(player.parts);
            }
        }

        const auto dummy = players_cache->dummy_snapshot();
        if (dummy && dummy->address != 0)
        {
            collect_from_parts(dummy->parts);
        }

        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    void download_mesh_assets_to_temp()
    {
        {
            std::lock_guard<std::mutex> lock(g_mesh_refresh_mutex);
            if (g_mesh_refresh_running)
            {
                return;
            }
            g_mesh_refresh_running = true;
        }

        struct mesh_refresh_guard
        {
            ~mesh_refresh_guard()
            {
                std::lock_guard<std::mutex> lock(g_mesh_refresh_mutex);
                g_mesh_refresh_running = false;
            }
        } guard;

        const auto ids = collect_player_mesh_asset_ids();
        if (ids.empty())
        {
            //logger_core::log_warning("No mesh asset ids collected; skipping download");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mesh_refresh_mutex);
            g_last_mesh_ids = ids;
        }

        const auto dir = mesh_temp_directory();

        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
        }

        int downloaded = 0;
        int failed = 0;
        for (std::uint64_t id : ids)
        {
            std::filesystem::path file_path = mesh_temp_path(id);
            if (download_mesh_to_file(id, file_path))
            {
                ++downloaded;
                {
                    std::lock_guard<std::mutex> lock(g_mesh_cache_mutex);
                    g_mesh_cache.erase(id);
                }

                rbx::mesh_parse::mesh_data parsed;
                (void)get_mesh_data(id, parsed);
            }
            else
            {
                ++failed;
            }
        }

        //logger_core::log_info("Downloaded {} / {} meshes to {} (failed {})", downloaded, static_cast<int>(ids.size()), dir.string(), failed);
    }

    bool get_mesh_data(std::uint64_t asset_id, rbx::mesh_parse::mesh_data& out_mesh)
    {
        {
            std::lock_guard<std::mutex> lock(g_mesh_cache_mutex);
            if (auto it = g_mesh_cache.find(asset_id); it != g_mesh_cache.end() && it->second.loaded)
            {
                out_mesh = it->second.data;
                return true;
            }
        }

        const auto path = mesh_temp_path(asset_id);
        if (!std::filesystem::exists(path))
        {
            return false;
        }

        rbx::mesh_parse::mesh_data parsed = rbx::mesh_parse::load_roblox_mesh(path.string(), false);
        if (parsed.vertices.empty() || parsed.indices.empty())
        {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_mesh_cache_mutex);
            g_mesh_cache[asset_id] = mesh_cache_entry{ true, parsed };
        }
        out_mesh = std::move(parsed);
        return true;
    }

    bool ensure_mesh_data(std::uint64_t asset_id, rbx::mesh_parse::mesh_data& out_mesh)
    {
        if (get_mesh_data(asset_id, out_mesh))
        {
            return true;
        }

        const auto path = mesh_temp_path(asset_id);
        if (!std::filesystem::exists(path))
        {
            if (!download_mesh_to_file(asset_id, path))
            {
                return false;
            }
            std::lock_guard<std::mutex> lock(g_mesh_cache_mutex);
            g_mesh_cache.erase(asset_id);
        }

        return get_mesh_data(asset_id, out_mesh);
    }
}
