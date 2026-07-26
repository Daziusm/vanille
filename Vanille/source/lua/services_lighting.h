#pragma once

#include "lua/instance.h"

#include <memory>

namespace sandbox
{
    class lighting_service : public instance
    {
    public:
        lighting_service();
    };

    std::shared_ptr<instance> create_lighting_service();
}
