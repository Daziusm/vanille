#include "sdk/engine.h"

#include "memory/memory.h"
#include "sdk/offsets.h"
#include "utils/logger.h"
#include <atomic>
#include <chrono>

namespace
{
    constexpr std::int64_t k_engine_log_interval_ms = 2000;

    bool should_log_engine_failure()
    {
        static std::atomic<std::int64_t> last_log_ms{ 0 };
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto last = last_log_ms.load(std::memory_order_relaxed);
        if ((now - last) < k_engine_log_interval_ms)
        {
            return false;
        }
        last_log_ms.store(now, std::memory_order_relaxed);
        return true;
    }

    bool is_likely_datamodel(std::uintptr_t address)
    {
        if (!address)
        {
            return false;
        }

        try
        {
            return rbx::instance_t(address).get_class_name() == "DataModel";
        }
        catch (...)
        {
            return false;
        }
    }

    std::uintptr_t resolve_fake_datamodel_to_datamodel(std::uintptr_t fake_datamodel)
    {
        if (!fake_datamodel)
        {
            return 0;
        }

        const auto datamodel_offset = roblox::offsets::datamodel::datamodel_ptr1
            ? roblox::offsets::datamodel::datamodel_ptr1
            : roblox::offsets::task_scheduler::fake_datamodel_to_datamodel;
        if (!datamodel_offset)
        {
            return 0;
        }

        return memory->read<std::uintptr_t>(fake_datamodel + datamodel_offset);
    }

    std::uintptr_t find_datamodel_from_static_pointer()
    {
        const auto module_base = memory->get_module_address();
        const auto fake_datamodel_pointer = roblox::offsets::datamodel::datamodel_ptr0;
        if (!module_base || !fake_datamodel_pointer)
        {
            return 0;
        }

        const auto fake_datamodel = memory->read<std::uintptr_t>(module_base + fake_datamodel_pointer);
        return resolve_fake_datamodel_to_datamodel(fake_datamodel);
    }

    std::uintptr_t find_datamodel_via_visualengine()
    {
        const auto module_base = memory->get_module_address();
        if (!module_base || !roblox::offsets::visualengine::visualengine_ptr)
        {
            return 0;
        }

        const auto visualengine = memory->read<std::uintptr_t>(module_base + roblox::offsets::visualengine::visualengine_ptr);
        if (!visualengine)
        {
            return 0;
        }

        // roblox-dumper: VisualEngine::FakeDataModel
        constexpr std::uintptr_t k_visualengine_fake_datamodel_offset = 0xA90;
        const auto fake_datamodel = memory->read<std::uintptr_t>(visualengine + k_visualengine_fake_datamodel_offset);
        return resolve_fake_datamodel_to_datamodel(fake_datamodel);
    }

    std::uintptr_t find_render_job()
    {
        if (!memory->is_process_alive())
        {
            return 0;
        }

        const auto module_base = memory->get_module_address();
        if (!module_base)
        {
            if (should_log_engine_failure())
            {
                logger_core::log_error("rbx engine -> module base unavailable");
            }
            return 0;
        }

        const auto scheduler_offset = roblox::offsets::task_scheduler::pointer;
        const auto job_start_offset = roblox::offsets::task_scheduler::job_start;
        const auto job_end_offset = roblox::offsets::task_scheduler::job_end;
        const auto job_name_offset = roblox::offsets::task_scheduler::job_name;
        const auto job_stride = roblox::offsets::task_scheduler::job_stride;
        if (!scheduler_offset || !job_start_offset || !job_end_offset || !job_name_offset || !job_stride)
        {
            if (should_log_engine_failure())
            {
                logger_core::log_warning("rbx engine -> task scheduler offsets not configured");
            }
            return 0;
        }

        const auto scheduler = memory->read<std::uintptr_t>(module_base + scheduler_offset);
        if (!scheduler)
        {
            if (should_log_engine_failure())
            {
                logger_core::log_warning("rbx engine -> failed to read task scheduler pointer @ 0x{:X}", module_base + scheduler_offset);
            }
            return 0;
        }

        const auto job_start = memory->read<std::uintptr_t>(scheduler + job_start_offset);
        const auto job_end = memory->read<std::uintptr_t>(scheduler + job_end_offset);
        if (!job_start || !job_end || job_start >= job_end)
        {
            if (should_log_engine_failure())
            {
                logger_core::log_warning("rbx engine -> invalid task scheduler jobs range (start=0x{:X}, end=0x{:X})", job_start, job_end);
            }
            return 0;
        }

        for (auto job = job_start; job < job_end; job += job_stride)
        {
            const auto job_ptr = memory->read<std::uintptr_t>(job);
            if (!job_ptr)
            {
                continue;
            }

            const auto job_name = memory->read_string(job_ptr + job_name_offset);
            if (job_name == "RenderJob")
            {
                return job_ptr;
            }
        }

        if (should_log_engine_failure())
        {
            logger_core::log_warning("rbx engine -> render job not found");
        }
        return 0;
    }

