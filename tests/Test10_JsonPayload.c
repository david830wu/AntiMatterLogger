/*
 * ============================================================================
 *  CHAPTER 10 — COMPOSING JSON PAYLOADS WITH AMC_JSON
 * ============================================================================
 *  Hand-writing payload format strings ("{\"a\":%d,...}") is easy to get
 *  wrong: escaped quotes everywhere, keys far from their values, and a missing
 *  comma silently produces invalid JSON. AMC_JSON fixes this at COMPILE TIME:
 *
 *      AMC_LOGGER_ERROR("VolumeError", AMC_JSON(("threshold", "%d", 1000),
 *                                               ("volume",    "%d", volume)));
 *
 *  Each (key, printf_fmt, value...) tuple becomes "key":fmt in the payload;
 *  quotes around keys, colons, commas and braces are generated for you. The
 *  whole thing expands to exactly the same single format literal as the raw
 *  form — zero runtime cost, and the compiler still type-checks every value.
 *
 *  Typed helpers make the common cases even shorter (string values get their
 *  quotes automatically):  AMC_KV_INT / I64 / U64 / F64 / STR / BOOL.
 *  A raw tuple remains the escape hatch: custom precision ("px", "%.2f", px),
 *  nested objects ("order", "{\"vol\":%d}", v), or constant fragments
 *  ("armed", "true") with no value arguments at all.
 *
 *  Rules: 1..16 pairs (17+ fails to compile mentioning TOO_MANY_KEYS); for an
 *  empty payload omit the payload entirely; values are NOT escaped at runtime
 *  — a %s string containing '"' or '\' still breaks the JSON (planned v1.1
 *  helper). Keys and formats must be string literals.
 * ============================================================================
 */

#include "TestSupport.h"

void setUp(void)   { amc_internal_test_reset(); }
void tearDown(void){ amc_internal_test_reset(); amc_release_all_streams(); }

/* the motivating example, tuple form */
static void test_TuplesComposeJson(void)
{
    int volume = 1200;
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_ERROR("VolumeError", AMC_JSON(("threshold", "%d", 1000),
                                             ("volume",    "%d", volume)));
    amc_logger_shutdown();
    char *out = release_stdout();

    char line[512];
    amc_assert_log_line(amc_get_line(out, 0, line, sizeof(line)),
                        "ERROR", "Test10_JsonPayload", __func__,
                        "VolumeError", "-", "{\"threshold\":1000,\"volume\":1200}");
    free(out);
}

/* AMC_JSON expands to the identical bytes the raw form produces */
static void test_IdenticalToTheRawForm(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_INFO("RawForm", "{\"threshold\":%d,\"volume\":%d}", 1000, 1200);
    AMC_LOGGER_INFO("Composed", AMC_JSON(("threshold", "%d", 1000),
                                         ("volume", "%d", 1200)));
    amc_logger_shutdown();
    char *out = release_stdout();

    char l0[512], l1[512];
    amc_get_line(out, 0, l0, sizeof(l0));
    amc_get_line(out, 1, l1, sizeof(l1));
    const char *p0 = strstr(l0, "]{");   /* payload starts after the id field */
    const char *p1 = strstr(l1, "]{");
    TEST_ASSERT_NOT_NULL(p0);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_EQUAL_STRING(p0 + 1, p1 + 1);
    free(out);
}

/* the typed helpers; note AMC_KV_STR quotes its value for you */
static void test_TypedHelpers(void)
{
    const char *tag = "opening";
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_INFO("Typed", AMC_JSON(AMC_KV_INT("volume", 1200),
                                      AMC_KV_I64("epoch_ns", 1753167600123456789LL),
                                      AMC_KV_U64("order_id", 18446744073709551615ULL),
                                      AMC_KV_F64("px", 99.25),
                                      AMC_KV_STR("tag", tag),
                                      AMC_KV_BOOL("armed", 1)));
    amc_logger_shutdown();
    char *out = release_stdout();

    char line[512];
    amc_assert_log_line(amc_get_line(out, 0, line, sizeof(line)),
                        "INFO", "Test10_JsonPayload", __func__, "Typed", "-",
                        "{\"volume\":1200,\"epoch_ns\":1753167600123456789,"
                        "\"order_id\":18446744073709551615,\"px\":99.25,"
                        "\"tag\":\"opening\",\"armed\":true}");
    free(out);
}

/* raw tuples mix freely with helpers: custom precision, nested objects,
 * and constant fragments with no value arguments */
static void test_RawTuplesForPrecisionNestingAndConstants(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_INFO("Mixed", AMC_JSON(AMC_KV_INT("vol", 1200),
                                      ("px", "%.2f", 99.5),
                                      ("order", "{\"vol\":%d,\"px\":%.2f}", 7, 8.5),
                                      ("state", "null"),
                                      ("armed", "true")));
    amc_logger_shutdown();
    char *out = release_stdout();

    char line[512];
    amc_assert_log_line(amc_get_line(out, 0, line, sizeof(line)),
                        "INFO", "Test10_JsonPayload", __func__, "Mixed", "-",
                        "{\"vol\":1200,\"px\":99.50,"
                        "\"order\":{\"vol\":7,\"px\":8.50},"
                        "\"state\":null,\"armed\":true}");
    free(out);
}

/* composes with every macro, including the _ID variants */
static void test_WorksWithTraderIdVariants(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_WARN_ID("PositionLimit", 61, AMC_JSON(AMC_KV_INT("limit", 500)));
    amc_logger_shutdown();
    char *out = release_stdout();

    char line[512];
    amc_assert_log_line(amc_get_line(out, 0, line, sizeof(line)),
                        "WARN", "Test10_JsonPayload", __func__,
                        "PositionLimit", "61", "{\"limit\":500}");
    free(out);
}

/* the documented maximum: 16 pairs in one payload */
static void test_SixteenPairsIsTheLimit(void)
{
    capture_stdout();
    amc_logger_init(NULL);
    AMC_LOGGER_INFO("Wide", AMC_JSON(
        ("k01", "%d", 1),  ("k02", "%d", 2),  ("k03", "%d", 3),  ("k04", "%d", 4),
        ("k05", "%d", 5),  ("k06", "%d", 6),  ("k07", "%d", 7),  ("k08", "%d", 8),
        ("k09", "%d", 9),  ("k10", "%d", 10), ("k11", "%d", 11), ("k12", "%d", 12),
        ("k13", "%d", 13), ("k14", "%d", 14), ("k15", "%d", 15), ("k16", "%d", 16)));
    amc_logger_shutdown();
    char *out = release_stdout();

    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(out, "\"k01\":1,"));
    TEST_ASSERT_EQUAL_INT(1, amc_count_lines_containing(out, "\"k16\":16}"));
    free(out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_TuplesComposeJson);
    RUN_TEST(test_IdenticalToTheRawForm);
    RUN_TEST(test_TypedHelpers);
    RUN_TEST(test_RawTuplesForPrecisionNestingAndConstants);
    RUN_TEST(test_WorksWithTraderIdVariants);
    RUN_TEST(test_SixteenPairsIsTheLimit);
    return UNITY_END();
}
