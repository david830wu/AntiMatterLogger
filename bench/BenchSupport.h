#ifndef AMC_BENCH_SUPPORT_H_
#define AMC_BENCH_SUPPORT_H_

/* Shared helpers for the AmcLogger benchmark harness.
 *
 * Methodology notes:
 *  - build Release (-O2; the CMake bench targets force -O2 regardless);
 *  - quiet machine, ideally pinned: `taskset -c 2 ./BenchEnqueue`;
 *  - numbers on shared/virtualized hardware are indicative, not authoritative.
 *    The Design §10 targets assume production x86-64. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static inline uint64_t bench_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t bench_percentile(const uint64_t *sorted, size_t n, double p)
{
    size_t idx = (size_t)(p / 100.0 * (double)(n - 1));
    return sorted[idx];
}

static inline void bench_write_config(const char *path, const char *content)
{
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror("bench: cannot write config");
        exit(1);
    }
    fputs(content, fp);
    fclose(fp);
}

#endif /* AMC_BENCH_SUPPORT_H_ */
