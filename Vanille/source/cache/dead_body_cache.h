#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cache/player_parts.h"
#include "sdk/instance.h"

namespace cache
{
    struct dead_body_state
    {
        std::uintptr_t address = 0;
        rbx::instance_t model;
        character_parts parts;
        bool expired = false;

        void clear()
        {
            *this = {};
        }
    };

    class dead_body_cache
    {
    public:
        ~dead_body_cache();

        bool start();
        void stop();
        std::shared_ptr<const std::vector<dead_body_state>> snapshot();

    private:
        struct lifetime_entry
        {
            std::chrono::steady_clock::time_point first_seen{};
            std::chrono::steady_clock::time_point expired_at{};
            bool expired = false;
        };

        void run();
        void update_state();
        bool refresh_core_instances();
        void update_body_parts(dead_body_state& out_state, const rbx::instance_t& model);

        std::atomic<bool> running{ false };
        std::thread worker;
        std::mutex mutex;
        std::shared_ptr<std::vector<dead_body_state>> bodies{ std::make_shared<std::vector<dead_body_state>>() };
        std::unordered_map<std::uintptr_t, lifetime_entry> lifetime_map;
    };

    inline std::unique_ptr<dead_body_cache> dead_bodies_cache = std::make_unique<dead_body_cache>();
}
