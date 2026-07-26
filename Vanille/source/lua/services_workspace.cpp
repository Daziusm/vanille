#include "lua/services_workspace.h"

namespace sandbox
{
    workspace_service::workspace_service()
        : instance("Workspace", "Workspace")
    {
    }

    std::shared_ptr<instance> create_workspace_service()
    {
        return std::make_shared<workspace_service>();
    }
}
