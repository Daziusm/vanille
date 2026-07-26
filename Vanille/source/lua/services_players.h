#pragma once

#include "lua/instance.h"

#include <memory>

namespace sandbox
{
    class players_service : public instance
    {
    public:
        players_service();
    };

    std::shared_ptr<instance> create_players_service();
}
