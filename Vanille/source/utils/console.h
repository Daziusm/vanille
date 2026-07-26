#pragma once

#include <memory>

namespace console_core
{
    class console_service final
    {
    public:
        void initialize();
    };
}

inline std::unique_ptr<console_core::console_service> console = std::make_unique<console_core::console_service>();
