/* AmcLoggerConfig.c — hand-rolled strict YAML-subset parser (Design §7.3).
 * Pure functions, no globals: the unit-test workhorse.
 *
 * Accepted: comments, %YAML directive, ---, block mappings (root + one child
 * level), plain/'single'/"double" scalars, integers, true/false, the empty
 * flow mapping {}. Everything else is a loud error, never a silent misparse. */

#include "AmcLoggerInternal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct pline {
    int   line_no;
    int   indent;
    char *key;
    char *val;        /* NULL = section header (no value) */
    int   empty_map;  /* val was the literal {} */
};

struct perr {
    char       *buf;
    size_t      sz;
    const char *name;
};

static int fail(struct perr *e, int line_no, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int fail(struct perr *e, int line_no, const char *fmt, ...)
{
    int n = snprintf(e->buf, e->sz, "%s:%d: ", e->name, line_no);
    if (n < 0 || (size_t)n >= e->sz)
        return -1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->buf + n, e->sz - (size_t)n, fmt, ap);
    va_end(ap);
    return -1;
}

static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

/* strip a trailing comment, respecting quotes */
static void strip_comment(char *s)
{
    int in_s = 0, in_d = 0;
    for (char *p = s; *p; p++) {
        if (in_d) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == '"')
                in_d = 0;
        } else if (in_s) {
            if (*p == '\'')
                in_s = 0;
        } else if (*p == '"') {
            in_d = 1;
        } else if (*p == '\'') {
            in_s = 1;
        } else if (*p == '#' && (p == s || p[-1] == ' ' || p[-1] == '\t')) {
            *p = '\0';
            break;
        }
    }
    rtrim(s);
}

static int key_charset_ok(const char *k)
{
    for (const char *p = k; *p; p++) {
        int c = (unsigned char)*p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return 0;
    }
    return k[0] != '\0';
}

/* Scan `text` (a private mutable copy) into pl[]; returns count or -1. */
static int scan_lines(char *text, struct pline **out_pl, struct perr *e)
{
    size_t cap = 64, n = 0;
    struct pline *pl = malloc(cap * sizeof(*pl));
    if (!pl)
        return fail(e, 0, "out of memory");

    int line_no = 0;
    char *cur = text;
    while (cur) {
        line_no++;
        char *line = cur;
        char *nl = strchr(cur, '\n');
        if (nl) {
            *nl = '\0';
            cur = nl + 1;
        } else {
            cur = NULL;
        }

        int indent = 0;
        while (line[indent] == ' ')
            indent++;
        char *rest = line + indent;
        if (*rest == '\t')
            goto err_tab;
        strip_comment(rest);
        if (*rest == '\0')
            continue;
        if (indent == 0 && *rest == '%') {
            if (strncmp(rest, "%YAML", 5) == 0)
                continue;
            fail(e, line_no, "unsupported directive '%s'", rest);
            goto err;
        }
        if (indent == 0 && strcmp(rest, "---") == 0)
            continue;
        if (rest[0] == '-' && (rest[1] == ' ' || rest[1] == '\0')) {
            fail(e, line_no, "sequences are not supported");
            goto err;
        }

        char *colon = strchr(rest, ':');
        if (!colon) {
            fail(e, line_no, "expected 'key: value'");
            goto err;
        }
        *colon = '\0';
        char *key = rest;
        rtrim(key);
        if (!key_charset_ok(key)) {
            fail(e, line_no, "invalid key '%s'", key);
            goto err;
        }

        char *val = colon + 1;
        if (*val != '\0' && *val != ' ') {
            fail(e, line_no, "missing space after ':'");
            goto err;
        }
        while (*val == ' ')
            val++;

        int empty_map = 0;
        if (*val == '\0') {
            val = NULL;                                   /* section header */
        } else if (*val == '\'') {
            char *end = strchr(val + 1, '\'');
            if (!end) {
                fail(e, line_no, "unterminated single-quoted value");
                goto err;
            }
            *end = '\0';
            if (end[1] != '\0') {
                fail(e, line_no, "trailing characters after quoted value");
                goto err;
            }
            val = val + 1;
        } else if (*val == '"') {
            char *w = val, *r = val + 1;
            int closed = 0;
            while (*r) {
                if (*r == '\\') {
                    if (r[1] == '"' || r[1] == '\\') {
                        *w++ = r[1];
                        r += 2;
                    } else {
                        fail(e, line_no, "unsupported escape '\\%c'", r[1] ? r[1] : ' ');
                        goto err;
                    }
                } else if (*r == '"') {
                    closed = 1;
                    r++;
                    break;
                } else {
                    *w++ = *r++;
                }
            }
            if (!closed) {
                fail(e, line_no, "unterminated double-quoted value");
                goto err;
            }
            if (*r != '\0') {
                fail(e, line_no, "trailing characters after quoted value");
                goto err;
            }
            *w = '\0';
        } else if (strcmp(val, "{}") == 0) {
            empty_map = 1;
        } else if (strchr("[{&*|>!@`", (unsigned char)val[0])) {
            fail(e, line_no, "unsupported YAML syntax '%s'", val);
            goto err;
        }

        if (n == cap) {
            cap *= 2;
            struct pline *np = realloc(pl, cap * sizeof(*pl));
            if (!np) {
                fail(e, line_no, "out of memory");
                goto err;
            }
            pl = np;
        }
        pl[n].line_no = line_no;
        pl[n].indent = indent;
        pl[n].key = key;
        pl[n].val = val;
        pl[n].empty_map = empty_map;
        n++;
        continue;
err_tab:
        fail(e, line_no, "tab character in indentation");
        goto err;
    }
    *out_pl = pl;
    return (int)n;
err:
    free(pl);
    return -1;
}

