#ifndef AMC_LOGGER_INTERNAL_H_
#define AMC_LOGGER_INTERNAL_H_

/* Internal contracts shared by the AmcLogger implementation files.
 * Nothing here is public API; tests may declare amc_internal_test_reset(). */

#include "AmcLogger.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

/* ---- limits (see docs/Architecture.md §11) ---- */
#define AMC_MODULE_MAX        128
#define AMC_MSG_SIZE_MIN      256
#define AMC_MSG_SIZE_MAX      8192
#define AMC_QUEUE_SIZE_MIN    16
#define AMC_QUEUE_SIZE_MAX    1048576
#define AMC_FLUSH_MS_MAX      3600000
#define AMC_WORKER_BATCH      64
#define AMC_FILE_IOBUF_SIZE   65536
#define AMC_CONFIG_SIZE_MAX   (1024 * 1024)
#define AMC_PATH_MAX          4096
#define AMC_TRUNC_MARKER      "...(truncated)"

#define AMC_ALIGN64(x) (((x) + 63u) & ~((size_t)63u))

enum amc_state  { AMC_UNINIT = 0, AMC_READY, AMC_SHUTDOWN };
enum amc_policy { AMC_POLICY_BLOCK = 0, AMC_POLICY_OVERRUN_OLDEST, AMC_POLICY_DISCARD_NEW };

/* ---- logger: public prefix + internal extension, one allocation ---- */
struct amc_logger_full {
    struct amc_logger       pub;      /* MUST stay first (checked by _Static_assert) */
    char                   *module;   /* owned copy, stripped basename               */
    struct amc_logger_full *next;     /* registry chain                              */
};

/* ---- message slot header; payload bytes follow contiguously ---- */
struct amc_msg {
    struct timespec ts;
    const char     *module;           /* -> logger_full.module (permanent)           */
    const char     *function;         /* __func__, static storage                    */
    const char     *event;            /* string literal (Design D3 contract)         */
    int32_t         trader_id;
    uint8_t         level;
    uint8_t         has_id;
    uint8_t         truncated;        /* payload hit its cap at capture              */
    uint16_t        payload_len;
};

/* ---- bounded MPSC ring, mutex + condvars ---- */
struct amc_queue {
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    pthread_cond_t  flush_done;
    char           *slots;            /* capacity * slot_stride                      */
    char           *batch;            /* AMC_WORKER_BATCH * slot_stride (worker only)*/
    uint32_t        capacity;
    uint32_t        slot_stride;
    uint32_t        head, count;      /* ring state, under mtx                       */
    uint64_t        seq_enq;          /* total accepted, under mtx                   */
    uint64_t        seq_flushed;      /* written + fflushed watermark, under mtx     */
    uint64_t        flush_req;        /* highest requested flush target, under mtx   */
    int             stop;
    int             inited;
};

struct amc_sink {
    pthread_mutex_t mtx;
    FILE           *fp;               /* stdout, or fopen'd file                     */
    char           *iobuf;            /* file sink only                              */
    int             is_file;
    int             closed;           /* checked under mtx; post-close writes no-op  */
    int             dirty;
    int             mtx_inited;
};

struct amc_cfg_logger {
    char                  *module;
    int                    level;
    struct amc_cfg_logger *next;
};

struct amc_config {
    int      default_level;
    int      async;
    int      critical_sync;
    int      utc_offset_hours;
    enum amc_policy policy;
    uint32_t queue_size;
    uint32_t max_message_size;
    uint32_t flush_every_ms;
    int      has_stdout;
    int      has_file;
    char    *file_path_template;      /* owned                                       */
    struct amc_cfg_logger *loggers;   /* owned                                       */
};

struct amc_global {
    _Atomic int       state;          /* enum amc_state                              */
    struct amc_config cfg;
    struct amc_sink   sinks[2];       /* [0] = stdout, [1] = file                    */
    struct amc_queue  queue;
    pthread_t         worker;
    int               worker_started;

    _Atomic uint64_t  st_enqueued, st_dropped_new, st_overwritten, st_blocks,
                      st_truncated, st_write_errors, st_high_water;
    /* last values reported in a MessageLoss summary (worker/shutdown only) */
    uint64_t          rep_dropped, rep_overwritten, rep_truncated, rep_werr;

    _Atomic int       warned_before_init;
};
extern struct amc_global g_amc;

#define AMC_STAT_INC(field) \
    atomic_fetch_add_explicit(&g_amc.field, 1, memory_order_relaxed)

/* ---- Core ---- */
void amc_internal_now(struct timespec *out);
struct amc_logger_full *amc_internal_logger_get(const char *module);
void amc_internal_maybe_emit_loss_summary(void);

/* ---- Render ---- */
size_t amc_internal_render_line(char *dst, size_t cap, const struct amc_msg *m,
                                const char *payload, int *out_truncated);

/* ---- Sink ---- */
int  amc_internal_sinks_setup(char *err, size_t errsz);
void amc_internal_emit(const struct amc_msg *m, const char *payload);
void amc_internal_sinks_flush(void);
void amc_internal_sinks_close(void);
int  amc_internal_path_expand(const char *tmpl, char *out, size_t outsz,
                              char *err, size_t errsz);

/* ---- Config (pure; no globals) ---- */
void amc_internal_config_defaults(struct amc_config *out);
int  amc_internal_config_parse(const char *text, size_t len, const char *diag_name,
                               struct amc_config *out, char *err, size_t errsz);
void amc_internal_config_free(struct amc_config *cfg);

/* ---- Queue / worker ---- */
int  amc_internal_async_start(char *err, size_t errsz);
void amc_internal_async_stop(void);
void amc_internal_async_enqueue(const struct amc_msg *m, const char *payload);
int  amc_internal_async_flush(void);

#ifdef AMC_LOGGER_TESTING
/* Test-only: tear everything down to AMC_UNINIT so a test binary can simulate
 * many independent process lifetimes. Registry loggers are kept alive (call-site
 * caches point at them); init re-applies levels to them. */
void amc_internal_test_reset(void);
void amc_internal_async_destroy(void);
void amc_internal_sinks_destroy(void);
typedef void (*amc_clock_fn)(struct timespec *out);
void amc_internal_set_clock(amc_clock_fn fn);
#endif

#endif /* AMC_LOGGER_INTERNAL_H_ */
