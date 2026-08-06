/* AmcLoggerCore.c — global state, lifecycle, registry, hot-path entry. */

#include "AmcLoggerInternal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

struct amc_global g_amc;

_Static_assert(offsetof(struct amc_logger_full, pub) == 0,
               "public logger prefix must be the first member");
_Static_assert(AMC_MSG_SIZE_MAX <= UINT16_MAX, "payload_len must fit uint16_t");

/* ---- clock (test seam) ---- */

#ifdef AMC_LOGGER_TESTING
static amc_clock_fn g_clock_override;
void amc_internal_set_clock(amc_clock_fn fn) { g_clock_override = fn; }
#endif

void amc_internal_now(struct timespec *out)
{
#ifdef AMC_LOGGER_TESTING
    if (g_clock_override) { g_clock_override(out); return; }
#endif
    clock_gettime(CLOCK_REALTIME, out);
}

/* ---- registry (permanent; mutex-protected slow path) ---- */

static struct {
    pthread_mutex_t         mtx;
    struct amc_logger_full *head;
} g_registry = { PTHREAD_MUTEX_INITIALIZER, NULL };

struct amc_logger_full *amc_internal_logger_get(const char *module)
{
    struct amc_logger_full *lg;

    pthread_mutex_lock(&g_registry.mtx);
    for (lg = g_registry.head; lg; lg = lg->next)
        if (strcmp(lg->module, module) == 0)
            break;
    if (!lg) {
        lg = calloc(1, sizeof(*lg));
        char *mod = lg ? strdup(module) : NULL;
        if (!lg || !mod) {
            free(lg);
            free(mod);
            pthread_mutex_unlock(&g_registry.mtx);
            return NULL;
        }
        lg->module = mod;
        atomic_store_explicit(&lg->pub.level, g_amc.cfg.default_level,
                              memory_order_relaxed);
        lg->next = g_registry.head;
        g_registry.head = lg;
    }
    pthread_mutex_unlock(&g_registry.mtx);
    return lg;
}

static void registry_set_all_levels(int level)
{
    pthread_mutex_lock(&g_registry.mtx);
    for (struct amc_logger_full *lg = g_registry.head; lg; lg = lg->next)
        atomic_store_explicit(&lg->pub.level, level, memory_order_relaxed);
    pthread_mutex_unlock(&g_registry.mtx);
}

/* ---- module derivation: basename of __FILE__ minus a trailing C/C++ source suffix ---- */

static void derive_module(const char *file, char *out /* AMC_MODULE_MAX */)
{
    static const char *const suffixes[] = { ".cpp", ".hpp", ".cc", ".c", ".h" };
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    size_t n = strlen(base);
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; ++i) {
        size_t sl = strlen(suffixes[i]);
        if (n > sl && strcmp(base + n - sl, suffixes[i]) == 0) {
            n -= sl;
            break;
        }
    }
    if (n == 0) {
        base = "unknown";
        n = 7;
    }
    if (n > AMC_MODULE_MAX - 1)
        n = AMC_MODULE_MAX - 1;
    memcpy(out, base, n);
    out[n] = '\0';
}

struct amc_logger *amc_logger_resolve(const char *file)
{
    int st = atomic_load_explicit(&g_amc.state, memory_order_acquire);
    if (st != AMC_READY) {
        if (st == AMC_UNINIT &&
            atomic_exchange_explicit(&g_amc.warned_before_init, 1,
                                     memory_order_relaxed) == 0)
            fprintf(stderr,
                "amc_logger: log call before amc_logger_init(); messages dropped\n");
        return NULL;
    }
    char module[AMC_MODULE_MAX];
    derive_module(file, module);
    struct amc_logger_full *lg = amc_internal_logger_get(module);
    return lg ? &lg->pub : NULL;
}

/* ---- hot-path entry: capture, render payload, dispatch ---- */

