#include "utils/console.h"
#include "utils/logger.h"
#include "utils/debug_diag.h"
#include <auth/auth.h>

int entry_point();

int main()
{
    console->initialize();
    debug_diag::initialize();
    logger_core::log_info("welcome to vanille!");
    logger_core::log_info("build : {} {}", __DATE__, __TIME__);
    logger_core::log_info("debug log -> {}", debug_diag::log_file_path());

    if (!LoadOffsets())
    {
        logger_core::log_error("couldn't load offsets");
        return 1;
    }

    return entry_point();
}
