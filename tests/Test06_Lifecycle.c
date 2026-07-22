/*
 * ============================================================================
 *  CHAPTER 06 — LIFECYCLE RULES, AND THE ACCIDENTS THEY FORGIVE
 * ============================================================================
 *  The contract is simple:
 *
 *      1. amc_logger_init()  — once, before you spawn threads
 *      2. log freely from any thread
 *      3. amc_logger_shutdown() — once, at exit
 *
 *  This chapter documents what happens when reality deviates:
 *
 *  - Logging BEFORE init: messages are dropped, and the very first such call
 *    prints a single warning to stderr (once per process, never repeated).
 *  - init twice: the second call fails with a diagnostic; the first stays.
 *  - shutdown twice: idempotent, both return 0.
 *  - Logging AFTER shutdown: a safe no-op. No crash, no output.
 *  - amc_logger_flush() outside the init..shutdown window: returns -1.
 *  - amc_logger_get_stats(): callable at ANY time, never blocks.
 * ============================================================================
 */

#include "TestSupport.h"

void setUp(void)   { amc_internal_test_reset(); }
void tearDown(void){ amc_internal_test_reset(); amc_release_all_streams(); }

static void test_LoggingBeforeInit_DropsWithOneWarning(void)
{
    capture_stderr();
    AMC_LOGGER_INFO("TooEarly1");
    AMC_LOGGER_INFO("TooEarly2");
    AMC_LOGGER_ERROR("TooEarly3");
    char *err = release_stderr();

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, amc_count_lines(err),
        "exactly one warning, no matter how many early calls");
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(err,
        "log call before amc_logger_init(); messages dropped"));
    free(err);

    /* the early messages are gone — they do not appear after init */
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_INFO("AfterInit");
    amc_logger_shutdown();
    char *out = release_stdout();
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines(out));
    TEST_ASSERT_EQUAL_INT(0, amc_count_lines_containing(out, "TooEarly"));
    free(out);
}

static void test_SecondInitFails_FirstStaysFunctional(void)
{
    capture_stdout();
    TEST_ASSERT_EQUAL_INT(0, amc_logger_init(NULL));

    capture_stderr();
    TEST_ASSERT_EQUAL_INT(-1, amc_logger_init(NULL));
    char *err = release_stderr();
    TEST_ASSERT_EQUAL_INT(1,
        amc_count_lines_containing(err, "already initialized"));
    free(err);

    AMC_LOGGER_INFO("StillWorking");
    amc_logger_shutdown();
    char *out = release_stdout();
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(out, "StillWorking"));
    free(out);
}

static void test_ShutdownIsIdempotent(void)
{
    amc_logger_init(NULL);
    TEST_ASSERT_EQUAL_INT(0, amc_logger_shutdown());
    TEST_ASSERT_EQUAL_INT(0, amc_logger_shutdown());
}

static void test_LoggingAfterShutdownIsANoOp(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_INFO("BeforeClose");
    amc_logger_shutdown();
    AMC_LOGGER_CRITICAL("AfterClose");     /* dropped, silently and safely */
    char *out = release_stdout();

    TEST_ASSERT_EQUAL_INT(1, amc_count_lines(out));
    TEST_ASSERT_EQUAL_INT(0, amc_count_lines_containing(out, "AfterClose"));
    free(out);
}

static void test_FlushOnlyWorksBetweenInitAndShutdown(void)
{
    TEST_ASSERT_EQUAL_INT(-1, amc_logger_flush());   /* before init */
    capture_stdout();
    amc_logger_init(NULL);
    TEST_ASSERT_EQUAL_INT(0, amc_logger_flush());    /* fine when ready */
    amc_logger_shutdown();
    free(release_stdout());
    TEST_ASSERT_EQUAL_INT(-1, amc_logger_flush());   /* after shutdown */
}

static void test_StatsAreReadableAnytime(void)
{
    struct amc_logger_stats st;
    amc_logger_get_stats(&st);                       /* before init: zeros */
    TEST_ASSERT_EQUAL_UINT64(0, st.enqueued);

    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_INFO("CountMe");
    AMC_LOGGER_INFO("CountMeToo");
    amc_logger_shutdown();
    free(release_stdout());

    amc_logger_get_stats(&st);                       /* after shutdown: totals */
    TEST_ASSERT_EQUAL_UINT64(2, st.enqueued);
    TEST_ASSERT_EQUAL_UINT64(0, st.dropped_new);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_LoggingBeforeInit_DropsWithOneWarning);
    RUN_TEST(test_SecondInitFails_FirstStaysFunctional);
    RUN_TEST(test_ShutdownIsIdempotent);
    RUN_TEST(test_LoggingAfterShutdownIsANoOp);
    RUN_TEST(test_FlushOnlyWorksBetweenInitAndShutdown);
    RUN_TEST(test_StatsAreReadableAnytime);
    return UNITY_END();
}
