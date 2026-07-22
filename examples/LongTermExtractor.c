/* filename: LongTermExtractor.c
 * Logs under MODULE "LongTermExtractor" purely because the macros are expanded
 * in this file — no registration anywhere. The config file raises this module
 * to DEBUG while the rest of the process stays at INFO. */

#include "LongTermExtractor.h"
#include "AmcLogger.h"

#include <stdlib.h>

struct LongTermExtractor {
    int trader_id;
    int value;
};

struct LongTermExtractor *long_term_extractor_init(int trader_id, int value)
{
    struct LongTermExtractor *p_result = malloc(sizeof(struct LongTermExtractor));
    p_result->trader_id = trader_id;
    p_result->value = value;
    AMC_LOGGER_INFO_ID("ExtractorInit", p_result->trader_id,
                       "{\"trader_id\":%d,\"value\":%d}",
                       p_result->trader_id, p_result->value);
    return p_result;
}

int long_term_extractor_close(struct LongTermExtractor *p_lte)
{
    if (p_lte == NULL) {
        AMC_LOGGER_ERROR("ReleaseNullPointer");   /* empty payload renders {} */
        return -1;
    }
    AMC_LOGGER_INFO_ID("ExtractorClose", p_lte->trader_id,
                       "{\"value\":%d}", p_lte->value);
    free(p_lte);
    return 0;
}
