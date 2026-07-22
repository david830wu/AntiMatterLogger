/* TestHelperModule.c — logs from a different source file than the test chapter.
 * Its lines carry MODULE "TestHelperModule" and this file's function names,
 * purely because the macros are expanded here. Nothing was registered. */

#include "TestHelperModule.h"
#include "AmcLogger.h"

void helper_module_log_info(int value)
{
    AMC_LOGGER_INFO("HelperEvent", "{\"value\":%d}", value);
    (void)value;   /* a compile-stripped call (chapter 08) discards its args;
                      this keeps the parameter "used" in stripped builds */
}

void helper_module_log_error_no_payload(void)
{
    AMC_LOGGER_ERROR("HelperError");
}
