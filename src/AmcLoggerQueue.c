/* AmcLoggerQueue.c — bounded MPSC ring (mutex + condvars), the single worker
 * thread, overflow policies, and the flush watermark protocol.
 * See docs/Architecture.md §6.3–§6.5. */

#define _GNU_SOURCE   /* pthread_setname_np on Linux */

#include "AmcLoggerInternal.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

static uint64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static struct timespec abs_from_ms(uint64_t ms)
{
    struct timespec t;
    t.tv_sec  = (time_t)(ms / 1000u);
    t.tv_nsec = (long)(ms % 1000u) * 1000000L;
    return t;
}

/* ---- producer side ---- */

void amc_internal_async_enqueue(const struct amc_msg *m, const char *payload)
{
    struct amc_queue *q = &g_amc.queue;

    pthread_mutex_lock(&q->mtx);
    if (g_amc.cfg.policy == AMC_POLICY_BLOCK) {
        while (q->count == q->capacity && !q->stop) {
            AMC_STAT_INC(st_blocks);
            pthread_cond_wait(&q->not_full, &q->mtx);
        }
    }
    if (q->stop) {                       /* shutdown: never accepted */
        AMC_STAT_INC(st_dropped_new);
        pthread_mutex_unlock(&q->mtx);
        return;
    }
    if (q->count == q->capacity) {
        if (g_amc.cfg.policy == AMC_POLICY_DISCARD_NEW) {
            AMC_STAT_INC(st_dropped_new);
            pthread_mutex_unlock(&q->mtx);
            return;
        }
        /* overrun_oldest */
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        AMC_STAT_INC(st_overwritten);
    }

    char *slot = q->slots +
        (size_t)((q->head + q->count) % q->capacity) * q->slot_stride;
    memcpy(slot, m, sizeof(*m));
    memcpy(slot + sizeof(*m), payload, m->payload_len);

    q->count++;
    q->seq_enq++;
    AMC_STAT_INC(st_enqueued);
    if ((uint64_t)q->count >
        atomic_load_explicit(&g_amc.st_high_water, memory_order_relaxed))
        atomic_store_explicit(&g_amc.st_high_water, q->count,
                              memory_order_relaxed);
    if (q->count == 1)
        pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

/* ---- flush watermark (Architecture §6.5) ---- */

int amc_internal_async_flush(void)
{
    struct amc_queue *q = &g_amc.queue;

    pthread_mutex_lock(&q->mtx);
    uint64_t target = q->seq_enq;
    if (q->flush_req < target)
        q->flush_req = target;
    pthread_cond_signal(&q->not_empty);
    while (q->seq_flushed < target && !q->stop)
        pthread_cond_wait(&q->flush_done, &q->mtx);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

/* ---- worker ---- */

static void *worker_main(void *arg)
{
    (void)arg;
    struct amc_queue *q = &g_amc.queue;
    const uint32_t stride = q->slot_stride;
    uint64_t last_flush = wall_ms();

    for (;;) {
        pthread_mutex_lock(&q->mtx);
        while (q->count == 0 && !q->stop && q->flush_req <= q->seq_flushed) {
            if (g_amc.cfg.flush_every_ms == 0) {
                pthread_cond_wait(&q->not_empty, &q->mtx);
            } else {
                struct timespec dl =
                    abs_from_ms(last_flush + g_amc.cfg.flush_every_ms);
                if (pthread_cond_timedwait(&q->not_empty, &q->mtx, &dl) ==
                    ETIMEDOUT)
                    break;                       /* periodic flush tick */
            }
        }
        uint32_t take = q->count < AMC_WORKER_BATCH ? q->count : AMC_WORKER_BATCH;
        for (uint32_t i = 0; i < take; i++) {
            char *slot = q->slots +
                (size_t)((q->head + i) % q->capacity) * stride;
            const struct amc_msg *sm = (const struct amc_msg *)slot;
            memcpy(q->batch + (size_t)i * stride, slot,
                   sizeof(struct amc_msg) + sm->payload_len);
        }
        q->head = (q->head + take) % q->capacity;
        q->count -= take;
        if (take && g_amc.cfg.policy == AMC_POLICY_BLOCK)
            pthread_cond_broadcast(&q->not_full);
        uint64_t end_seq  = q->seq_enq - q->count;
        int want_flush    = q->flush_req > q->seq_flushed;
        int stopping      = q->stop;
        pthread_mutex_unlock(&q->mtx);

        for (uint32_t i = 0; i < take; i++) {
            const struct amc_msg *m =
                (const struct amc_msg *)(q->batch + (size_t)i * stride);
            amc_internal_emit(m, (const char *)m + sizeof(struct amc_msg));
        }

        uint64_t now = wall_ms();
        int tick = g_amc.cfg.flush_every_ms != 0 &&
                   now - last_flush >= g_amc.cfg.flush_every_ms;
        if (want_flush || tick || stopping) {
            amc_internal_maybe_emit_loss_summary();
            amc_internal_sinks_flush();
            last_flush = now;

            pthread_mutex_lock(&q->mtx);
            if (q->seq_flushed < end_seq)
                q->seq_flushed = end_seq;
            pthread_cond_broadcast(&q->flush_done);
            int done = stopping && q->count == 0;
            pthread_mutex_unlock(&q->mtx);
            if (done)
                return NULL;
        }
    }
}

/* ---- start / stop / (test) destroy ---- */

int amc_internal_async_start(char *err, size_t errsz)
{
    struct amc_queue *q = &g_amc.queue;

    q->capacity    = g_amc.cfg.queue_size;
    q->slot_stride = (uint32_t)AMC_ALIGN64(sizeof(struct amc_msg) +
                                           g_amc.cfg.max_message_size);
    q->slots = malloc((size_t)q->capacity * q->slot_stride);
    q->batch = malloc((size_t)AMC_WORKER_BATCH * q->slot_stride);
    if (!q->slots || !q->batch) {
        free(q->slots);
        free(q->batch);
        q->slots = q->batch = NULL;
        snprintf(err, errsz, "cannot allocate queue (%u slots of %u bytes)",
                 q->capacity, q->slot_stride);
        return -1;
    }
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->flush_done, NULL);
    q->head = q->count = 0;
    q->seq_enq = q->seq_flushed = q->flush_req = 0;
    q->stop = 0;
    q->inited = 1;

    /* the worker inherits a fully-blocked signal mask */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    int rc = pthread_create(&g_amc.worker, NULL, worker_main, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc != 0) {
        snprintf(err, errsz, "cannot create worker thread: %s", strerror(rc));
        pthread_mutex_destroy(&q->mtx);
        pthread_cond_destroy(&q->not_empty);
        pthread_cond_destroy(&q->not_full);
        pthread_cond_destroy(&q->flush_done);
        free(q->slots);
        free(q->batch);
        memset(q, 0, sizeof(*q));
        return -1;
    }
#if defined(__linux__)
    pthread_setname_np(g_amc.worker, "amc-worker");
#endif
    g_amc.worker_started = 1;
    return 0;
}

void amc_internal_async_stop(void)
{
    struct amc_queue *q = &g_amc.queue;
    if (!g_amc.worker_started)
        return;
    pthread_mutex_lock(&q->mtx);
    q->stop = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_cond_broadcast(&q->flush_done);
    pthread_mutex_unlock(&q->mtx);
    pthread_join(g_amc.worker, NULL);
    g_amc.worker_started = 0;

    /* Waiters that arrive after the join: watermark is final. */
    pthread_mutex_lock(&q->mtx);
    q->seq_flushed = q->seq_enq;
    pthread_cond_broadcast(&q->flush_done);
    pthread_mutex_unlock(&q->mtx);
}

#ifdef AMC_LOGGER_TESTING
void amc_internal_async_destroy(void)
{
    struct amc_queue *q = &g_amc.queue;
    if (!q->inited)
        return;
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->flush_done);
    free(q->slots);
    free(q->batch);
    memset(q, 0, sizeof(*q));
}
#endif
