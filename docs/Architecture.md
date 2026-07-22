# AntiMatterLogger — Architecture (v1)

**Status**: IMPLEMENTED (2026-07-22). Implements `Design.md` (FINAL). The v1 code lives
under `src/`/`include/`; the test suite under `tests/` doubles as the user manual.
§14 records every refinement made relative to the original draft, including the ones
discovered while making the tests green.

§14 lists every place where this document *refines* `Design.md` — read that section even if
you skim the rest. §15 proposes the implementation order.

---

## 1. Overview

```
                    ┌────────────────────────────────────────────────────────┐
 user code          │                     amc_logger library                 │
 ┌────────────┐     │  ┌──────────┐   ┌───────────┐   ┌────────┐  ┌───────┐  │
 │AMC_LOGGER_*│────►│  │ Core      │──►│ Queue      │──►│ Render │─►│ Sinks │ │
 │  macros    │     │  │ registry, │   │ MPSC ring, │   │ line   │  │stdout │ │
 └────────────┘     │  │ hot path  │   │ worker     │   │ builder│  │ file  │ │
                    │  └────┬─────┘   └───────────┘   └────────┘  └───────┘  │
                    │       │              ▲                                  │
                    │  ┌────▼─────┐        │ (sync mode / critical_sync       │
                    │  │ Config   │        │  bypass the queue and call       │
                    │  │ YAML     │        │  Render + Sinks directly)        │
                    │  └──────────┘        │                                  │
                    └────────────────────────────────────────────────────────┘
```

Five internal modules, one public header. All state lives in one static global
(`g_amc`) plus one static registry; there is no dynamic sink vector and no per-logger
state beyond a level and a name.

## 2. Repository layout

```
AntiMatterLogger/
├── CMakeLists.txt
├── include/
│   └── AmcLogger.h            # the only public header (full listing in §3)
├── src/
│   ├── AmcLoggerInternal.h    # shared internal structs/decls (amc_internal_* namespace)
│   ├── AmcLoggerCore.c        # g_amc, init/shutdown/flush/stats, resolve, amc_logger_log
│   ├── AmcLoggerQueue.c       # MPSC ring, enqueue policies, worker thread, flush protocol
│   ├── AmcLoggerRender.c      # line renderer, timestamp cache, truncation
│   ├── AmcLoggerSink.c        # path expansion, mkdir -p, open/write/flush/close, error report
│   └── AmcLoggerConfig.c      # YAML subset parser (pure function) + defaults + validation
├── tests/                     # TestConfig.c TestRender.c TestQueue.c TestLifecycle.c
│                              # TestModule.c TestStress.c + AmcTest.h (tiny harness)
├── bench/                     # BenchDisabled.c BenchEnqueue.c BenchThroughput.c
│                              # BenchSpdlog.cpp (optional, -DAMC_BENCH_SPDLOG=ON)
├── examples/                  # main.c, LongTermExtractor.{h,c} from Design.md §9
├── config/logger.yaml         # example configuration
└── docs/                      # InitDesign.md (superseded), Answer1.md, Design.md, this file
```

Estimated size: ~1,700 LOC library, well within "small and simple".

## 3. Public header — `include/AmcLogger.h` (complete)

