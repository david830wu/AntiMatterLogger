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
#include <stddef.h>   /* NULL — amc_logger_init(NULL) is the canonical first call */
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

/* ---- AMC_JSON: compile-time JSON payload composer -----------------------
 *
 * Builds the payload from (key, printf_fmt, value...) tuples instead of a
 * hand-written format string. It expands to EXACTLY the raw form's single
 * string literal plus argument list, so it costs nothing at runtime and the
 * compiler still type-checks every value against its format:
 *
 *     AMC_LOGGER_ERROR("VolumeError", AMC_JSON(("threshold", "%d", 1000),
 *                                              ("volume",    "%d", volume)));
 *     -> {"threshold":1000,"volume":1200}
 *
 * Keys are quoted automatically; braces, colons and commas are generated —
 * malformed JSON structure becomes a compile error instead of a bad log line.
 * Typed helpers cover the common cases (string values get their quotes for
 * free); a raw tuple gives full printf control and may hold several values,
 * which also covers nested objects and constant fragments:
 *
 *     AMC_JSON(AMC_KV_INT("volume", v), AMC_KV_STR("tag", tag),
 *              ("px", "%.2f", px), ("order", "{\"vol\":%d}", v), ("armed", "true"))
 *
 * Limits and rules:
 *  - 1..16 pairs; exceeding 16 fails to compile mentioning TOO_MANY_KEYS.
 *  - For an empty payload omit the payload argument entirely (renders {}).
 *  - Keys and formats must be string literals (enforced by concatenation).
 *  - Values are NOT escaped at runtime (Design.md §3): a %s value that
 *    contains '"' or '\' still breaks the JSON. Escaping helper: v1.1.
 */
#define AMC_JSON(...)                                                          \
    "{" AMC_JSON_CAT_(AMC_JSON_FMT_, AMC_JSON_NARG_(__VA_ARGS__))(__VA_ARGS__) \
    "}" AMC_JSON_CAT_(AMC_JSON_ARG_, AMC_JSON_NARG_(__VA_ARGS__))(__VA_ARGS__)

/* typed helpers (each expands to a plain tuple) */
#define AMC_KV_INT(key_, val_)  (key_, "%d",    (int)(val_))
#define AMC_KV_I64(key_, val_)  (key_, "%lld",  (long long)(val_))
#define AMC_KV_U64(key_, val_)  (key_, "%llu",  (unsigned long long)(val_))
#define AMC_KV_F64(key_, val_)  (key_, "%.17g", (double)(val_))   /* round-trip;
                                          use a raw tuple for fixed decimals */
#define AMC_KV_STR(key_, val_)  (key_, "\"%s\"", (val_))
#define AMC_KV_BOOL(key_, val_) (key_, "%s", (val_) ? "true" : "false")

/* -- internals ----------------------------------------------------------- */
#define AMC_JSON_CAT2_(a, b) a##b
#define AMC_JSON_CAT_(a, b) AMC_JSON_CAT2_(a, b)
#define AMC_JSON_NARGN_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12,     \
                        _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, \
                        _24, _25, _26, _27, _28, _29, _30, _31, _32, N, ...) N
#define AMC_JSON_NARG_(...)                                                    \
    AMC_JSON_NARGN_(__VA_ARGS__,                                               \
        TOO_MANY_KEYS, TOO_MANY_KEYS, TOO_MANY_KEYS, TOO_MANY_KEYS,            \
        TOO_MANY_KEYS, TOO_MANY_KEYS, TOO_MANY_KEYS, TOO_MANY_KEYS,            \
        TOO_MANY_KEYS, TOO_MANY_KEYS, TOO_MANY_KEYS, TOO_MANY_KEYS,            \
        TOO_MANY_KEYS, TOO_MANY_KEYS, TOO_MANY_KEYS, TOO_MANY_KEYS,            \
        16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)

#define AMC_JSON_KV_FMT2_(key_, fmt_, ...) "\"" key_ "\":" fmt_
#define AMC_JSON_KV_FMT_(t) AMC_JSON_KV_FMT2_ t
#define AMC_JSON_KV_ARG2_(key_, fmt_, ...) , ##__VA_ARGS__
#define AMC_JSON_KV_ARG_(t) AMC_JSON_KV_ARG2_ t

