#ifndef AMC_LOGGER_H_
#define AMC_LOGGER_H_

/*
 * AmcLogger — AntiMatterLogger public API.
 *
 * User-facing surface: amc_logger_init / amc_logger_shutdown / amc_logger_flush /
 * amc_logger_get_stats and the AMC_LOGGER_* macros. Every other visible symbol is
 * macro plumbing — never call it directly.
 *
 * Contracts (see docs/Design.md):
 *  - call amc_logger_init() once, before spawning threads
 *  - EVENT and the payload format string must be string literals
 *  - payload arguments are NOT evaluated when the call is filtered out
 *  - logging from signal handlers is unsupported
 */

#ifdef __cplusplus
#error "AmcLogger.h is C-only in v1 (uses C11 _Atomic in its macros)"
#endif

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
int amc_logger_init(const char *config_path); /* NULL = built-in defaults (empty config).
                                                 0 ok; -1 + stderr diagnostic on error.
                                                 A failed init leaves the library
                                                 uninitialized and may be retried.     */
int amc_logger_shutdown(void);                /* drain, join, flush, close; idempotent */
int amc_logger_flush(void);                   /* block until everything accepted before
                                                 the call is written and fflushed;
                                                 -1 if the library is not initialized  */

/* ---- Statistics (monotonic counters since init) ---- */
struct amc_logger_stats {
    uint64_t enqueued;         /* messages accepted (queued, or written in sync mode) */
    uint64_t dropped_new;      /* discard_new rejections + drops during shutdown      */
    uint64_t overwritten_old;  /* overrun_oldest overwrites                           */
    uint64_t producer_blocks;  /* waits caused by a full queue under `block`          */
    uint64_t truncated;        /* lines cut at max_message_size                       */
    uint64_t write_errors;     /* failed sink writes/flushes                          */
    uint64_t queue_high_water; /* maximum queue occupancy observed                    */
};
void amc_logger_get_stats(struct amc_logger_stats *out); /* callable at any time */

/* ---- Macro plumbing (do not use directly) ---- */

/* Public hot-path prefix of a logger. `level` is the only field the macros read;
 * the implementation extends this struct internally. Treat as read-only. */
struct amc_logger {
    _Atomic int level;
};

/* Derives the module name from `file` and returns the (auto-created) logger.
 * Returns NULL before init / after shutdown; the first pre-init call prints a
 * one-time stderr warning. */
struct amc_logger *amc_logger_resolve(const char *file);

void amc_logger_log(struct amc_logger *logger, int level,
                    const char *function, const char *event,
                    int has_id, int trader_id,
                    const char *payload_fmt, ...)
                    __attribute__((format(printf, 7, 8)));

/* The `"\1" __VA_ARGS__` concatenation makes a non-literal format string a
 * compile error and lets a missing payload become the 1-byte sentinel string
 * (rendered as {}). The implementation skips the sentinel byte; it exists so
 * the literal is never zero-length (silences -Wformat-zero-length in user
 * builds). Escapes are processed before literal concatenation, so the
 * sentinel can never absorb characters from the payload string. */
#define AMC_LOGGER_LOG_IMPL_(level_, event_, has_id_, id_, ...)               \
    do {                                                                      \
        static struct amc_logger *_Atomic amc_site_logger_;                   \
        struct amc_logger *amc_lg_ = atomic_load_explicit(                    \
            &amc_site_logger_, memory_order_acquire);                         \
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
                           (has_id_), (id_), "\1" __VA_ARGS__);               \
    } while (0)

#if AMC_LOGGER_ACTIVE_LEVEL <= AMC_LOGGER_LEVEL_DEBUG
#define AMC_LOGGER_DEBUG(event_, ...)                                          \
    AMC_LOGGER_LOG_IMPL_(AMC_LOGGER_LEVEL_DEBUG, (event_), 0, 0, ##__VA_ARGS__)
#define AMC_LOGGER_DEBUG_ID(event_, trader_id_, ...)                           \
    AMC_LOGGER_LOG_IMPL_(AMC_LOGGER_LEVEL_DEBUG, (event_), 1, (trader_id_), ##__VA_ARGS__)
#else
#define AMC_LOGGER_DEBUG(event_, ...)                ((void)0)
#define AMC_LOGGER_DEBUG_ID(event_, trader_id_, ...) ((void)0)
#endif

#if AMC_LOGGER_ACTIVE_LEVEL <= AMC_LOGGER_LEVEL_INFO
#define AMC_LOGGER_INFO(event_, ...)                                           \
    AMC_LOGGER_LOG_IMPL_(AMC_LOGGER_LEVEL_INFO, (event_), 0, 0, ##__VA_ARGS__)
#define AMC_LOGGER_INFO_ID(event_, trader_id_, ...)                            \
    AMC_LOGGER_LOG_IMPL_(AMC_LOGGER_LEVEL_INFO, (event_), 1, (trader_id_), ##__VA_ARGS__)
#else
#define AMC_LOGGER_INFO(event_, ...)                ((void)0)
#define AMC_LOGGER_INFO_ID(event_, trader_id_, ...) ((void)0)
#endif

#if AMC_LOGGER_ACTIVE_LEVEL <= AMC_LOGGER_LEVEL_WARN
#define AMC_LOGGER_WARN(event_, ...)                                           \
    AMC_LOGGER_LOG_IMPL_(AMC_LOGGER_LEVEL_WARN, (event_), 0, 0, ##__VA_ARGS__)
#define AMC_LOGGER_WARN_ID(event_, trader_id_, ...)                            \
    AMC_LOGGER_LOG_IMPL_(AMC_LOGGER_LEVEL_WARN, (event_), 1, (trader_id_), ##__VA_ARGS__)
#else
#define AMC_LOGGER_WARN(event_, ...)                ((void)0)
#define AMC_LOGGER_WARN_ID(event_, trader_id_, ...) ((void)0)
#endif

#if AMC_LOGGER_ACTIVE_LEVEL <= AMC_LOGGER_LEVEL_ERROR
#define AMC_LOGGER_ERROR(event_, ...)                                          \
    AMC_LOGGER_LOG_IMPL_(AMC_LOGGER_LEVEL_ERROR, (event_), 0, 0, ##__VA_ARGS__)
#define AMC_LOGGER_ERROR_ID(event_, trader_id_, ...)                           \
    AMC_LOGGER_LOG_IMPL_(AMC_LOGGER_LEVEL_ERROR, (event_), 1, (trader_id_), ##__VA_ARGS__)
#else
#define AMC_LOGGER_ERROR(event_, ...)                ((void)0)
#define AMC_LOGGER_ERROR_ID(event_, trader_id_, ...) ((void)0)
#endif

#if AMC_LOGGER_ACTIVE_LEVEL <= AMC_LOGGER_LEVEL_CRITICAL
#define AMC_LOGGER_CRITICAL(event_, ...)                                       \
    AMC_LOGGER_LOG_IMPL_(AMC_LOGGER_LEVEL_CRITICAL, (event_), 0, 0, ##__VA_ARGS__)
#define AMC_LOGGER_CRITICAL_ID(event_, trader_id_, ...)                        \
    AMC_LOGGER_LOG_IMPL_(AMC_LOGGER_LEVEL_CRITICAL, (event_), 1, (trader_id_), ##__VA_ARGS__)
#else
#define AMC_LOGGER_CRITICAL(event_, ...)                ((void)0)
#define AMC_LOGGER_CRITICAL_ID(event_, trader_id_, ...) ((void)0)
#endif

#endif /* AMC_LOGGER_H_ */
