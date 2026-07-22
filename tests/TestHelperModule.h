#ifndef AMC_TEST_HELPER_MODULE_H_
#define AMC_TEST_HELPER_MODULE_H_

/* A second source file that logs — used to demonstrate that MODULE is derived
 * per source file automatically, with no registration anywhere. */

void helper_module_log_info(int value);
void helper_module_log_error_no_payload(void);

#endif
