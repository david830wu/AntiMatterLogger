# AntiMatterLogger — Agreed Design (v2)

**Status**: FINAL (2026-07-22). Consolidates the draft `InitDesign.md`, the decisions in
`Answer1.md`, and the follow-up confirmations FQ1–FQ3 (recorded in §12). This document
supersedes `InitDesign.md`; where they differ, this document wins.

**Amendment (2026-07-22, agreed)**: the `AMC_JSON` compile-time payload composer and
`AMC_KV_*` typed helpers were added to §3 rule 6 and §8; runtime string escaping is
explicitly deferred to v1.1 (§13).

**Next step**: architecture design (module/file layout, public header, core data structures).

Decision references like (D3) or (Q14) point to the numbered items in `Answer1.md`;
(FQ1)–(FQ3) point to the resolved follow-ups in §12.

---

## 0. Guiding principles

1. **Report errors as early as possible** (00). A detectable mistake — config typo, bad path,
   unknown key — fails `amc_logger_init()` with a precise diagnostic. Information loss at
   runtime (drops, truncation, write errors) is counted and reported, never silent.
2. **Stability and safety over exotic speed** (Q19, E). The target is performance comparable
   to spdlog, not record-setting latency. No lock-free structures in v1; the queue is a
   mutex-and-condvar protected ring.
3. **As small and simple as possible.** No formatter objects (D3), no `_st`/`_mt` API split
   (D4), no reference counting (D5), one queue, one worker (D2), fixed output format.
4. **Pure C, zero runtime dependencies** beyond libc and pthreads (D6).

## 1. Scope

### v1 required
- Five levels `DEBUG INFO WARN ERROR CRITICAL` (+ `OFF` as a config value only), per-module
  level filtering, auto-flush after `ERROR`/`CRITICAL`.
- Synchronous and asynchronous modes behind one global switch; async is the default (D1).
- Sinks: `stdout`, `basic_file` with `$TODAY`/`$PID` path variables (Q14).
- YAML configuration via a hand-rolled strict subset parser (D6).
- Statistics API, periodic drop/truncation summary lines.
- Benchmark harness and test suite from day one (Q19, Q20).

### v1 non-goals (parking lot in §13)
File rotation / midnight rollover, console colors, per-sink levels, per-logger sink
selection, runtime level changes / SIGHUP reload (Q17), Windows, multiple workers (D2),
custom formatters (D3), binary/deferred logging, logging from signal handlers.

## 2. Platform and toolchain (Q18)

- **Linux x86-64, gcc-13** (production) and **macOS arm64/x86-64, AppleClang ≥ 14**
  (development). POSIX APIs only; no Linux-isms.
- Language level `-std=gnu11`: C11 `stdatomic`, pthreads, `##__VA_ARGS__`,
  `__attribute__((format(printf, …)))` so payload format/argument mismatches are compile
  errors.
- Build: CMake ≥ 3.20 producing a static library plus `AmcLogger.h`.
  `-Wall -Wextra -Wformat=2 -Werror`.
- Naming (Q21): public prefix `amc_` / `AMC_LOGGER_` (AMC = AntiMatterCommon, the parent
  repo this library integrates into). Functions snake_case, file names PascalCase.

## 3. Log line format

```
[YYYY-MM-DD HH:MM:SS.ssssss][LEVEL][MODULE][FUNCTION][EVENT][TRADER_ID]PAYLOAD\n
```

| Field | Rule |
| ----- | ---- |
| Timestamp | `CLOCK_REALTIME` captured at the call site, microsecond precision, rendered at fixed offset `utc_offset_hours` (default +8, no DST) (Q10). |
| `LEVEL` | One of the five uppercase names. |
| `MODULE` | Basename of the calling source file with a trailing `.c`/`.h` stripped, derived from `__FILE__` once per call site and cached. Files sharing a basename share one logger (documented behavior). |
| `FUNCTION` | `__func__` of the calling function. |
| `EVENT` | User-supplied **string literal** (D3), PascalCase by convention. Stored by pointer; passing a non-literal/temporary is undefined behavior in async mode. |
| `TRADER_ID` | Any `int` (no validated bound, Q7) via the `_ID` macro variants; `-` for the plain variants. |
| `PAYLOAD` | JSON text with quoted keys, e.g. `{"error_id":"10001","order_volume":1200}`, produced by the caller's printf format string. Opaque to the library: never parsed, validated, or escaped (Q8). |

Rules:
1. No spaces between brackets, no space before the payload, exactly one trailing `\n`.
2. Empty payload: a call without a format string renders the payload as the empty JSON
   object `{}`, and the line ends there (FQ3).
3. A rendered line longer than `max_message_size` (default 2048 bytes, including header and
   newline) is cut and ends with the marker `...(truncated)`; truncated payloads are no
   longer valid JSON, and each truncation increments a counter (Q11).