```c
#ifndef AMC_LOGGER_H_
#define AMC_LOGGER_H_

/*
 * AmcLogger — AntiMatterLogger public API.
 * C11 (-std=gnu11) only in v1; do not include from C++ translation units (§14.6).
 * User-facing surface: amc_logger_init/shutdown/flush/get_stats + AMC_LOGGER_* macros.
 * Every other visible symbol is macro plumbing — never call it directly.
 */

#include <stdatomic.h>
#include <stdint.h>

/* ---- Severity levels (ascending) ---- */
#define AMC_LOGGER_LEVEL_DEBUG    0
#define AMC_LOGGER_LEVEL_INFO     1
#define AMC_LOGGER_LEVEL_WARN     2
#define AMC_LOGGER_LEVEL_ERROR    3
#define AMC_LOGGER_LEVEL_CRITICAL 4
#define AMC_LOGGER_LEVEL_OFF      5   /* config value only, never a message level */

/* Compile-time floor: calls strictly below it compile to ((void)0).
 * Define before including this header (or via -D) to strip levels from a build. */
#ifndef AMC_LOGGER_ACTIVE_LEVEL
#define AMC_LOGGER_ACTIVE_LEVEL AMC_LOGGER_LEVEL_DEBUG
#endif

/* ---- Lifecycle ---- */
int amc_logger_init(const char *config_path);  /* NULL = built-in defaults (empty config).
                                                  0 ok; -1 + stderr diagnostic on error.
                                                  A failed init leaves the library
                                                  uninitialized and may be retried. */
int amc_logger_shutdown(void);                 /* drain, join, flush, close; idempotent. */
int amc_logger_flush(void);                    /* block until everything enqueued before
                                                  the call is written and fflushed;
                                                  -1 if the library is not initialized. */

/* ---- Statistics (all counters monotonic since init) ---- */
struct amc_logger_stats {
    uint64_t enqueued;         /* messages accepted (queued, or written in sync mode)   */
    uint64_t dropped_new;      /* discard_new rejections + drops during shutdown        */
    uint64_t overwritten_old;  /* overrun_oldest overwrites                             */
    uint64_t producer_blocks;  /* waits caused by a full queue under `block`            */
    uint64_t truncated;        /* lines cut at max_message_size                         */
    uint64_t write_errors;     /* failed sink writes/flushes                            */
    uint64_t queue_high_water; /* maximum queue occupancy observed                      */
};
void amc_logger_get_stats(struct amc_logger_stats *out);   /* callable at any time */

/* ---- Macro plumbing ---- */

/* Public hot-path prefix of a logger. `level` is the only field the macros read.
 * The implementation extends this struct internally; treat it as read-only. */
struct amc_logger {
    _Atomic int level;
};

/* Derives the module name from `file`, returns the (auto-created) logger.
 * Returns NULL before init / after shutdown; the first pre-init call prints a
 * one-time stderr warning (Design FQ1). */
struct amc_logger *amc_logger_resolve(const char *file);

void amc_logger_log(struct amc_logger *logger, int level,
                    const char *function, const char *event,
                    int has_id, int trader_id,
                    const char *payload_fmt, ...)
                    __attribute__((format(printf, 7, 8)));

#define AMC_LOGGER_LOG_IMPL_(level_, event_, has_id_, id_, ...)               \
    do {                                                                      \
        static struct amc_logger *_Atomic amc_site_logger_;                   \
        struct amc_logger *amc_lg_ = atomic_load_explicit(                    \
            &amc_site_logger_, memory_order_acquire);                        \
        if (amc_lg_ == (struct amc_logger *)0) {                              \
            amc_lg_ = amc_logger_resolve(__FILE__);                           \
            if (amc_lg_ != (struct amc_logger *)0)                            \
                atomic_store_explicit(&amc_site_logger_, amc_lg_,             \
                                      memory_order_release);                  \
        }                                                                     \
        if (amc_lg_ != (struct amc_logger *)0 &&                              \
            (level_) >= atomic_load_explicit(&amc_lg_->level,                 \
                                             memory_order_relaxed))           \
            amc_logger_log(amc_lg_, (level_), __func__, (event_),             \
                           (has_id_), (id_), "" __VA_ARGS__);                 \
    } while (0)

#if AMC_LOGGER_ACTIVE_LEVEL <= AMC_LOGGER_LEVEL_DEBUG
#define AMC_LOGGER_DEBUG(event_, ...)                                         \
    AMC_LOGGER_LOG_IMPL_(AMC_LOGGER_LEVEL_DEBUG, (event_), 0, 0, ##__VA_ARGS__)
#define AMC_LOGGER_DEBUG_ID(event_, trader_id_, ...)                          \
    AMC_LOGGER_LOG_IMPL_(AMC_LOGGER_LEVEL_DEBUG, (event_), 1, (trader_id_), ##__VA_ARGS__)
#else
#define AMC_LOGGER_DEBUG(event_, ...)                ((void)0)
#define AMC_LOGGER_DEBUG_ID(event_, trader_id_, ...) ((void)0)
#endif

/* INFO / WARN / ERROR / CRITICAL blocks are identical in shape.               */
/* ... AMC_LOGGER_INFO, AMC_LOGGER_INFO_ID                                     */
/* ... AMC_LOGGER_WARN, AMC_LOGGER_WARN_ID                                     */
/* ... AMC_LOGGER_ERROR, AMC_LOGGER_ERROR_ID                                   */
/* ... AMC_LOGGER_CRITICAL, AMC_LOGGER_CRITICAL_ID                             */

#endif /* AMC_LOGGER_H_ */
```

Notes on deliberate details:

- **The `"" __VA_ARGS__` trick** concatenates the empty string literal with the payload
  format string. With no payload the format becomes `""` (rendered as `{}`, FQ3). As a
  bonus it makes a **non-literal format string a compile error** — Design §3 rule 5 is
  enforced by the compiler for free, and `__attribute__((format))` checks the arguments.
- **`##__VA_ARGS__`** (GNU, agreed toolchain) allows both `AMC_LOGGER_ERROR("Evt")` and
  `AMC_LOGGER_ERROR("Evt", "{...}", args)` through one macro.
- **Acquire/release on the call-site cache**: the pointer may be stored by one thread and
  loaded by another; acquire ensures the logger's construction is visible before use. On
  x86 both orderings are free; on macOS arm64 the acquire load is an `ldar` (negligible).
  The level check itself stays relaxed (Design §4.4).
