/* PackageSmoke.c — a minimal downstream consumer built against an INSTALLED
 * AntiMatterLogger package (see CMakeLists.txt in this directory). Exercises
 * the installed header (including the AMC_JSON composer) and links the
 * installed static library. */

#include "AmcLogger.h"

int main(void)
{
    if (amc_logger_init(NULL) != 0)
        return 1;
    AMC_LOGGER_INFO("PackageSmoke", AMC_JSON(AMC_KV_STR("via", "find_package"),
                                             AMC_KV_STR_ESC("esc", "with \"quotes\""),
                                             AMC_KV_BOOL("exported", 1)));
    return amc_logger_shutdown();
}
