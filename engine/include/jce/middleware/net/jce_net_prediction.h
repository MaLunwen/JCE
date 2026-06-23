/*
 * jce_net_prediction.h — client-side prediction + reconciliation/rollback core.
 *
 * The deterministic-resim engine of rollback netcode, kept deliberately
 * SMALL, GENERIC, and TRANSPORT-FREE so that:
 *   - it has no ENet / flecs / physics / scene dependency (100% headless),
 *   - any game supplies its OWN input and state structs (opaque blobs of
 *     caller-specified byte sizes — the buffer never hard-codes a layout),
 *   - the deterministic simulation step is a caller-provided pure function.
 *
 * Mental model
 * ------------
 * A client predicts its owned object forward every tick by running the
 * sim locally on freshly-sampled local input (it cannot wait a round-trip
 * for the server).  It keeps a RING of {tick, input, predicted_state} so
 * that when an AUTHORITATIVE state for an older tick arrives, it can:
 *   1. compare predicted_state[auth_tick] vs the authority,
 *   2. if they match — nothing to do (the common case),
 *   3. if they diverge — ROLL BACK to the authoritative state at auth_tick
 *      and RE-PLAY every buffered input after it to re-derive "now".
 *
 * This is exactly the GGPO / Unreal "client-side prediction + server
 * reconciliation" loop, minus the wire codec and the gameplay sim — both
 * of which stay outside this module (the caller owns them).
 *
 * What this module is NOT (documented followups, intentionally deferred):
 *   - Input-wire codec / quantisation                  (game + jce_net_quant)
 *   - Transport command channel (sending inputs up)    (jce_session / jce_net)
 *   - Physics / ECS resimulation hookup                (runtime integration)
 *   - Runtime player-loop scheduling                   (L5, never from here)
 *
 * Determinism contract: the caller's JcePredictionStepFn MUST be pure and
 * deterministic — out_state = f(prev_state, input) with no hidden global
 * inputs — otherwise rollback/replay cannot reproduce the timeline.
 *
 * Layer: L4 (middleware/net).  Consumed via <jce/api_net.h>.  Reuses
 * JceNetTick from <jce/middleware/net/jce_replication.h>.
 */

#ifndef JCE_NET_PREDICTION_H
#define JCE_NET_PREDICTION_H

#include <jce/middleware/net/jce_replication.h>  /* JceNetTick (single source) */
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Limits                                                              */
/* ================================================================== */

/* Per-entry blob caps.  input/state sizes given at create are clamped to
 * these so a single contiguous allocation has a known upper bound and no
 * caller can request a pathological per-entry stride. */
#define JCE_PREDICTION_MAX_INPUT_SIZE  ((uint32_t)256)
#define JCE_PREDICTION_MAX_STATE_SIZE  ((uint32_t)256)

/* ================================================================== */
/* Step function                                                       */
/* ================================================================== */

/*
 * The caller's deterministic simulation step.  Computes the state AFTER
 * applying `input` to `prev_state`:  *out_state = f(prev_state, input).
 *
 *   prev_state : read-only, state_size bytes (the state BEFORE this input)
 *   input      : read-only, input_size bytes
 *   out_state  : write-only, state_size bytes (the resulting state)
 *   user       : caller cookie passed straight through
 *
 * Return 0 on success; non-zero is treated as a step failure and aborts
 * the current apply/replay (the buffer is left in a consistent state —
 * see jce_prediction_apply_input / _reconcile docs).  prev_state and
 * out_state never alias the same storage; out_state may be fully
 * overwritten.
 */
typedef int (*JcePredictionStepFn)(const void *prev_state,
                                   const void *input,
                                   void       *out_state,
                                   void       *user);

/*
 * Optional state comparator used by reconcile.  Return true when the two
 * states are considered EQUAL (within whatever tolerance the caller
 * wants).  When NULL, reconcile falls back to a byte-exact memcmp over
 * state_size bytes.
 *
 *   a, b : state_size bytes each
 *   user : caller cookie (the reconcile `user`)
 */
typedef bool (*JcePredictionCompareFn)(const void *a,
                                       const void *b,
                                       void       *user);

/* ================================================================== */
/* Buffer                                                              */
/* ================================================================== */

typedef struct JcePredictionBuffer JcePredictionBuffer;