- **Runtime-disabled calls do not evaluate payload arguments**; compile-stripped calls
  don't either. Argument expressions with side effects are a documented user error.
- Message passes if `level >= logger->level`; a logger at `OFF` (5) passes nothing.

## 4. Internal data structures — `src/AmcLoggerInternal.h`

```c
/* ---- limits (config validation ranges in §11) ---- */
#define AMC_MODULE_MAX        128     /* module name incl. NUL                  */
#define AMC_MSG_SIZE_MIN      256     /* max_message_size lower bound           */
#define AMC_MSG_SIZE_MAX      8192    /* upper bound; also sync-path stack buf  */
#define AMC_WORKER_BATCH_MAX  256     /* messages per worker checkout           */
#define AMC_FILE_IOBUF_SIZE   65536   /* setvbuf buffer for the file sink       */
#define AMC_TRUNC_MARKER      "...(truncated)"

enum amc_state    { AMC_UNINIT = 0, AMC_READY, AMC_SHUTDOWN };
enum amc_policy   { AMC_POLICY_BLOCK, AMC_POLICY_OVERRUN_OLDEST, AMC_POLICY_DISCARD_NEW };

/* ---- logger: public prefix + internal extension, one allocation ---- */
struct amc_logger_full {
    struct amc_logger        pub;      /* MUST be first (offset 0, _Static_assert'ed) */
    char                    *module;   /* owned copy, stripped basename               */
    struct amc_logger_full  *next;     /* registry chain                              */
};

/* ---- registry: permanent, mutex-protected, slow path only ---- */
struct amc_registry {
    pthread_mutex_t          mtx;      /* PTHREAD_MUTEX_INITIALIZER (works pre-init)  */
    struct amc_logger_full  *head;
};

/* ---- message: fixed-size slot header + payload bytes ---- */
struct amc_msg {
    struct timespec ts;                /* CLOCK_REALTIME captured in amc_logger_log   */
    const char     *module;            /* -> logger_full.module (permanent)           */
    const char     *function;          /* __func__, static storage                    */
    const char     *event;             /* string literal (Design D3 contract)         */
    int32_t         trader_id;
    uint8_t         level;
    uint8_t         has_id;
    uint8_t         truncated;         /* payload hit its cap at capture              */
    uint16_t        payload_len;
    /* payload bytes follow in the slot; slot stride =
       align_up(sizeof(struct amc_msg) + cfg.max_message_size, 64) */
};

/* ---- queue: bounded MPSC ring, mutex + condvars ---- */
struct amc_queue {
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;         /* worker + flush requests wait here           */
    pthread_cond_t  not_full;          /* producers wait here under `block`           */
    pthread_cond_t  flush_done;        /* amc_logger_flush() waiters                  */
    char           *slots;             /* queue_size * slot_stride, malloc'd at init  */
    uint32_t        capacity, slot_stride;
    uint32_t        head, count;       /* ring state, under mtx                       */
    uint64_t        seq_enq;           /* total accepted, under mtx                   */
    uint64_t        seq_flushed;       /* written + fflushed watermark, under mtx     */
    uint64_t        flush_req;         /* highest requested flush target, under mtx   */
    int             stop;              /* shutdown signal, under mtx                  */
};

/* ---- sink ---- */
struct amc_sink {
    pthread_mutex_t mtx;
    FILE           *fp;                /* stdout, or fopen'd file                     */
    char           *iobuf;             /* file sink only (never setvbuf on stdout)    */
    int             is_file;
    int             closed;            /* checked under mtx; post-close writes no-op  */
    int             dirty;             /* bytes written since last fflush             */
};

/* ---- parsed configuration (immutable after init) ---- */
struct amc_config {
    int  default_level, async, critical_sync;
    int  utc_offset_hours;
    enum amc_policy policy;
    uint32_t queue_size, max_message_size, flush_every_ms;
    int  has_stdout, has_file;
    char *file_path_template;          /* pre-expansion, owned                        */
    struct amc_cfg_logger { char *module; int level;
                            struct amc_cfg_logger *next; } *loggers;
};

/* ---- the global ---- */
struct amc_global {
    _Atomic int              state;    /* enum amc_state                              */
    struct amc_config        cfg;
    struct amc_sink          sinks[2]; /* [0]=stdout, [1]=file; fixed, no vector      */
    int                      sink_count;
    struct amc_queue         queue;
    pthread_t                worker;
    _Atomic uint64_t         st_enqueued, st_dropped_new, st_overwritten,
                             st_blocks, st_truncated, st_write_errors, st_high_water;
    _Atomic int              warned_before_init;   /* FQ1 one-shot                    */
};
extern struct amc_global g_amc;        /* BSS, zero-initialized                       */
```

