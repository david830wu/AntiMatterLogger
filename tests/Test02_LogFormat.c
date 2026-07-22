/*
 * ============================================================================
 *  CHAPTER 02 — THE LINE FORMAT, FIELD BY FIELD
 * ============================================================================
 *      [YYYY-MM-DD HH:MM:SS.ssssss][LEVEL][MODULE][FUNCTION][EVENT][ID]PAYLOAD
 *
 *  - Timestamp: wall clock at the call site, microsecond precision, rendered
 *    at the configured fixed UTC offset (default +8). Always 26 characters.
 *  - LEVEL:    DEBUG / INFO / WARN / ERROR / CRITICAL.
 *  - MODULE:   the basename of the calling *source file* (".c"/".h" stripped).
 *    Nothing to register: each file gets its own module automatically.
 *  - FUNCTION: the enclosing function (__func__).
 *  - EVENT:    your PascalCase identifier for the call site. MUST be a string
 *    literal — the library keeps a pointer, not a copy.
 *  - ID:       trader id from the _ID macros, else `-`.
 *  - PAYLOAD:  JSON text you compose with a printf format string. The library
 *    never parses or validates it; by convention keys are quoted. The format
 *    string must be a literal (this is enforced at compile time) and argument
 *    types are checked by the compiler like printf.
 * ============================================================================
 */

#include "TestSupport.h"
#include "TestHelperModule.h"

void setUp(void)   { amc_internal_test_reset(); }
void tearDown(void){ amc_internal_test_reset(); amc_release_all_streams(); }

/* MODULE comes from the source file: this file logs as "Test02_LogFormat",
 * the helper file logs as "TestHelperModule" — automatically. */
static void test_ModuleIsThePerFileIdentity(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_INFO("FromChapterFile");
    helper_module_log_info(7);
    amc_logger_shutdown();
    char *out = release_stdout();

    char line[512];
    amc_assert_log_line(amc_get_line(out, 0, line, sizeof(line)),
                        "INFO", "Test02_LogFormat", __func__,
                        "FromChapterFile", "-", "{}");
    amc_assert_log_line(amc_get_line(out, 1, line, sizeof(line)),
                        "INFO", "TestHelperModule", "helper_module_log_info",
                        "HelperEvent", "-", "{\"value\":7}");
    free(out);
}

/* FUNCTION tracks the enclosing function without any annotation. */
static void log_from_a_named_function(void)
{
    AMC_LOGGER_INFO("InsideNamedFunction");
}

static void test_FunctionFieldIsTheEnclosingFunction(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    log_from_a_named_function();
    amc_logger_shutdown();
    char *out = release_stdout();

    char line[512];
    amc_assert_log_line(amc_get_line(out, 0, line, sizeof(line)),
                        "INFO", "Test02_LogFormat", "log_from_a_named_function",
                        "InsideNamedFunction", "-", "{}");
    free(out);
}

/* The payload is ordinary printf composition — numbers, strings, nesting. */
static void test_PayloadIsPrintfComposed(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_INFO("OrderAccepted",
                    "{\"order\":{\"volume\":%d,\"price\":%.2f},\"tag\":\"%s\"}",
                    1200, 99.5, "opening");
    amc_logger_shutdown();
    char *out = release_stdout();

    char line[512];
    amc_assert_log_line(amc_get_line(out, 0, line, sizeof(line)),
                        "INFO", "Test02_LogFormat", __func__, "OrderAccepted", "-",
                        "{\"order\":{\"volume\":1200,\"price\":99.50},\"tag\":\"opening\"}");
    free(out);
}

/* Negative trader ids print as-is; the id is any int, no validated bound. */
static void test_TraderIdIsAnyInt(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_INFO_ID("OddAccount", -3, "{\"x\":%d}", 1);
    amc_logger_shutdown();
    char *out = release_stdout();

    char line[512];
    amc_assert_log_line(amc_get_line(out, 0, line, sizeof(line)),
                        "INFO", "Test02_LogFormat", __func__,
                        "OddAccount", "-3", "{\"x\":1}");
    free(out);
}

/* Timestamps within one producer are non-decreasing, and the textual format
 * sorts chronologically (a deliberate property of the layout). */
static void test_TimestampsAreNonDecreasingText(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_INFO("First");
    AMC_LOGGER_INFO("Second");
    amc_logger_shutdown();
    char *out = release_stdout();

    char l0[512], l1[512];
    amc_get_line(out, 0, l0, sizeof(l0));
    amc_get_line(out, 1, l1, sizeof(l1));
    TEST_ASSERT_TRUE(memcmp(l0 + 1, l1 + 1, 26) <= 0);
    free(out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ModuleIsThePerFileIdentity);
    RUN_TEST(test_FunctionFieldIsTheEnclosingFunction);
    RUN_TEST(test_PayloadIsPrintfComposed);
    RUN_TEST(test_TraderIdIsAnyInt);
    RUN_TEST(test_TimestampsAreNonDecreasingText);
    return UNITY_END();
}
