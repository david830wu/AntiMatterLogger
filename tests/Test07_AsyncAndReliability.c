/*
 * ============================================================================
 *  CHAPTER 07 — ASYNC MODE, BACKPRESSURE, AND WHAT THE LOGGER PROMISES
 * ============================================================================
 *  In async mode (the default) your thread only formats the payload and drops
 *  it into a bounded in-memory queue; a single worker thread does the file
 *  I/O. Three things follow, and each has a control:
 *
 *  - VISIBILITY: lines appear "soon", not instantly. amc_logger_flush()
 *    blocks until everything accepted so far is on disk. ERROR and CRITICAL
 *    always flush automatically after being written.
 *  - A FULL QUEUE needs a policy (`queue_full_policy`):
 *        block          — lossless; the producer waits (default)
 *        discard_new    — drop the arriving message, keep the old ones
 *        overrun_oldest — overwrite the oldest, keep the newest
 *    Whatever is lost is COUNTED (amc_logger_get_stats) and the logger
 *    prints its own [AmcLogger]...[MessageLoss] summary line. Loss is never
 *    silent.
 *  - CRASH SAFETY: queued lines can die with a crashing process. For last
 *    words, `critical_sync: true` makes CRITICAL bypass the queue and hit
 *    the file before the call returns (it may overtake older queued lines).
 *  - `async: false` turns all of this off: every call writes inline before
 *    returning. Simplest for small tools and deterministic tests.
 * ============================================================================
 */

#include "TestSupport.h"

void setUp(void)   { amc_internal_test_reset(); }
void tearDown(void){ amc_internal_test_reset(); amc_release_all_streams(); }

/* flush() = "everything I logged so far is now on disk" */
static void test_FlushMakesLinesVisibleMidRun(void)
{
    unlink("flush.log");
    amc_write_file("cfg.yaml",
        "flush_every_ms: 60000\n"          /* periodic flush too slow to help */
        "sinks:\n"
        "  basic_file: \"./flush.log\"\n");
    amc_logger_init("cfg.yaml");

    AMC_LOGGER_INFO("VisibleAfterFlush", "{\"n\":%d}", 1);
    TEST_ASSERT_EQUAL_INT(0, amc_logger_flush());

    char *file = amc_read_file("flush.log");     /* mid-run, before shutdown */
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(file, "VisibleAfterFlush"));
    free(file);
    amc_logger_shutdown();
}

/* block (the default): lossless, every accepted message reaches the file */
static void test_BlockPolicyIsLossless(void)
{
    unlink("block.log");
    amc_write_file("cfg.yaml",
        "queue_size: 16\n"
        "queue_full_policy: block\n"
        "sinks:\n"
        "  basic_file: \"./block.log\"\n");
    amc_logger_init("cfg.yaml");
    for (int i = 0; i < 2000; i++)
        AMC_LOGGER_INFO("Burst", "{\"n\":%d}", i);
    amc_logger_shutdown();

    char *file = amc_read_file("block.log");
    TEST_ASSERT_EQUAL_INT(2000, amc_count_app_lines(file));
    struct amc_logger_stats st;
    amc_logger_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT64(2000, st.enqueued);
    TEST_ASSERT_EQUAL_UINT64(0, st.dropped_new);
    TEST_ASSERT_EQUAL_UINT64(0, st.overwritten_old);
    free(file);
}

/* discard_new: producers never wait; the accounting always adds up, and any
 * loss is announced in a MessageLoss summary line */
static void test_DiscardNewIsHonestAboutDrops(void)
{
    unlink("discard.log");
    amc_write_file("cfg.yaml",
        "queue_size: 16\n"
        "queue_full_policy: discard_new\n"
        "sinks:\n"
        "  basic_file: \"./discard.log\"\n");
    amc_logger_init("cfg.yaml");
    enum { TOTAL = 5000 };
    for (int i = 0; i < TOTAL; i++)
        AMC_LOGGER_INFO("Flood", "{\"n\":%d}", i);
    amc_logger_shutdown();

    char *file = amc_read_file("discard.log");
    struct amc_logger_stats st;
    amc_logger_get_stats(&st);

    /* accepted + dropped == sent, and the file holds exactly the accepted */
    TEST_ASSERT_EQUAL_UINT64(TOTAL, st.enqueued + st.dropped_new);
    TEST_ASSERT_EQUAL_INT((int)st.enqueued, amc_count_app_lines(file));
    if (st.dropped_new > 0)
        TEST_ASSERT_TRUE_MESSAGE(
            amc_count_lines_containing(file, "[MessageLoss]") >= 1,
            "drops must be announced in a MessageLoss summary");
    free(file);
}

