#pragma once

#include <string>
#include <vector>

namespace vanille::assistant
{
    struct chat_message
    {
        bool is_user = false;
        std::string text;
    };

    void update();
    bool send_message(const std::string& input);

    const std::vector<chat_message>& messages();
    bool pending();
    const std::string& error();

    bool consume_scroll_to_bottom();
    void clear_messages();
    void clear_error();
}