4. Newlines inside a payload are the caller's responsibility; they break line-oriented
   parsing and are not escaped.
5. The payload format string must be a literal (compile-time checked via the printf format
   attribute).
6. The recommended way to build payloads is the `AMC_JSON` composer: `(key, fmt, value…)`
   tuples (or `AMC_KV_INT/I64/U64/F64/STR/BOOL` helpers) expand at compile time to the
   identical single format literal of the raw form — keys auto-quoted, structure generated,
   zero runtime cost, printf checking preserved. 1–16 pairs; raw tuples cover custom
   precision, nested objects, and constant fragments. The raw string form remains valid.
   Values are still not escaped at runtime (see §13 for the deferred v1.1 helper).

## 4. Execution model

### 4.1 Hot path at the call site

```
AMC_LOGGER_INFO("Event", "{...}", args)
  ├─ compile-time: entire call removed if INFO < AMC_LOGGER_ACTIVE_LEVEL
  ├─ cached logger lookup: block-scope static pointer, resolved once per call
  │  site through the registry (mutex + auto-create), then one relaxed load
  ├─ relaxed atomic level check → cheapest possible "disabled" exit
  ├─ capture: timestamp, level, module/function/event pointers (literals),
  │  trader_id (+ has_id flag), payload rendered by vsnprintf
  └─ async: enqueue   |   sync: render header + locked write to each sink
```

- Loggers **auto-create on first use** for modules not listed in the config, with
  `default_level` and all configured sinks (D5).
- There are no formatter objects. Each message is rendered into its final byte form exactly
  once — by the worker in async mode, by the caller in sync mode — and sinks are dumb byte
  writers (D3). The `YYYY-MM-DD HH:MM:SS.` seconds prefix is cached and only the
  microseconds are re-rendered per message (E).

### 4.2 Async mode (default)

```
producer threads ──► bounded MPSC ring (mutex + condvar, ──► single worker ──► sinks
   (capture +         queue_size preallocated slots,          (render header,
    render payload     fixed-size, no steady-state malloc)     batch write,
    into the slot)                                             flush policy)
```

- Exactly **one worker thread**, forever (D2). File order == queue order.
- The worker drains in batches, one buffered write per batch, and uses its condvar wait
  timeout to implement `flush_every_ms` — no extra flush thread exists (E).
- Global mode: one queue and one worker serve all loggers; no per-logger async choice (D1).

### 4.3 Locking (D4)

Every sink always owns one mutex protecting write/flush — there is no `_st` variant and no
suffix in any public name. In async mode the worker is effectively the only sink user, so
the mutex is uncontended; it exists so `critical_sync` writers and `amc_logger_flush()`
remain safe.

### 4.4 Lifecycle and contracts

- `amc_logger_init()` is called **once**, before spawning threads; it is not itself
  thread-safe, and a second call fails (report early, 00).
- Loggers live until `amc_logger_shutdown()`; queued items therefore need no ownership
  management (no refcounting) (D5).
- Levels are relaxed atomics, not user-mutable in v1 (Q17); shutdown sets all levels to
  `OFF`, making late calls no-ops.
- Before init, calls are dropped, and the first such call prints a one-time warning to
  stderr — `amc_logger: log call before amc_logger_init(); messages dropped` (FQ1, per 00).
- Logging from signal handlers is unsupported (documented).

## 5. Reliability semantics

- `queue_full_policy` (async): **`block`** (default, Q12 — lossless; a producer can stall on
  disk backpressure, accepted), `overrun_oldest`, `discard_new`. Every policy outcome is
  counted.
- Auto-flush after `ERROR`/`CRITICAL`, performed by the worker. "Flush" always means
  `fflush` (userspace → kernel), never `fsync` (E).
- `flush_every_ms` (async, default 1000, `0` disables): periodic flush via the worker's wait
  timeout.
- `critical_sync` (async, default `false`, Q13): when `true`, `CRITICAL` messages bypass the
  queue and are rendered and written+flushed synchronously under the sink mutexes. They may
  appear earlier than still-queued older lines (documented trade-off).
- Crash behavior: queued-but-unwritten messages can be lost with the process (documented);
  `amc_logger_flush()` exists for checkpoints; `critical_sync` is the stronger opt-in (Q13).
- Sink write/flush errors: reported to stderr (rate-limited), counted in `write_errors`;
  the library never terminates the process.
- Honesty about loss (E): when drops/overwrites/truncations occurred, the worker emits a
  periodic summary line and a final one at shutdown; totals via `amc_logger_get_stats()`.

## 6. Sinks