/* Reconcile result codes. */
typedef enum JcePredictionResult {
    JCE_PREDICT_OK_NO_CORRECTION = 0, /* prediction matched authority   */
    JCE_PREDICT_CORRECTED        = 1, /* rolled back + replayed         */
    JCE_PREDICT_ERR_NULL         = 2, /* NULL buffer / arg              */
    JCE_PREDICT_ERR_NOT_FOUND    = 3, /* auth_tick evicted / not buffered */
    JCE_PREDICT_ERR_NO_BASELINE  = 4, /* initial state never seeded     */
    JCE_PREDICT_ERR_STEP_FAILED  = 5  /* step_fn returned non-zero      */
} JcePredictionResult;

/*
 * Create a prediction buffer.
 *
 *   input_size : bytes per input blob   (1..JCE_PREDICTION_MAX_INPUT_SIZE)
 *   state_size : bytes per state blob   (1..JCE_PREDICTION_MAX_STATE_SIZE)
 *   capacity   : ring entry count       (>= 1)
 *
 * Sizes above the caps are clamped to the cap.  Returns NULL on a zero
 * size/capacity or allocation failure.  The ring + the base state are one
 * contiguous allocation per the struct (see impl).
 */
JCE_API JcePredictionBuffer *JCE_CALL
jce_prediction_buffer_create(uint32_t input_size,
                             uint32_t state_size,
                             uint32_t capacity);

/* Destroy a buffer.  NULL-safe. */
JCE_API void JCE_CALL jce_prediction_buffer_destroy(JcePredictionBuffer *buf);

/*
 * Seed the base state — the state BEFORE the first input is applied.
 * Must be called once before the first jce_prediction_apply_input().
 * Copies state_size bytes from `state`.  NULL-safe (no-op on NULL args).
 * Calling it again replaces the base and CLEARS the ring (fresh timeline).
 */
JCE_API void JCE_CALL
jce_prediction_set_initial_state(JcePredictionBuffer *buf, const void *state);

/*
 * Local PREDICT step.  Takes the most recent state (the base state if the
 * ring is empty, else the newest ring entry's state) as `prev`, runs
 * step_fn(prev, input) -> new_state, and stores {tick, input, new_state}
 * as a new newest ring entry, evicting the oldest once capacity is
 * exceeded.
 *
 * Returns:
 *   JCE_PREDICT_OK_NO_CORRECTION  on success (entry stored),
 *   JCE_PREDICT_ERR_NULL          on NULL buf/input/step,
 *   JCE_PREDICT_ERR_NO_BASELINE   if set_initial_state was never called,
 *   JCE_PREDICT_ERR_STEP_FAILED   if step_fn returned non-zero (no entry
 *                                 is stored; the buffer is unchanged).
 *
 * `tick` is caller-assigned and is expected to be strictly increasing
 * across calls (the reconcile lookup is by exact tick value); the buffer
 * does not enforce monotonicity but the replay logic assumes contiguous
 * +1 ticks for the inputs it re-plays.
 */
JCE_API JcePredictionResult JCE_CALL
jce_prediction_apply_input(JcePredictionBuffer *buf,
                           uint32_t             tick,
                           const void          *input,
                           JcePredictionStepFn  step_fn,
                           void                *user);

/*
 * RECONCILE against an authoritative state for `auth_tick`.
 *
 * Locates the ring entry whose tick == auth_tick.  If absent (too old /
 * evicted, or never predicted) returns JCE_PREDICT_ERR_NOT_FOUND and
 * leaves the buffer untouched.
 *
 * Otherwise compares predicted_state[auth_tick] against `auth_state`
 * (via `cmp` if non-NULL, else byte-exact memcmp):
 *   - EQUAL     -> JCE_PREDICT_OK_NO_CORRECTION, buffer untouched.
 *   - DIFFERENT -> ROLLBACK + REPLAY:
 *       entry[auth_tick].state = auth_state, then for each subsequent
 *       buffered entry t (in tick order) recompute
 *       entry[t].state = step_fn(entry[t-1].state, entry[t].input).
 *     Returns JCE_PREDICT_CORRECTED.  The authoritative input for
 *     auth_tick is NOT changed (only its resulting state is overwritten,
 *     since the authority gives us state, not the input it consumed).
 *
 * On a step_fn failure mid-replay the function returns
 * JCE_PREDICT_ERR_STEP_FAILED; entries up to the failing tick reflect the
 * corrected replay, the rest are left as they were (the buffer stays
 * structurally valid — count/head unchanged).
 *
 * NULL-safe: returns JCE_PREDICT_ERR_NULL on NULL buf/auth_state/step.
 */
