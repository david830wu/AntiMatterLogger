# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   # configure (tests ON when top-level)
cmake --build build -j                          # build library + tests
ctest --test-dir build --output-on-failure      # run all test chapters
ctest --test-dir build -R Test07                # run a single chapter
```

Sanitizer legs (both must stay green; run before claiming concurrency work done):

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1"
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -g"
```

Each ctest chapter runs in its own working directory (`build/run/<chapter>/`) — look
there for a failing test's config/log files. The library target builds with `-Werror`;
tests build without it.

Benchmarks and the example (both OFF by default):

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release -DAMC_BUILD_BENCH=ON -DAMC_BUILD_EXAMPLES=ON
taskset -c 2,3 ./build-rel/BenchEnqueue     # pin TWO cores: worker needs its own
./build-rel/amc_example                     # run from repo root (config path)
# spdlog comparison: add -DAMC_BENCH_SPDLOG=ON (FetchContent, needs network)
```

Before accepting any queue/worker change: all three suites, the TSan stress loop
(`ctest -R "Test07|Test09"` ×25), and BenchEnqueue/BenchThroughput — the queue's
tuning history and why each piece exists is Architecture §14.13; don't re-tune
without reading it.

## Where truth lives

The project is developed in agreed phases; documents are canonical and kept current:

- `docs/Design.md` — FINAL agreed requirements. Do not change behavior away from it
  without discussing with David first.
- `docs/Architecture.md` — the implementation contract; §14 records every refinement
  (12 so far). When implementation forces a deviation, add it there.
- `docs/InitDesign.md` — superseded draft, historical only. `Question1.md`/`Answer1.md`
  are the design-review Q&A the decisions trace back to.

Workflow preference: propose and agree before moving to a new phase; questions are
numbered so David can answer tersely.

## Big picture

Data flow (async, the default):

```
AMC_LOGGER_*() macro                    AmcLoggerCore.c
  → call-site static cached logger (acquire/release atomic)
  → relaxed level check (struct amc_logger.level is public for exactly this)
  → amc_logger_log(): capture timestamp + vsnprintf payload on the stack
  → AmcLoggerQueue.c: copy into MPSC ring slot under queue mutex
  → single worker thread: batched dequeue, then per-line
  → AmcLoggerSink.c amc_internal_emit(): render (AmcLoggerRender.c) + fwrite
```

Sync mode and `critical_sync` CRITICALs skip the queue and call `amc_internal_emit()`
directly from the calling thread — one shared emission path for all modes.

Two library targets: `amc_logger` (production) and `amc_logger_testing`
(`-DAMC_LOGGER_TESTING`: adds `amc_internal_test_reset()` and a clock hook). Tests must
link the testing variant; production has no reset and init/shutdown happen once per
process, ever.

## Invariants you must not break

- **No thread ever holds two locks.** The worker emits line-at-a-time; the queue mutex
  is always released before any sink mutex is taken. Deadlock-freedom rests on this.
- **Shutdown never frees** logger structs, the registry, or queue slot memory — that is
  what makes post-shutdown log calls safe without refcounting. Only OS resources
  (worker thread, `FILE*`) are released. Same reason registry loggers survive
  `amc_internal_test_reset()`: call-site static caches point at them, so
  `amc_logger_init` re-sweeps all existing loggers' levels before applying overrides.
- **The `"\1" __VA_ARGS__` sentinel** in the macros: enforces literal format strings,
  makes empty payloads work, and avoids `-Wformat-zero-length` in user builds.
  `amc_logger_log` skips the first byte. EVENT strings are stored by pointer — the
  literal-only contract is documented API.
- **`AMC_JSON` must stay a pure compile-time literal composer** (Architecture §14.14):
  it expands to exactly the raw form's format literal + args — Test10 asserts byte
  equality. Never turn it into anything with runtime cost.
- **Fail fast** (philosophy 00): config mistakes are init failures with a
  `file:line: message` stderr diagnostic — never a silent default. Every new
  diagnostic needs a chapter-04 test asserting its exact text. Runtime loss (drops,
  truncation, write errors) is counted in stats and announced via `[MessageLoss]`
  summary lines, never silent.
- The config parser (`AmcLoggerConfig.c`) is a pure function over a string — keep it
  free of globals so it stays directly unit-testable. It accepts a strict YAML subset
  and must reject everything outside it loudly (no sequences, anchors, multiline).

## Tests are the user manual

`tests/Test01…Test10` are documentation chapters written from the user's point of view,
meant to be read in order — narrative header comment per file, one behavior per test.
New user-visible behavior belongs in the fitting chapter, written in the same style.
Test discipline: always release captured stdout/stderr (`release_stdout()`) *before*
asserting, or Unity's failure output lands in the capture file. `TestSupport.h`'s
`amc_assert_log_line()` is the executable line-format spec. Test08 is compiled with
`-DAMC_LOGGER_ACTIVE_LEVEL=AMC_LOGGER_LEVEL_WARN` to prove compile-time stripping.

## Conventions

- Pure C, `-std=gnu11` (C11 + `##__VA_ARGS__`, `_Thread_local`, GNU attributes);
  the public header is C-only (`#error` under `__cplusplus`). Linux gcc-13 and macOS
  AppleClang are the supported toolchains; POSIX APIs only.
- Naming: public `amc_`/`AMC_LOGGER_` (AMC = AntiMatterCommon, the parent repo this
  integrates into), internals `amc_internal_`, PascalCase filenames, snake_case
  functions.
- Zero runtime dependencies beyond libc/pthreads. Unity is vendored in
  `third_party/unity` (test-only); a future spdlog benchmark dependency is allowed for
  bench targets only.

## Current state / roadmap

v1 complete: library + 9-chapter test suite green (incl. TSan/ASan/UBSan), `bench/`
harness with measured numbers (README table; investigation in Architecture §14.13),
`examples/` target, GitHub Actions CI matrix ({ubuntu gcc-13, macos-14} ×
{Release, ASan/UBSan, TSan}) in `.github/workflows/ci.yml`. Remaining ideas live in
Design.md §13 future-work parking lot.