void amc_logger_log(struct amc_logger *logger, int level,
                    const char *function, const char *event,
                    int has_id, int trader_id,
                    const char *payload_fmt, ...)
{
    if (atomic_load_explicit(&g_amc.state, memory_order_acquire) != AMC_READY)
        return;

    struct amc_msg m;
    amc_internal_now(&m.ts);
    m.module    = ((struct amc_logger_full *)logger)->module;
    m.function  = function;
    m.event     = event;
    m.trader_id = trader_id;
    m.level     = (uint8_t)level;
    m.has_id    = (uint8_t)(has_id != 0);
    m.truncated = 0;

    /* the macro prepends a 1-byte sentinel to the format string; skip it */
    const char *fmt = (payload_fmt[0] == '\1') ? payload_fmt + 1 : payload_fmt;

    char buf[AMC_MSG_SIZE_MAX];
    const char *payload;
    uint32_t cap = g_amc.cfg.max_message_size;
    if (fmt[0] == '\0') {
        payload = "{}";
        m.payload_len = 2;
    } else {
        va_list ap;
        va_start(ap, payload_fmt);
        /* `fmt` is payload_fmt advanced past the "\1" sentinel; the format
         * attribute on amc_logger_log already type-checked the arguments at
         * the call site. GCC traces the derived pointer and stays quiet;
         * AppleClang does not, hence this scoped suppression. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
        int r = vsnprintf(buf, cap, fmt, ap);
#pragma GCC diagnostic pop
        va_end(ap);
        if (r < 0) {
            payload = "{}";
            m.payload_len = 2;
        } else {
            if ((uint32_t)r >= cap) {
                m.payload_len = (uint16_t)(cap - 1);
                m.truncated   = 1;
            } else {
                m.payload_len = (uint16_t)r;
            }
            payload = buf;
        }
    }

    if (!g_amc.cfg.async) {
        AMC_STAT_INC(st_enqueued);
        amc_internal_emit(&m, payload);
    } else if (m.level == AMC_LOGGER_LEVEL_CRITICAL && g_amc.cfg.critical_sync) {
        AMC_STAT_INC(st_enqueued);
        amc_internal_emit(&m, payload);   /* emit flushes on ERROR+ */
    } else {
        amc_internal_async_enqueue(&m, payload);
    }
}

/* ---- loss summary (worker flush ticks + shutdown; single-threaded callers) ---- */

void amc_internal_maybe_emit_loss_summary(void)
{
    uint64_t d = atomic_load_explicit(&g_amc.st_dropped_new,  memory_order_relaxed);
    uint64_t o = atomic_load_explicit(&g_amc.st_overwritten,  memory_order_relaxed);
    uint64_t t = atomic_load_explicit(&g_amc.st_truncated,    memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&g_amc.st_write_errors, memory_order_relaxed);
    if (d == g_amc.rep_dropped && o == g_amc.rep_overwritten &&
        t == g_amc.rep_truncated && w == g_amc.rep_werr)
        return;
    g_amc.rep_dropped = d;
    g_amc.rep_overwritten = o;
    g_amc.rep_truncated = t;
    g_amc.rep_werr = w;

    struct amc_msg m;
    amc_internal_now(&m.ts);
    m.module    = "AmcLogger";
    m.function  = "worker";
    m.event     = "MessageLoss";
    m.trader_id = 0;
    m.level     = AMC_LOGGER_LEVEL_WARN;
    m.has_id    = 0;
    m.truncated = 0;

    char payload[256];
    int n = snprintf(payload, sizeof(payload),
        "{\"dropped_new\":%llu,\"overwritten_old\":%llu,"
        "\"truncated\":%llu,\"write_errors\":%llu}",
        (unsigned long long)d, (unsigned long long)o,
        (unsigned long long)t, (unsigned long long)w);
    m.payload_len = (uint16_t)n;
    amc_internal_emit(&m, payload);
}

/* ---- config file loading ---- */

static char *read_all(const char *path, size_t *out_len, char *err, size_t errsz)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(err, errsz, "cannot read config '%s': %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) goto io_fail;
    long sz = ftell(fp);
    if (sz < 0) goto io_fail;
    if (sz > AMC_CONFIG_SIZE_MAX) {
        snprintf(err, errsz, "config '%s' exceeds %d bytes", path, AMC_CONFIG_SIZE_MAX);
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) goto io_fail;
    char *text = malloc((size_t)sz + 1);
    if (!text) goto io_fail;
    if (sz > 0 && fread(text, 1, (size_t)sz, fp) != (size_t)sz) {
        free(text);
        goto io_fail;
    }
    text[sz] = '\0';
    fclose(fp);
    *out_len = (size_t)sz;
    return text;
io_fail:
    snprintf(err, errsz, "cannot read config '%s': %s", path, strerror(errno));
    fclose(fp);
    return NULL;
}

/* ---- lifecycle ---- */