JCE_API JcePredictionResult JCE_CALL
jce_prediction_reconcile(JcePredictionBuffer   *buf,
                         uint32_t               auth_tick,
                         const void            *auth_state,
                         JcePredictionStepFn    step_fn,
                         JcePredictionCompareFn cmp,
                         void                  *user);

/*
 * SERVER-side re-tick on a LATE input — the server analogue of reconcile.
 *
 * On the server, the authoritative timeline is built by feeding each client's
 * inputs forward through jce_prediction_apply_input as they arrive in order.
 * When a client's input for an ALREADY-SIMULATED tick arrives LATE (jitter /
 * reorder) but is still WITHIN the buffered window, the server must not drop it
 * (the latest-wins store does): it should re-apply it at its tick and re-derive
 * the present.  This does exactly that.
 *
 * Locates the ring entry whose tick == `tick` (the window).  If absent (evicted
 * / never simulated) returns JCE_PREDICT_ERR_NOT_FOUND, buffer untouched.
 *
 * Otherwise, unlike reconcile (whose correction source is an authoritative
 * STATE), the correction source here is the late INPUT:
 *   - if the late input is byte-identical to the one already applied at `tick`
 *     -> JCE_PREDICT_OK_NO_CORRECTION, buffer untouched;
 *   - else OVERWRITE entry[tick].input with the late input, recompute
 *     entry[tick].state = step_fn(prev_state, late_input) (prev_state is the
 *     prior entry's state, or the base state for the oldest tick), then REPLAY
 *     every later buffered input to re-derive "now".  Returns
 *     JCE_PREDICT_CORRECTED.
 *
 * A step_fn failure mid-replay returns JCE_PREDICT_ERR_STEP_FAILED with the
 * buffer left structurally valid (count/head unchanged), matching reconcile.
 * NULL-safe (JCE_PREDICT_ERR_NULL on NULL buf/input/step; ERR_NO_BASELINE when
 * no base was seeded).
 *
 * The deterministic-step contract is identical to reconcile: the authoritative
 * server sim MUST be a pure JcePredictionStepFn (e.g. jce_predict_locomotion)
 * for the re-tick to reproduce the timeline — a non-deterministic rigid-body
 * authority cannot be re-ticked (documented runtime follow-up).
 */
JCE_API JcePredictionResult JCE_CALL
jce_prediction_resim_from_input(JcePredictionBuffer *buf,
                                uint32_t             tick,
                                const void          *input,
                                JcePredictionStepFn  step_fn,
                                void                *user);

/* ================================================================== */
/* Queries (all NULL-safe)                                             */
/* ================================================================== */

/*
 * Copy the CURRENT (newest) predicted state into `out` (state_size bytes).
 * If the ring is empty, copies the base state.  Returns true on success,
 * false on NULL args or when neither a ring entry nor a base state exists.
 */
JCE_API bool JCE_CALL
jce_prediction_get_current_state(const JcePredictionBuffer *buf, void *out);

/*
 * Tick of the newest buffered entry, or 0 when the ring is empty.
 * (0 is also a legal tick value; pair with jce_prediction_count to
 * disambiguate empty.)
 */
JCE_API uint32_t JCE_CALL
jce_prediction_current_tick(const JcePredictionBuffer *buf);

/* Number of entries currently held in the ring (0..capacity). */
JCE_API uint32_t JCE_CALL
jce_prediction_count(const JcePredictionBuffer *buf);

/* Configured ring capacity (entry count), or 0 for NULL. */
JCE_API uint32_t JCE_CALL
jce_prediction_capacity(const JcePredictionBuffer *buf);

/* Tick of the OLDEST buffered entry, or 0 when the ring is empty.
 * Useful to test "is auth_tick still reconcilable?" before reconcile. */
JCE_API uint32_t JCE_CALL
jce_prediction_oldest_tick(const JcePredictionBuffer *buf);

JCE_EXTERN_C_END

#endif /* JCE_NET_PREDICTION_H */
