/* AmcLoggerSink.c — stdout + basic_file sinks: path expansion, mkdir -p,
 * per-line emission, flushing, rate-limited write-error reporting. */

#include "AmcLoggerInternal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- write-error reporting: count always, stderr rate-limited ---- */

static void report_write_error(void)
{
    static _Atomic uint64_t n;
    AMC_STAT_INC(st_write_errors);
    uint64_t seen = atomic_fetch_add_explicit(&n, 1, memory_order_relaxed);
    if (seen % 1000 == 0)
        fprintf(stderr, "amc_logger: sink write error: %s\n", strerror(errno));
}

/* ---- $TODAY / $PID expansion (once, at open — Design Q14) ---- */

int amc_internal_path_expand(const char *tmpl, char *out, size_t outsz,
                             char *err, size_t errsz)
{
    size_t o = 0;
    for (const char *p = tmpl; *p; p++) {
        char sub[64];
        const char *ins = NULL;
        size_t insn = 0;

        if (*p == '$') {
            const char *v = p + 1;
            size_t vn = 0;
            while (v[vn] >= 'A' && v[vn] <= 'Z')
                vn++;
            if (vn == 5 && strncmp(v, "TODAY", 5) == 0) {
                struct timespec ts;
                amc_internal_now(&ts);
                time_t shifted = ts.tv_sec + (time_t)g_amc.cfg.utc_offset_hours * 3600;
                struct tm tm;
                gmtime_r(&shifted, &tm);
                insn = (size_t)snprintf(sub, sizeof(sub), "%04d-%02d-%02d",
                                        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
                ins = sub;
                p += 5;
            } else if (vn == 3 && strncmp(v, "PID", 3) == 0) {
                insn = (size_t)snprintf(sub, sizeof(sub), "%ld", (long)getpid());
                ins = sub;
                p += 3;
            } else {
                snprintf(err, errsz, "unknown variable '$%.*s' in sink path",
                         (int)(vn ? vn : 1), v);
                return -1;
            }
        } else {
            ins = p;
            insn = 1;
        }
        if (o + insn + 1 > outsz) {
            snprintf(err, errsz, "expanded sink path is too long");
            return -1;
        }
        memcpy(out + o, ins, insn);
        o += insn;
    }
    out[o] = '\0';
    return 0;
}

static int mkdir_parents(const char *filepath, char *err, size_t errsz)
{
    char tmp[AMC_PATH_MAX];
    size_t n = strlen(filepath);
    if (n >= sizeof(tmp)) {
        snprintf(err, errsz, "sink path is too long");
        return -1;
    }
    memcpy(tmp, filepath, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
            snprintf(err, errsz, "cannot create directory '%s': %s",
                     tmp, strerror(errno));
            return -1;
        }
        tmp[i] = '/';
    }
    return 0;
}

/* ---- setup / teardown ---- */

static void sink_teardown_one(struct amc_sink *s)
{
    if (s->mtx_inited)
        pthread_mutex_destroy(&s->mtx);
    if (s->is_file && s->fp)
        fclose(s->fp);
    free(s->iobuf);
    memset(s, 0, sizeof(*s));
}

int amc_internal_sinks_setup(char *err, size_t errsz)
{
    struct amc_sink *so = &g_amc.sinks[0];
    struct amc_sink *sf = &g_amc.sinks[1];

    if (g_amc.cfg.has_stdout) {
        pthread_mutex_init(&so->mtx, NULL);
        so->mtx_inited = 1;
        so->fp = stdout;             /* app-owned stream: no setvbuf, never fclosed */
        so->is_file = 0;
    }
    if (g_amc.cfg.has_file) {
        char path[AMC_PATH_MAX];
        if (amc_internal_path_expand(g_amc.cfg.file_path_template, path,
                                     sizeof(path), err, errsz) != 0 ||
            mkdir_parents(path, err, errsz) != 0)
            goto fail;
        FILE *fp = fopen(path, "a");
        if (!fp) {
            snprintf(err, errsz, "cannot open log file '%s': %s",
                     path, strerror(errno));
            goto fail;
        }
        pthread_mutex_init(&sf->mtx, NULL);
        sf->mtx_inited = 1;
        sf->fp = fp;
        sf->is_file = 1;
        sf->iobuf = malloc(AMC_FILE_IOBUF_SIZE);
        if (sf->iobuf)
            setvbuf(fp, sf->iobuf, _IOFBF, AMC_FILE_IOBUF_SIZE);
    }
    return 0;
fail:
    sink_teardown_one(so);
    return -1;
}

/* ---- emission ---- */

void amc_internal_emit(const struct amc_msg *m, const char *payload)
{
    char line[AMC_MSG_SIZE_MAX];
    int truncated = 0;
    size_t len = amc_internal_render_line(line, g_amc.cfg.max_message_size, m,
                                          payload, &truncated);
    if (truncated)
        AMC_STAT_INC(st_truncated);

    int flush_now = m->level >= AMC_LOGGER_LEVEL_ERROR;   /* Design §1 auto-flush */
    for (int i = 0; i < 2; i++) {
        struct amc_sink *s = &g_amc.sinks[i];
        if (!s->mtx_inited)
            continue;
        pthread_mutex_lock(&s->mtx);
        if (!s->closed && s->fp) {
            if (fwrite(line, 1, len, s->fp) != len)
                report_write_error();
            else
                s->dirty = 1;
            if (flush_now) {
                if (fflush(s->fp) != 0)
                    report_write_error();
                else
                    s->dirty = 0;
            }
        }
        pthread_mutex_unlock(&s->mtx);
    }
}

void amc_internal_sinks_flush(void)
{
    for (int i = 0; i < 2; i++) {
        struct amc_sink *s = &g_amc.sinks[i];
        if (!s->mtx_inited)
            continue;
        pthread_mutex_lock(&s->mtx);
        if (!s->closed && s->fp && s->dirty) {
            if (fflush(s->fp) != 0)
                report_write_error();
            else
                s->dirty = 0;
        }
        pthread_mutex_unlock(&s->mtx);
    }
}

void amc_internal_sinks_close(void)
{
    for (int i = 0; i < 2; i++) {
        struct amc_sink *s = &g_amc.sinks[i];
        if (!s->mtx_inited)
            continue;
        pthread_mutex_lock(&s->mtx);
        if (!s->closed && s->fp) {
            fflush(s->fp);
            if (s->is_file) {
                fclose(s->fp);
                s->fp = NULL;
            }
            s->closed = 1;
        }
        pthread_mutex_unlock(&s->mtx);
    }
}

#ifdef AMC_LOGGER_TESTING
void amc_internal_sinks_destroy(void)
{
    for (int i = 0; i < 2; i++) {
        struct amc_sink *s = &g_amc.sinks[i];
        if (s->mtx_inited)
            pthread_mutex_destroy(&s->mtx);
        if (s->is_file && s->fp && !s->closed)
            fclose(s->fp);
        free(s->iobuf);
        memset(s, 0, sizeof(*s));
    }
}
#endif
