/*
 * ============================================================================
 *  CHAPTER 09 — MANY THREADS, ONE LOG
 * ============================================================================
 *  After the single-threaded init, every macro is safe from any thread with
 *  no setup per thread. The guarantees:
 *
 *  - lines never interleave or tear — each line is written atomically;
 *  - each thread's own messages appear in its program order;
 *  - messages from different threads interleave in queue-arrival order
 *    (which is what a post-mortem reader wants).
 *
 *  This chapter hammers one call site from several threads at once — which
 *  also exercises the thread-safe first-call logger resolution.
 * ============================================================================
 */

#include "TestSupport.h"

#include <pthread.h>

void setUp(void)   { amc_internal_test_reset(); }
void tearDown(void){ amc_internal_test_reset(); amc_release_all_streams(); }

enum { THREADS = 4, PER_THREAD = 500 };

static void *producer(void *arg)
{
    int tid = (int)(long)arg;
    for (int n = 0; n < PER_THREAD; n++)
        AMC_LOGGER_INFO("ThreadedMsg", "{\"t\":%d,\"n\":%d}", tid, n);
    return NULL;
}

static void test_ConcurrentProducersLoseNothingAndKeepPerThreadOrder(void)
{
    unlink("mt.log");
    amc_write_file("cfg.yaml",
        "queue_full_policy: block\n"          /* lossless under load */
        "sinks:\n"
        "  basic_file: \"./mt.log\"\n");
    amc_logger_init("cfg.yaml");

    pthread_t th[THREADS];
    for (long t = 0; t < THREADS; t++)
        pthread_create(&th[t], NULL, producer, (void *)t);
    for (int t = 0; t < THREADS; t++)
        pthread_join(th[t], NULL);
    amc_logger_shutdown();

    char *file = amc_read_file("mt.log");
    TEST_ASSERT_EQUAL_INT(THREADS * PER_THREAD, amc_count_app_lines(file));

    /* per-thread sequence numbers must be strictly increasing */
    int last_seen[THREADS];
    for (int t = 0; t < THREADS; t++)
        last_seen[t] = -1;
    char line[512];
    for (int i = 0; amc_get_line(file, i, line, sizeof(line)); i++) {
        const char *p = strstr(line, "]{\"t\":");
        if (!p)
            continue;
        int t = -1, n = -1;
        TEST_ASSERT_EQUAL_INT(2, sscanf(p, "]{\"t\":%d,\"n\":%d}", &t, &n));
        TEST_ASSERT_TRUE(t >= 0 && t < THREADS);
        TEST_ASSERT_TRUE_MESSAGE(n > last_seen[t],
            "a thread's messages must appear in its program order");
        last_seen[t] = n;
    }
    for (int t = 0; t < THREADS; t++)
        TEST_ASSERT_EQUAL_INT(PER_THREAD - 1, last_seen[t]);
    free(file);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ConcurrentProducersLoseNothingAndKeepPerThreadOrder);
    return UNITY_END();
}
