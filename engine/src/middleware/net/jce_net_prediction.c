/*
 * jce_net_prediction.c — client-side prediction + reconciliation/rollback.
 *
 * Implementation notes:
 *
 *   - The ring is a flat array of `capacity` slots, each holding
 *     {tick, valid, input[input_size], state[state_size]}.  Rather than a
 *     struct-of-arrays we store one CONTIGUOUS byte blob per slot laid out
 *     as [input_size][state_size]; the per-slot metadata (tick, valid)
 *     lives in a parallel small array.  One malloc for the slot metadata,
 *     one for the blob pool, one for the base state.
 *
 *   - `head` is the index of the OLDEST slot; `count` is how many are live.
 *     The newest slot is (head + count - 1) % capacity.  This mirrors the
 *     ring shape used by jce_net_transform.c's snapshot history.
 *
 *   - The base state (state BEFORE the first input) is the rollback floor
 *     when the ring is empty and the "prev" for the oldest entry's replay.
 *
 *   - No transport, no flecs, no physics: the caller's pure step_fn is the
 *     only simulation seam.  Fully headless + deterministic.
 *
 * Layer: L4 (middleware/net).  Public header: jce_net_prediction.h.
 */

#include <jce/middleware/net/jce_net_prediction.h>

#include "os/core/jce_memory.h"

#include <string.h>

/* ================================================================== */
/* State                                                               */
/* ================================================================== */

typedef struct PredSlot {
    uint32_t tick;
    bool     valid;
} PredSlot;

struct JcePredictionBuffer {
    uint32_t  input_size;
    uint32_t  state_size;
    uint32_t  stride;        /* input_size + state_size (per-slot bytes) */
    uint32_t  capacity;      /* ring slot count                          */

    uint32_t  head;          /* index of oldest live slot                */
    uint32_t  count;         /* number of live slots (0..capacity)       */

    bool      has_base;
    uint8_t  *base_state;    /* state_size bytes                         */

    PredSlot *slots;         /* capacity metadata entries                */
    uint8_t  *pool;          /* capacity * stride bytes                  */
};

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static uint8_t *slot_input(JcePredictionBuffer *buf, uint32_t idx)
{
    return buf->pool + (size_t)idx * buf->stride;
}

static uint8_t *slot_state(JcePredictionBuffer *buf, uint32_t idx)
{
    return buf->pool + (size_t)idx * buf->stride + buf->input_size;
}

/* Ring index of the newest live slot.  Requires count > 0. */
static uint32_t newest_index(const JcePredictionBuffer *buf)
{
    return (buf->head + buf->count - 1u) % buf->capacity;
}

/* Read-only accessor to the most recent state: newest ring entry's state
 * if the ring is non-empty, else the base state.  Returns NULL when no
 * state exists at all (no base, empty ring). */
