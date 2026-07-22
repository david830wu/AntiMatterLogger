/*
 * ============================================================================
 *  CHAPTER 03 — LEVELS AND FILTERING
 * ============================================================================
 *  Filtering is per MODULE. The config sets a global `default_level`, and the
 *  `loggers:` section overrides it for individual modules:
 *
 *      default_level: INFO
 *      loggers:
 *        NoisyModule: ERROR       # quiet this one down
 *        Test03_Levels: DEBUG     # verbose for this one
 *
 *  `OFF` silences a module entirely. Level names in the config are
 *  case-insensitive. Modules never listed anywhere simply use default_level —
 *  they exist the moment a file logs.
 *
 *  A practical consequence worth knowing: when a call is filtered out, its
 *  payload arguments are NOT evaluated. Never put side effects in them.
 * ============================================================================
 */

#include "TestSupport.h"
#include "TestHelperModule.h"

void setUp(void)   { amc_internal_test_reset(); }
void tearDown(void){ amc_internal_test_reset(); amc_release_all_streams(); }

/* default_level applies to every module not listed in `loggers:`. */
static void test_ConfigDefaultLevelDebugShowsDebug(void)
{
    amc_write_file("cfg.yaml", "default_level: DEBUG\n");
    capture_stdout();
    amc_logger_init("cfg.yaml");
    AMC_LOGGER_DEBUG("NowVisible");
    amc_logger_shutdown();
    char *out = release_stdout();

    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(out, "[DEBUG]"));
    free(out);
}

/* A per-module override changes only that module; others keep the default.
 * Here: this chapter's module is raised to ERROR, the helper module is not. */
static void test_PerModuleOverride(void)
{
    amc_write_file("cfg.yaml",
        "default_level: INFO\n"
        "loggers:\n"
        "  Test03_Levels: ERROR\n");
    capture_stdout();
    amc_logger_init("cfg.yaml");
    AMC_LOGGER_INFO("SuppressedHere");        /* below ERROR -> filtered   */
    AMC_LOGGER_ERROR("StillLoud");            /* passes                    */
    helper_module_log_info(1);                /* helper stays at INFO      */
    amc_logger_shutdown();
    char *out = release_stdout();

    TEST_ASSERT_EQUAL_INT(0, amc_count_lines_containing(out, "SuppressedHere"));
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(out, "StillLoud"));
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(out, "HelperEvent"));
    free(out);
}

/* OFF silences a module completely — even CRITICAL. */
static void test_LevelOffSilencesEverything(void)
{
    amc_write_file("cfg.yaml",
        "loggers:\n"
        "  Test03_Levels: OFF\n");
    capture_stdout();
    amc_logger_init("cfg.yaml");
    AMC_LOGGER_CRITICAL("ShouldNeverAppear");
    helper_module_log_info(2);                /* other modules unaffected  */
    amc_logger_shutdown();
    char *out = release_stdout();

    TEST_ASSERT_EQUAL_INT(0, amc_count_lines_containing(out, "ShouldNeverAppear"));
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(out, "HelperEvent"));
    free(out);
}

/* Config level names are case-insensitive ("debug" == "DEBUG"). */
static void test_LevelNamesAreCaseInsensitive(void)
{
    amc_write_file("cfg.yaml", "default_level: \"debug\"\n");
    capture_stdout();
    TEST_ASSERT_EQUAL_INT(0, amc_logger_init("cfg.yaml"));
    AMC_LOGGER_DEBUG("LowercaseWorked");
    amc_logger_shutdown();
    char *out = release_stdout();

    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(out, "LowercaseWorked"));
    free(out);
}

/* Filtered-out calls do not evaluate their arguments. The counter below is
 * untouched because DEBUG is below the default INFO threshold. */
static void test_FilteredCallsDoNotEvaluateArguments(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    int counter = 0;
    AMC_LOGGER_DEBUG("SideEffectTrap", "{\"n\":%d}", counter++);
    amc_logger_shutdown();
    free(release_stdout());

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, counter,
        "payload args of a filtered call must not run");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ConfigDefaultLevelDebugShowsDebug);
    RUN_TEST(test_PerModuleOverride);
    RUN_TEST(test_LevelOffSilencesEverything);
    RUN_TEST(test_LevelNamesAreCaseInsensitive);
    RUN_TEST(test_FilteredCallsDoNotEvaluateArguments);
    return UNITY_END();
}
