/*
 * jce_log_ring.h  Internal MPSC ring buffer for async logging.
 *
 * Multiple producer threads push pre-formatted log messages into the ring.
 * A single backend IO thread drains the ring in batches.
 *
 * Design:
 *   - write_pos / read_pos are monotonically increasing uint32_t counters.
 *   - Actual buffer index = pos & mask  (power-of-two capacity).
 *   - Ring is empty when read_pos == write_pos.
 *   - Ring is full  when write_pos - read_pos >= capacity.
 *   - Producers lock a lightweight mutex to serialize enqueue (brief
 *     critical section: one struct copy).  This is still ~20-50x faster
 *     than fprintf per message and avoids complex lock-free CAS loops.
 *
 * This is an internal header — not part of the public jce API.
 */

#ifndef JCE_LOG_RING_H
#define JCE_LOG_RING_H

#include <jce/os/core/jce_log.h>

#include "jce_memory.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ================================================================== */
/* Log message (fixed-size, POD, lives in the ring buffer)             */
/* ================================================================== */

typedef struct JceLogMessage {
    JceLogLevel level;
    int         line;
    uint64_t    timestamp_ms;   /* SDL_GetTicks() */
    SDL_Time    wall_time;      /* SDL_GetCurrentTime() */
    char        tag[32];
    char        file[64];
    char        thread_name[32];
    char        message[352];   /* pre-formatted user message */
} JceLogMessage;

/* ================================================================== */
/* Ring buffer (only on platforms with threading)                       */
/* ================================================================== */

#ifdef JCE_LOG_ASYNC

/* Capacity MUST be a power of two. */
#define JCE_LOG_RING_CAPACITY 4096u

typedef struct JceLogRing {
    JceLogMessage  *buf;        /* heap-allocated array [capacity]      */
    uint32_t        write_pos;  /* next slot to write (monotonic)       */
    uint32_t        read_pos;   /* next slot to read  (monotonic)       */
    uint32_t        capacity;
    uint32_t        mask;       /* capacity - 1                         */
    SDL_Mutex      *push_mtx;   /* serializes producers on push         */
    SDL_Mutex      *wake_mtx;   /* protects condvar for backend wakeup  */
    SDL_Condition  *wake_cond;  /* backend sleeps here                  */
    SDL_AtomicInt   dropped;    /* messages dropped when ring is full   */
} JceLogRing;

/* ------------------------------------------------------------------ */
/* Lifetime                                                            */
/* ------------------------------------------------------------------ */

static inline JceLogRing *jce_log_ring_create(void)
{
    JceLogRing *r = (JceLogRing *)JCE_CALLOC(1, sizeof(JceLogRing));
    if (!r) return NULL;

    r->capacity  = JCE_LOG_RING_CAPACITY;
    r->mask      = r->capacity - 1;
    r->write_pos = 0;
    r->read_pos  = 0;
    r->buf       = (JceLogMessage *)JCE_CALLOC(r->capacity, sizeof(JceLogMessage));
    r->push_mtx  = SDL_CreateMutex();
    r->wake_mtx  = SDL_CreateMutex();
    r->wake_cond = SDL_CreateCondition();

    SDL_SetAtomicInt(&r->dropped, 0);

    if (!r->buf || !r->push_mtx || !r->wake_mtx || !r->wake_cond) {
        JCE_FREE(r->buf);
        if (r->push_mtx)  SDL_DestroyMutex(r->push_mtx);
        if (r->wake_mtx)  SDL_DestroyMutex(r->wake_mtx);
        if (r->wake_cond) SDL_DestroyCondition(r->wake_cond);
        JCE_FREE(r);
        return NULL;
    }
    return r;
}

static inline void jce_log_ring_destroy(JceLogRing *r)
{
    if (!r) return;
    SDL_DestroyCondition(r->wake_cond);
    SDL_DestroyMutex(r->wake_mtx);
    SDL_DestroyMutex(r->push_mtx);
    JCE_FREE(r->buf);
    JCE_FREE(r);
}

/* ------------------------------------------------------------------ */
/* Push (producer — may be called from any thread)                     */
/* ------------------------------------------------------------------ */

/* Enqueue a message.  Returns true on success, false if the ring is
   full (message dropped).  Never blocks on IO.                        */
static inline bool jce_log_ring_push(JceLogRing *r, const JceLogMessage *msg)
{
    bool ok = false;

    SDL_LockMutex(r->push_mtx);
    {
        uint32_t used = r->write_pos - r->read_pos;
        if (used < r->capacity) {
            uint32_t idx = r->write_pos & r->mask;
            r->buf[idx] = *msg;               /* struct copy */
            SDL_MemoryBarrierRelease();
            r->write_pos++;
            ok = true;
        } else {
            SDL_AddAtomicInt(&r->dropped, 1);
        }
    }
    SDL_UnlockMutex(r->push_mtx);

    if (ok) {
        /* Wake the backend thread (brief lock just to signal). */
        SDL_LockMutex(r->wake_mtx);
        SDL_SignalCondition(r->wake_cond);
        SDL_UnlockMutex(r->wake_mtx);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Pop batch (consumer — ONLY called by the single backend thread)     */
/* ------------------------------------------------------------------ */

/* Drain up to max_count messages into out[].  Returns the count. */
static inline int jce_log_ring_pop_batch(JceLogRing *r,
                                         JceLogMessage *out,
                                         int max_count)
{
    int count = 0;
    while (count < max_count) {
        SDL_MemoryBarrierAcquire();
        if (r->read_pos == r->write_pos)
            break;                              /* ring empty */
        uint32_t idx = r->read_pos & r->mask;
        out[count] = r->buf[idx];              /* struct copy */
        r->read_pos++;
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Number of messages currently in the ring (approximate). */
static inline uint32_t jce_log_ring_count(const JceLogRing *r)
{
    return r->write_pos - r->read_pos;
}

#endif /* JCE_LOG_ASYNC */

#endif /* JCE_LOG_RING_H */
