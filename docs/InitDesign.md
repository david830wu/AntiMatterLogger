# Logger library AntiMatterLogger

> **Superseded (2026-07-22)**: this file is the original draft, kept for history. The agreed,
> current design is [Design.md](Design.md); where the two differ, Design.md wins.

## 1. Features
- Very fast
- The codebase is organized as small and simple as possible
- Asynchronous mode
- Multi/Single threaded loggers
- Various log targets:
  - log files
  - Console logging
- Log filtering - multiple log levels are supported, auto flush after ERROR and CRITICAL log
- Support for loading configurations from yaml file

## 2. Log format

```
[YYYY-MM-DD HH:MM:SS.ssssss][LEVEL][MODULE][FUNCTION][EVENT][TRADER_ID]PAYLOAD
```

| Field | Description | Example | Comments |
| ------ | ----------- | ------- | -------- |
| `YYYY-MM-DD HH:MM:SS.ssssss` | Log timestamp (microsecond precision) | `2025-10-13 15:00:02.834470` | UTC+8 |
| `LEVEL` | Severity level | `DEBUG / INFO / WARN / ERROR / CRITICAL` | One of the given 5 enumerates, All capital letters |
| `MODULE` | System module | `LongTermExtractor, LongTermActionInterpreter` | Module name, defined to be the basename of source code file excluding ".c" extend in C |
| `FUNCTION` | Function name | `long_term_action_interpreter_init` | Function identifier |
| `EVENT` | Event | `Reset, VolumeError` | Call site identifier |
| `TRADER_ID` | Trading account | `61 / 800` | Any integer or `-`; numeric account identifier. Use `-` when not associated with any account |
| `PAYLOAD` | JSON FORMAT | `{"error_id":"10001","threshold_volume":1000,"order_volume":1200,"message":"order volume exceeds threshold"}` | `{"key1":val1,"key2":val2}`; supports nested objects, numbers, strings |

Notes:
1. No spaces between brackets; no space before the payload.
2. Per code conventions: class names use UpperCamelCase (PascalCase); function names use lowercase with underscores (snake_case).
3. Event strings are user-defined and must use UpperCamelCase (PascalCase).

## 3. Thread safety
The central idea is:
> Synchronize the shared output resource—the sink—not the entire logger.

This gives good concurrency while preventing data races in files, consoles, formatters, and rotation state. It does not make every operation automatically thread-safe. Correct use requires choosing `_mt` sinks and treating configuration structures as immutable while logging.

### Threading model
A synchronous log call approximately follows:
```text
calling thread
    │
    ├─ atomic level check
    ├─ format user arguments into a local buffer
    ├─ iterate logger's sink vector
    │     ├─ atomic sink-level check
    │     └─ lock that sink
    │           ├─ apply sink's formatter
    │           ├─ write/rotate/flush
    │           └─ unlock
    └─ return
```
There is no logger-wide lock. Consequently:
Different sinks can operate concurrently.
Different loggers using different sinks do not block each other.
Multiple loggers sharing one `_mt` sink are correctly serialized by that shared sink.
A write is atomic only relative to one sink. A multi-sink logger does not provide an atomic transaction across all its sinks.

### Main concepts and trade-offs

| Concept | How races are prevented | Performance trade-off |
|---|---|---|
| `_mt` and `_st` sinks | `_mt` uses a real mutex; `_st` substitutes a no-op `null_mutex` | `_st` removes locking cost but is unsafe under concurrent access |
| Per-sink locking | Formatting to final output, writing, rotation, flushing, and formatter replacement use the same sink mutex | Contention is limited to threads targeting the same destination |
| Atomic levels | Logger and sink levels use `atomic_int` with relaxed loads/stores | Runtime filtering is cheap and needs no mutex |
| Formatter per sink | Each sink receives its own formatter instance | Avoids shared formatter state and cross-sink races at a small memory cost |
| Stable sink vector | The logger reads its sink vector without locking | Very fast fan-out, but concurrent modification is undefined behavior |
| Locked global registry | Registration and lookup use a registry mutex | Safe lookup, but repeated `amc_logger_get()` is unsuitable for a hot path |
| Async bounded queue | Producers enqueue owned messages; workers perform sink I/O | Removes I/O latency from producers but adds queue synchronization and copying |
| Overflow policies | Full queue can block, overwrite the oldest message, or discard the new message | Explicit choice between reliability, backpressure, and latency |

1. `_mt` versus `_st`
The names do not represent two different logger classes. For example:
```c
amc_logger_basic_logger_mt(...)  // uses amc_logger_basic_file_sink_mutex
amc_logger_basic_logger_st(...)  // uses amc_logger_basic_file_sink_null_mutex
```
`amc_logger_base_sink_mutex` locks the chosen mutex around log, flush, set_pattern, and set_formatter. Therefore the mutex protects both output state and formatter state. Sink locking implementation
This compile-time policy avoids a runtime branch. It also makes the contract visible in the type name:
Use `_mt` whenever a sink may be reached from multiple threads.
Use `_st` only with one thread or external serialization.
`amc_logger_flush_every()` introduces another thread, so it requires `_mt` loggers.

