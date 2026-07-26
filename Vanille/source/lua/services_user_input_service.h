#pragma once

#include "lua/instance.h"

#include <memory>

namespace sandbox
{
    class user_input_service : public instance
    {
    public:
        user_input_service();
    };

    std::shared_ptr<instance> create_user_input_service();
}
