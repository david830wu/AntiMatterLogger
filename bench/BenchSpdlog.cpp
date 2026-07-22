/* BenchSpdlog — the optional spdlog comparison (Design §10 acceptance: same
 * order of magnitude as spdlog async, default configs, same machine).
 *
 * Build with: -DAMC_BUILD_BENCH=ON -DAMC_BENCH_SPDLOG=ON   (FetchContent; the
 * C++ dependency is confined to this benchmark target — the library is pure C).
 *
 * Runs the same two scenarios as BenchEnqueue / BenchThroughput on spdlog's
 * async logger (thread pool: 131072 slots, 1 worker, block policy) with a
 * comparable output pattern, and prints results in the same format. Compare by
 * running the Amc benches and this one back to back. */

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static uint64_t now_ns()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static uint64_t pct(const std::vector<uint64_t> &sorted, double p)
{
    size_t idx = (size_t)(p / 100.0 * (double)(sorted.size() - 1));
    return sorted[idx];
}

int main()
{
    std::remove("bench_spdlog.log");
    spdlog::init_thread_pool(131072, 1);
    auto logger = spdlog::basic_logger_mt<spdlog::async_factory>(
        "bench", "./bench_spdlog.log", true);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%f][%l][BenchSpdlog][main][BenchMsg][-]%v");

    constexpr int N_SAMPLES = 100000;
    std::vector<uint64_t> samples(N_SAMPLES);

    for (int i = 0; i < 10000; i++)
        logger->info("{{\"i\":{}}}", i);
    logger->flush();

    for (int i = 0; i < N_SAMPLES; i++) {
        uint64_t s = now_ns();
        uint64_t e = now_ns();
        samples[i] = e - s;
    }
    std::sort(samples.begin(), samples.end());
    uint64_t timer = samples[N_SAMPLES / 2];

    for (int i = 0; i < N_SAMPLES; i++) {
        uint64_t s = now_ns();
        logger->info("{{\"threshold_volume\":{},\"order_volume\":{}}}",
                     1000 + i, 2000 + i);
        uint64_t e = now_ns();
        samples[i] = e - s;
    }
    std::sort(samples.begin(), samples.end());

    std::printf("BenchSpdlog enqueue: %d samples, timer-pair overhead ~%llu ns\n",
                N_SAMPLES, (unsigned long long)timer);
    std::printf("  raw      p50=%llu  p90=%llu  p99=%llu  p99.9=%llu  max=%llu ns\n",
                (unsigned long long)pct(samples, 50), (unsigned long long)pct(samples, 90),
                (unsigned long long)pct(samples, 99), (unsigned long long)pct(samples, 99.9),
                (unsigned long long)samples.back());
    std::printf("  adjusted p50=%lld ns   p99=%lld ns\n",
                (long long)(pct(samples, 50) - timer),
                (long long)(pct(samples, 99) - timer));

    constexpr int N = 1000000;
    uint64_t t0 = now_ns();
    for (int i = 0; i < N; i++)
        logger->info("{{\"seq\":{},\"price\":{}}}", i, 90000 + (i % 1000));
    uint64_t t_prod = now_ns();
    logger->flush();
    uint64_t t1 = now_ns();

    std::printf("BenchSpdlog throughput: %d lines\n", N);
    std::printf("  producer-side  %.0f lines/s\n",
                (double)N / ((double)(t_prod - t0) / 1e9));
    std::printf("  end-to-end     %.0f lines/s\n",
                (double)N / ((double)(t1 - t0) / 1e9));

    spdlog::shutdown();
    std::remove("bench_spdlog.log");
    return 0;
}