static const uint8_t *latest_state(JcePredictionBuffer *buf)
{
    if (buf->count > 0u)
        return slot_state(buf, newest_index(buf));
    if (buf->has_base)
        return buf->base_state;
    return NULL;
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JcePredictionBuffer *JCE_CALL
jce_prediction_buffer_create(uint32_t input_size,
                             uint32_t state_size,
                             uint32_t capacity)
{
    JcePredictionBuffer *buf;

    if (input_size == 0u || state_size == 0u || capacity == 0u)
        return NULL;

    if (input_size > JCE_PREDICTION_MAX_INPUT_SIZE)
        input_size = JCE_PREDICTION_MAX_INPUT_SIZE;
    if (state_size > JCE_PREDICTION_MAX_STATE_SIZE)
        state_size = JCE_PREDICTION_MAX_STATE_SIZE;

    buf = JCE_NEW(JcePredictionBuffer);
    if (!buf)
        return NULL;

    buf->input_size = input_size;
    buf->state_size = state_size;
    buf->stride     = input_size + state_size;
    buf->capacity   = capacity;
    buf->head       = 0u;
    buf->count      = 0u;
    buf->has_base   = false;

    buf->base_state = (uint8_t *)JCE_CALLOC(1u, (size_t)state_size);
    buf->slots      = (PredSlot *)JCE_CALLOC((size_t)capacity, sizeof(PredSlot));
    buf->pool       = (uint8_t *)JCE_CALLOC((size_t)capacity, (size_t)buf->stride);

    if (!buf->base_state || !buf->slots || !buf->pool) {
        jce_prediction_buffer_destroy(buf);
        return NULL;
    }

    return buf;
}

void JCE_CALL jce_prediction_buffer_destroy(JcePredictionBuffer *buf)
{
    if (!buf)
        return;
    JCE_FREE(buf->base_state);
    JCE_FREE(buf->slots);
    JCE_FREE(buf->pool);
    JCE_FREE(buf);
}

/* ================================================================== */
/* Seeding                                                             */
/* ================================================================== */

void JCE_CALL
jce_prediction_set_initial_state(JcePredictionBuffer *buf, const void *state)
{
    if (!buf || !state)
        return;
    memcpy(buf->base_state, state, (size_t)buf->state_size);
    buf->has_base = true;
    /* A fresh base starts a fresh timeline. */
    buf->head  = 0u;
    buf->count = 0u;
}

/* ================================================================== */
/* Predict                                                             */
/* ================================================================== */

JcePredictionResult JCE_CALL
jce_prediction_apply_input(JcePredictionBuffer *buf,
                           uint32_t             tick,
                           const void          *input,
                           JcePredictionStepFn  step_fn,
                           void                *user)
{
    const uint8_t *prev;
    uint8_t        prev_copy[JCE_PREDICTION_MAX_STATE_SIZE];
    uint32_t       dst_index;
    uint8_t       *dst_input;
    uint8_t       *dst_state;
    int            rc;

    if (!buf || !input || !step_fn)
        return JCE_PREDICT_ERR_NULL;
    if (!buf->has_base)
        return JCE_PREDICT_ERR_NO_BASELINE;

    prev = latest_state(buf);            /* base when ring empty */

    /* Snapshot prev into a scratch so prev/out never alias the destination
     * slot — when the ring is full the destination IS the oldest slot, and
     * with capacity==1 that oldest slot is also the newest (== prev). */
    memcpy(prev_copy, prev, (size_t)buf->state_size);

    /* Choose the destination slot WITHOUT mutating ring bookkeeping until
     * the step succeeds, so a failed step leaves the buffer unchanged. */
    if (buf->count < buf->capacity) {
        dst_index = (buf->head + buf->count) % buf->capacity;
    } else {
        /* Full: the new newest overwrites the current oldest slot, and the
         * oldest advances by one.  Compute the target up front. */
        dst_index = buf->head;
        /* The entry being evicted is the current oldest (slot[head]); its
         * resulting state becomes the new rollback FLOOR (the state BEFORE what
         * is about to become the new oldest live entry).  Advancing the base
         * with eviction keeps a later reconcile/resim anchored at the oldest
         * live tick computing from a correct predecessor.  Captured here, BEFORE
         * step_fn overwrites slot[head]'s state below.  (Inert for the existing
         * paths: apply uses the newest as prev, reconcile overwrites the found
         * state from authority, get_current reads base only when empty.) */
        memcpy(buf->base_state, slot_state(buf, buf->head),
               (size_t)buf->state_size);
    }

    dst_input = buf->pool + (size_t)dst_index * buf->stride;
    dst_state = dst_input + buf->input_size;

    /* Run the sim from the snapshot into the destination state. */
    rc = step_fn(prev_copy, input, dst_state, user);
    if (rc != 0)
        return JCE_PREDICT_ERR_STEP_FAILED;

    memcpy(dst_input, input, (size_t)buf->input_size);
    buf->slots[dst_index].tick  = tick;
    buf->slots[dst_index].valid = true;

    /* Commit ring bookkeeping. */
    if (buf->count < buf->capacity) {
        ++buf->count;
    } else {
        buf->head = (buf->head + 1u) % buf->capacity;
    }

    return JCE_PREDICT_OK_NO_CORRECTION;
}

/* ================================================================== */
/* Reconcile (rollback + replay)                                       */
/* ================================================================== */

JcePredictionResult JCE_CALL
jce_prediction_reconcile(JcePredictionBuffer   *buf,
                         uint32_t               auth_tick,
                         const void            *auth_state,
                         JcePredictionStepFn    step_fn,
                         JcePredictionCompareFn cmp,
                         void                  *user)
{
    uint32_t       found_ring_pos = 0u; /* 0..count-1 logical position */
    bool           found          = false;
    uint32_t       i;
    uint32_t       found_index;
    const uint8_t *predicted;
    bool           equal;

    if (!buf || !auth_state || !step_fn)
        return JCE_PREDICT_ERR_NULL;
    if (buf->count == 0u)
        return JCE_PREDICT_ERR_NOT_FOUND;

    /* Locate the slot whose tick == auth_tick (linear scan in logical
     * oldest->newest order). */
    for (i = 0u; i < buf->count; ++i) {
        uint32_t idx = (buf->head + i) % buf->capacity;
        if (buf->slots[idx].valid && buf->slots[idx].tick == auth_tick) {
            found_ring_pos = i;
            found          = true;
            break;
        }
    }
    if (!found)
        return JCE_PREDICT_ERR_NOT_FOUND;

    found_index = (buf->head + found_ring_pos) % buf->capacity;
    predicted   = slot_state(buf, found_index);

    if (cmp)
        equal = cmp(predicted, auth_state, user);
    else
        equal = (memcmp(predicted, auth_state, (size_t)buf->state_size) == 0);

    if (equal)
        return JCE_PREDICT_OK_NO_CORRECTION;

    /* ROLLBACK: overwrite the authoritative tick's resulting state. */
    memcpy(slot_state(buf, found_index), auth_state, (size_t)buf->state_size);

    /* REPLAY: for each subsequent buffered entry, recompute its state from
     * the (now corrected) previous entry's state and its stored input.  We
     * replay into a small scratch so prev/out never alias the same slot
     * storage, then copy back. */
    for (i = found_ring_pos + 1u; i < buf->count; ++i) {
        uint32_t       prev_index = (buf->head + i - 1u) % buf->capacity;
        uint32_t       cur_index  = (buf->head + i) % buf->capacity;
        const uint8_t *prev_state = slot_state(buf, prev_index);
        const uint8_t *cur_input  = slot_input(buf, cur_index);
        uint8_t        scratch[JCE_PREDICTION_MAX_STATE_SIZE];
        int            rc;

        rc = step_fn(prev_state, cur_input, scratch, user);
        if (rc != 0)
            return JCE_PREDICT_ERR_STEP_FAILED;

        memcpy(slot_state(buf, cur_index), scratch, (size_t)buf->state_size);
    }

    return JCE_PREDICT_CORRECTED;
}

/* ================================================================== */
/* Server-side re-tick on a late input (rollback + replay)             */
/* ================================================================== */

JcePredictionResult JCE_CALL
jce_prediction_resim_from_input(JcePredictionBuffer *buf,
                                uint32_t             tick,
                                const void          *input,
                                JcePredictionStepFn  step_fn,
                                void                *user)
{
    uint32_t       found_ring_pos = 0u;
    bool           found          = false;
    uint32_t       i;
    uint32_t       found_index;
    const uint8_t *prev_state;
    uint8_t        scratch[JCE_PREDICTION_MAX_STATE_SIZE];
    int            rc;

    if (!buf || !input || !step_fn)
        return JCE_PREDICT_ERR_NULL;
    if (!buf->has_base)
        return JCE_PREDICT_ERR_NO_BASELINE;
    if (buf->count == 0u)
        return JCE_PREDICT_ERR_NOT_FOUND;

    /* Locate the slot the late input belongs to (tick == tick), oldest->newest. */
    for (i = 0u; i < buf->count; ++i) {
        uint32_t idx = (buf->head + i) % buf->capacity;
        if (buf->slots[idx].valid && buf->slots[idx].tick == tick) {
            found_ring_pos = i;
            found          = true;
            break;
        }
    }
    if (!found)
        return JCE_PREDICT_ERR_NOT_FOUND;   /* outside the replay window */

    found_index = (buf->head + found_ring_pos) % buf->capacity;

    /* The late input is byte-identical to what the server already applied at
     * this tick — the timeline is already correct, nothing to replay. */
    if (memcmp(slot_input(buf, found_index), input, (size_t)buf->input_size) == 0)
        return JCE_PREDICT_OK_NO_CORRECTION;

    /* Overwrite the input the server applied at `tick` with the late arrival. */
    memcpy(slot_input(buf, found_index), input, (size_t)buf->input_size);

    /* Predecessor state for the corrected tick: the prior ring entry's state,
     * or the base state when this is the oldest buffered tick. */
    if (found_ring_pos == 0u)
        prev_state = buf->base_state;
    else
        prev_state = slot_state(buf,
                        (buf->head + found_ring_pos - 1u) % buf->capacity);

    /* Recompute this tick's resulting state from the corrected input (into a
     * scratch so prev/out never alias), then commit it. */
    rc = step_fn(prev_state, slot_input(buf, found_index), scratch, user);
    if (rc != 0)
        return JCE_PREDICT_ERR_STEP_FAILED;
    memcpy(slot_state(buf, found_index), scratch, (size_t)buf->state_size);

    /* REPLAY every later buffered input to re-derive "now" (identical to the
     * reconcile replay loop). */
    for (i = found_ring_pos + 1u; i < buf->count; ++i) {
        uint32_t       prev_index = (buf->head + i - 1u) % buf->capacity;
        uint32_t       cur_index  = (buf->head + i) % buf->capacity;
        const uint8_t *ps         = slot_state(buf, prev_index);
        const uint8_t *ci         = slot_input(buf, cur_index);
        uint8_t        sc[JCE_PREDICTION_MAX_STATE_SIZE];

        rc = step_fn(ps, ci, sc, user);
        if (rc != 0)
            return JCE_PREDICT_ERR_STEP_FAILED;
        memcpy(slot_state(buf, cur_index), sc, (size_t)buf->state_size);
    }

    return JCE_PREDICT_CORRECTED;
}

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

bool JCE_CALL
jce_prediction_get_current_state(const JcePredictionBuffer *buf, void *out)
{
    const uint8_t *src;

    if (!buf || !out)
        return false;

    /* latest_state needs a mutable buf for its slot math; cast away const
     * (read-only access only). */
    src = latest_state((JcePredictionBuffer *)buf);
    if (!src)
        return false;

    memcpy(out, src, (size_t)buf->state_size);
    return true;
}

uint32_t JCE_CALL
jce_prediction_current_tick(const JcePredictionBuffer *buf)
{
    if (!buf || buf->count == 0u)
        return 0u;
    return buf->slots[newest_index(buf)].tick;
}

uint32_t JCE_CALL
jce_prediction_count(const JcePredictionBuffer *buf)
{
    return buf ? buf->count : 0u;
}

uint32_t JCE_CALL
jce_prediction_capacity(const JcePredictionBuffer *buf)
{
    return buf ? buf->capacity : 0u;
}

uint32_t JCE_CALL
jce_prediction_oldest_tick(const JcePredictionBuffer *buf)
{
    if (!buf || buf->count == 0u)
        return 0u;
    return buf->slots[buf->head].tick;
}
