#pragma once

#include <memory>

#include "sdk/instance.h"

namespace rbx
{
    class engine_t final
    {
    public:
        bool initialize();
        void shutdown();
        bool refresh();

        instance_t get_datamodel() const;
        instance_t get_players();
        instance_t get_renderview() const;
        std::uintptr_t get_visualengine() const;

    private:
        std::uintptr_t datamodel = 0;
        bool ready = false;
    };

    inline std::unique_ptr<engine_t> engine = std::make_unique<engine_t>();
}