2. Small atomic state instead of a logger mutex
Log and flush levels are atomics. The hot-path check uses relaxed ordering.
Relaxed ordering is sufficient because the level is an independent configuration value; it is not being used to publish other data. A concurrent level change is race-free, although a call occurring at the same instant may legitimately observe either the old or new threshold.
`AMC_LOGGER_NO_ATOMIC_LEVELS` replaces these atomics with ordinary integers for additional optimization. That option is safe only when levels are never modified concurrently. 
3. Formatting placement
Library divides formatting into two stages:
User arguments are formatted into a call-local buffer before acquiring the sink mutex.
Pattern formatting—timestamp, logger name—is performed inside the sink.
This keeps relatively expensive argument formatting outside the critical section. Each sink owns a formatter clone, allowing it to cache state without sharing that mutable state with other sinks.
4. Asynchronous logging
Async logging does not eliminate synchronization; it moves slow sink work off producer threads:
```text
producer threads
    └─ format + deep-copy message
          └─ bounded MPMC queue
                └─ worker thread(s)
                      └─ sink mutex + I/O
```
The queue is a mutex-and-condition-variable protected circular queue, not a lock-free queue. Slots are preallocated, preventing unbounded growth and most steady-state queue allocations.

Queued messages own copies of the logger name, payload, and source-location strings, while each queued item holds a shared pointer to its logger. This prevents dangling caller buffers and premature logger destruction. 

Queue-full behavior is an explicit policy:
- `block`: lossless, but producer latency can become sink latency under overload.
- `overrun_oldest`: bounded producer latency, but loses old messages.
- `discard_new`: preserves already queued messages but drops new arrivals.

The default single worker preserves queue order. Multiple workers may reorder messages after dequeueing. Even with one worker, simultaneous producers are ordered by queue acquisition, not necessarily by timestamp or real-world event order. 

## 4. Output sinks

In current stage, the following sinks are required:
- `File sinks`: basic_file_sink
- `Console and stream sinks`: stdout_sink

## 5. Configuration file

```yaml
%YAML 1.2
---
default_level: "INFO"
queue_full_policy: block

sinks:
    stdout: {} 
    basic_file: "./log/$TODAY/MyApp.$PID.log"

predefined_logger:
    main: "INFO"
    LongTermExtractor: "DEBUG"
```

## 6. Usage examples
```c
/*filename: LongTermExtractor.h */
#ifndef LONG_TERM_EXTRACTOR_H_
#define LONG_TERM_EXTRACTOR_H_

struct LongTermExtractor;

struct LongTermExtractor* long_term_extractor_init(int trader_id, int value);
int long_term_extractor_close(struct LongTermExtractor* p_lte);

#endif
```

```c
/*filename: LongTermExtractor.c*/
#include "LongTermExtractor.h"
#include "AmcLogger.h"
#include <stdlib.h>

struct LongTermExtractor {
    int trader_id;
    int value;
};

struct LongTermExtractor* long_term_extractor_init(int trader_id, int value) {
    struct LongTermExtractor* p_result = malloc(sizeof(struct LongTermExtractor));
    p_result->trader_id = trader_id;
    p_result->value = value;
    AMC_LOGGER_INFO_ID("ExtractorInit", p_result->trader_id, "{\"trader_id\":%d,\"value\":%d}", p_result->trader_id, p_result->value);
    return p_result;
}
int long_term_extractor_close(struct LongTermExtractor* p_lte) {
    if(p_lte == NULL) {
        AMC_LOGGER_ERROR("ReleaseNullPointer");
        return -1;
    }
    AMC_LOGGER_INFO_ID("ExtractorClose", p_lte->trader_id, "{\"value\":%d}", p_lte->value);
    free(p_lte);
    return 0;
}

```

```c
/*filename: main.c */
#include "AmcLogger.h"
#include "LongTermExtractor.h"

int main() {
    amc_logger_init("config/logger.yaml");
    AMC_LOGGER_DEBUG("ShowLevel", "{\"level\":\"%s\"}", "debug");
    AMC_LOGGER_INFO("PrintPi", "{\"pi\":%.7f}", 3.1415926);
    AMC_LOGGER_WARN("VagueAnswer", "{\"answer\":%d}", 42);
    AMC_LOGGER_ERROR("InstrumentError", "{\"instrument\":\"%06d\"}", 42);
    AMC_LOGGER_CRITICAL("TypeMisMatch", "{\"lhs\":\"%s\",\"rhs\":\"%s\"}", "cat", "fruit");
    
    struct LongTermExtractor* p_lte = long_term_extractor_init(100, 42);
    long_term_extractor_close(p_lte);
    
    amc_logger_shutdown();
    return 0;
}
```

expected output:
```text
[2016-07-21 13:56:16.432145][DEBUG][main][main][ShowLevel][-]{"level":"debug"}
[2016-07-21 13:56:16.432145][INFO][main][main][PrintPi][-]{"pi":3.1415926}
[2016-07-21 13:56:16.432146][WARN][main][main][VagueAnswer][-]{"answer":42}
[2016-07-21 13:56:16.432147][ERROR][main][main][InstrumentError][-]{"instrument":"000042"}
[2016-07-21 13:56:16.432147][CRITICAL][main][main][TypeMisMatch][-]{"lhs":"cat","rhs":"fruit"}
[2016-07-21 13:56:16.432148][INFO][LongTermExtractor][long_term_extractor_init][ExtractorInit][100]{"trader_id":100,"value":42}
[2016-07-21 13:56:16.432149][INFO][LongTermExtractor][long_term_extractor_close][ExtractorClose][100]{"value":42}
```