Deliberate absences: no per-message logger pointer (the worker needs only what the slot
carries), no sink vector (exactly two possible sinks in v1), no refcounts, no heap
allocation after init on any path.

## 5. Concurrency design

### 5.1 Locks and atomics — who protects what

| Primitive | Protects | Held / used by |
| --------- | -------- | -------------- |
| `registry.mtx` | logger list lookup/create; OFF sweep at shutdown | `amc_logger_resolve` (once per call site), init, shutdown |
| `queue.mtx` | ring indices, seq counters, stop flag, policy waits | producers (enqueue), worker (checkout/ack), `amc_logger_flush`, shutdown |
| `sink.mtx` (each) | `fp`, `dirty`, `closed` — every write/flush/close | worker, `critical_sync` writers, sync-mode writers, flush, shutdown |
| `g_amc.state` (atomic) | lifecycle gate | everyone (acquire load / release store) |
| `logger.pub.level` (atomic) | runtime threshold | macros (relaxed load), shutdown (relaxed store `OFF`) |
| call-site static ptr (atomic) | cached logger | macros (acquire load / release store) |
| `st_*` counters (atomic) | statistics | all paths (relaxed), `amc_logger_get_stats` (relaxed) |

**Lock-ordering rule: no thread ever holds two different locks at once**, with one
exception — the worker holds the (at most two) sink mutexes together while writing a
batch, and it acquires them in fixed array order and holds no other lock while doing so.
`queue.mtx` is always released before any `sink.mtx` is taken. Deadlock is impossible by
construction.

### 5.2 Publication and the shutdown races

- **Init publishes to future threads** via the documented contract (init before spawning
  threads → `pthread_create` gives happens-before). The `state` release store and the
  registry mutex are belt-and-braces on top.
- **Post-shutdown safety** (Design §4.4 "late calls are no-ops") is achieved by never
  freeing: logger structs, the registry, and the queue slot memory are permanent for the
  process lifetime. Shutdown releases *OS* resources only (worker thread, `FILE*`s).
  A straggler producer that passed its level check just before the OFF sweep will at worst
  enqueue into a live-but-unconsumed ring (message dropped and counted) or hit a sink
  whose `closed` flag it observes under the sink mutex (write skipped). No use-after-free
  exists on any interleaving. Memory intentionally kept: registry + loggers + queue slots
  (reachable from `g_amc`, so leak checkers stay quiet).

## 6. Control flows

### 6.1 `amc_logger_log` (producer, hot path)

```
capture:   clock_gettime(CLOCK_REALTIME); build amc_msg header on stack
payload:   fmt[0]=='\0'  → payload = "{}" (FQ3)
           else vsnprintf into stack buf[AMC_MSG_SIZE_MAX], cap = cfg.max_message_size;
           ret >= cap → truncated = 1, st_truncated++
dispatch:  !cfg.async                        → §6.2 sync write
           level==CRITICAL && critical_sync  → §6.2 direct write (same code path)
           else                              → §6.3 enqueue
```

### 6.2 Sync / `critical_sync` write path (calling thread)

Render the full line into a stack buffer (§7), then for each configured sink in order:
`lock → if (!closed) fwrite(line) → if (level ≥ ERROR) fflush → unlock`. Auto-flush on
ERROR/CRITICAL is therefore inline in sync mode. `st_enqueued` counts these accepted
messages too (they were "accepted and written").

### 6.3 Enqueue (producer, async)

```
lock(queue.mtx)
  while count == capacity and policy == block and !stop:
        st_blocks++; wait(not_full)
  if stop:                        st_dropped_new++;  unlock; return   /* honest: never accepted */
  if count == capacity:
        policy discard_new →      st_dropped_new++;  unlock; return
        policy overrun_oldest →   head = (head+1)%capacity; count--; st_overwritten++
  slot = slots + ((head + count) % capacity) * slot_stride
  copy msg header + payload_len bytes into slot            /* ~250 B typical */
  count++; seq_enq++; st_enqueued++; high-water update
  if count == 1: signal(not_empty)
unlock
```

The payload is rendered *outside* the lock into a stack buffer and memcpy'd *inside* the
lock (§14.1 explains this refinement). Critical-section cost is a bounded memcpy plus
index arithmetic — tens of nanoseconds typically.

### 6.4 Worker loop (single consumer)

