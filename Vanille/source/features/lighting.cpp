#include "features/lighting.h"

#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <algorithm>

#include "globals/globals.h"
#include "memory/memory.h"
#include "sdk/offsets.h"
#include "utils/logger.h"

namespace
{
    constexpr const char* k_default_sun_texture = "rbxassetid://6239837869";
    constexpr const char* k_default_moon_texture = "rbxassetid://6239837869";

    bool is_valid_ptr(std::uintptr_t ptr)
    {
        constexpr std::uintptr_t k_min = 0x10000;
        constexpr std::uintptr_t k_max = 0x00007FFFFFFFFFFF;
        return ptr >= k_min && ptr <= k_max;
    }

    bool write_value(std::uintptr_t address, float value)
    {
        return memory->write<float>(address, value);
    }

    bool write_vector3(std::uintptr_t address, const rbx::Vector3& value)
    {
        return memory->write<rbx::Vector3>(address, value);
    }

    struct lighting_defaults
    {
        bool brightness_cached = false;
        float brightness = 0.0f;

        bool ambient_cached = false;
        rbx::Vector3 ambient{};

        bool outdoor_ambient_cached = false;
        rbx::Vector3 outdoor_ambient{};

        bool fog_color_cached = false;
        rbx::Vector3 fog_color{};

        bool fog_start_cached = false;
        float fog_start = 0.0f;

        bool fog_end_cached = false;
        float fog_end = 0.0f;

        bool exposure_compensation_cached = false;
        float exposure_compensation = 0.0f;

        bool colorshift_top_cached = false;
        rbx::Vector3 colorshift_top{};

        bool colorshift_bottom_cached = false;
        rbx::Vector3 colorshift_bottom{};

        bool star_count_cached = false;
        int star_count = 0;

        bool sun_texture_cached = false;
        std::string sun_texture{};

        bool moon_texture_cached = false;
        std::string moon_texture{};

        bool skybox_cached = false;
        std::array<std::string, 6> skybox_values{};

    };

        lighting_defaults defaults;

    std::atomic_bool sky_loop_running{ false };
    std::thread sky_loop_thread;
    std::mutex lighting_apply_mutex;
    constexpr auto k_sky_loop_sleep_granularity = std::chrono::milliseconds(100);

    constexpr std::size_t k_skybox_face_count = 6;

    struct skybox_preset
    {
        const char* name;
        std::array<const char*, k_skybox_face_count> faces;
    };

    constexpr std::array<skybox_preset, 5> k_skybox_presets = { {
        {
            "Default",
            {
                "",
                "",
                "",
                "",
                "",
                ""
            }
        },
        {
            "Aurora Blue",
            {
                "rbxassetid://15983968922",
                "rbxassetid://15983966825",
                "rbxassetid://15983965025",
                "rbxassetid://15983967420",
                "rbxassetid://15983966246",
                "rbxassetid://15983964246"
            }
        },
        {
            "Nebula",
            {
                "rbxassetid://159454299",
                "rbxassetid://159454296",
                "rbxassetid://159454293",
                "rbxassetid://159454286",
                "rbxassetid://159454288",
                "rbxassetid://159454300"
            }
        },
        {
            "Realistic",
            {
                "rbxassetid://144933338",
                "rbxassetid://144931530",
                "rbxassetid://144933262",
                "rbxassetid://144933244",
                "rbxassetid://144933299",
                "rbxassetid://144931564"
            }
        },
        {
            "Dark",
            {
                "rbxassetid://17359299523",
                "rbxassetid://17359302440",
                "rbxassetid://17359305344",
                "rbxassetid://17359309400",
                "rbxassetid://17359311050",
                "rbxassetid://17359315951"
            }
        }
    } };

    bool has_active_lighting_override()
    {
        return features
            && (features->lighting_enable_brightness
                || features->lighting_enable_ambient
                || features->lighting_enable_outdoor_ambient
                || features->lighting_enable_fog_color
                || features->lighting_enable_fog_start
                || features->lighting_enable_fog_end
                || features->lighting_enable_exposure_compensation
                || features->lighting_enable_colorshift_top
                || features->lighting_enable_colorshift_bottom
                || features->lighting_enable_skybox
                || features->lighting_enable_sun_texture
                || features->lighting_enable_moon_texture);
    }

    float get_reapply_interval_seconds()
    {
        if (!features)
        {
            return 1.0f;
        }

        return std::clamp(features->lighting_reapply_interval_seconds, 0.1f, 10.0f);
    }

