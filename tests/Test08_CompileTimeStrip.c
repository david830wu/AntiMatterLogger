/*
 * ============================================================================
 *  CHAPTER 08 — STRIPPING LEVELS OUT OF A BUILD ENTIRELY
 * ============================================================================
 *  Runtime filtering (chapter 03) costs a few nanoseconds per filtered call.
 *  For release builds you can remove low levels COMPLETELY at compile time:
 *
 *      cc -DAMC_LOGGER_ACTIVE_LEVEL=AMC_LOGGER_LEVEL_WARN ...
 *
 *  Calls below the active level compile to ((void)0): no code, no branch, no
 *  arguments evaluated — they cannot be re-enabled by any config.
 *
 *  THIS FILE is compiled with exactly that flag (see CMakeLists.txt), so the
 *  DEBUG/INFO calls below do not exist in the binary, even though the config
 *  sets default_level: DEBUG.
 * ============================================================================
 */

#include "TestSupport.h"

void setUp(void)   { amc_internal_test_reset(); }
void tearDown(void){ amc_internal_test_reset(); amc_release_all_streams(); }

static void test_StrippedLevelsProduceNothingEvenWhenRuntimeEnabled(void)
{
    amc_write_file("cfg.yaml", "default_level: DEBUG\n");
    capture_stdout();
    amc_logger_init("cfg.yaml");
    AMC_LOGGER_DEBUG("CompiledOut");
    AMC_LOGGER_INFO("AlsoCompiledOut");
    AMC_LOGGER_WARN("StillHere");             /* WARN is the active floor */
    amc_logger_shutdown();
    char *out = release_stdout();

    TEST_ASSERT_EQUAL_INT(1, amc_count_lines(out));
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(out, "StillHere"));
    free(out);
}

static void test_StrippedCallsDoNotEvaluateArguments(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    int counter = 0;
    AMC_LOGGER_INFO("Gone", "{\"n\":%d}", counter++);   /* not even compiled */
    amc_logger_shutdown();
    free(release_stdout());

    TEST_ASSERT_EQUAL_INT(0, counter);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_StrippedLevelsProduceNothingEvenWhenRuntimeEnabled);
    RUN_TEST(test_StrippedCallsDoNotEvaluateArguments);
    return UNITY_END();
}