```
block all signals; pthread_setname("amc-worker")
loop:
  lock(queue.mtx)
    while count==0 and !stop and flush_req<=seq_flushed:
        timedwait(not_empty, deadline = last_flush + flush_every_ms)
        on timeout → break                      /* periodic flush tick */
    take   = min(count, AMC_WORKER_BATCH_MAX)   /* also capped by batch byte budget */
    copy 'take' messages out to the worker-private buffer (header + payload_len each)
    head = (head+take)%capacity; count -= take
    if policy==block and take>0: broadcast(not_full)
    batch_end_seq = seq_enq - count             /* seq of last message taken */
    want_flush    = (flush_req > seq_flushed)
    stopping      = stop
  unlock
  render each message (§7) and write the batch:
       lock sinks[0..n] in order → fwrite each line to each open sink → unlock all
  need_flush = batch contained level ≥ ERROR
            or want_flush
            or (now - last_flush ≥ flush_every_ms and any sink dirty)
  if need_flush: fflush each sink (under its mutex); last_flush = now
  lock(queue.mtx)
    if need_flush: seq_flushed = batch_end_seq; broadcast(flush_done)
  unlock
  if stopping and queue drained: emit loss summary if any (§10), final fflush, exit
```

Waiting uses a `CLOCK_REALTIME`-based absolute deadline (macOS has no
`pthread_condattr_setclock`); a wall-clock step can jitter one flush tick — harmless,
documented.

### 6.5 `amc_logger_flush` (async)

```
lock(queue.mtx)
  target = seq_enq
  flush_req = max(flush_req, target); signal(not_empty)
  while seq_flushed < target and !stop: wait(flush_done)
unlock
```

Multiple concurrent flushers coexist via the monotonic watermarks. In sync mode the
function just fflushes both sinks under their mutexes. Returns −1 unless state is READY.

### 6.6 `amc_logger_init` (single-threaded by contract)

1. `state != UNINIT` → stderr diagnostic, return −1 (failed init stays UNINIT, retryable).
2. `config_path == NULL` → `amc_internal_config_defaults()`; else read file (size-capped),
   `amc_internal_config_parse()` → on error print `amc_logger: config: <file>:<line>: <msg>`,
   return −1.
3. Open sinks: stdout (flag only), file (expand `$TODAY`/`$PID` → `mkdir -p` prefix →
   `fopen(…, "a")` → `setvbuf` 64 KB). Any failure → close what opened, return −1.
4. Create predefined loggers from `cfg.loggers` (registry insert with their levels).
5. If async: allocate `queue_size × slot_stride`, init mutex/conds; `pthread_create`
   worker — failure unwinds sinks + queue, return −1.
6. `state = READY` (release). Return 0.

### 6.7 `amc_logger_shutdown`

1. `state != READY` → return 0 (idempotent; also benign before init).
2. Registry sweep: every logger level ← `OFF` (relaxed stores).
3. Async: `lock(queue.mtx); stop = 1; broadcast(not_empty, not_full, flush_done); unlock;`
   `pthread_join(worker)` — the worker drains everything already accepted, emits the final
   loss summary, and fflushes before exiting.
4. Per sink under its mutex: `fflush`, `closed = 1`, `fclose` if `is_file` (never close
   stdout). 5. `state = SHUTDOWN` (release). Return 0.

## 7. Rendering — `AmcLoggerRender.c`

Line assembly is hand-rolled `memcpy` of fixed fragments plus a small unsigned/signed
itoa — no `snprintf` on the per-line path (≈50 ns vs ≈250 ns; needed for the 1 M lines/s
drain target).

- **Timestamp cache**: thread-local `{ time_t sec; char prefix[21]; }` holding
  `"YYYY-MM-DD HH:MM:SS."`. On a new second: `t = ts.tv_sec + utc_offset_hours*3600`,
  `gmtime_r`, re-render the prefix. Same-second messages pay only a 6-digit
  zero-padded microsecond render. Thread-local so the worker, sync-mode callers, and
  `critical_sync` callers share one implementation without sharing state.
- **Assembly**: `[prefix+usec][LEVEL][module][function][event][id|-]payload\n` with level
  names from a fixed table.
- **Truncation**: if the assembled length would exceed `cfg.max_message_size`, the line is
  cut so that `"...(truncated)\n"` exactly fits at the end (the marker is never itself
  cut); `st_truncated++` — this catches header+payload overflow even when the payload
  alone fit its capture cap.
- Testing seam: with `-DAMC_LOGGER_TESTING`, the clock is read through an overridable
  hook, and `amc_internal_test_reset()` tears everything down to `AMC_UNINIT` so one
  test binary can simulate many process lifetimes (§14.12). Production builds call
  `clock_gettime` directly and have no reset.

## 8. Sinks — `AmcLoggerSink.c`

- **Path expansion**: scan the template once; `$TODAY` → `YYYY-MM-DD` (at
  `utc_offset_hours`), `$PID` → `getpid()`. Any other `$NAME` → init error (fail fast).
  Expansion happens exactly once, at open (Q14).
- **`mkdir -p`**: create each `/`-separated prefix with mode `0777` (umask applies);
  `EEXIST` ignored; other errno → init error with the failing component in the message.
