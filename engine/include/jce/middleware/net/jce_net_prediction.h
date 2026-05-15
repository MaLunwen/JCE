/*
 * jce_net_prediction.h  Client-side prediction + reconciliation.
 *
 * Standard pattern for movement-heavy multiplayer games:
 *   1. Client samples input each tick and applies it locally to the
 *      predicted state.  Both the input and the resulting state go
 *      into ring buffers, keyed by `tick`.
 *   2. Client sends the input + tick to the server.
 *   3. Server simulates, sends back authoritative state + ack tick.
 *   4. On receiving an auth state for tick T:
 *        a. Compare state[T] against authoritative.  If diverged,
 *           rewind state[T] = authoritative, then replay inputs
 *           from T..now.
 *
 * This module is the data layer: input + state ring + reconcile
 * helper.  Actual physics / movement integration is the caller's
 * job (provide a step function that takes state + input → new state).
 *
 * Layer: middleware/net (Layer 4) — public.
 */

#ifndef JCE_NET_PREDICTION_H
#define JCE_NET_PREDICTION_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_PRED_RING_SIZE     128
#define JCE_PRED_INPUT_BYTES   32
#define JCE_PRED_STATE_BYTES   64

typedef struct {
    uint32_t tick;
    uint8_t  payload[JCE_PRED_INPUT_BYTES];
    bool     active;
} JcePredInput;

typedef struct {
    uint32_t tick;
    uint8_t  payload[JCE_PRED_STATE_BYTES];
    bool     active;
} JcePredState;

typedef struct {
    JcePredInput inputs[JCE_PRED_RING_SIZE];
    JcePredState states[JCE_PRED_RING_SIZE];
    uint32_t     latest_tick;
    /* Highest authoritative ack received from server. */
    uint32_t     ack_tick;
    /* Threshold below which a divergence is ignored (per byte). */
    uint8_t      tolerance_bytes;
} JcePredRing;

/* Caller-supplied step function: state' = step(state, input).
 * Must be deterministic — same inputs give same outputs. */
typedef void (*JcePredStepFn)(const uint8_t *state_in,
                               const uint8_t *input,
                               uint8_t       *state_out,
                               void          *user_data);

JCE_API void jce_pred_init(JcePredRing *r);

/* Push input for `tick`, run step against the previous state, store
 * the resulting state.  Returns false if `tick` is older than
 * latest_tick (stale input). */
JCE_API bool jce_pred_step(JcePredRing  *r,
                            uint32_t      tick,
                            const uint8_t input[JCE_PRED_INPUT_BYTES],
                            JcePredStepFn step_fn,
                            void         *user_data);

/* Lookup state by tick.  Returns NULL when not in the ring. */
JCE_API const JcePredState *jce_pred_get_state(const JcePredRing *r,
                                                 uint32_t           tick);
JCE_API const JcePredInput *jce_pred_get_input(const JcePredRing *r,
                                                 uint32_t           tick);

/* Reconcile against server authoritative state at `auth_tick`.  If
 * the local state diverges (per-byte difference > tolerance), the
 * local state is overwritten and inputs from auth_tick+1..latest_tick
 * are replayed via `step_fn`.  Returns the number of ticks replayed
 * (0 = no divergence). */
JCE_API uint32_t jce_pred_reconcile(JcePredRing  *r,
                                      uint32_t      auth_tick,
                                      const uint8_t auth_state[JCE_PRED_STATE_BYTES],
                                      JcePredStepFn step_fn,
                                      void         *user_data);

/* Latest predicted state (most recent tick). */
JCE_API const JcePredState *jce_pred_latest(const JcePredRing *r);

JCE_EXTERN_C_END

#endif /* JCE_NET_PREDICTION_H */
