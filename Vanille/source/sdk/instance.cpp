#include "sdk/instance.h"

#include <cmath>
#include <optional>
#include <atomic>
#include <limits>
#include <thread>
#include <chrono>
#include <immintrin.h>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <cstring>

#include "memory/memory.h"
#include "sdk/offsets.h"
#include "sdk/mouse.h"

namespace rbx
{
    instance_t::instance_t(std::uintptr_t address)
        : address(address)
    {
    }

    instance_t::instance_t(const instance_t& other)
        : address(other.address)
    {
        input_object_cache.store(other.input_object_cache.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    instance_t& instance_t::operator=(const instance_t& other)
    {
        if (this != &other)
        {
            address = other.address;
            input_object_cache.store(other.input_object_cache.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    instance_t::instance_t(instance_t&& other) noexcept
        : address(other.address)
    {
        input_object_cache.store(other.input_object_cache.load(std::memory_order_relaxed), std::memory_order_relaxed);
        other.address = 0;
        other.input_object_cache.store(0, std::memory_order_relaxed);
    }

    instance_t& instance_t::operator=(instance_t&& other) noexcept
    {
        if (this != &other)
        {
            address = other.address;
            input_object_cache.store(other.input_object_cache.load(std::memory_order_relaxed), std::memory_order_relaxed);
            other.address = 0;
            other.input_object_cache.store(0, std::memory_order_relaxed);
        }
        return *this;
    }

    bool instance_t::is_valid() const
    {
        return address != 0;
    }

    std::uintptr_t instance_t::get_address() const
    {
        return address;
    }

    namespace
    {
        struct cached_string
        {
            std::string value;
            double time = 0.0;
        };

        constexpr double k_name_cache_ttl = 0.5;
        constexpr double k_class_cache_ttl = 1.0;
        constexpr std::size_t k_cache_max_entries = 8192;

        std::unordered_map<std::uintptr_t, cached_string> g_name_cache;
        std::unordered_map<std::uintptr_t, cached_string> g_class_cache;
        std::shared_mutex g_string_cache_mutex;

        double now_seconds()
        {
            using clock = std::chrono::steady_clock;
            return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
        }
    }

    std::vector<rbx::instance_t> rbx::instance_t::get_children() const
    {
        std::vector<rbx::instance_t> children;

        if (!is_valid())
        {
            return children;
        }

        constexpr std::size_t k_max_children = 2048;

        try
        {
            const std::uint64_t stride = roblox::offsets::instance::children_stride;
            if (stride == 0)
            {
                return children;
            }

            const std::uint64_t begin = memory->read<std::uint64_t>(this->address + roblox::offsets::instance::children_start);
            if (!begin)
            {
                return children;
            }

            const std::uint64_t end = memory->read<std::uint64_t>(begin + roblox::offsets::instance::children_end);
            std::uint64_t instances = memory->read<std::uint64_t>(begin);
            std::size_t count_limit = k_max_children;

            if (end != 0 && end > instances)
            {
                const std::uint64_t distance = end - instances;
                const std::uint64_t end_limited = distance / stride;
                if (end_limited < count_limit)
                {
                    count_limit = static_cast<std::size_t>(end_limited);
                }
            }

            children.reserve(count_limit);

            const std::size_t total_bytes = count_limit * stride;
            std::vector<std::uint8_t> buffer(total_bytes);
            if (memory->read_raw(buffer.data(), instances, total_bytes))
            {
                for (std::size_t i = 0; i < count_limit; ++i)
                {
                    std::uintptr_t child_ptr = 0;
                    std::memcpy(&child_ptr, buffer.data() + (i * stride), sizeof(child_ptr));
                    if (child_ptr == 0)
                    {
                        continue;
                    }
                    children.emplace_back(child_ptr);
                }
            }
            else
            {
                for (std::size_t i = 0; i < count_limit; ++i)
                {
                    if (!instances)
                    {
                        break;
                    }

                    children.emplace_back(memory->read<rbx::instance_t>(instances));
                    instances += stride;
                }
            }
        }
        catch (...)
        {
            children.clear();
        }

        return children;
    }

    std::vector<rbx::instance_t> rbx::instance_t::get_descendants() const
    {
        std::vector<rbx::instance_t> descendants;

        if (!is_valid())
        {
            return descendants;
        }

        constexpr std::size_t k_max_nodes = 20000;
        std::vector<rbx::instance_t> stack = get_children();
        if (stack.size() > k_max_nodes)
        {
            stack.resize(k_max_nodes);
        }
        descendants.reserve(stack.size());

        std::size_t processed = 0;
        while (!stack.empty() && processed < k_max_nodes)
        {
            rbx::instance_t current = stack.back();
            stack.pop_back();
            ++processed;

            if (!current.is_valid())
            {
                continue;
            }

            descendants.push_back(current);

            auto children = current.get_children();
            if (children.empty())
            {
                continue;
            }

            if (stack.size() + children.size() > k_max_nodes)
            {
                const std::size_t remaining = k_max_nodes - stack.size();
                if (remaining == 0)
                {
                    break;
                }
                if (children.size() > remaining)
                {
                    children.resize(remaining);
                }
            }

            stack.insert(stack.end(), children.begin(), children.end());
        }

        return descendants;
    }

    instance_t instance_t::find_first_child(std::string_view name) const
    {
        if (!is_valid())
        {
            return {};
        }

        for (const auto& child : get_children())
        {
            if (child.get_name() == name)
            {
                return child;
            }
        }
        return {};
    }

    instance_t instance_t::find_first_child_by_class(std::string_view class_name) const
    {
        if (!is_valid())
        {
            return {};
        }

        for (const auto& child : get_children())
        {
            if (child.get_class_name() == class_name)
            {
                return child;
            }
        }
        return {};
    }

    instance_t instance_t::get_current_camera() const
    {
        if (!is_valid() || !roblox::offsets::instance::current_camera)
        {
            return {};
        }

        const auto camera_ptr = memory->read<std::uintptr_t>(address + roblox::offsets::instance::current_camera);
        return instance_t(camera_ptr);
    }

    bool instance_t::IsA(std::string_view class_name) const
    {
        if (!is_valid())
        {
            return false;
        }
        return get_class_name() == class_name;
    }

    std::string instance_t::get_name() const
    {
        if (!is_valid())
        {
            return {};
        }

        const double now = now_seconds();
        {
            std::shared_lock lock(g_string_cache_mutex);
            auto it = g_name_cache.find(address);
            if (it != g_name_cache.end() && (now - it->second.time) <= k_name_cache_ttl && !it->second.value.empty())
            {
                return it->second.value;
            }
        }

        auto read_c_string = [](std::uintptr_t ptr) -> std::string
        {
            if (!ptr)
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

        const auto name_ptr = memory->read<std::uintptr_t>(address + roblox::offsets::instance::name);
        if (name_ptr)
        {
            const auto value = memory->read_string(name_ptr);
            if (value != "Unknown" && !value.empty())
            {
                {
                    std::unique_lock lock(g_string_cache_mutex);
                    if (g_name_cache.size() >= k_cache_max_entries)
                    {
                        g_name_cache.clear();
                    }
                    g_name_cache[address] = cached_string{ value, now };
                }
                return value;
            }

            const auto raw_value = read_c_string(name_ptr);
            if (!raw_value.empty())
            {
                {
                    std::unique_lock lock(g_string_cache_mutex);
                    if (g_name_cache.size() >= k_cache_max_entries)
                    {
                        g_name_cache.clear();
                    }
                    g_name_cache[address] = cached_string{ raw_value, now };
                }
                return raw_value;
            }
        }

        const auto inline_value = memory->read_string(address + roblox::offsets::instance::name);
        if (inline_value != "Unknown")
        {
            if (!inline_value.empty())
            {
                std::unique_lock lock(g_string_cache_mutex);
                if (g_name_cache.size() >= k_cache_max_entries)
                {
                    g_name_cache.clear();
                }
                g_name_cache[address] = cached_string{ inline_value, now };
            }
            return inline_value;
        }

        const auto fallback = read_c_string(address + roblox::offsets::instance::name);
        if (!fallback.empty())
        {
            std::unique_lock lock(g_string_cache_mutex);
            if (g_name_cache.size() >= k_cache_max_entries)
            {
                g_name_cache.clear();
            }
            g_name_cache[address] = cached_string{ fallback, now };
        }
        return fallback;
    }

    std::string instance_t::get_class_name() const
    {
        if (!is_valid())
        {
            return "unknown_class";
        }

        const double now = now_seconds();
        {
            std::shared_lock lock(g_string_cache_mutex);
            auto it = g_class_cache.find(address);
            if (it != g_class_cache.end() && (now - it->second.time) <= k_class_cache_ttl && !it->second.value.empty())
            {
                return it->second.value;
            }
        }

        const auto descriptor = memory->read<std::uintptr_t>(address + roblox::offsets::instance::class_descriptor);
        const auto class_name_ptr = descriptor ? memory->read<std::uintptr_t>(descriptor + roblox::offsets::instance::class_name) : 0;
        if (class_name_ptr)
        {
            const auto value = memory->read_string(class_name_ptr);
            if (!value.empty() && value != "unknown_class")
            {
                std::unique_lock lock(g_string_cache_mutex);
                if (g_class_cache.size() >= k_cache_max_entries)
                {
                    g_class_cache.clear();
                }
                g_class_cache[address] = cached_string{ value, now };
            }
            return value;
        }
        return "unknown_class";
    }

    std::optional<Vector3> instance_t::get_position(std::uintptr_t primitive_override) const
    {
        if (!is_valid() || !roblox::offsets::base_part::position)
        {
            return std::nullopt;
        }

        std::uintptr_t primitive = primitive_override;
        if (!primitive)
        {
            if (!roblox::offsets::base_part::primitive)
            {
                return std::nullopt;
            }

            primitive = memory->read<std::uintptr_t>(address + roblox::offsets::base_part::primitive);
        }

        if (!primitive)
        {
            return std::nullopt;
        }

        const auto position = memory->read<Vector3>(primitive + roblox::offsets::base_part::position);
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        {
            return std::nullopt;
        }

        return position;
    }

    std::uint64_t instance_t::get_cached_input_object() const
    {
        return input_object_cache.load(std::memory_order_relaxed);
    }

    void instance_t::initialise_mouse(std::uintptr_t address) const
    {
        constexpr std::uint64_t invalid_ptr = (std::numeric_limits<std::uint64_t>::max)();
        const auto input_obj = rbx::mouse_service::get_input_object(address);
        if (input_obj != 0 && input_obj != invalid_ptr)
        {
            input_object_cache.store(input_obj, std::memory_order_relaxed);
            const char* base_ptr = reinterpret_cast<const char*>(input_obj);
            _mm_prefetch(base_ptr + roblox::offsets::mouse_service::mouse_position, _MM_HINT_T0);
            _mm_prefetch(base_ptr + roblox::offsets::mouse_service::mouse_position + sizeof(rbx::Vector2), _MM_HINT_T0);
        }
    }

    bool instance_t::set_frame_pos_x(std::uint64_t position) const
    {
        if (!is_valid() || !roblox::offsets::gui_object::frame_position_x)
        {
            return false;
        }

        struct dim { float scale; std::int32_t offset; };
        const auto clamped = position > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())
            ? static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())
            : position;
        dim value{ 0.0f, static_cast<std::int32_t>(clamped) };
        static HANDLE cached_handle = nullptr;
        HANDLE handle = cached_handle ? cached_handle : memory->get_process_handle();
        if (!handle)
        {
            cached_handle = nullptr;
            return false;
        }
        cached_handle = handle;
        return nt_fast_write(handle, address + roblox::offsets::gui_object::frame_position_x, &value, sizeof(value));
    }

    bool instance_t::set_frame_pos_y(std::uint64_t position) const
    {
        if (!is_valid() || !roblox::offsets::gui_object::frame_position_y)
        {
            return false;
        }

        struct dim { float scale; std::int32_t offset; };
        const auto clamped = position > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())
            ? static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())
            : position;
        dim value{ 0.0f, static_cast<std::int32_t>(clamped) };
        static HANDLE cached_handle = nullptr;
        HANDLE handle = cached_handle ? cached_handle : memory->get_process_handle();
        if (!handle)
        {
            cached_handle = nullptr;
            return false;
        }
        cached_handle = handle;
        return nt_fast_write(handle, address + roblox::offsets::gui_object::frame_position_y, &value, sizeof(value));
    }

    bool instance_t::set_base_part_color(std::uint8_t r, std::uint8_t g, std::uint8_t b) const
    {
        if (!is_valid() || !roblox::offsets::base_part::color3)
        {
            return false;
        }

        const sdk::math::color3 color{
            static_cast<float>(r) / 255.0f,
            static_cast<float>(g) / 255.0f,
            static_cast<float>(b) / 255.0f
        };

        return memory->write<sdk::math::color3>(address + roblox::offsets::base_part::color3, color);
    }

    Vector3 instance_t::get_camera_position() const
    {
        if (!is_valid() || !roblox::offsets::camera::position)
        {
            return {};
        }
        return memory->read<Vector3>(address + roblox::offsets::camera::position);
    }

    DirectX::XMFLOAT3X3 instance_t::get_rotation() const
    {
        if (!is_valid() || !roblox::offsets::camera::rotation)
        {
            return {};
        }
        return memory->read<DirectX::XMFLOAT3X3>(address + roblox::offsets::camera::rotation);
    }

    DirectX::XMFLOAT3X3 instance_t::look_at(const Vector3& target) const
    {
        Vector3 forward = (target - get_camera_position());
        forward.Normalize();

        Vector3 up = { 0.0f, 1.0f, 0.0f };

        Vector3 right = up.Cross(forward);
        right.Normalize();
        up = forward.Cross(right);
        up.Normalize();

        DirectX::XMFLOAT3X3 look_at_matrix{};
        look_at_matrix._11 = -right.x;  look_at_matrix._12 = up.x;  look_at_matrix._13 = -forward.x;
        look_at_matrix._21 = right.y;  look_at_matrix._22 = up.y;  look_at_matrix._23 = -forward.y;
        look_at_matrix._31 = -right.z;  look_at_matrix._32 = up.z;  look_at_matrix._33 = -forward.z;
        return look_at_matrix;
    }

    void instance_t::set_rotation(const DirectX::XMFLOAT3X3& rotation) const
    {
        if (!is_valid() || !roblox::offsets::camera::rotation)
        {
            return;
        }
        memory->write<DirectX::XMFLOAT3X3>(address + roblox::offsets::camera::rotation, rotation);
    }

    void instance_t::cache_input_object() const
    {
        constexpr std::uint64_t invalid_ptr = (std::numeric_limits<std::uint64_t>::max)();
        while (true)
        {
            if (address != 0)
            {
                const auto input_obj = rbx::mouse_service::get_input_object(address);
                if (input_obj != 0 && input_obj != invalid_ptr)
                {
                    input_object_cache.store(input_obj, std::memory_order_relaxed);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::int64_t instance_t::get_game_id() const
    {
        if (!is_valid() || !roblox::offsets::datamodel::place_id)
        {
            return 0;
        }
        return memory->read<std::int64_t>(address + roblox::offsets::datamodel::place_id);
    }
}
