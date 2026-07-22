/*
 * ============================================================================
 *  CHAPTER 05 — LOGGING TO FILES
 * ============================================================================
 *  The `sinks:` section chooses destinations. Two exist:
 *
 *      sinks:
 *        stdout: {}                                  # the console
 *        basic_file: "./log/$TODAY/MyApp.$PID.log"   # one append-mode file
 *
 *  Listing a sink enables it; every module writes to every listed sink. If
 *  you write a `sinks:` section, it is exact — listing only basic_file means
 *  stdout stays silent. Omitting the section entirely means stdout only.
 *
 *  Path templates support exactly two variables, expanded ONCE when the file
 *  is opened at init: $TODAY (YYYY-MM-DD at the configured UTC offset) and
 *  $PID. Missing directories are created automatically. The file is opened in
 *  append mode, so a restarted process continues the same file.
 * ============================================================================
 */

#include "TestSupport.h"

#include <time.h>

void setUp(void)   { amc_internal_test_reset(); }
void tearDown(void){ amc_internal_test_reset(); amc_release_all_streams(); }

/* an explicit sinks section with only basic_file: the console stays silent */
static void test_FileOnly_StdoutStaysSilent(void)
{
    unlink("out.log");
    amc_write_file("cfg.yaml",
        "sinks:\n"
        "  basic_file: \"./out.log\"\n");
    capture_stdout();
    amc_logger_init("cfg.yaml");
    AMC_LOGGER_INFO("ToFileOnly", "{\"n\":%d}", 1);
    AMC_LOGGER_INFO("AlsoToFile", "{\"n\":%d}", 2);
    amc_logger_shutdown();
    char *console = release_stdout();
    char *file = amc_read_file("out.log");

    TEST_ASSERT_EQUAL_INT(0, amc_count_lines(console));
    TEST_ASSERT_EQUAL_INT(2, amc_count_lines(file));
    char line[512];
    amc_assert_log_line(amc_get_line(file, 0, line, sizeof(line)),
                        "INFO", "Test05_FileSink", __func__,
                        "ToFileOnly", "-", "{\"n\":1}");
    free(console);
    free(file);
}

/* $TODAY and $PID expand at open; intermediate directories appear on demand */
static void test_PathVariablesAndAutoMkdir(void)
{
    amc_write_file("cfg.yaml",
        "utc_offset_hours: 8\n"
        "sinks:\n"
        "  basic_file: \"./deep/$TODAY/nested/app.$PID.log\"\n");

    /* compute the same expansion the library performs */
    time_t shifted = time(NULL) + 8 * 3600;
    struct tm tm;
    gmtime_r(&shifted, &tm);
    char expected[256];
    snprintf(expected, sizeof(expected), "./deep/%04d-%02d-%02d/nested/app.%ld.log",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, (long)getpid());
    unlink(expected);

    TEST_ASSERT_EQUAL_INT(0, amc_logger_init("cfg.yaml"));
    AMC_LOGGER_INFO("IntoDatedDir");
    amc_logger_shutdown();

    char *file = amc_read_file(expected);
    TEST_ASSERT_NOT_NULL_MESSAGE(file, "expanded path was not created");
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines(file));
    free(file);
}

/* append mode: a restarted process continues the same file.
 * (amc_internal_test_reset() simulates the process restart here.) */
static void test_AppendAcrossProcessRuns(void)
{
    unlink("append.log");
    amc_write_file("cfg.yaml",
        "sinks:\n"
        "  basic_file: \"./append.log\"\n");

    amc_logger_init("cfg.yaml");
    AMC_LOGGER_INFO("FirstRun");
    amc_logger_shutdown();

    amc_internal_test_reset();                 /* "restart" */

    amc_logger_init("cfg.yaml");
    AMC_LOGGER_INFO("SecondRun");
    amc_logger_shutdown();

    char *file = amc_read_file("append.log");
    TEST_ASSERT_EQUAL_INT(2, amc_count_lines(file));
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(file, "FirstRun"));
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(file, "SecondRun"));
    free(file);
}

/* both sinks listed: every line goes to both, byte-identical */
static void test_StdoutAndFileBothReceive(void)
{
    unlink("both.log");
    amc_write_file("cfg.yaml",
        "sinks:\n"
        "  stdout: {}\n"
        "  basic_file: \"./both.log\"\n");
    capture_stdout();
    amc_logger_init("cfg.yaml");
    AMC_LOGGER_WARN("Everywhere", "{\"k\":%d}", 9);
    amc_logger_shutdown();
    char *console = release_stdout();
    char *file = amc_read_file("both.log");

    TEST_ASSERT_EQUAL_INT(1, amc_count_lines(console));
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines(file));
    TEST_ASSERT_EQUAL_STRING(console, file);   /* identical bytes */
    free(console);
    free(file);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_FileOnly_StdoutStaysSilent);
    RUN_TEST(test_PathVariablesAndAutoMkdir);
    RUN_TEST(test_AppendAcrossProcessRuns);
    RUN_TEST(test_StdoutAndFileBothReceive);
    return UNITY_END();
}
