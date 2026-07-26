#include "lua/services_players.h"

namespace sandbox
{
    players_service::players_service()
        : instance("Players", "Players")
    {
    }

    std::shared_ptr<instance> create_players_service()
    {
        return std::make_shared<players_service>();
    }
}