/* ---- typed value parsers ---- */

static int parse_level_name(const char *s, int *out)
{
    static const char *const names[] =
        { "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL", "OFF" };
    for (int i = 0; i < 6; i++)
        if (strcasecmp(s, names[i]) == 0) {
            *out = i;
            return 0;
        }
    return -1;
}

static int parse_bool(const char *s, int *out)
{
    if (strcasecmp(s, "true") == 0)  { *out = 1; return 0; }
    if (strcasecmp(s, "false") == 0) { *out = 0; return 0; }
    return -1;
}

static int parse_int_range(const char *s, long min, long max, long *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0')
        return -1;
    if (v < min || v > max)
        return -2;
    *out = v;
    return 0;
}

/* ---- public entry points ---- */

void amc_internal_config_defaults(struct amc_config *out)
{
    memset(out, 0, sizeof(*out));
    out->default_level    = AMC_LOGGER_LEVEL_INFO;
    out->async            = 1;
    out->critical_sync    = 0;
    out->worker_cpu       = -1;   /* unpinned */
    out->utc_offset_hours = 8;
    out->policy           = AMC_POLICY_BLOCK;
    out->queue_size       = 8192;
    out->max_message_size = 2048;
    out->flush_every_ms   = 1000;
    out->has_stdout       = 1;   /* omitted `sinks:` means stdout only */
    out->has_file         = 0;
}

void amc_internal_config_free(struct amc_config *cfg)
{
    free(cfg->file_path_template);
    cfg->file_path_template = NULL;
    struct amc_cfg_logger *cl = cfg->loggers;
    while (cl) {
        struct amc_cfg_logger *next = cl->next;
        free(cl->module);
        free(cl);
        cl = next;
    }
    cfg->loggers = NULL;
}

enum root_key {
    RK_DEFAULT_LEVEL, RK_ASYNC, RK_QUEUE_SIZE, RK_POLICY, RK_MAX_MSG,
    RK_FLUSH_MS, RK_UTC_OFFSET, RK_CRITICAL_SYNC, RK_WORKER_CPU,
    RK_SINKS, RK_LOGGERS,
    RK_COUNT
};

static const char *const ROOT_KEYS[RK_COUNT] = {
    "default_level", "async", "queue_size", "queue_full_policy",
    "max_message_size", "flush_every_ms", "utc_offset_hours", "critical_sync",
    "worker_cpu", "sinks", "loggers"
};