- **stdout sink**: uses the process's `stdout` as-is — **no `setvbuf`** (the stream is
  shared with the application; one `fwrite` per line stays atomic against the app's own
  stdio use). Never fclosed.
- **file sink**: own `FILE*`, 64 KB `setvbuf` full buffering — batch writes come free.
- **Write errors**: on `fwrite`/`fflush` failure, `st_write_errors++` and a rate-limited
  stderr report (first occurrence, then at most one per 1000); the library never
  terminates the process.

## 9. Config parser — `AmcLoggerConfig.c`

A pure function, no globals — the unit-test workhorse:

```c
int amc_internal_config_parse(const char *text, size_t len, const char *diag_name,
                              struct amc_config *out, char *err, size_t err_sz);
void amc_internal_config_defaults(struct amc_config *out);
```

Two layers:

1. **Line scanner**: split into lines; per line compute the space-indent (any tab in
   indentation → error), strip comments (`#` outside quotes), classify: blank / `%YAML`
   directive / `---` / `key:` / `key: value`. Scalars: plain, `'single'`,
   `"double"` (escapes: `\"` `\\` only). The token `{}` is the empty flow mapping.
2. **Indent-driven descent**: root mapping → known scalar keys
   (`default_level async queue_size queue_full_policy max_message_size flush_every_ms
   utc_offset_hours critical_sync`) and two section keys (`sinks`, `loggers`). Children
   of a section must share one common indent deeper than their parent. `sinks` accepts
   only `stdout: {}` and `basic_file: <path>`; `loggers` accepts `<Module>: <LEVEL>`
   entries (module charset `[A-Za-z0-9_.-]`, ≤ 127 chars).

Fail-fast semantics (Design §7.2): first error wins; message format
`amc_logger: config: <file>:<line>: <what>`, e.g. `unknown key 'que_size'`. Detected:
unknown keys, duplicate keys, tabs, out-of-subset syntax (sequences, anchors, multiline,
flow), bad enum values, out-of-range numbers (§11), unknown `$VAR` (checked at expansion,
still init time). Level names case-insensitive. `amc_logger_init(NULL)` ≡ parsing an
empty document (FQ2): defaults, stdout only.

## 10. Errors and observability

- **Before-init warning** (FQ1): `amc_logger_resolve` in state UNINIT does a relaxed
  exchange on `warned_before_init`; the first caller prints
  `amc_logger: log call before amc_logger_init(); messages dropped` to stderr. Returns
  NULL (call site stays uncached and retries after init).
- **Loss summaries**: on each flush tick the worker compares loss counters against the
  previously reported values and, if anything grew, writes a line *through its own
  format* directly to the sinks:
  `[ts][WARN][AmcLogger][worker][MessageLoss][-]{"dropped_new":N,"overwritten_old":N,"truncated":N,"write_errors":N}`
  A final summary is emitted during shutdown drain. (Summary lines bypass the queue and
  are not counted in `enqueued`.)
- **Stats**: relaxed atomic counters, `amc_logger_get_stats` readable at any time,
  including before init (zeros) — it never blocks on any lock.

## 11. Constants and config validation ranges

| Key / constant | Range | Default |
| -------------- | ----- | ------- |
| `default_level` / logger levels | DEBUG…CRITICAL, OFF | INFO |
| `async` | true/false | true |
| `queue_size` | 16 … 1,048,576 | 8192 |
| `queue_full_policy` | block, overrun_oldest, discard_new | block |
| `max_message_size` | 256 … 8192 (`AMC_MSG_SIZE_MIN/MAX`, §14.4) | 2048 |
| `flush_every_ms` | 0 (off) … 3,600,000 | 1000 |
| `utc_offset_hours` | −12 … +14 | 8 |
| `critical_sync` | true/false | false |
| module name | ≤ 127 chars | — |
| worker batch | ≤ 256 msgs / 256 KB private buffer | — |
| config file size | ≤ 1 MB | — |
| queue memory | `queue_size × (sizeof(amc_msg)+max_message_size)` → defaults ≈ 17 MB | — |

## 12. Build system and CI

- **CMake ≥ 3.20**: target `amc_logger` (STATIC), `C_STANDARD 11` + `C_EXTENSIONS ON`
  (= gnu11), warnings `-Wall -Wextra -Wformat=2 -Werror`, link `Threads::Threads`.
  Alias `AntiMatter::AmcLogger`; basic `install()` of the lib + header.
- **Options**: `AMC_BUILD_TESTS` (ON when top-level), `AMC_BUILD_BENCH` (OFF),
  `AMC_BENCH_SPDLOG` (OFF; FetchContent, C++ confined to `bench/BenchSpdlog.cpp`),
  `AMC_BUILD_EXAMPLES` (OFF), `AMC_SANITIZE=off|address|thread|undefined`.