- **`stdout`** — writes to standard output. No colors in v1.
- **`basic_file`** — appends to one file. Path variables, expanded **once at open** (Q14):
  `$TODAY` (`YYYY-MM-DD` at `utc_offset_hours`) and `$PID`. Unknown `$VAR` fails init (00).
  Missing directories are created automatically (`mkdir -p` equivalent); creation or open
  failure fails init. No midnight rollover in v1 — long-running processes keep the opened
  path (daemons restart daily).
- Every logger writes to every configured sink; no per-sink levels (Q15). A message is
  atomic per sink, not across sinks.

## 7. Configuration (D6, Q15)

### 7.1 Schema — all keys optional with these defaults

```yaml
%YAML 1.2
---
default_level: INFO            # DEBUG | INFO | WARN | ERROR | CRITICAL | OFF
async: true                    # false = synchronous mode (no queue, no worker)
queue_size: 8192               # async: preallocated message slots
queue_full_policy: block       # block | overrun_oldest | discard_new
max_message_size: 2048         # bytes per rendered line, incl. header and \n
flush_every_ms: 1000           # async: periodic flush; 0 disables
utc_offset_hours: 8            # timestamps and $TODAY
critical_sync: false           # async: CRITICAL bypasses the queue (Q13)

sinks:                         # omitted entirely → stdout only
  stdout: {}
  basic_file: "./log/$TODAY/MyApp.$PID.log"

loggers:                       # module name → level override (flat form)
  main: INFO
  LongTermExtractor: DEBUG
```

### 7.2 Validation — fail fast (00, Q16)

`amc_logger_init()` returns `-1` and prints a precise diagnostic to stderr on: unreadable
file, any syntax outside the YAML subset, **any unknown key**, unknown level/policy names,
out-of-range numbers, unknown `$VAR` in a path, or unopenable sink. Level strings are
case-insensitive; canonical form is uppercase.

### 7.3 The YAML subset (exact contract)

Accepted: UTF-8; `#` comments; optional `%YAML 1.2` directive and `---` marker;
block mappings nested ≤ 3 levels, indented with spaces (tabs are an error); plain,
single-quoted, and double-quoted scalars; integers; `true`/`false`; the empty flow mapping
`{}` as a value. **Rejected loudly**: sequences, anchors/aliases, multi-line scalars, any
other flow syntax, duplicate keys. The subset is documented in the user manual; anything
outside it is an init error, never a silent misparse.

## 8. Public API

```c
/* AmcLogger.h — complete v1 surface */

int  amc_logger_init(const char *config_path); /* NULL → built-in defaults (FQ2);
                                                  0 on success, -1 + stderr detail */
int  amc_logger_shutdown(void);                /* drain, join, flush, close; idempotent */
int  amc_logger_flush(void);                   /* returns once everything enqueued
                                                  before the call is written + fflushed */

struct amc_logger_stats {
    uint64_t enqueued;          /* accepted messages                       */
    uint64_t dropped_new;       /* discard_new rejections                  */
    uint64_t overwritten_old;   /* overrun_oldest overwrites               */
    uint64_t producer_blocks;   /* waits caused by a full queue (block)    */
    uint64_t truncated;         /* lines cut at max_message_size           */
    uint64_t write_errors;      /* failed sink writes/flushes              */
    uint64_t queue_high_water;  /* max queue occupancy observed            */
};
void amc_logger_get_stats(struct amc_logger_stats *out);

/* Logging macros — payload format string + args are optional; omitted → payload {} */
AMC_LOGGER_DEBUG(event, ...)          AMC_LOGGER_DEBUG_ID(event, trader_id, ...)
AMC_LOGGER_INFO(event, ...)           AMC_LOGGER_INFO_ID(event, trader_id, ...)
AMC_LOGGER_WARN(event, ...)           AMC_LOGGER_WARN_ID(event, trader_id, ...)
AMC_LOGGER_ERROR(event, ...)          AMC_LOGGER_ERROR_ID(event, trader_id, ...)
AMC_LOGGER_CRITICAL(event, ...)       AMC_LOGGER_CRITICAL_ID(event, trader_id, ...)

/* Compile-time floor: calls below it compile to ((void)0). Default: DEBUG (keep all). */
#define AMC_LOGGER_ACTIVE_LEVEL AMC_LOGGER_LEVEL_DEBUG
/* Level constants: DEBUG=0, INFO=1, WARN=2, ERROR=3, CRITICAL=4, OFF=5 */

/* Payload composer (§3 rule 6): expands to the raw form's literal at compile time */
AMC_JSON((key, fmt, value...) , ...)          /* 1..16 pairs                        */
AMC_KV_INT(key, v)   AMC_KV_I64(key, v)   AMC_KV_U64(key, v)
AMC_KV_F64(key, v)   AMC_KV_STR(key, v)   AMC_KV_BOOL(key, v)
```

`amc_logger_init(NULL)` behaves exactly like loading an empty config file: every key takes
its §7.1 default and the sink set is stdout only — the convenient form for unit tests and
small tools (FQ2).

