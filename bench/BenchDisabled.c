/* BenchDisabled — the cost of a runtime-disabled log call.
 *
 * Design §10 target: <= ~2 ns per call.
 *
 * The DEBUG calls below are filtered by the default INFO level, so each
 * iteration pays exactly the hot-path filter: the call-site cached-pointer
 * load (acquire) plus one relaxed atomic level check. Payload arguments are
 * not evaluated. The empty asm keeps the compiler from collapsing the loop. */

#include "AmcLogger.h"
#include "BenchSupport.h"

int main(void)
{
    if (amc_logger_init(NULL) != 0)
        return 1;

    for (int i = 0; i < 1000; i++)                     /* warm the call-site cache */
        AMC_LOGGER_DEBUG("Warmup", "{\"i\":%d}", i);

    enum { N = 200000000 };
    uint64_t t0 = bench_now_ns();
    for (long i = 0; i < N; i++) {
        AMC_LOGGER_DEBUG("Filtered", "{\"i\":%ld}", i);
        __asm__ volatile("");
    }
    uint64_t t1 = bench_now_ns();

    printf("BenchDisabled: %.2f ns per disabled call over %d calls "
           "(target <= ~2 ns; includes loop overhead)\n",
           (double)(t1 - t0) / (double)N, (int)N);

    amc_logger_shutdown();
    return 0;
}
