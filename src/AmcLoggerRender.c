/* AmcLoggerRender.c — one-pass line renderer with a per-thread timestamp cache.
 * Format: [YYYY-MM-DD HH:MM:SS.ssssss][LEVEL][MODULE][FUNCTION][EVENT][ID]PAYLOAD\n */

#include "AmcLoggerInternal.h"

#include <string.h>

static const char *const LEVEL_NAMES[] = { "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL" };
static const size_t      LEVEL_LENS[]  = { 5, 4, 4, 5, 8 };

/* Cached "YYYY-MM-DD HH:MM:SS." prefix — re-rendered only when the second (or
 * the configured offset) changes. Thread-local: worker, sync-mode callers and
 * critical_sync callers each keep their own. */
static _Thread_local struct {
    time_t sec;
    int    off;
    int    valid;
    char   prefix[21];                 /* 20 chars + NUL */
} tls_ts;

/* bounded append helper */
struct ap {
    char  *dst;
    size_t cap, len;
    int    over;
};

static void ap_mem(struct ap *a, const char *s, size_t n)
{
    if (a->over)
        return;
    if (a->len + n > a->cap) {
        n = a->cap - a->len;
        a->over = 1;
    }
    memcpy(a->dst + a->len, s, n);
    a->len += n;
}

static void ap_ch(struct ap *a, char c) { ap_mem(a, &c, 1); }

static void ap_i64(struct ap *a, int64_t v)
{
    char tmp[24];
    size_t n = 0;
    uint64_t u = (v < 0) ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    do {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (v < 0)
        tmp[n++] = '-';
    for (size_t i = 0; i < n / 2; i++) {
        char c = tmp[i];
        tmp[i] = tmp[n - 1 - i];
        tmp[n - 1 - i] = c;
    }
    ap_mem(a, tmp, n);
}

static void put_digits(char *dst, int v, int width)
{
    for (int i = width - 1; i >= 0; i--) {
        dst[i] = (char)('0' + (v % 10));
        v /= 10;
    }
}

static void render_ts(struct ap *a, const struct timespec *ts)
{
    int off = g_amc.cfg.utc_offset_hours;
    if (!tls_ts.valid || tls_ts.sec != ts->tv_sec || tls_ts.off != off) {
        time_t shifted = ts->tv_sec + (time_t)off * 3600;
        struct tm tm;
        gmtime_r(&shifted, &tm);
        char *p = tls_ts.prefix;                 /* "YYYY-MM-DD HH:MM:SS." */
        put_digits(p, tm.tm_year + 1900, 4);
        p[4] = '-';
        put_digits(p + 5, tm.tm_mon + 1, 2);
        p[7] = '-';
        put_digits(p + 8, tm.tm_mday, 2);
        p[10] = ' ';
        put_digits(p + 11, tm.tm_hour, 2);
        p[13] = ':';
        put_digits(p + 14, tm.tm_min, 2);
        p[16] = ':';
        put_digits(p + 17, tm.tm_sec, 2);
        p[19] = '.';
        tls_ts.sec = ts->tv_sec;
        tls_ts.off = off;
        tls_ts.valid = 1;
    }
    ap_mem(a, tls_ts.prefix, 20);

    char usec[6];
    uint32_t v = (uint32_t)(ts->tv_nsec / 1000);
    for (int i = 5; i >= 0; i--) {
        usec[i] = (char)('0' + (v % 10));
        v /= 10;
    }
    ap_mem(a, usec, 6);
}

size_t amc_internal_render_line(char *dst, size_t cap, const struct amc_msg *m,
                                const char *payload, int *out_truncated)
{
    const size_t marker_len = sizeof(AMC_TRUNC_MARKER) - 1;
    struct ap a = { dst, cap - 1, 0, 0 };   /* keep one byte for '\n' */

    ap_ch(&a, '[');
    render_ts(&a, &m->ts);
    ap_mem(&a, "][", 2);
    ap_mem(&a, LEVEL_NAMES[m->level], LEVEL_LENS[m->level]);
    ap_mem(&a, "][", 2);
    ap_mem(&a, m->module, strlen(m->module));
    ap_mem(&a, "][", 2);
    ap_mem(&a, m->function, strlen(m->function));
    ap_mem(&a, "][", 2);
    ap_mem(&a, m->event, strlen(m->event));
    ap_mem(&a, "][", 2);
    if (m->has_id)
        ap_i64(&a, m->trader_id);
    else
        ap_ch(&a, '-');
    ap_ch(&a, ']');
    ap_mem(&a, payload, m->payload_len);

    int flagged = m->truncated || a.over;
    if (flagged) {
        /* rewind if needed so the marker + '\n' always fit inside cap */
        if (a.len > cap - marker_len - 1)
            a.len = cap - marker_len - 1;
        memcpy(dst + a.len, AMC_TRUNC_MARKER, marker_len);
        a.len += marker_len;
    }
    dst[a.len++] = '\n';
    if (out_truncated)
        *out_truncated = flagged;
    return a.len;
}
