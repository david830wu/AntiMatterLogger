/*
 * ============================================================================
 *  CHAPTER 04 — THE CONFIG FILE, AND WHAT HAPPENS WHEN YOU GET IT WRONG
 * ============================================================================
 *  amc_logger_init("path.yaml") loads a strict YAML subset. The full
 *  reference (every key, with its default) is config/logger.yaml.
 *
 *  The error philosophy is FAIL FAST: any detectable mistake — a typo'd key,
 *  a bad level name, a tab, YAML features outside the subset — makes init
 *  return -1 with a precise `file:line: message` diagnostic on stderr.
 *  Nothing is ever silently ignored or half-loaded.
 *
 *  YOUR CODE MUST CHECK THE RETURN VALUE OF amc_logger_init(). A failed init
 *  leaves the library uninitialized (logging is dropped) — and it may be
 *  retried with a corrected file.
 * ============================================================================
 */

#include "TestSupport.h"

void setUp(void)   { amc_internal_test_reset(); }
void tearDown(void){ amc_internal_test_reset(); amc_release_all_streams(); }

/* the reference config loads cleanly */
static void test_ReferenceConfigLoads(void)
{
    amc_write_file("cfg.yaml",
        "%YAML 1.2\n"
        "---\n"
        "# reference config, every key spelled out\n"
        "default_level: INFO\n"
        "async: true\n"
        "queue_size: 8192\n"
        "queue_full_policy: block\n"
        "max_message_size: 2048\n"
        "flush_every_ms: 1000\n"
        "utc_offset_hours: 8\n"
        "critical_sync: false\n"
        "sinks:\n"
        "  stdout: {}\n"
        "  basic_file: \"./logs/$TODAY/App.$PID.log\"\n"
        "loggers:\n"
        "  main: INFO\n"
        "  Test04_Configuration: DEBUG\n");
    capture_stdout();
    TEST_ASSERT_EQUAL_INT(0, amc_logger_init("cfg.yaml"));
    AMC_LOGGER_DEBUG("OverrideActive");   /* per-module DEBUG from the config */
    amc_logger_shutdown();
    char *out = release_stdout();
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(out, "OverrideActive"));
    free(out);
}

/* a missing file is an error — and init can be retried afterwards */
static void test_MissingFileFailsFast_ThenRetryWorks(void)
{
    capture_stderr();
    int rc = amc_logger_init("no/such/file.yaml");
    char *err = release_stderr();

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_INT(1,
        amc_count_lines_containing(err, "cannot read config 'no/such/file.yaml'"));
    free(err);

    /* the failed init left the library uninitialized; a retry succeeds */
    TEST_ASSERT_EQUAL_INT(0, amc_logger_init(NULL));
    amc_logger_shutdown();
}

/* a typo'd key names itself, with the file and line number */
static void test_UnknownKeyIsRejectedWithFileAndLine(void)
{
    amc_write_file("cfg.yaml",
        "default_level: INFO\n"
        "que_size: 10\n");
    capture_stderr();
    int rc = amc_logger_init("cfg.yaml");
    char *err = release_stderr();

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_INT(1,
        amc_count_lines_containing(err, "cfg.yaml:2: unknown key 'que_size'"));
    free(err);
}

static void test_BadLevelNameIsRejected(void)
{
    amc_write_file("cfg.yaml", "default_level: LOUD\n");
    capture_stderr();
    int rc = amc_logger_init("cfg.yaml");
    char *err = release_stderr();

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_INT(1,
        amc_count_lines_containing(err, "cfg.yaml:1: unknown level 'LOUD'"));
    free(err);
}

/* tabs in indentation are a classic YAML trap — rejected explicitly */
static void test_TabsAreRejected(void)
{
    amc_write_file("cfg.yaml",
        "sinks:\n"
        "\tstdout: {}\n");
    capture_stderr();
    int rc = amc_logger_init("cfg.yaml");
    char *err = release_stderr();

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_INT(1,
        amc_count_lines_containing(err, "cfg.yaml:2: tab character in indentation"));
    free(err);
}

/* YAML features outside the documented subset fail loudly, never misparse */
static void test_SequencesAreOutsideTheSubset(void)
{
    amc_write_file("cfg.yaml",
        "sinks:\n"
        "  - stdout\n");
    capture_stderr();
    int rc = amc_logger_init("cfg.yaml");
    char *err = release_stderr();

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_INT(1,
        amc_count_lines_containing(err, "cfg.yaml:2: sequences are not supported"));
    free(err);
}

