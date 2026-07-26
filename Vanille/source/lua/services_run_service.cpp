#include "lua/services_run_service.h"

namespace sandbox
{
    run_service::run_service()
        : instance("RunService", "RunService")
    {
    }

    std::shared_ptr<instance> create_run_service()
    {
        return std::make_shared<run_service>();
    }
}
