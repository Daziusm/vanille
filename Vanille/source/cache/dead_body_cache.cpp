#include "cache/dead_body_cache.h"

#include <chrono>
#include <initializer_list>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "globals/globals_fixed.h"
#include "sdk/engine.h"
#include "sdk/part.h"

namespace
{
    constexpr std::int64_t phantom_forces_place_id = 292439477;
    constexpr std::size_t k_max_children = 2048;
    constexpr std::size_t k_max_dead_bodies = 512;
    constexpr float k_dead_body_lifespan_seconds = 2.5f;

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
        catch (...)
        {
            return {};
        }
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

}

namespace cache
{
    dead_body_cache::~dead_body_cache()
    {
        stop();
    }

    bool dead_body_cache::start()
    {
        if (running.load())
        {
            return true;
        }

        running = true;
        worker = std::thread(&dead_body_cache::run, this);
        return true;
    }

    void dead_body_cache::stop()
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

    std::shared_ptr<const std::vector<dead_body_state>> dead_body_cache::snapshot()
    {
        std::scoped_lock lock(mutex);
        return bodies;
    }

    void dead_body_cache::run()
    {
        while (running.load())
        {
            update_state();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }

    bool dead_body_cache::refresh_core_instances()
    {
        const bool need_refresh = !globals->datamodel.is_valid() || !globals->workspace.is_valid();
        if (need_refresh)
        {
            if (!rbx::engine->refresh())
            {
                return false;
            }
            globals->datamodel = rbx::engine->get_datamodel();
        }

        if (!globals->workspace.is_valid() && globals->datamodel.is_valid())
        {
            globals->workspace = globals->datamodel.find_first_child_by_class("Workspace");
        }

        return globals->datamodel.is_valid() && globals->workspace.is_valid();
    }

    void dead_body_cache::update_body_parts(dead_body_state& out_state, const rbx::instance_t& model)
    {
        out_state.parts.clear();

        if (!model.is_valid())
        {
            return;
        }

        const auto children = get_children_safely(model, "dead_body");
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

        auto find_by_names = [&](std::initializer_list<std::string_view> names) -> rbx::instance_t
        {
            for (const auto& name : names)
            {
                auto part = find_by_name(name);
                if (part.is_valid())
                {
                    return part;
                }
            }
            return {};
        };

        out_state.parts.is_r15 = false;
        fill_part(out_state.parts.head, find_by_names({ "Head" }));
        fill_part(out_state.parts.torso, find_by_names({ "Torso", "UpperTorso", "LowerTorso" }));
        fill_part(out_state.parts.left_arm, find_by_names({ "Left Arm", "LeftArm" }));
        fill_part(out_state.parts.right_arm, find_by_names({ "Right Arm", "RightArm" }));
        fill_part(out_state.parts.left_leg, find_by_names({ "Left Leg", "LeftLeg" }));
        fill_part(out_state.parts.right_leg, find_by_names({ "Right Leg", "RightLeg" }));
    }

    void dead_body_cache::update_state()
    {
        if (!refresh_core_instances())
        {
            auto empty = std::make_shared<std::vector<dead_body_state>>();
            std::scoped_lock lock(mutex);
            bodies = std::move(empty);
            return;
        }

        if (globals->game_id != phantom_forces_place_id)
        {
            auto empty = std::make_shared<std::vector<dead_body_state>>();
            std::scoped_lock lock(mutex);
            bodies = std::move(empty);
            return;
        }

        const auto workspace = globals->workspace;
        if (!workspace.is_valid())
        {
            auto empty = std::make_shared<std::vector<dead_body_state>>();
            std::scoped_lock lock(mutex);
            bodies = std::move(empty);
            return;
        }

        const auto ignore_folder = workspace.find_first_child("Ignore");
        const auto dead_body_folder = ignore_folder.is_valid() ? ignore_folder.find_first_child("DeadBody") : rbx::instance_t{};
        if (!dead_body_folder.is_valid())
        {
            auto empty = std::make_shared<std::vector<dead_body_state>>();
            std::scoped_lock lock(mutex);
            bodies = std::move(empty);
            return;
        }

        const auto children = get_children_safely(dead_body_folder, "dead_body_folder");
        const auto now = std::chrono::steady_clock::now();
        const float fade_duration = features->fade_dormant
            ? (features->dormant_fade_time > 0.0f ? features->dormant_fade_time : 0.0f)
            : 0.35f;
        std::unordered_set<std::uintptr_t> present_addresses;
        present_addresses.reserve(children.size());

        std::vector<dead_body_state> next_bodies;
        const std::size_t reserve_count = children.size() < k_max_dead_bodies ? children.size() : k_max_dead_bodies;
        next_bodies.reserve(reserve_count);

        for (const auto& child : children)
        {
            if (!child.is_valid())
            {
                continue;
            }
            if (child.get_class_name() != "Model")
            {
                continue;
            }
            if (child.get_name() != "Dead")
            {
                continue;
            }

            const std::uintptr_t address = child.get_address();
            if (address == 0)
            {
                continue;
            }
            present_addresses.insert(address);

            auto [it, inserted] = lifetime_map.emplace(address, lifetime_entry{ now, now, false });
            if (!inserted && !it->second.expired)
            {
                const float age = std::chrono::duration<float>(now - it->second.first_seen).count();
                if (age >= k_dead_body_lifespan_seconds)
                {
                    it->second.expired = true;
                    it->second.expired_at = now;
                }
            }

            bool expired = it->second.expired;
            if (expired)
            {
                const float expired_age = std::chrono::duration<float>(now - it->second.expired_at).count();
                if (fade_duration <= 0.0f || expired_age >= fade_duration)
                {
                    continue;
                }
            }

            if (next_bodies.size() >= k_max_dead_bodies)
            {
                continue;
            }

            dead_body_state state{};
            state.model = child;
            state.address = address;
            update_body_parts(state, child);
            state.expired = expired;
            if (state.address != 0)
            {
                next_bodies.push_back(std::move(state));
            }
        }

        for (auto it = lifetime_map.begin(); it != lifetime_map.end();)
        {
            if (present_addresses.find(it->first) == present_addresses.end())
            {
                it = lifetime_map.erase(it);
            }
            else
            {
                ++it;
            }
        }

        auto next_ptr = std::make_shared<std::vector<dead_body_state>>(std::move(next_bodies));
        {
            std::scoped_lock lock(mutex);
            bodies = std::move(next_ptr);
        }
    }
}
