#include "lua/services_user_input_service.h"

namespace sandbox
{
    user_input_service::user_input_service()
        : instance("UserInputService", "UserInputService")
    {
    }

    std::shared_ptr<instance> create_user_input_service()
    {
        return std::make_shared<user_input_service>();
    }
}
