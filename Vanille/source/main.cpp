#include "utils/console.h"
#include "utils/logger.h"
#include <auth/auth.h>

#include <windows.h>

int entry_point();

int main()
{
    console->initialize();
    logger_core::log_info("welcome to vanille!");
    logger_core::log_info("build : {} {}", __DATE__, __TIME__);

    if (!LoadOffsets())
    {
        logger_core::log_error("couldn't load offsets");
#ifndef VANILLE_ENABLE_CONSOLE
        MessageBoxA(
            nullptr,
            "Couldn't load offsets.\n\nPlace values.txt next to vanille.exe.",
            "Vanille",
            MB_OK | MB_ICONERROR);
#endif
        return 1;
    }

    return entry_point();
}