    rbx::instance_t resolve_sky_instance()
    {
        if (!globals->lighting.is_valid())
        {
            return {};
        }

        auto sky_instance = globals->lighting.find_first_child_by_class("Sky");
        if (sky_instance.is_valid() && is_valid_ptr(sky_instance.get_address()))
        {
            return sky_instance;
        }

        if (roblox::offsets::lighting::sky)
        {
            const auto sky_ptr = memory->read<std::uintptr_t>(
                globals->lighting.get_address() + roblox::offsets::lighting::sky);
            if (is_valid_ptr(sky_ptr))
            {
                return rbx::instance_t(sky_ptr);
            }
        }

        return {};
    }

    void ensure_sky_loop_started()
    {
        if (sky_loop_running.exchange(true))
        {
            return;
        }

        sky_loop_thread = std::thread([]()
        {
            while (sky_loop_running.load(std::memory_order_relaxed))
            {
                if (has_active_lighting_override())
                {
                    lighting::apply();
                    lighting::force_renderview_flag();
                }

                const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<float>(get_reapply_interval_seconds());
                while (sky_loop_running.load(std::memory_order_relaxed))
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline)
                    {
                        break;
                    }

                    const auto remaining = deadline - now;
                    const auto sleep_for = remaining < k_sky_loop_sleep_granularity ? remaining : k_sky_loop_sleep_granularity;
                    std::this_thread::sleep_for(sleep_for);
                }
            }
        });
    }

}

