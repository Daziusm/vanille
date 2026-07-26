#include "imgui_initialize.h"

namespace imgui_initialize
{
    float main_scale = 1.0f;

    void configure_io(ImGuiIO& io)
    {
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
    }
}
