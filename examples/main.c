/* filename: main.c — the usage example from docs/Design.md §9.
 *
 * Run from the repository root (the config path is relative):
 *     cmake -S . -B build -DAMC_BUILD_EXAMPLES=ON && cmake --build build -j
 *     ./build/amc_example
 *
 * Output goes to stdout and to ./log/$TODAY/MyApp.$PID.log per the config.
 * Note the DEBUG line does not appear: config/logger.yaml keeps module "main"
 * at INFO, while module "LongTermExtractor" is raised to DEBUG. */

#include "AmcLogger.h"
#include "LongTermExtractor.h"

#include <stdio.h>

int main(void)
{
    if (amc_logger_init("config/logger.yaml") != 0) {
        fprintf(stderr, "logger init failed; fix the config and retry\n");
        return 1;
    }

    AMC_LOGGER_DEBUG("ShowLevel", "{\"level\":\"%s\"}", "debug");  /* filtered */
    AMC_LOGGER_INFO("PrintPi", "{\"pi\":%.7f}", 3.1415926);
    AMC_LOGGER_WARN("VagueAnswer", "{\"answer\":%d}", 42);
    AMC_LOGGER_ERROR("InstrumentError", "{\"instrument\":\"%06d\"}", 42);
    AMC_LOGGER_CRITICAL("TypeMisMatch", "{\"lhs\":\"%s\",\"rhs\":\"%s\"}",
                        "cat", "fruit");

    struct LongTermExtractor *p_lte = long_term_extractor_init(100, 42);
    long_term_extractor_close(p_lte);
    long_term_extractor_close(NULL);   /* demonstrates the ERROR path */

    amc_logger_shutdown();
    return 0;
}