int amc_internal_config_parse(const char *text, size_t len, const char *diag_name,
                              struct amc_config *out, char *err, size_t errsz)
{
    struct perr e = { err, errsz, diag_name };
    amc_internal_config_defaults(out);

    char *copy = malloc(len + 1);
    if (!copy)
        return fail(&e, 0, "out of memory");
    memcpy(copy, text, len);
    copy[len] = '\0';

    struct pline *pl = NULL;
    int n = scan_lines(copy, &pl, &e);
    if (n < 0) {
        free(copy);
        return -1;
    }

    int seen[RK_COUNT] = { 0 };
    int i = 0;
    while (i < n) {
        struct pline *p = &pl[i];
        if (p->indent != 0) {
            fail(&e, p->line_no, "unexpected indentation");
            goto err;
        }
        int rk = -1;
        for (int k = 0; k < RK_COUNT; k++)
            if (strcmp(p->key, ROOT_KEYS[k]) == 0) {
                rk = k;
                break;
            }
        if (rk < 0) {
            fail(&e, p->line_no, "unknown key '%s'", p->key);
            goto err;
        }
        if (seen[rk]) {
            fail(&e, p->line_no, "duplicate key '%s'", p->key);
            goto err;
        }
        seen[rk] = 1;

        if (rk == RK_SINKS || rk == RK_LOGGERS) {
            if (p->val != NULL || p->empty_map) {
                fail(&e, p->line_no, "'%s' does not take a value", p->key);
                goto err;
            }
            int child_indent = -1;
            int j = i + 1;
            int child_count = 0;
            if (rk == RK_SINKS) {         /* explicit section: start from zero */
                out->has_stdout = 0;
                out->has_file = 0;
            }
            for (; j < n && pl[j].indent > 0; j++) {
                struct pline *c = &pl[j];
                if (child_indent < 0)
                    child_indent = c->indent;
                if (c->indent != child_indent) {
                    fail(&e, c->line_no,
                         "inconsistent indentation (expected %d spaces)", child_indent);
                    goto err;
                }
                child_count++;
                if (rk == RK_SINKS) {
                    if (strcmp(c->key, "stdout") == 0) {
                        if (out->has_stdout) {
                            fail(&e, c->line_no, "duplicate sink 'stdout'");
                            goto err;
                        }
                        if (!c->empty_map) {
                            fail(&e, c->line_no, "sink 'stdout' takes '{}'");
                            goto err;
                        }
                        out->has_stdout = 1;
                    } else if (strcmp(c->key, "basic_file") == 0) {
                        if (out->has_file) {
                            fail(&e, c->line_no, "duplicate sink 'basic_file'");
                            goto err;
                        }
                        if (c->empty_map || !c->val || c->val[0] == '\0') {
                            fail(&e, c->line_no, "sink 'basic_file' needs a path");
                            goto err;
                        }
                        out->file_path_template = strdup(c->val);
                        out->has_file = 1;
                    } else {
                        fail(&e, c->line_no, "unknown sink '%s'", c->key);
                        goto err;
                    }
                } else { /* RK_LOGGERS: <Module>: <LEVEL> */
                    if (c->empty_map || !c->val) {
                        fail(&e, c->line_no, "logger '%s' needs a level", c->key);
                        goto err;
                    }
                    if (strlen(c->key) > AMC_MODULE_MAX - 1) {
                        fail(&e, c->line_no, "module name too long");
                        goto err;
                    }
                    for (struct amc_cfg_logger *cl = out->loggers; cl; cl = cl->next)
                        if (strcmp(cl->module, c->key) == 0) {
                            fail(&e, c->line_no, "duplicate logger '%s'", c->key);
                            goto err;
                        }
                    int lvl;
                    if (parse_level_name(c->val, &lvl) != 0) {
                        fail(&e, c->line_no, "unknown level '%s'", c->val);
                        goto err;
                    }
                    struct amc_cfg_logger *cl = calloc(1, sizeof(*cl));
                    if (!cl) {
                        fail(&e, c->line_no, "out of memory");
                        goto err;
                    }
                    cl->module = strdup(c->key);
                    cl->level = lvl;
                    cl->next = out->loggers;
                    out->loggers = cl;
                }
            }
            if (child_count == 0) {
                fail(&e, p->line_no, "section '%s' is empty", p->key);
                goto err;
            }
            i = j;
            continue;
        }

        /* scalar root keys */
        if (p->val == NULL || p->empty_map) {
            fail(&e, p->line_no, "expected a value for '%s'", p->key);
            goto err;
        }
        long v;
        switch (rk) {
        case RK_DEFAULT_LEVEL:
            if (parse_level_name(p->val, &out->default_level) != 0) {
                fail(&e, p->line_no, "unknown level '%s'", p->val);
                goto err;
            }
            break;
        case RK_ASYNC:
            if (parse_bool(p->val, &out->async) != 0)
                goto err_bool;
            break;
        case RK_CRITICAL_SYNC:
            if (parse_bool(p->val, &out->critical_sync) != 0)
                goto err_bool;
            break;
        case RK_POLICY:
            if (strcasecmp(p->val, "block") == 0)
                out->policy = AMC_POLICY_BLOCK;
            else if (strcasecmp(p->val, "overrun_oldest") == 0)
                out->policy = AMC_POLICY_OVERRUN_OLDEST;
            else if (strcasecmp(p->val, "discard_new") == 0)
                out->policy = AMC_POLICY_DISCARD_NEW;
            else {
                fail(&e, p->line_no, "unknown queue_full_policy '%s'", p->val);
                goto err;
            }
            break;
        case RK_QUEUE_SIZE:
            switch (parse_int_range(p->val, AMC_QUEUE_SIZE_MIN, AMC_QUEUE_SIZE_MAX, &v)) {
            case 0: out->queue_size = (uint32_t)v; break;
            case -2:
                fail(&e, p->line_no, "queue_size out of range (%d..%d)",
                     AMC_QUEUE_SIZE_MIN, AMC_QUEUE_SIZE_MAX);
                goto err;
            default: goto err_int;
            }
            break;
        case RK_MAX_MSG:
            switch (parse_int_range(p->val, AMC_MSG_SIZE_MIN, AMC_MSG_SIZE_MAX, &v)) {
            case 0: out->max_message_size = (uint32_t)v; break;
            case -2:
                fail(&e, p->line_no, "max_message_size out of range (%d..%d)",
                     AMC_MSG_SIZE_MIN, AMC_MSG_SIZE_MAX);
                goto err;
            default: goto err_int;
            }
            break;
        case RK_FLUSH_MS:
            switch (parse_int_range(p->val, 0, AMC_FLUSH_MS_MAX, &v)) {
            case 0: out->flush_every_ms = (uint32_t)v; break;
            case -2:
                fail(&e, p->line_no, "flush_every_ms out of range (0..%d)",
                     AMC_FLUSH_MS_MAX);
                goto err;
            default: goto err_int;
            }
            break;
        case RK_UTC_OFFSET:
            switch (parse_int_range(p->val, -12, 14, &v)) {
            case 0: out->utc_offset_hours = (int)v; break;
            case -2:
                fail(&e, p->line_no, "utc_offset_hours out of range (-12..14)");
                goto err;
            default: goto err_int;
            }
            break;
        case RK_WORKER_CPU:
            switch (parse_int_range(p->val, 0, AMC_WORKER_CPU_MAX, &v)) {
            case 0: out->worker_cpu = (int)v; break;
            case -2:
                fail(&e, p->line_no, "worker_cpu out of range (0..%d)",
                     AMC_WORKER_CPU_MAX);
                goto err;
            default: goto err_int;
            }
            break;
        default:
            break;
        }
        i++;
        continue;
err_bool:
        fail(&e, p->line_no, "'%s' expects true or false", p->key);
        goto err;
err_int:
        fail(&e, p->line_no, "'%s' expects an integer", p->key);
        goto err;
    }

    free(pl);
    free(copy);
    return 0;
err:
    free(pl);
    free(copy);
    amc_internal_config_free(out);
    memset(out, 0, sizeof(*out));
    return -1;
}
