#pragma once

#include <cstdint>

#include "sdk/instance.h"

namespace cache
{
    struct primitive_part
    {
        rbx::instance_t instance;
        std::uintptr_t primitive = 0;
        rbx::Vector3 size{};
    };

    struct character_parts
    {
        primitive_part head;
        primitive_part torso;
        primitive_part upper_torso;
        primitive_part lower_torso;
        primitive_part humanoid_root_part;
        primitive_part left_arm;
        primitive_part right_arm;
        primitive_part left_leg;
        primitive_part right_leg;
        primitive_part left_upper_arm;
        primitive_part left_lower_arm;
        primitive_part left_hand;
        primitive_part right_upper_arm;
        primitive_part right_lower_arm;
        primitive_part right_hand;
        primitive_part left_upper_leg;
        primitive_part left_lower_leg;
        primitive_part left_foot;
        primitive_part right_upper_leg;
        primitive_part right_lower_leg;
        primitive_part right_foot;
        bool is_r15 = false;

        void clear()
        {
            *this = {};
        }
    };
}