Renamed from the draft: `amc_logger_close_logger()` → `amc_logger_shutdown()` (Q21).
Not in v1: `amc_logger_set_level()` and any runtime reconfiguration (Q17).

## 9. Usage example (corrected)

```c
/* LongTermExtractor.c */
#include "LongTermExtractor.h"
#include "AmcLogger.h"
#include <stdlib.h>

struct LongTermExtractor { int trader_id; int value; };

struct LongTermExtractor* long_term_extractor_init(int trader_id, int value) {
    struct LongTermExtractor* p_result = malloc(sizeof(struct LongTermExtractor));
    p_result->trader_id = trader_id;
    p_result->value = value;
    AMC_LOGGER_INFO_ID("ExtractorInit", p_result->trader_id,
                       "{\"trader_id\":%d,\"value\":%d}", p_result->trader_id, p_result->value);
    return p_result;
}

int long_term_extractor_close(struct LongTermExtractor* p_lte) {
    if (p_lte == NULL) {
        AMC_LOGGER_ERROR("ReleaseNullPointer");          /* empty payload */
        return -1;
    }
    AMC_LOGGER_INFO_ID("ExtractorClose", p_lte->trader_id, "{\"value\":%d}", p_lte->value);
    free(p_lte);
    return 0;
}
```

```text
[2026-07-21 13:56:16.432148][INFO][LongTermExtractor][long_term_extractor_init][ExtractorInit][100]{"trader_id":100,"value":42}
[2026-07-21 13:56:16.432149][ERROR][LongTermExtractor][long_term_extractor_close][ReleaseNullPointer][-]{}
[2026-07-21 13:56:16.432150][INFO][LongTermExtractor][long_term_extractor_close][ExtractorClose][100]{"value":42}
```

## 10. Performance (Q19)

Production envelope: ~10 k messages/sec aggregate from ~10 producer threads — the design
targets below leave two orders of magnitude of headroom.

| Measurement | Target |
| ----------- | ------ |
| Disabled-level call | ≤ ~2 ns (cached pointer + relaxed load + compare) |
| Enabled async call (payload render + enqueue) | p50 ≤ 200 ns, p99 ≤ 1 µs, uncontended |
| Worker drain to `basic_file` | ≥ 1 M lines/sec |
| Overall acceptance | same order of magnitude as spdlog async, default configs, same machine |

`bench/` contains: disabled-call latency, enqueue latency percentiles, sustained
throughput, and an **optional** spdlog comparison enabled by `-DAMC_BENCH_SPDLOG=ON`
(FetchContent; C++ used by the benchmark target only — the library itself stays pure C).

## 11. Testing and QA (Q20)

- Unit tests: line renderer (empty payload, truncation, `_ID`/no-ID), config parser (valid
  configs plus a battery of malformed files — every diagnostic path exercised), overflow
  policies, lifecycle (double init, idempotent shutdown, log-before-init, log-after-shutdown).
- Concurrency: multi-producer stress tests run under ThreadSanitizer; ASan/UBSan jobs.
- CI matrix: Linux gcc-13 and macOS AppleClang.
- Post-v1: amalgamated single `.c`/`.h` distribution for vendoring.

## 12. Resolved follow-ups and editorial defaults

Follow-up questions, confirmed 2026-07-22:

- **FQ1 — before-init behavior**: calls are dropped, plus a **one-time stderr warning** on
  the first such call (philosophy 00 at zero hot-path cost).
- **FQ2 — `amc_logger_init(NULL)`**: uses the built-in defaults — equivalent to an empty
  config file (stdout only, INFO, async, block, 2 KB).
- **FQ3 — empty payload**: a call without a payload renders the empty JSON object `{}` and
  the line ends there.

Editorial defaults, accepted without veto: key named `utc_offset_hours` (integer hours;
half-hour zones unsupported); `flush_every_ms` default 1000; `$TODAY` = `YYYY-MM-DD`;
omitted `sinks:` → stdout only; case-insensitive level strings; auto `mkdir -p`; module =
basename minus `.c`/`.h`, same-basename files share a logger; trader_id is any `int`
carried with an internal has-id flag (no sentinel value); write errors → rate-limited
stderr + counter; AppleClang ≥ 14; spdlog as a benchmark-only dependency; no console
colors; expected-output examples use quoted-key JSON payloads.

## 13. Future work (explicitly deferred)

Daily rollover / size rotation; `amc_logger_set_level()` and SIGHUP config reload;
per-logger sink selection and per-sink levels; Windows; console colors; TSC-based
timestamps; NanoLog-style binary deferred logging if a sub-100 ns producer path is ever
required; runtime JSON string escaping for `%s` payload values (v1.1 candidate:
`AMC_KV_STR_ESC`, escaping into a stack buffer at some runtime cost).
