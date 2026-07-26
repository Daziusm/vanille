#pragma once

#include <simplemaths/SimpleMath.h>

namespace rbx
{
    using Vector2 = DirectX::SimpleMath::Vector2;
    using Vector3 = DirectX::SimpleMath::Vector3;
    using Vector4 = DirectX::SimpleMath::Vector4;
    using Matrix = DirectX::SimpleMath::Matrix;
}

namespace sdk
{
    namespace math
    {
        struct color3
        {
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
        };
    }
}
