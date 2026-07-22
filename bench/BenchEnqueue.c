/* BenchEnqueue — per-call latency of an ENABLED asynchronous log call:
 * timestamp capture + payload vsnprintf + copy into the ring under the queue
 * mutex.
 *
 * Design §10 targets: p50 <= 200 ns, p99 <= 1 us (uncontended).
 *
 * The queue (131072 slots) is sized so the producer never hits the full-queue
 * path during the 100k samples while the worker drains concurrently. Each raw
 * sample includes one bench_now_ns() pair; its median cost is measured first
 * and subtracted in the "adjusted" line. */

#include "AmcLogger.h"
#include "BenchSupport.h"

#include <unistd.h>

#define N_SAMPLES 100000

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint64_t samples[N_SAMPLES];

int main(void)
{
    unlink("bench_enqueue.log");
    bench_write_config("bench_enqueue.yaml",
        "queue_size: 131072\n"
        "max_message_size: 256\n"
        "sinks:\n"
        "  basic_file: \"./bench_enqueue.log\"\n");
    if (amc_logger_init("bench_enqueue.yaml") != 0)
        return 1;

    for (int i = 0; i < 10000; i++)
        AMC_LOGGER_INFO("Warmup", "{\"i\":%d}", i);
    amc_logger_flush();

    /* timer-pair overhead, measured the same way it is spent below */
    for (int i = 0; i < N_SAMPLES; i++) {
        uint64_t s = bench_now_ns();
        uint64_t e = bench_now_ns();
        samples[i] = e - s;
    }
    qsort(samples, N_SAMPLES, sizeof(uint64_t), cmp_u64);
    uint64_t timer = samples[N_SAMPLES / 2];

    for (int i = 0; i < N_SAMPLES; i++) {
        uint64_t s = bench_now_ns();
        AMC_LOGGER_INFO("BenchMsg",
                        "{\"threshold_volume\":%d,\"order_volume\":%d}",
                        1000 + i, 2000 + i);
        uint64_t e = bench_now_ns();
        samples[i] = e - s;
    }
    qsort(samples, N_SAMPLES, sizeof(uint64_t), cmp_u64);

    uint64_t p50 = bench_percentile(samples, N_SAMPLES, 50.0);
    uint64_t p90 = bench_percentile(samples, N_SAMPLES, 90.0);
    uint64_t p99 = bench_percentile(samples, N_SAMPLES, 99.0);
    uint64_t p999 = bench_percentile(samples, N_SAMPLES, 99.9);
    uint64_t max = samples[N_SAMPLES - 1];

    printf("BenchEnqueue burst (back-to-back calls): %d samples, "
           "timer-pair overhead ~%llu ns (median)\n",
           (int)N_SAMPLES, (unsigned long long)timer);
    printf("  raw      p50=%llu  p90=%llu  p99=%llu  p99.9=%llu  max=%llu ns\n",
           (unsigned long long)p50, (unsigned long long)p90,
           (unsigned long long)p99, (unsigned long long)p999,
           (unsigned long long)max);
    printf("  adjusted p50=%lld ns (target <= 200)   p99=%lld ns (target <= 1000)\n",
           (long long)(p50 - timer), (long long)(p99 - timer));

    /* Paced phase: one call every 100 us — the ~10k msgs/s production
     * envelope. Each call typically finds the queue empty and the worker
     * asleep, so the sample includes the condvar wake of the worker. This is
     * the honest per-call cost of SPARSE logging (busy-spin pacing keeps the
     * producer core hot). */
    enum { N_PACED = 20000 };
    amc_logger_flush();
    uint64_t next = bench_now_ns() + 100000;
    for (int i = 0; i < N_PACED; i++) {
        while (bench_now_ns() < next) { /* spin */ }
        uint64_t s = bench_now_ns();
        AMC_LOGGER_INFO("PacedMsg",
                        "{\"threshold_volume\":%d,\"order_volume\":%d}",
                        1000 + i, 2000 + i);
        uint64_t e = bench_now_ns();
        samples[i] = e - s;
        next += 100000;
    }
    qsort(samples, N_PACED, sizeof(uint64_t), cmp_u64);
    printf("BenchEnqueue paced (1 call / 100 us, idle queue): %d samples\n",
           (int)N_PACED);
    printf("  adjusted p50=%lld  p99=%lld ns (includes waking the worker)\n",
           (long long)(bench_percentile(samples, N_PACED, 50.0) - timer),
           (long long)(bench_percentile(samples, N_PACED, 99.0) - timer));

    amc_logger_shutdown();
    unlink("bench_enqueue.log");
    unlink("bench_enqueue.yaml");
    return 0;
}