    std::uintptr_t find_datamodel_address()
    {
        if (!memory->is_process_alive())
        {
            return 0;
        }

        const auto static_datamodel = find_datamodel_from_static_pointer();
        if (is_likely_datamodel(static_datamodel))
        {
            return static_datamodel;
        }

        const auto visualengine_datamodel = find_datamodel_via_visualengine();
        if (is_likely_datamodel(visualengine_datamodel))
        {
            return visualengine_datamodel;
        }

        std::uintptr_t datamodel = 0;
        const auto render_job = find_render_job();
        if (render_job)
        {
            const auto fake_datamodel_offset = roblox::offsets::task_scheduler::render_job_to_fake_datamodel;
            const auto datamodel_offset = roblox::offsets::task_scheduler::fake_datamodel_to_datamodel;
            if (!fake_datamodel_offset || !datamodel_offset)
            {
                if (should_log_engine_failure())
                {
                    logger_core::log_warning("rbx engine -> datamodel offsets not configured");
                }
            }
            else
            {
                const auto fake_datamodel = memory->read<std::uintptr_t>(render_job + fake_datamodel_offset);
                if (!fake_datamodel)
                {
                    if (should_log_engine_failure())
                    {
                        logger_core::log_warning("rbx engine -> failed to read fake datamodel ptr @ 0x{:X}", render_job + fake_datamodel_offset);
                    }
                }
                else
                {
                    datamodel = memory->read<std::uintptr_t>(fake_datamodel + datamodel_offset);
                    if (!datamodel && should_log_engine_failure())
                    {
                        logger_core::log_warning("rbx engine -> failed to read real datamodel ptr @ 0x{:X}", fake_datamodel + datamodel_offset);
                    }
                }

                if (is_likely_datamodel(datamodel))
                {
                    return datamodel;
                }
            }
        }

        if (is_likely_datamodel(static_datamodel))
        {
            return static_datamodel;
        }

        if (is_likely_datamodel(visualengine_datamodel))
        {
            return visualengine_datamodel;
        }

        return datamodel ? datamodel : (static_datamodel ? static_datamodel : visualengine_datamodel);
    }

    std::uintptr_t find_renderview_address()
    {
        if (!memory->is_process_alive())
        {
            return 0;
        }

        const auto render_job = find_render_job();
        if (render_job)
        {
            const auto renderview_offset = roblox::offsets::task_scheduler::render_job_to_renderview;
            if (renderview_offset)
            {
                const auto renderview = memory->read<std::uintptr_t>(render_job + renderview_offset);
                if (renderview)
                {
                    return renderview;
                }

                if (should_log_engine_failure())
                {
                    logger_core::log_warning("rbx engine -> failed to read renderview ptr via render job @ 0x{:X}", render_job + renderview_offset);
                }
            }
            else if (should_log_engine_failure())
            {
                logger_core::log_warning("rbx engine -> renderview offset not configured");
            }
        }

        // Fallback path for newer layouts: VisualEngine + 0xB50.
        constexpr std::uintptr_t k_visualengine_renderview_offset = 0xB50;
        const auto module_base = memory->get_module_address();
        if (!module_base || !roblox::offsets::visualengine::visualengine_ptr)
        {
            if (should_log_engine_failure())
            {
                logger_core::log_warning("rbx engine -> visualengine pointer offset not configured");
            }
            return 0;
        }

        const auto visualengine = memory->read<std::uintptr_t>(module_base + roblox::offsets::visualengine::visualengine_ptr);
        if (!visualengine)
        {
            if (should_log_engine_failure())
            {
                logger_core::log_warning("rbx engine -> failed to read visualengine ptr @ 0x{:X}", module_base + roblox::offsets::visualengine::visualengine_ptr);
            }
            return 0;
        }

        const auto fallback_renderview = memory->read<std::uintptr_t>(visualengine + k_visualengine_renderview_offset);
        if (!fallback_renderview && should_log_engine_failure())
        {
            logger_core::log_warning("rbx engine -> failed to read renderview ptr via visualengine @ 0x{:X}", visualengine + k_visualengine_renderview_offset);
        }

        return fallback_renderview;
    }
}

namespace rbx
{
    bool engine_t::initialize()
    {
        if (ready)
        {
            return true;
        }

        if (!memory->get_process_handle())
        {
            logger_core::log_error("rbx engine -> process handle missing");
            return false;
        }

        if (!refresh())
        {
            return false;
        }

        ready = true;
        return true;
    }

    void engine_t::shutdown()
    {
        ready = false;
        datamodel = 0;
    }

    bool engine_t::refresh()
    {
        if (!memory->is_process_alive())
        {
            datamodel = 0;
            return false;
        }

        const auto new_datamodel = find_datamodel_address();
        if (!new_datamodel)
        {
            if (should_log_engine_failure())
            {
                logger_core::log_error("rbx engine -> failed to locate datamodel");
            }
            return false;
        }

        datamodel = new_datamodel;
        return true;
    }

    instance_t engine_t::get_datamodel() const
    {
        return instance_t(datamodel);
    }

    instance_t engine_t::get_players()
    {
        const auto datamodel_instance = get_datamodel();
        if (!datamodel_instance.is_valid())
        {
            return {};
        }
        return datamodel_instance.find_first_child_by_class("Players");
    }

    instance_t engine_t::get_renderview() const
    {
        return instance_t(find_renderview_address());
    }

    std::uintptr_t engine_t::get_visualengine() const
    {
        const auto module_base = memory->get_module_address();
        if (!module_base || !roblox::offsets::visualengine::visualengine_ptr)
        {
            return 0;
        }
        return memory->read<std::uintptr_t>(module_base + roblox::offsets::visualengine::visualengine_ptr);
    }
}
