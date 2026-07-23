/* AmcLoggerQueue.c — bounded MPSC ring (mutex + condvars), the single worker
 * thread, overflow policies, and the flush watermark protocol.
 * See docs/Architecture.md §6.3–§6.5 and §14.13.
 *
 * Checkout is policy-dependent (Architecture §14.13):
 *
 *  - block / discard_new: ZERO-COPY. The worker moves indices under an O(1)
 *    lock hold and emits directly from ring slots outside the lock; producers
 *    count the checked-out ("in-flight") region as occupied, so the tail
 *    insert can never reach a slot the worker is reading. Slot contents are
 *    ordered by the mutex itself (written before unlock, checked out after
 *    lock). Short lock holds keep every producer/worker collision out of the
 *    futex-sleep path — this is what makes the saturated latency tail clean.
 *
 *  - overrun_oldest: COPY-OUT under the lock. Overrunning frees space at the
 *    HEAD while inserting needs space at the TAIL; with a full ring the tail
 *    lands exactly on the first in-flight slot (caught by TSan), so this
 *    policy's semantics are incompatible with zero-copy checkout. The worker
 *    copies the batch out while holding the lock instead; overrun producers
 *    never wait, they overwrite. */

#define _GNU_SOURCE   /* pthread_setname_np on Linux */

#include "AmcLoggerInternal.h"

