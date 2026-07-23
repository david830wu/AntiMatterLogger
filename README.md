# AntiMatterLogger

[![CI](https://github.com/david830wu/AntiMatterLogger/actions/workflows/ci.yml/badge.svg)](https://github.com/david830wu/AntiMatterLogger/actions/workflows/ci.yml)

A fast, small, pure-C (C11) logging library for trading systems, built for the
AntiMatterCommon ecosystem (`amc_` / `AMC_LOGGER_` prefix). Inspired by spdlog's
architecture, stripped to a fixed format and a deliberately tiny core (~1,600 LOC,
zero dependencies beyond libc and pthreads).

## Features

- **Fixed, parseable line format** with microsecond timestamps:
  `[YYYY-MM-DD HH:MM:SS.ssssss][LEVEL][MODULE][FUNCTION][EVENT][TRADER_ID]PAYLOAD`
- **Zero-registration modules** — `MODULE` is the calling source file's basename,
  derived automatically per call site and cached
- **Asynchronous by default**: producers format the payload and enqueue; one worker
  thread does all file I/O. Synchronous mode via one config switch
- **Explicit overflow policies** on the bounded queue — `block` (lossless, default),
  `overrun_oldest`, `discard_new` — with honest accounting: every loss is counted in
  stats and announced in a `[MessageLoss]` summary line
- **Auto-flush after ERROR/CRITICAL**, periodic flush, blocking `amc_logger_flush()`,
  and opt-in `critical_sync` (CRITICAL is on disk before the call returns)
- **Compile-time JSON payload composer** — `AMC_JSON((key, fmt, value), …)` and
  `AMC_KV_*` typed helpers generate the payload structure for you (quoted keys,
  colons, commas, braces) and expand to the identical single format literal of the
  raw form: zero runtime cost, printf type checking preserved, malformed JSON
  structure becomes a compile error
- **Fail-fast YAML configuration**: a strict subset parser; any mistake — typo'd key,
  bad level, tab, out-of-subset YAML — fails init with a `file:line:` diagnostic
- **Compile-time level stripping** (`AMC_LOGGER_ACTIVE_LEVEL`) and cheap runtime
  filtering (one relaxed atomic load per filtered call)
- Sinks: `stdout` and `basic_file` with `$TODAY`/`$PID` path variables and automatic
  directory creation

## Quick start

```c
#include "AmcLogger.h"

int main(void) {
    amc_logger_init(NULL);                /* or amc_logger_init("config/logger.yaml") */

    AMC_LOGGER_INFO("PrintPi", "{\"pi\":%.7f}", 3.1415926);
    AMC_LOGGER_WARN("JustAnEvent");                          /* payload optional */
    AMC_LOGGER_ERROR_ID("VolumeError", 100,                  /* composed payload */
                        AMC_JSON(AMC_KV_INT("threshold", 1000),
                                 AMC_KV_INT("volume", 1200)));

    amc_logger_shutdown();                /* drains the queue: nothing is lost */
    return 0;
}
```

```
[2026-07-22 17:01:02.834470][INFO][main][main][PrintPi][-]{"pi":3.1415926}
[2026-07-22 17:01:02.834471][WARN][main][main][JustAnEvent][-]{}
[2026-07-22 17:01:02.834472][ERROR][main][main][VolumeError][100]{"threshold":1000,"volume":1200}
```

Rules of the road: call `amc_logger_init()` once, before spawning threads (check its
return value — a bad config fails fast); log from any thread; EVENT and the payload
format string must be string literals (the compiler enforces the latter); payload
arguments are not evaluated when a call is filtered out.

Payloads can be written as a raw printf format string, or — recommended — composed
with `AMC_JSON`, which builds the same literal at compile time from
`(key, fmt, value…)` tuples and `AMC_KV_INT/I64/U64/F64/STR/BOOL` helpers: keys are
quoted automatically, structure can't be malformed, and a type mismatch is a compile
error. Chapter 10 of the test suite is the full guide.

## Configuration

Every key with its default lives in [`config/logger.yaml`](config/logger.yaml):

```yaml
default_level: INFO            # DEBUG | INFO | WARN | ERROR | CRITICAL | OFF
async: true                    # false = synchronous mode
queue_size: 8192
queue_full_policy: block       # block | overrun_oldest | discard_new
max_message_size: 2048         # bytes per line; overflow ends with ...(truncated)
flush_every_ms: 1000
utc_offset_hours: 8            # timestamps and $TODAY
critical_sync: false           # CRITICAL bypasses the queue

sinks:                         # omitted entirely -> stdout only
  stdout: {}
  basic_file: "./log/$TODAY/MyApp.$PID.log"

loggers:                       # per-module level overrides
  LongTermExtractor: DEBUG
```

`amc_logger_init(NULL)` equals loading an empty file: stdout only, INFO, async.
Unknown keys, levels, sinks, `$VAR`s, ranges — all are init errors with precise
diagnostics on stderr. The supported YAML subset is deliberately small; anything
outside it (sequences, anchors, multiline scalars, tabs) is rejected loudly.

## Building

Requires CMake ≥ 3.20 and gcc-13 (Linux) or AppleClang ≥ 14 (macOS); POSIX only.

```bash
cmake -S . -B build && cmake --build build -j     # static lib + tests
ctest --test-dir build --output-on-failure        # run the test suite
```

Consume from CMake either way — the target name is identical:

```cmake
# vendored:
add_subdirectory(AntiMatterLogger)
# or installed (cmake --install build --prefix <prefix>):
find_package(AntiMatterLogger 0.1 REQUIRED)

target_link_libraries(app PRIVATE AntiMatter::AmcLogger)
```

The install tree carries the full CMake package (targets export, config with the
Threads dependency, `SameMinorVersion` version file) under
`lib/cmake/AntiMatterLogger/`; `tests/package/` is a smoke-test consumer that CI
builds against a fresh install. Include `AmcLogger.h` (the header is C-only).

## The tests are the manual

`tests/Test01…Test09` are written as documentation chapters, meant to be read in
order — each demonstrates one slice of practical usage, including the mistakes:

| Chapter | Covers |
| ------- | ------ |
| 01 QuickStart | init → log → shutdown, levels, empty payload, trader ids |
| 02 LogFormat | every field, per-file module attribution, payload composition |
| 03 Levels | defaults, per-module overrides, `OFF`, filtered args not evaluated |
| 04 Configuration | the fail-fast diagnostic battery, line by line |
| 05 FileSink | sink selection, `$TODAY`/`$PID`, auto-mkdir, append across restarts |
| 06 Lifecycle | log-before-init, double init, shutdown twice, log-after-shutdown |
| 07 AsyncAndReliability | flush visibility, overflow policies, truncation, `critical_sync` |
| 08 CompileTimeStrip | `AMC_LOGGER_ACTIVE_LEVEL` removes calls from the binary |
| 09 MultiThreaded | lossless concurrent producers, per-thread ordering |
| 10 JsonPayload | composing payloads with `AMC_JSON` and the `AMC_KV_*` helpers |

The suite passes under ThreadSanitizer and ASan+UBSan. Tests use the vendored
[Unity](https://github.com/ThrowTheSwitch/Unity) framework (`third_party/unity`, MIT).

## Performance

Measured with the in-repo harness (`-DAMC_BUILD_BENCH=ON`, always compiled `-O2`;
run on a quiet machine, e.g. `taskset -c 2,3 ./build/BenchEnqueue`). Numbers below
are from a development VM (x86-64, gcc-13, two pinned cores) — indicative, not
authoritative; expect better on production hardware:

| Metric | Target | AmcLogger | spdlog v1.14.1 async, same box |
| ------ | ------ | --------- | ------------------------------ |
| Runtime-disabled call | ≤ ~2 ns | **0.8–0.9 ns** | — |
| Enabled async call, p50 (burst) | ≤ 200 ns | **187–208 ns** | 383 ns |
| Enabled async call, p99 (saturated) | ≤ 1 µs | 5.3 µs¹ | 1.6 µs |
| Sparse call incl. waking the worker | — | ~3 µs | — |
| End-to-end drain to file | ≥ 1 M lines/s | **1.7–3.6 M lines/s** | 0.5–0.9 M lines/s |

¹ Saturated-tail mechanics, the tuning that got here (zero-copy checkout,
wake-gating, spin-before-park), and why a mutex+condvar queue at saturation cannot
reach 1 µs p99 are documented in [`docs/Architecture.md`](docs/Architecture.md)
§14.13. At the intended production envelope (~10k msgs/s) the saturated regime does
not apply.

Compare against spdlog yourself with `-DAMC_BENCH_SPDLOG=ON` (FetchContent; C++ is
confined to that one benchmark target).

## Example program

`examples/` builds the Design.md §9 walkthrough:

```bash
cmake -S . -B build -DAMC_BUILD_EXAMPLES=ON && cmake --build build -j
./build/amc_example        # run from the repo root; uses config/logger.yaml
```

## Design documents

The library was designed before it was built, and the documents are kept current:
[`docs/Design.md`](docs/Design.md) (agreed requirements),
[`docs/Architecture.md`](docs/Architecture.md) (implementation contract and every
refinement made along the way, including the benchmark-driven queue tuning).

## License

MIT — see [LICENSE](LICENSE).
