#pragma once

#include "lua/instance.h"

#include <memory>

namespace sandbox
{
    class workspace_service : public instance
    {
    public:
        workspace_service();
    };

    std::shared_ptr<instance> create_workspace_service();
}
