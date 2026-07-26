#pragma once
#include <string>

class c_keybind
{
public:
    enum keybind_mode : int
    {
        TOGGLE,
        HOLD,
        ALWAYS
    };

    bool awaiting_release = false;
    int key = 0;
    bool was_down = false;
    keybind_mode type = TOGGLE;
    const char* name = "keybind";
    bool menu_opened = false;
    bool enabled = false;
    bool waiting_for_input = false;

    explicit c_keybind(const char* _name = "keybind")
        : name(_name)
    {
    }

    void update();
    std::string get_key_name() const;
    std::string get_name() const;
    std::string get_type() const;
    bool set_key();
};