- **CI** (GitHub Actions): matrix {ubuntu (gcc-13), macos-14 (AppleClang)} ×
  {Release, Debug+ASan/UBSan, Debug+TSan} → build, ctest. Bench targets are
  build-verified in CI, not timed.

## 13. Test and benchmark plan (maps Design §11/§10)

The tests use the vendored **Unity** framework (`third_party/unity`, v2.6.1, MIT) and
are organized as **documentation chapters** written from the user's point of view —
read `tests/Test01…Test09` in order and you have read the user manual:

| Chapter | What a user learns (and what it proves) |
| ------- | --------------------------------------- |
| 01 QuickStart | init(NULL) → log → shutdown; one macro per level; default INFO; `{}` payload; `_ID` variants |
| 02 LogFormat | every field of the line, per-file MODULE attribution, printf payload composition, chronological text order |
| 03 Levels | default_level, per-module overrides, OFF, case-insensitive names, filtered calls don't evaluate args |
| 04 Configuration | the reference config; the fail-fast battery: unknown key/level/sink, tabs, sequences, duplicates, `$VAR`, ranges — each with its `file:line:` diagnostic |
| 05 FileSink | sink selection is exact; `$TODAY`/`$PID`; auto mkdir; append across restarts; stdout+file byte-identical |
| 06 Lifecycle | before-init warn-once + drop, double init, idempotent shutdown, post-shutdown no-op, flush gating, stats anytime |
| 07 AsyncAndReliability | flush visibility, the three overflow policies with honest accounting + MessageLoss summary, truncation marker, critical_sync, sync mode |
| 08 CompileTimeStrip | `AMC_LOGGER_ACTIVE_LEVEL` removes calls from the binary (compiled with `-D…=WARN`) |
| 09 MultiThreaded | concurrent producers: lossless under `block`, intact lines, per-thread program order |

The full suite passes under ThreadSanitizer and ASan+UBSan (CI runs those legs).
Bench (`-DAMC_BUILD_BENCH=ON`, always compiled -O2): `BenchDisabled`, `BenchEnqueue`
(burst percentiles + a paced 10k msg/s phase measuring the worker-wake cost),
`BenchThroughput`, and `BenchSpdlog` (`-DAMC_BENCH_SPDLOG=ON`, FetchContent, C++
confined to that target). Measured results and the tail investigation: §14.13.

## 14. Refinements relative to Design.md — please eyeball these

1. **Payload is rendered into a stack buffer, then memcpy'd into the slot under the queue
   mutex** (Design §4.2 sketched "render into the slot"). Formatting inside the lock
   would serialize producers; slot-claiming before formatting would need a lock-free
   reservation scheme (principle: no lock-free in v1). Cost: one ≈250 B copy (~15 ns).
2. **The worker copies each batch out under the lock** into a private buffer instead of
   reading ring slots after unlock. Zero-copy checkout is unsafe against
   `overrun_oldest` overwriting in-flight slots; the copy (~15 ns/msg typical) buys
   policy-independent correctness. Zero-copy is a listed future optimization.
3. **Public struct exposure**: `struct amc_logger { _Atomic int level; }` appears in the
   public header (extended internally via containment) so the disabled-call check inlines
   to one relaxed load — required for the ≤ ~2 ns target. `amc_logger_resolve` /
   `amc_logger_log` are likewise public-by-necessity macro plumbing (Design §8 listed
   only the user-facing four functions).
4. **`max_message_size` range is [256, 8192]** — new upper bound so sync-mode and
   `critical_sync` paths can use fixed 8 KB stack buffers instead of dynamic allocation.
5. **Shutdown never frees** logger structs, the registry, or queue slot memory (still
   reachable from `g_amc`) — this is what makes "late calls are no-ops" airtight without
   refcounting. OS resources (thread, files) are released.
6. **The header is C-only in v1** (`_Atomic` in the macro). C++ inclusion support is
   parked (a `__cplusplus` branch using `std::atomic` or `__atomic` builtins is easy to
   add later).
7. **stdout buffering is left untouched** (stream shared with the app); only the file
   sink gets a private 64 KB buffer.
8. **Failed init is retryable** ("a second call fails" in Design §4.4 applies after a
   *successful* init); `amc_logger_flush()` returns −1 when not READY.
9. **Blocked producers woken by shutdown drop their message** (counted in `dropped_new`)
   — the message had never been accepted, and waiting for space that will never appear
   would deadlock the join.
10. **The worker writes line-at-a-time** through the same emit path as sync mode (one
    sink lock per line) instead of holding both sink mutexes across a batch; only the
    *dequeue* is batched. This strengthens "no thread ever holds two locks" to absolute
    and shares one code path; revisit only if the 1 M lines/s bench target is missed.