int amc_logger_init(const char *config_path)
{
    char err[512];

    int st = atomic_load_explicit(&g_amc.state, memory_order_relaxed);
    if (st != AMC_UNINIT) {
        fprintf(stderr, "amc_logger: init failed: already %s\n",
                st == AMC_READY ? "initialized" : "shut down");
        return -1;
    }

    struct amc_config cfg;
    if (config_path == NULL) {
        amc_internal_config_defaults(&cfg);
    } else {
        size_t len = 0;
        char *text = read_all(config_path, &len, err, sizeof(err));
        if (!text) {
            fprintf(stderr, "amc_logger: %s\n", err);
            return -1;
        }
        int rc = amc_internal_config_parse(text, len, config_path, &cfg,
                                           err, sizeof(err));
        free(text);
        if (rc != 0) {
            fprintf(stderr, "amc_logger: config: %s\n", err);
            return -1;
        }
    }
    g_amc.cfg = cfg;   /* ownership moves into the global */

    if (amc_internal_sinks_setup(err, sizeof(err)) != 0) {
        fprintf(stderr, "amc_logger: %s\n", err);
        amc_internal_config_free(&g_amc.cfg);
        memset(&g_amc.cfg, 0, sizeof(g_amc.cfg));
        return -1;
    }

    /* Reset every pre-existing logger to the new default, then apply the
     * configured overrides. (The registry is empty on a production first init;
     * under AMC_LOGGER_TESTING it persists across resets.) */
    registry_set_all_levels(g_amc.cfg.default_level);
    for (struct amc_cfg_logger *cl = g_amc.cfg.loggers; cl; cl = cl->next) {
        struct amc_logger_full *lg = amc_internal_logger_get(cl->module);
        if (lg)
            atomic_store_explicit(&lg->pub.level, cl->level, memory_order_relaxed);
    }

    if (g_amc.cfg.async) {
        if (amc_internal_async_start(err, sizeof(err)) != 0) {
            fprintf(stderr, "amc_logger: %s\n", err);
            amc_internal_sinks_close();
#ifdef AMC_LOGGER_TESTING
            amc_internal_sinks_destroy();
#endif
            amc_internal_config_free(&g_amc.cfg);
            memset(&g_amc.cfg, 0, sizeof(g_amc.cfg));
            return -1;
        }
    }

    atomic_store_explicit(&g_amc.state, AMC_READY, memory_order_release);
    return 0;
}

int amc_logger_shutdown(void)
{
    if (atomic_load_explicit(&g_amc.state, memory_order_relaxed) != AMC_READY)
        return 0;

    registry_set_all_levels(AMC_LOGGER_LEVEL_OFF);

    if (g_amc.cfg.async)
        amc_internal_async_stop();          /* drains, final summary, flush, join */
    else
        amc_internal_maybe_emit_loss_summary();

    amc_internal_sinks_close();
    atomic_store_explicit(&g_amc.state, AMC_SHUTDOWN, memory_order_release);
    return 0;
}

int amc_logger_flush(void)
{
    if (atomic_load_explicit(&g_amc.state, memory_order_acquire) != AMC_READY)
        return -1;
    if (!g_amc.cfg.async) {
        amc_internal_sinks_flush();
        return 0;
    }
    return amc_internal_async_flush();
}

void amc_logger_get_stats(struct amc_logger_stats *out)
{
    out->enqueued        = atomic_load_explicit(&g_amc.st_enqueued,     memory_order_relaxed);
    out->dropped_new     = atomic_load_explicit(&g_amc.st_dropped_new,  memory_order_relaxed);
    out->overwritten_old = atomic_load_explicit(&g_amc.st_overwritten,  memory_order_relaxed);
    out->producer_blocks = atomic_load_explicit(&g_amc.st_blocks,       memory_order_relaxed);
    out->truncated       = atomic_load_explicit(&g_amc.st_truncated,    memory_order_relaxed);
    out->write_errors    = atomic_load_explicit(&g_amc.st_write_errors, memory_order_relaxed);
    out->queue_high_water= atomic_load_explicit(&g_amc.st_high_water,   memory_order_relaxed);
}

#ifdef AMC_LOGGER_TESTING
void amc_internal_test_reset(void)
{
    amc_logger_shutdown();
    amc_internal_async_destroy();
    amc_internal_sinks_destroy();
    amc_internal_config_free(&g_amc.cfg);
    memset(&g_amc.cfg, 0, sizeof(g_amc.cfg));

    atomic_store(&g_amc.st_enqueued, 0);
    atomic_store(&g_amc.st_dropped_new, 0);
    atomic_store(&g_amc.st_overwritten, 0);
    atomic_store(&g_amc.st_blocks, 0);
    atomic_store(&g_amc.st_truncated, 0);
    atomic_store(&g_amc.st_write_errors, 0);
    atomic_store(&g_amc.st_high_water, 0);
    g_amc.rep_dropped = g_amc.rep_overwritten = 0;
    g_amc.rep_truncated = g_amc.rep_werr = 0;
    atomic_store(&g_amc.warned_before_init, 0);
    g_clock_override = NULL;

    atomic_store_explicit(&g_amc.state, AMC_UNINIT, memory_order_release);
}
#endif
