#include "utils/logger.h"
#include "globals/globals.h"
#include "cache/local_player_cache.h"
#include "cache/player_cache.h"
#include "cache/dead_body_cache.h"
#include "gui/overlay.hpp"
#include "sdk/engine.h"
#include "sdk/offsets.h"
#include "features/lighting.h"
#include "features/aimbot.h"
#include "features/free_aim.h"
#include "features/pf_silent.h"
#include "features/hacks.h"
#include "features/shooter.h"
#include "features/triggerbot.h"
#include "features/visibility.h"
#include "features/tests.h"
#include "utils/window_utils.h"
#include <memory/memory.h>
#include <cstdint>
#include <array>
#include <chrono>
#include <atomic>
#include <thread>
#include <Windows.h>

namespace
{
    std::atomic<bool> teleport_watch_running{ false };
    std::thread teleport_watch_thread;
    std::atomic<bool> tests_loop_running{ false };
    std::thread tests_loop_thread;
    std::uintptr_t last_datamodel_address = 0;
    std::int64_t last_place_id = 0;

    bool sync_globals()
    {
        globals->datamodel = rbx::engine->get_datamodel();
        if (!globals->datamodel.is_valid())
        {
            logger_core::log_warning("sync_globals -> datamodel invalid");
            return false;
        }

        globals->visualengine = rbx::instance_t(rbx::engine->get_visualengine());
        globals->renderview = rbx::engine->get_renderview();
        globals->players = globals->datamodel.find_first_child_by_class("Players");
        globals->workspace = globals->datamodel.find_first_child_by_class("Workspace");
        globals->lighting = globals->datamodel.find_first_child_by_class("Lighting");
        globals->mouse_service = globals->datamodel.find_first_child_by_class("MouseService");
        globals->text_chat_service = globals->datamodel.find_first_child("TextChatService");
        globals->user_input_service = globals->datamodel.find_first_child("UserInputService");
        globals->chat_input_bar_configuration = {};
        if (globals->text_chat_service.is_valid())
        {
            globals->chat_input_bar_configuration = globals->text_chat_service.find_first_child("ChatInputBarConfiguration");
        }
        globals->game_id = globals->datamodel.get_game_id();

        if (!globals->players.is_valid() || !globals->workspace.is_valid())
        {
            logger_core::log_warning(
                "sync_globals -> players=0x{:X}, workspace=0x{:X}, lighting=0x{:X}, mouse_service=0x{:X}",
                globals->players.get_address(),
                globals->workspace.get_address(),
                globals->lighting.get_address(),
                globals->mouse_service.get_address());
            return false;
        }

        return true;
    }

    void log_globals()
    {
        logger_core::log_success("datamodel: 0x{:X}", globals->datamodel.get_address());
        logger_core::log_success("players: 0x{:X}", globals->players.get_address());
        logger_core::log_success("game_id: {}", globals->game_id);
        logger_core::log_success("workspace: 0x{:X}", globals->workspace.get_address());
        logger_core::log_success("renderview: 0x{:X}", globals->renderview.get_address());
        logger_core::log_success("lighting: 0x{:X}", globals->lighting.get_address());
        logger_core::log_success("mouse_service: 0x{:X}", globals->mouse_service.get_address());
        logger_core::log_success("text_chat_service: 0x{:X}", globals->text_chat_service.get_address());
        logger_core::log_success("chat_input_bar_configuration: 0x{:X}", globals->chat_input_bar_configuration.get_address());
        logger_core::log_success("user_input_service: 0x{:X}", globals->user_input_service.get_address());
    }

    void restart_caches()
    {
        cache::localplayer->stop();
        cache::players_cache->stop();
        cache::dead_bodies_cache->stop();
        visibility::reset_occluder_cache();
        cache::localplayer->start();
        cache::players_cache->start();
        cache::dead_bodies_cache->start();
    }