11. **The macros prepend a 1-byte sentinel** (`"\1" __VA_ARGS__`) to the payload format
    instead of an empty string. Same literal-enforcement trick, but the literal is never
    zero-length — otherwise every payload-less call would trigger
    `-Wformat-zero-length` in user builds compiled with `-Wall`. `amc_logger_log`
    skips the sentinel byte.
12. **`amc_internal_test_reset()`** (AMC_LOGGER_TESTING only) tears down to `AMC_UNINIT`
    so test binaries can simulate many process lifetimes. Registry loggers deliberately
    survive resets — call-site caches must stay valid — so `amc_logger_init` first
    sweeps every existing logger to the new `default_level` before applying overrides.
    The sweep is harmless in production, where the registry is empty at first init.
13. **The saturated-tail investigation** (benchmark-driven; supersedes the §14.2
    rationale). The numeric targets exposed a chain of real effects; the final queue
    design is their sum:
    - Copy-out-under-lock checkout held the queue mutex across a 64-message copy;
      producer collisions became futex sleeps. Replaced by **zero-copy checkout with
      in-flight accounting** for `block`/`discard_new`. TSan then proved
      `overrun_oldest` incompatible with zero-copy — with a full ring, the
      post-overwrite tail insert lands exactly on the first in-flight slot — so that
      policy alone keeps copy-out (policy-split checkout).
    - `PTHREAD_MUTEX_ADAPTIVE_NP`: measured, rejected — producer-side spinning starves
      the worker (drain halves) without improving the tail.
    - Ring pre-faulting at init: kept as hygiene (first-touch page faults off the hot
      path), though it was not the measured tail.
    - The dominant mechanism (found via strace: 68k futex syscalls per ~150k
      messages): a worker that drains as fast as production keeps the queue near
      EMPTY, so it parked on the condvar between batches and nearly every enqueue paid
      a futex wake + IPI. spdlog's clean saturated tail is the same effect mirrored —
      its several-times-slower worker never parks. Fixes (standard condvar practice):
      producers signal only when `worker_waiting` is set, and only after unlocking;
      the worker acks the previous batch inside the next checkout's critical section
      (one lock per batch, not two); the worker spin-polls an atomic enqueue hint for
      ~5 µs — exiting early once ~8 messages accumulate — before parking. Futex count
      fell to ~13k, almost all legitimate sparse-traffic wakes.
    - Final numbers vs spdlog v1.14.1 async, same dev VM, gcc-13 -O2, two pinned
      cores: p50 **208 vs 383 ns**, drain **1.7–3.6M vs 0.5–0.9M lines/s**, saturated
      p99 **5.3 vs 1.6 µs** — same order of magnitude, better median and throughput
      (the Q19 acceptance). The saturated p99 misses the aspirational 1 µs; the
      residual is mutex cacheline traffic plus VM scheduler noise, intrinsic to a
      mutex+condvar queue at saturation. At the production envelope (~10k msg/s) the
      regime differs: sparse calls pay one worker wake (~3 µs, the paced bench
      phase), micro-burst calls pay ~200 ns, and the worker's idle spin costs ~5% of
      one core.
14. **`AMC_JSON` payload composer** (agreed amendment, Design §3 rule 6). A FOR-EACH
    preprocessor block in the public header turns `(key, fmt, value…)` tuples into the
    raw form's exact single format literal via string concatenation — braces, quoted
    keys, colons and commas are generated, so malformed payload structure becomes a
    compile error, while the expansion is byte-identical to the hand-written form
    (proved by a Test10 equality test): zero runtime cost, printf checking and the
    literal-only contract preserved. Value expansion embeds a leading `, ##__VA_ARGS__`
    so tuples may carry zero value arguments (constant fragments like
    `("armed", "true")`). Capacity 1–16 pairs; 17+ dispatches to the deliberately
    undefined `AMC_JSON_FMT_TOO_MANY_KEYS`, making the compile error self-explanatory.
    Typed sugar `AMC_KV_INT/I64/U64/F64/STR/BOOL` expands to plain tuples. No `src/`
    changes; runtime escaping of `%s` values stays deferred (Design §13).

## 15. Implementation order (each milestone ends green)

- **M1 — walking skeleton**: CMake + full header + Core (sync mode only) + Render +
  stdout sink + `init(NULL)`; the §9 example runs and matches golden output.
- **M2 — config + file sink**: parser with full diagnostics battery, path expansion,
  `mkdir -p`; TestConfig/TestModule/TestRender complete.
- **M3 — async**: queue, policies, worker, flush watermark, shutdown drain; TestQueue,
  TestLifecycle, TestStress under TSan.
- **M4 — reliability trim**: `critical_sync`, loss summaries, stats surface, before-init
  warning, write-error reporting.
- **M5 — proof**: benchmarks vs targets (and optional spdlog run), macOS CI leg,
  examples/config polish, README.