static void test_DuplicateKeysAreRejected(void)
{
    amc_write_file("cfg.yaml",
        "default_level: INFO\n"
        "default_level: DEBUG\n");
    capture_stderr();
    int rc = amc_logger_init("cfg.yaml");
    char *err = release_stderr();

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_INT(1,
        amc_count_lines_containing(err, "cfg.yaml:2: duplicate key 'default_level'"));
    free(err);
}

static void test_UnknownSinkIsRejected(void)
{
    amc_write_file("cfg.yaml",
        "sinks:\n"
        "  syslog: {}\n");
    capture_stderr();
    int rc = amc_logger_init("cfg.yaml");
    char *err = release_stderr();

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_INT(1,
        amc_count_lines_containing(err, "cfg.yaml:2: unknown sink 'syslog'"));
    free(err);
}

/* only $TODAY and $PID exist; anything else in a path is an error */
static void test_UnknownPathVariableIsRejected(void)
{
    amc_write_file("cfg.yaml",
        "sinks:\n"
        "  basic_file: \"./logs/$HOSTNAME/app.log\"\n");
    capture_stderr();
    int rc = amc_logger_init("cfg.yaml");
    char *err = release_stderr();

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_INT(1,
        amc_count_lines_containing(err, "unknown variable '$HOSTNAME' in sink path"));
    free(err);
}

static void test_OutOfRangeNumbersAreRejected(void)
{
    amc_write_file("cfg.yaml", "queue_size: 4\n");
    capture_stderr();
    int rc = amc_logger_init("cfg.yaml");
    char *err = release_stderr();

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_INT(1,
        amc_count_lines_containing(err, "cfg.yaml:1: queue_size out of range"));
    free(err);
}

/* worker_cpu (chapter 07) is bounded by the OS cpu-set width */
static void test_WorkerCpuOutOfRangeRejected(void)
{
    amc_write_file("cfg.yaml", "worker_cpu: 1024\n");
    capture_stderr();
    int rc = amc_logger_init("cfg.yaml");
    char *err = release_stderr();

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_INT(1,
        amc_count_lines_containing(err, "cfg.yaml:1: worker_cpu out of range (0..1023)"));
    free(err);
}

/* in sync mode there is no worker thread; worker_cpu is accepted and
 * silently ignored (agreed decision) */
static void test_WorkerCpuIgnoredInSyncMode(void)
{
    amc_write_file("cfg.yaml",
        "async: false\n"
        "worker_cpu: 2\n");
    TEST_ASSERT_EQUAL_INT(0, amc_logger_init("cfg.yaml"));
    amc_logger_shutdown();
}

/* comments and quoting styles that ARE inside the subset */
static void test_CommentsAndQuotesAccepted(void)
{
    amc_write_file("cfg.yaml",
        "# leading comment\n"
        "default_level: 'WARN'      # single-quoted\n"
        "async: false               # plain scalar\n"
        "\n"
        "sinks:\n"
        "  basic_file: \"./out with space.log\"   # double-quoted path\n");
    TEST_ASSERT_EQUAL_INT(0, amc_logger_init("cfg.yaml"));
    amc_logger_shutdown();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ReferenceConfigLoads);
    RUN_TEST(test_MissingFileFailsFast_ThenRetryWorks);
    RUN_TEST(test_UnknownKeyIsRejectedWithFileAndLine);
    RUN_TEST(test_BadLevelNameIsRejected);
    RUN_TEST(test_TabsAreRejected);
    RUN_TEST(test_SequencesAreOutsideTheSubset);
    RUN_TEST(test_DuplicateKeysAreRejected);
    RUN_TEST(test_UnknownSinkIsRejected);
    RUN_TEST(test_UnknownPathVariableIsRejected);
    RUN_TEST(test_OutOfRangeNumbersAreRejected);
    RUN_TEST(test_WorkerCpuOutOfRangeRejected);
    RUN_TEST(test_WorkerCpuIgnoredInSyncMode);
    RUN_TEST(test_CommentsAndQuotesAccepted);
    return UNITY_END();
}