/* overrun_oldest: everything is accepted, old lines may vanish, and the very
 * last message always survives */
static void test_OverrunOldestKeepsTheNewest(void)
{
    unlink("overrun.log");
    amc_write_file("cfg.yaml",
        "queue_size: 16\n"
        "queue_full_policy: overrun_oldest\n"
        "sinks:\n"
        "  basic_file: \"./overrun.log\"\n");
    amc_logger_init("cfg.yaml");
    enum { TOTAL = 3000 };
    for (int i = 0; i < TOTAL; i++)
        AMC_LOGGER_INFO("Roll", "{\"n\":%d}", i);
    amc_logger_shutdown();

    char *file = amc_read_file("overrun.log");
    struct amc_logger_stats st;
    amc_logger_get_stats(&st);

    TEST_ASSERT_EQUAL_UINT64(TOTAL, st.enqueued);   /* all were accepted */
    TEST_ASSERT_EQUAL_INT((int)(TOTAL - st.overwritten_old),
                          amc_count_app_lines(file));
    TEST_ASSERT_EQUAL_INT(1,
        amc_count_lines_containing(file, "{\"n\":2999}")); /* newest survives */
    free(file);
}

/* oversized payloads are cut at max_message_size with an explicit marker */
static void test_TruncationIsVisibleAndCounted(void)
{
    unlink("trunc.log");
    amc_write_file("cfg.yaml",
        "max_message_size: 256\n"
        "sinks:\n"
        "  basic_file: \"./trunc.log\"\n");
    amc_logger_init("cfg.yaml");

    char big[600];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    AMC_LOGGER_INFO("Oversized", "{\"data\":\"%s\"}", big);
    amc_logger_shutdown();

    char *file = amc_read_file("trunc.log");
    char line[512];
    TEST_ASSERT_NOT_NULL(amc_get_line(file, 0, line, sizeof(line)));
    size_t n = strlen(line);
    TEST_ASSERT_TRUE_MESSAGE(n < 256, "line must respect max_message_size");
    TEST_ASSERT_EQUAL_STRING("...(truncated)", line + n - 14);

    struct amc_logger_stats st;
    amc_logger_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT64(1, st.truncated);
    free(file);
}

/* critical_sync: CRITICAL is on disk before the macro returns */
static void test_CriticalSyncWritesBeforeReturning(void)
{
    unlink("critical.log");
    amc_write_file("cfg.yaml",
        "critical_sync: true\n"
        "flush_every_ms: 60000\n"
        "sinks:\n"
        "  basic_file: \"./critical.log\"\n");
    amc_logger_init("cfg.yaml");

    AMC_LOGGER_INFO("QueuedNormally");        /* may or may not be on disk yet */
    AMC_LOGGER_CRITICAL("LastWords", "{\"code\":%d}", 7);

    /* no flush, no shutdown — the CRITICAL line is already there.
     * (It may legitimately appear BEFORE the still-queued INFO line.) */
    char *file = amc_read_file("critical.log");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(file, "LastWords"));
    free(file);
    amc_logger_shutdown();
}

/* async: false — every call writes inline; nothing is deferred */
static void test_SyncModeWritesInline(void)
{
    unlink("sync.log");
    amc_write_file("cfg.yaml",
        "async: false\n"
        "sinks:\n"
        "  basic_file: \"./sync.log\"\n");
    amc_logger_init("cfg.yaml");

    AMC_LOGGER_ERROR("Immediate1");           /* ERROR auto-flushes */
    char *file = amc_read_file("sync.log");
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines(file));
    free(file);

    AMC_LOGGER_ERROR("Immediate2");
    file = amc_read_file("sync.log");
    TEST_ASSERT_EQUAL_INT(2, amc_count_lines(file));
    free(file);
    amc_logger_shutdown();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_FlushMakesLinesVisibleMidRun);
    RUN_TEST(test_BlockPolicyIsLossless);
    RUN_TEST(test_DiscardNewIsHonestAboutDrops);
    RUN_TEST(test_OverrunOldestKeepsTheNewest);
    RUN_TEST(test_TruncationIsVisibleAndCounted);
    RUN_TEST(test_CriticalSyncWritesBeforeReturning);
    RUN_TEST(test_SyncModeWritesInline);
    return UNITY_END();
}
