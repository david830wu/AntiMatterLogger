# AntiMatterLogger

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
    AMC_LOGGER_ERROR_ID("VolumeError", 100,                  /* with trader id   */
                        "{\"threshold\":%d,\"volume\":%d}", 1000, 1200);

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

Consume from CMake: `add_subdirectory(AntiMatterLogger)` and link
`AntiMatter::AmcLogger`; include `AmcLogger.h` (the header is C-only).

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

The suite passes under ThreadSanitizer and ASan+UBSan. Tests use the vendored
[Unity](https://github.com/ThrowTheSwitch/Unity) framework (`third_party/unity`, MIT).

## Design documents

The library was designed before it was built, and the documents are kept current:
[`docs/Design.md`](docs/Design.md) (agreed requirements),
[`docs/Architecture.md`](docs/Architecture.md) (implementation contract and every
refinement made along the way). Design targets: ≤ ~2 ns disabled call, ≤ ~200 ns
median enqueue, ≥ 1 M lines/s drain — the benchmark harness verifying them is the
next milestone (alongside CI and examples); numbers will be published when measured.

## License

MIT — see [LICENSE](LICENSE).
