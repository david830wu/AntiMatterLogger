/*
 * ============================================================================
 *  CHAPTER 01 — QUICK START
 * ============================================================================
 *  The minimal AmcLogger workflow is three steps:
 *
 *      amc_logger_init(NULL);                     // 1. once, before threads
 *      AMC_LOGGER_INFO("SomeEvent", "{...}", ...) // 2. log from anywhere
 *      amc_logger_shutdown();                     // 3. once, at exit
 *
 *  amc_logger_init(NULL) uses the built-in defaults: stdout sink only, level
 *  INFO, asynchronous mode. Passing a YAML path instead customizes everything
 *  (chapter 04). Because the default mode is asynchronous, lines are written
 *  by a background thread; amc_logger_shutdown() drains every accepted
 *  message before returning, so "log then shutdown" never loses output.
 *
 *  Every line has the fixed format (chapter 02 dissects it):
 *      [YYYY-MM-DD HH:MM:SS.ssssss][LEVEL][MODULE][FUNCTION][EVENT][ID]PAYLOAD
 * ============================================================================
 */

#include "TestSupport.h"

void setUp(void)   { amc_internal_test_reset(); }
void tearDown(void){ amc_internal_test_reset(); amc_release_all_streams(); }

/* The smallest possible program: one line in, one line out. */
static void test_MinimalUse_InitLogShutdown(void)
{
    capture_stdout();
    TEST_ASSERT_EQUAL_INT(0, amc_logger_init(NULL));
    AMC_LOGGER_INFO("HelloLogger", "{\"greeting\":\"%s\"}", "hello");
    TEST_ASSERT_EQUAL_INT(0, amc_logger_shutdown());
    char *out = release_stdout();

    TEST_ASSERT_EQUAL_INT(1, amc_count_lines(out));
    char line[512];
    amc_assert_log_line(amc_get_line(out, 0, line, sizeof(line)),
                        "INFO", "Test01_QuickStart", __func__,
                        "HelloLogger", "-", "{\"greeting\":\"hello\"}");
    free(out);
}

/* There is one macro per severity. The default runtime level is INFO, so the
 * DEBUG call below is filtered out — 5 calls produce 4 lines. */
static void test_OneMacroPerLevel_DefaultLevelIsInfo(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_DEBUG("ShowLevel", "{\"level\":\"%s\"}", "debug");   /* filtered */
    AMC_LOGGER_INFO("PrintPi", "{\"pi\":%.7f}", 3.1415926);
    AMC_LOGGER_WARN("VagueAnswer", "{\"answer\":%d}", 42);
    AMC_LOGGER_ERROR("InstrumentError", "{\"instrument\":\"%06d\"}", 42);
    AMC_LOGGER_CRITICAL("TypeMisMatch", "{\"lhs\":\"%s\",\"rhs\":\"%s\"}",
                        "cat", "fruit");
    amc_logger_shutdown();
    char *out = release_stdout();

    TEST_ASSERT_EQUAL_INT(4, amc_count_lines(out));
    TEST_ASSERT_EQUAL_INT(0, amc_count_lines_containing(out, "[DEBUG]"));

    char line[512];
    amc_assert_log_line(amc_get_line(out, 0, line, sizeof(line)),
                        "INFO", "Test01_QuickStart", __func__,
                        "PrintPi", "-", "{\"pi\":3.1415926}");
    amc_assert_log_line(amc_get_line(out, 1, line, sizeof(line)),
                        "WARN", "Test01_QuickStart", __func__,
                        "VagueAnswer", "-", "{\"answer\":42}");
    amc_assert_log_line(amc_get_line(out, 2, line, sizeof(line)),
                        "ERROR", "Test01_QuickStart", __func__,
                        "InstrumentError", "-", "{\"instrument\":\"000042\"}");
    amc_assert_log_line(amc_get_line(out, 3, line, sizeof(line)),
                        "CRITICAL", "Test01_QuickStart", __func__,
                        "TypeMisMatch", "-", "{\"lhs\":\"cat\",\"rhs\":\"fruit\"}");
    free(out);
}

/* The payload is optional. An event alone renders the empty JSON object {} —
 * the line ends right there. */
static void test_PayloadIsOptional_EmptyRendersEmptyJson(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_WARN("JustAnEvent");
    amc_logger_shutdown();
    char *out = release_stdout();

    char line[512];
    amc_assert_log_line(amc_get_line(out, 0, line, sizeof(line)),
                        "WARN", "Test01_QuickStart", __func__,
                        "JustAnEvent", "-", "{}");
    free(out);
}

/* Every macro has an _ID twin taking a trader id, printed in the last bracket
 * field. Without _ID the field is `-` (not associated with any account). */
static void test_TraderIdVariants(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_INFO_ID("ExtractorInit", 100, "{\"value\":%d}", 42);
    AMC_LOGGER_INFO("NoAccount");
    amc_logger_shutdown();
    char *out = release_stdout();

    char line[512];
    amc_assert_log_line(amc_get_line(out, 0, line, sizeof(line)),
                        "INFO", "Test01_QuickStart", __func__,
                        "ExtractorInit", "100", "{\"value\":42}");
    amc_assert_log_line(amc_get_line(out, 1, line, sizeof(line)),
                        "INFO", "Test01_QuickStart", __func__,
                        "NoAccount", "-", "{}");
    free(out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_MinimalUse_InitLogShutdown);
    RUN_TEST(test_OneMacroPerLevel_DefaultLevelIsInfo);
    RUN_TEST(test_PayloadIsOptional_EmptyRendersEmptyJson);
    RUN_TEST(test_TraderIdVariants);
    return UNITY_END();
}