#include <errno.h>
#include <sched.h>
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
    for (;;) {
        if (q->stop) {                    /* shutdown: never accepted */
            AMC_STAT_INC(st_dropped_new);
            pthread_mutex_unlock(&q->mtx);
            return;
        }
        if (q->count + q->inflight < q->capacity)
            break;                        /* room available */
        if (g_amc.cfg.policy == AMC_POLICY_DISCARD_NEW) {
            AMC_STAT_INC(st_dropped_new);
            pthread_mutex_unlock(&q->mtx);
            return;
        }
        if (g_amc.cfg.policy == AMC_POLICY_OVERRUN_OLDEST && q->count > 0) {
            q->head = (q->head + 1) % q->capacity;
            q->count--;
            AMC_STAT_INC(st_overwritten);
            break;
        }
        /* `block`, or overrun_oldest with every stored slot in flight:
         * wait for the worker to release space */
        AMC_STAT_INC(st_blocks);
        pthread_cond_wait(&q->not_full, &q->mtx);
    }

    char *slot = q->slots +
        (size_t)((q->head + q->count) % q->capacity) * q->slot_stride;
    memcpy(slot, m, sizeof(*m));
    memcpy(slot + sizeof(*m), payload, m->payload_len);

    q->count++;
    q->seq_enq++;
    atomic_store_explicit(&q->enq_hint, q->seq_enq, memory_order_relaxed);
    AMC_STAT_INC(st_enqueued);
    if ((uint64_t)(q->count + q->inflight) >
        atomic_load_explicit(&g_amc.st_high_water, memory_order_relaxed))
        atomic_store_explicit(&g_amc.st_high_water, q->count + q->inflight,
                              memory_order_relaxed);
    /* Wake the worker only if it is actually parked (skips the futex-wake
     * syscall entirely while the worker is busy draining), and signal after
     * unlocking so the woken worker never bounces off a still-held mutex.
     * No lost wakeup: the worker only parks while count == 0, under this
     * same mutex. */
    int need_signal = q->worker_waiting && q->count == 1;
    pthread_mutex_unlock(&q->mtx);
    if (need_signal)
        pthread_cond_signal(&q->not_empty);
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

    uint32_t prev_take = 0;
    for (;;) {
        pthread_mutex_lock(&q->mtx);
        if (prev_take) {          /* release the previous zero-copy batch */
            q->inflight = 0;
            pthread_cond_broadcast(&q->not_full);
            prev_take = 0;
        }
        int spun = 0;
        while (q->count == 0 && !q->stop && q->flush_req <= q->seq_flushed) {
            if (!spun) {
                /* Spin-before-park: when drain keeps up with production, the
                 * queue sits near empty; parking on every drain would make
                 * nearly every enqueue pay a futex wake (measured: the entire
                 * saturated latency tail). Poll the enqueue hint mutex-free
                 * for a few microseconds first — while spinning,
                 * worker_waiting stays 0, so producers skip the wake. */
                spun = 1;
                uint64_t seen =
                    atomic_load_explicit(&q->enq_hint, memory_order_relaxed);
                pthread_mutex_unlock(&q->mtx);
                /* Leave the spin only once a small batch has accumulated (or
                 * the window expires): re-locking for every single message
                 * would put the producer and worker into per-message mutex
                 * collisions. The extra microseconds of gathering are
                 * invisible next to fwrite latency. */
                for (int s = 0; s < AMC_WORKER_SPIN_ITERS; s++) {
                    if (atomic_load_explicit(&q->enq_hint,
                                             memory_order_relaxed) - seen >= 8)
                        break;
                    AMC_CPU_RELAX();
                }
                pthread_mutex_lock(&q->mtx);
                continue;                        /* recheck under the lock */
            }
            q->worker_waiting = 1;
            if (g_amc.cfg.flush_every_ms == 0) {
                pthread_cond_wait(&q->not_empty, &q->mtx);
                q->worker_waiting = 0;
            } else {
                struct timespec dl =
                    abs_from_ms(last_flush + g_amc.cfg.flush_every_ms);
                int rc = pthread_cond_timedwait(&q->not_empty, &q->mtx, &dl);
                q->worker_waiting = 0;
                if (rc == ETIMEDOUT)
                    break;                       /* periodic flush tick */
            }
        }
        uint32_t take  = q->count < AMC_WORKER_BATCH ? q->count : AMC_WORKER_BATCH;
        uint32_t start = q->head;
        int zero_copy  = g_amc.cfg.policy != AMC_POLICY_OVERRUN_OLDEST;
        if (zero_copy) {
            /* O(1) checkout: take indices only; slots are read outside the
             * lock, protected by the in-flight accounting */
            q->inflight = take;
        } else {
            /* overrun_oldest: producers may overwrite any stored slot, so the
             * batch must be copied out while the lock is held */
            for (uint32_t i = 0; i < take; i++) {
                const char *slot = q->slots +
                    (size_t)((start + i) % q->capacity) * stride;
                const struct amc_msg *sm = (const struct amc_msg *)slot;
                memcpy(q->batch + (size_t)i * stride, slot,
                       sizeof(struct amc_msg) + sm->payload_len);
            }
        }
        q->head = (q->head + take) % q->capacity;
        q->count -= take;
        if (!zero_copy && take)                  /* copy-out freed space now */
            pthread_cond_broadcast(&q->not_full);
        uint64_t end_seq  = q->seq_enq - q->count;
        int want_flush    = q->flush_req > q->seq_flushed;
        int stopping      = q->stop;
        pthread_mutex_unlock(&q->mtx);

        for (uint32_t i = 0; i < take; i++) {
            const struct amc_msg *m = zero_copy
                ? (const struct amc_msg *)
                      (q->slots + (size_t)((start + i) % q->capacity) * stride)
                : (const struct amc_msg *)(q->batch + (size_t)i * stride);
            amc_internal_emit(m, (const char *)m + sizeof(struct amc_msg));
        }
        /* zero-copy batches are released at the top of the next iteration —
         * one queue-lock acquisition per batch instead of two, which halves
         * the producer/worker collision rate at saturation */
        prev_take = zero_copy ? take : 0;

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
    q->batch = (g_amc.cfg.policy == AMC_POLICY_OVERRUN_OLDEST)
                   ? malloc((size_t)AMC_WORKER_BATCH * q->slot_stride)
                   : NULL;
    if (!q->slots ||
        (g_amc.cfg.policy == AMC_POLICY_OVERRUN_OLDEST && !q->batch)) {
        free(q->slots);
        free(q->batch);
        q->slots = q->batch = NULL;
        snprintf(err, errsz, "cannot allocate queue (%u slots of %u bytes)",
                 q->capacity, q->slot_stride);
        return -1;
    }
    /* Pre-fault the ring at init: first-touch page faults on cold slot pages
     * were measured to be the entire saturated latency tail (~7 us each, one
     * every ~13 slots). Init pays ~ms once so the hot path never does. */
    memset(q->slots, 0, (size_t)q->capacity * q->slot_stride);
    if (q->batch)
        memset(q->batch, 0, (size_t)AMC_WORKER_BATCH * q->slot_stride);
    /* Plain mutex, deliberately: PTHREAD_MUTEX_ADAPTIVE_NP was measured and
     * rejected — producer-side spinning starves the worker at saturation
     * (drain rate halves) without improving the tail. See Architecture §14. */
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->flush_done, NULL);
    q->head = q->count = q->inflight = 0;
    q->seq_enq = q->seq_flushed = q->flush_req = 0;
    q->stop = 0;
    q->inited = 1;

    /* worker_cpu: pin via the CREATION attributes, so the worker is born on
     * its core and never executes a quantum anywhere else. The kernel
     * validates the mask inside pthread_create (EINVAL for a core this
     * machine does not have) — a bad pin fails init, never runs unpinned.
     * macOS has no pinning API (Mach affinity tags are hints): per the
     * agreed policy it warns once and runs unpinned. */
    int rc = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if defined(__linux__)
    if (g_amc.cfg.worker_cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(g_amc.cfg.worker_cpu, &set);
        rc = pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
    }
#else
    if (g_amc.cfg.worker_cpu >= 0)
        fprintf(stderr, "amc_logger: worker_cpu is not supported on this "
                        "platform; worker runs unpinned\n");
#endif
    if (rc == 0) {
        /* the worker inherits a fully-blocked signal mask */
        sigset_t all, old;
        sigfillset(&all);
        pthread_sigmask(SIG_SETMASK, &all, &old);
        rc = pthread_create(&g_amc.worker, &attr, worker_main, NULL);
        pthread_sigmask(SIG_SETMASK, &old, NULL);
    }
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        if (g_amc.cfg.worker_cpu >= 0)
            snprintf(err, errsz, "cannot create worker thread pinned to cpu %d: %s",
                     g_amc.cfg.worker_cpu, strerror(rc));
        else
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
