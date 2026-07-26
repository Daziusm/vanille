#include "lua/services_lighting.h"

namespace sandbox
{
    lighting_service::lighting_service()
        : instance("Lighting", "Lighting")
    {
    }

    std::shared_ptr<instance> create_lighting_service()
    {
        return std::make_shared<lighting_service>();
    }
}