#define AMC_JSON_FMT_1(a)       AMC_JSON_KV_FMT_(a)
#define AMC_JSON_FMT_2(a, ...)  AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_1(__VA_ARGS__)
#define AMC_JSON_FMT_3(a, ...)  AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_2(__VA_ARGS__)
#define AMC_JSON_FMT_4(a, ...)  AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_3(__VA_ARGS__)
#define AMC_JSON_FMT_5(a, ...)  AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_4(__VA_ARGS__)
#define AMC_JSON_FMT_6(a, ...)  AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_5(__VA_ARGS__)
#define AMC_JSON_FMT_7(a, ...)  AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_6(__VA_ARGS__)
#define AMC_JSON_FMT_8(a, ...)  AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_7(__VA_ARGS__)
#define AMC_JSON_FMT_9(a, ...)  AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_8(__VA_ARGS__)
#define AMC_JSON_FMT_10(a, ...) AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_9(__VA_ARGS__)
#define AMC_JSON_FMT_11(a, ...) AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_10(__VA_ARGS__)
#define AMC_JSON_FMT_12(a, ...) AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_11(__VA_ARGS__)
#define AMC_JSON_FMT_13(a, ...) AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_12(__VA_ARGS__)
#define AMC_JSON_FMT_14(a, ...) AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_13(__VA_ARGS__)
#define AMC_JSON_FMT_15(a, ...) AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_14(__VA_ARGS__)
#define AMC_JSON_FMT_16(a, ...) AMC_JSON_FMT_1(a) "," AMC_JSON_FMT_15(__VA_ARGS__)

#define AMC_JSON_ARG_1(a)       AMC_JSON_KV_ARG_(a)
#define AMC_JSON_ARG_2(a, ...)  AMC_JSON_ARG_1(a) AMC_JSON_ARG_1(__VA_ARGS__)
#define AMC_JSON_ARG_3(a, ...)  AMC_JSON_ARG_1(a) AMC_JSON_ARG_2(__VA_ARGS__)
#define AMC_JSON_ARG_4(a, ...)  AMC_JSON_ARG_1(a) AMC_JSON_ARG_3(__VA_ARGS__)
#define AMC_JSON_ARG_5(a, ...)  AMC_JSON_ARG_1(a) AMC_JSON_ARG_4(__VA_ARGS__)
#define AMC_JSON_ARG_6(a, ...)  AMC_JSON_ARG_1(a) AMC_JSON_ARG_5(__VA_ARGS__)
#define AMC_JSON_ARG_7(a, ...)  AMC_JSON_ARG_1(a) AMC_JSON_ARG_6(__VA_ARGS__)
#define AMC_JSON_ARG_8(a, ...)  AMC_JSON_ARG_1(a) AMC_JSON_ARG_7(__VA_ARGS__)
#define AMC_JSON_ARG_9(a, ...)  AMC_JSON_ARG_1(a) AMC_JSON_ARG_8(__VA_ARGS__)
#define AMC_JSON_ARG_10(a, ...) AMC_JSON_ARG_1(a) AMC_JSON_ARG_9(__VA_ARGS__)
#define AMC_JSON_ARG_11(a, ...) AMC_JSON_ARG_1(a) AMC_JSON_ARG_10(__VA_ARGS__)
#define AMC_JSON_ARG_12(a, ...) AMC_JSON_ARG_1(a) AMC_JSON_ARG_11(__VA_ARGS__)
#define AMC_JSON_ARG_13(a, ...) AMC_JSON_ARG_1(a) AMC_JSON_ARG_12(__VA_ARGS__)
#define AMC_JSON_ARG_14(a, ...) AMC_JSON_ARG_1(a) AMC_JSON_ARG_13(__VA_ARGS__)
#define AMC_JSON_ARG_15(a, ...) AMC_JSON_ARG_1(a) AMC_JSON_ARG_14(__VA_ARGS__)
#define AMC_JSON_ARG_16(a, ...) AMC_JSON_ARG_1(a) AMC_JSON_ARG_15(__VA_ARGS__)

#endif /* AMC_LOGGER_H_ */
