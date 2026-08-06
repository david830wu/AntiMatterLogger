/* PackageSmokeCpp.cpp — the C++ twin of PackageSmoke.c: a minimal downstream
 * consumer built as C++ against an INSTALLED AntiMatterLogger package. Proves
 * the v2/v3 C++ surface end to end from the install tree: the extern "C"
 * lifecycle, the C++-capable AMC_LOGGER_* / AMC_JSON macros (std::atomic side
 * of the per-language adapters), and linking the installed static library from
 * a C++ target. */

#include "AmcLogger.h"

int main()
{
    if (amc_logger_init(nullptr) != 0)
        return 1;
    AMC_LOGGER_INFO("PackageSmokeCpp", AMC_JSON(AMC_KV_STR("via", "find_package"),
                                                AMC_KV_STR("lang", "c++"),
                                                AMC_KV_STR_ESC("esc", "with \"quotes\""),
                                                AMC_KV_BOOL("exported", true)));
    amc_logger_flush();
    return amc_logger_shutdown();
}
