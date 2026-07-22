/* BenchThroughput — sustained end-to-end rate into basic_file.
 *
 * Design §10 target: worker drain >= 1M lines/s.
 *
 * One producer logs 1M messages under the default `block` policy (queue 8192),
 * so the producer is throughput-bound by the worker: the end-to-end rate
 * (first call until amc_logger_flush() returns with everything on disk) IS the
 * drain rate. */

#include "AmcLogger.h"
#include "BenchSupport.h"

#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    unlink("bench_throughput.log");
    bench_write_config("bench_throughput.yaml",
        "sinks:\n"
        "  basic_file: \"./bench_throughput.log\"\n");
    if (amc_logger_init("bench_throughput.yaml") != 0)
        return 1;

    for (int i = 0; i < 10000; i++)
        AMC_LOGGER_INFO("Warmup", "{\"i\":%d}", i);
    amc_logger_flush();

    enum { N = 1000000 };
    uint64_t t0 = bench_now_ns();
    for (int i = 0; i < N; i++)
        AMC_LOGGER_INFO("Throughput", "{\"seq\":%d,\"price\":%d}",
                        i, 90000 + (i % 1000));
    uint64_t t_prod = bench_now_ns();
    amc_logger_flush();
    uint64_t t1 = bench_now_ns();

    struct stat st;
    double mb = (stat("bench_throughput.log", &st) == 0)
                    ? (double)st.st_size / (1024.0 * 1024.0) : 0.0;

    double prod_rate = (double)N / ((double)(t_prod - t0) / 1e9);
    double e2e_rate  = (double)N / ((double)(t1 - t0) / 1e9);
    printf("BenchThroughput: %d lines, %.1f MB written\n", (int)N, mb);
    printf("  producer-side  %.0f lines/s\n", prod_rate);
    printf("  end-to-end     %.0f lines/s (target >= 1000000)\n", e2e_rate);

    amc_logger_shutdown();
    unlink("bench_throughput.log");
    unlink("bench_throughput.yaml");
    return 0;
}