    void start_tp_watch()
    {
        teleport_watch_running = true;
        last_datamodel_address = globals->datamodel.get_address();
        last_place_id = globals->game_id;

        teleport_watch_thread = std::thread([]()
            {
                const auto sleep_interval = std::chrono::milliseconds(750);
                while (teleport_watch_running.load(std::memory_order_relaxed))
                {
                    if (!memory->is_process_alive())
                    {
                        globals->reset();
                        rbx::engine->shutdown();

                        const HWND overlay_window = vanille::overlay::g_overlay_window;
                        if (overlay_window)
                        {
                            ::PostMessage(overlay_window, WM_CLOSE, 0, 0);
                        }
                        break;
                    }

                    if (!rbx::engine->refresh())
                    {
                        globals->reset();
                        std::this_thread::sleep_for(sleep_interval);
                        continue;
                    }

                    if (!sync_globals())
                    {
                        globals->reset();
                        std::this_thread::sleep_for(sleep_interval);
                        continue;
                    }

                    const auto datamodel_addr = globals->datamodel.get_address();
                    const auto place_id = globals->game_id;

                    if (datamodel_addr != last_datamodel_address || place_id != last_place_id)
                    {
                        logger_core::log_info("teleport detected -> datamodel=0x{:X}, place_id={}", datamodel_addr, place_id);
                        last_datamodel_address = datamodel_addr;
                        last_place_id = place_id;

                        lighting::apply();
                        restart_caches();
                        if (globals->mouse_service.is_valid())
                        {
                            std::thread([] { globals->mouse_service.cache_input_object(); }).detach();
                        }
                    }

                    std::this_thread::sleep_for(sleep_interval);
                }
            });
    }

    void stop_tp_watch()
    {
        if (!teleport_watch_running.exchange(false))
        {
            return;
        }

        if (teleport_watch_thread.joinable())
        {
            teleport_watch_thread.join();
        }
    }

    void start_tests_loop()
    {
        tests_loop_running = true;
        tests_loop_thread = std::thread([]()
            {
                const auto sleep_interval = std::chrono::milliseconds(50);
                while (tests_loop_running.load(std::memory_order_relaxed))
                {
                    tests::render();
                    std::this_thread::sleep_for(sleep_interval);
                }
            });
    }

    void stop_tests_loop()
    {
        if (!tests_loop_running.exchange(false))
        {
            return;
        }

        if (tests_loop_thread.joinable())
        {
            tests_loop_thread.join();
        }
    }
}

int entry_point()
{
    static const char* process_name = "RobloxPlayerBeta.exe";

    if (!memory->find_process_id(process_name))
    {
        logger_core::log_error("failed to find process id -> {}", process_name);
        return 0;
    }
    logger_core::log_success("proc: {}", memory->get_process_id());

    if (!memory->attach_to_process(process_name))
    {
        logger_core::log_error("failed to attach to process -> {}", process_name);
        return 0;
    }

    if (!memory->get_process_handle())
    {
        logger_core::log_error("invalid process handle");
        return 0;
    }

    if (!memory->find_module_address(process_name))
    {
        logger_core::log_warning("invalid module address");
        return 0;
    }
    logger_core::log_success("module: {:X}", memory->get_module_address());

    HWND roblox_window = window_utils::find_main_window(memory->get_process_id());
    if (!roblox_window)
    {
        logger_core::log_error("failed to locate roblox window");
        return 0;
    }
    vanille::overlay::g_rbx_window = roblox_window;

    const bool engine_ready = rbx::engine->initialize();
    if (engine_ready)
    {
        if (sync_globals())
        {
            log_globals();
            lighting::apply();
        }
        else
        {
            logger_core::log_warning("failed to populate globals after engine init");
        }
    }
    else
    {
        logger_core::log_warning("rbx engine datamodel not ready at startup - player caches will keep retrying");
    }

    cache::localplayer->start();
    cache::players_cache->start();
    cache::dead_bodies_cache->start();
    hacks::apply();
    std::thread([] { globals->mouse_service.cache_input_object(); }).detach();
    aimbot::start();
    free_aim::start();
    pf_silent::start();
    triggerbot::start();
    shooter::start();
    start_tp_watch();
    start_tests_loop();

    vanille::overlay::run_overlay();
    stop_tests_loop();
    stop_tp_watch();
    hacks::stop();
    aimbot::stop();
    free_aim::stop();
    pf_silent::stop();
    triggerbot::stop();
    shooter::stop();
    lighting::stop();
    cache::localplayer->stop();
    cache::players_cache->stop();
    cache::dead_bodies_cache->stop();
    rbx::engine->shutdown();
    globals->reset();

    return 0;
}
