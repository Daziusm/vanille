#pragma once

#include "lua/instance.h"

#include <memory>

namespace sandbox
{
    class run_service : public instance
    {
    public:
        run_service();
    };

    std::shared_ptr<instance> create_run_service();
}