namespace lighting
{
    void apply()
    {
        ensure_sky_loop_started();
        std::lock_guard<std::mutex> apply_guard(lighting_apply_mutex);

        if (!globals->datamodel.is_valid())
        {
            return;
        }

        if (!globals->lighting.is_valid())
        {
            globals->lighting = globals->datamodel.find_first_child_by_class("Lighting");
        }

        if (!globals->lighting.is_valid())
        {
            return;
        }

        const auto lighting_base = globals->lighting.get_address();

        if (roblox::offsets::lighting::brightness)
        {
            const auto brightness_addr = lighting_base + roblox::offsets::lighting::brightness;
            if (features->lighting_enable_brightness)
            {
                if (!defaults.brightness_cached)
                {
                    defaults.brightness = memory->read<float>(brightness_addr);
                    defaults.brightness_cached = true;
                }
                write_value(brightness_addr, features->lighting_brightness_value);
            }
            else if (defaults.brightness_cached)
            {
                write_value(brightness_addr, defaults.brightness);
                defaults.brightness_cached = false;
            }
        }

        if (roblox::offsets::lighting::ambient)
        {
            const auto ambient_addr = lighting_base + roblox::offsets::lighting::ambient;
            if (features->lighting_enable_ambient)
            {
                if (!defaults.ambient_cached)
                {
                    defaults.ambient = memory->read<rbx::Vector3>(ambient_addr);
                    defaults.ambient_cached = true;
                }
                write_vector3(ambient_addr, features->lighting_ambient_color);
            }
            else if (defaults.ambient_cached)
            {
                write_vector3(ambient_addr, defaults.ambient);
                defaults.ambient_cached = false;
            }
        }

        if (roblox::offsets::lighting::outdoor_ambient)
        {
            const auto outdoor_addr = lighting_base + roblox::offsets::lighting::outdoor_ambient;
            if (features->lighting_enable_outdoor_ambient)
            {
                if (!defaults.outdoor_ambient_cached)
                {
                    defaults.outdoor_ambient = memory->read<rbx::Vector3>(outdoor_addr);
                    defaults.outdoor_ambient_cached = true;
                }
                write_vector3(outdoor_addr, features->lighting_outdoor_ambient_color);
            }
            else if (defaults.outdoor_ambient_cached)
            {
                write_vector3(outdoor_addr, defaults.outdoor_ambient);
                defaults.outdoor_ambient_cached = false;
            }
        }

        if (roblox::offsets::lighting::fog_color)
        {
            const auto fog_color_addr = lighting_base + roblox::offsets::lighting::fog_color;
            if (features->lighting_enable_fog_color)
            {
                if (!defaults.fog_color_cached)
                {
                    defaults.fog_color = memory->read<rbx::Vector3>(fog_color_addr);
                    defaults.fog_color_cached = true;
                }
                write_vector3(fog_color_addr, features->lighting_fog_color);
            }
            else if (defaults.fog_color_cached)
            {
                write_vector3(fog_color_addr, defaults.fog_color);
                defaults.fog_color_cached = false;
            }
        }

        if (roblox::offsets::lighting::fog_start)
        {
            const auto fog_start_addr = lighting_base + roblox::offsets::lighting::fog_start;
            if (features->lighting_enable_fog_start)
            {
                if (!defaults.fog_start_cached)
                {
                    defaults.fog_start = memory->read<float>(fog_start_addr);
                    defaults.fog_start_cached = true;
                }
                write_value(fog_start_addr, features->lighting_fog_start);
            }
            else if (defaults.fog_start_cached)
            {
                write_value(fog_start_addr, defaults.fog_start);
                defaults.fog_start_cached = false;
            }
        }

        if (roblox::offsets::lighting::fog_end)
        {
            const auto fog_end_addr = lighting_base + roblox::offsets::lighting::fog_end;
            if (features->lighting_enable_fog_end)
            {
                if (!defaults.fog_end_cached)
                {
                    defaults.fog_end = memory->read<float>(fog_end_addr);
                    defaults.fog_end_cached = true;
                }
                write_value(fog_end_addr, features->lighting_fog_end);
            }
            else if (defaults.fog_end_cached)
            {
                write_value(fog_end_addr, defaults.fog_end);
                defaults.fog_end_cached = false;
            }
        }

        if (roblox::offsets::lighting::exposure_compensation)
        {
            const auto exposure_addr = lighting_base + roblox::offsets::lighting::exposure_compensation;
            if (features->lighting_enable_exposure_compensation)
            {
                if (!defaults.exposure_compensation_cached)
                {
                    defaults.exposure_compensation = memory->read<float>(exposure_addr);
                    defaults.exposure_compensation_cached = true;
                }
                write_value(exposure_addr, features->lighting_exposure_compensation_value);
            }
            else if (defaults.exposure_compensation_cached)
            {
                write_value(exposure_addr, defaults.exposure_compensation);
                defaults.exposure_compensation_cached = false;
            }
        }

        if (roblox::offsets::lighting::colorshift_top)
        {
            const auto color_shift_top_addr = lighting_base + roblox::offsets::lighting::colorshift_top;
            if (features->lighting_enable_colorshift_top)
            {
                if (!defaults.colorshift_top_cached)
                {
                    defaults.colorshift_top = memory->read<rbx::Vector3>(color_shift_top_addr);
                    defaults.colorshift_top_cached = true;
                }
                write_vector3(color_shift_top_addr, features->lighting_colorshift_top);
            }
            else if (defaults.colorshift_top_cached)
            {
                write_vector3(color_shift_top_addr, defaults.colorshift_top);
                defaults.colorshift_top_cached = false;
            }
        }

        if (roblox::offsets::lighting::colorshift_bottom)
        {
            const auto color_shift_bottom_addr = lighting_base + roblox::offsets::lighting::colorshift_bottom;
            if (features->lighting_enable_colorshift_bottom)
            {
                if (!defaults.colorshift_bottom_cached)
                {
                    defaults.colorshift_bottom = memory->read<rbx::Vector3>(color_shift_bottom_addr);
                    defaults.colorshift_bottom_cached = true;
                }
                write_vector3(color_shift_bottom_addr, features->lighting_colorshift_bottom);
            }
            else if (defaults.colorshift_bottom_cached)
            {
                write_vector3(color_shift_bottom_addr, defaults.colorshift_bottom);
                defaults.colorshift_bottom_cached = false;
            }
        }

        const bool sky_offsets_available =
            roblox::offsets::sky::skybox_bk &&
            roblox::offsets::sky::skybox_dn &&
            roblox::offsets::sky::skybox_ft &&
            roblox::offsets::sky::skybox_lf &&
            roblox::offsets::sky::skybox_rt &&
            roblox::offsets::sky::skybox_up;

        const rbx::instance_t sky_instance = resolve_sky_instance();
        const bool sky_valid = sky_instance.is_valid() && is_valid_ptr(sky_instance.get_address());

        if (sky_offsets_available && sky_valid && features->lighting_enable_skybox)
        {
            const auto sky_base = sky_instance.get_address();
            const std::array<std::uintptr_t, k_skybox_face_count> sky_addresses = {
                sky_base + roblox::offsets::sky::skybox_bk,
                sky_base + roblox::offsets::sky::skybox_dn,
                sky_base + roblox::offsets::sky::skybox_ft,
                sky_base + roblox::offsets::sky::skybox_lf,
                sky_base + roblox::offsets::sky::skybox_rt,
                sky_base + roblox::offsets::sky::skybox_up
            };

            const int preset_count = static_cast<int>(k_skybox_presets.size());
            const int preset_index = std::clamp(features->lighting_skybox_preset, 0, preset_count - 1);
            const auto& faces = k_skybox_presets[static_cast<std::size_t>(preset_index)].faces;

            if (!defaults.skybox_cached)
            {
                for (std::size_t i = 0; i < k_skybox_face_count; ++i)
                {
                    defaults.skybox_values[i] = memory->read_string(sky_addresses[i]);
                }
                defaults.skybox_cached = true;
            }

            if (preset_index == 0)
            {
                for (std::size_t i = 0; i < k_skybox_face_count; ++i)
                {
                    const auto addr = sky_addresses[i];
                    if (!is_valid_ptr(addr))
                    {
                        continue;
                    }
                    memory->write_string(addr, defaults.skybox_values[i]);
                }
            }
            else
            {
                for (std::size_t i = 0; i < k_skybox_face_count; ++i)
                {
                    const auto addr = sky_addresses[i];
                    if (!is_valid_ptr(addr))
                    {
                        continue;
                    }
                    memory->write_string(addr, faces[i]);
                }
            }
        }
        else if (defaults.skybox_cached && sky_offsets_available && sky_valid)
        {
            const auto sky_base = sky_instance.get_address();
            const std::array<std::uintptr_t, k_skybox_face_count> sky_addresses = {
                sky_base + roblox::offsets::sky::skybox_bk,
                sky_base + roblox::offsets::sky::skybox_dn,
                sky_base + roblox::offsets::sky::skybox_ft,
                sky_base + roblox::offsets::sky::skybox_lf,
                sky_base + roblox::offsets::sky::skybox_rt,
                sky_base + roblox::offsets::sky::skybox_up
            };
            for (std::size_t i = 0; i < k_skybox_face_count; ++i)
            {
                if (!is_valid_ptr(sky_addresses[i]))
                {
                    continue;
                }
                memory->write_string(sky_addresses[i], defaults.skybox_values[i]);
            }
            defaults.skybox_cached = false;
        }
        else if (!sky_valid)
        {
            defaults.skybox_cached = false;
        }

        if (sky_valid)
        {
            const auto sky_base = sky_instance.get_address();

            if (roblox::offsets::sky::star_count)
            {
                const auto star_addr = sky_base + roblox::offsets::sky::star_count;
                if (features->lighting_enable_skybox)
                {
                    if (!defaults.star_count_cached)
                    {
                        defaults.star_count = memory->read<int>(star_addr);
                        defaults.star_count_cached = true;
                    }
                    memory->write<int>(star_addr, features->lighting_star_count);
                }
                else if (defaults.star_count_cached)
                {
                    memory->write<int>(star_addr, defaults.star_count);
                    defaults.star_count_cached = false;
                }
            }

            if (roblox::offsets::sky::sun_texture_id)
            {
                const auto sun_addr = sky_base + roblox::offsets::sky::sun_texture_id;
                if (!defaults.sun_texture_cached)
                {
                    defaults.sun_texture = memory->read_string(sun_addr);
                    defaults.sun_texture_cached = true;
                }
                if (features->lighting_enable_sun_texture)
                {
                    const std::string& sun_texture = !features->lighting_sun_texture.empty() ? features->lighting_sun_texture : k_default_sun_texture;
                    memory->write_string(sun_addr, sun_texture);
                }
                else if (!defaults.sun_texture.empty())
                {
                    memory->write_string(sun_addr, defaults.sun_texture);
                }
            }

            if (roblox::offsets::sky::moon_texture_id)
            {
                const auto moon_addr = sky_base + roblox::offsets::sky::moon_texture_id;
                if (!defaults.moon_texture_cached)
                {
                    defaults.moon_texture = memory->read_string(moon_addr);
                    defaults.moon_texture_cached = true;
                }
                if (features->lighting_enable_moon_texture)
                {
                    const std::string& moon_texture = !features->lighting_moon_texture.empty() ? features->lighting_moon_texture : k_default_moon_texture;
                    memory->write_string(moon_addr, moon_texture);
                }
                else if (!defaults.moon_texture.empty())
                {
                    memory->write_string(moon_addr, defaults.moon_texture);
                }
            }

            if (features->lighting_enable_skybox
                || features->lighting_enable_sun_texture
                || features->lighting_enable_moon_texture)
            {
                force_renderview_flag();
            }
        }
        else
        {
            defaults.star_count_cached = false;
            defaults.sun_texture_cached = false;
            defaults.moon_texture_cached = false;
        }

        if (has_active_lighting_override())
        {
            force_renderview_flag();
        }

    }

    void force_renderview_flag()
    {
        if (!globals->renderview.is_valid())
        {
            return;
        }

        const auto renderview_addr = globals->renderview.get_address();
        if (roblox::offsets::renderview::force_flag_byte)
        {
            memory->write<bool>(renderview_addr + roblox::offsets::renderview::force_flag_byte, false);
        }
        if (roblox::offsets::renderview::force_flag_bool)
        {
            memory->write<bool>(renderview_addr + roblox::offsets::renderview::force_flag_bool, false);
        }
    }

    void stop()
    {
        if (!sky_loop_running.exchange(false))
        {
            return;
        }

        if (sky_loop_thread.joinable())
        {
            sky_loop_thread.join();
        }
    }
}
