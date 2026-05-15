/*
 * jce_net_prediction.c  Input + state ring with reconcile replay.
 *
 * Ring indexing: slot = tick % JCE_PRED_RING_SIZE.  Old entries get
 * overwritten as we wrap.  Reconcile bails if the auth_tick has
 * already been overwritten (i.e. server ack lagged > ring size).
 */

#include <jce/middleware/net/jce_net_prediction.h>

#include <string.h>

#define SLOT(t) ((t) % JCE_PRED_RING_SIZE)

void jce_pred_init(JcePredRing *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->tolerance_bytes = 1;
}

bool jce_pred_step(JcePredRing *r, uint32_t tick,
                    const uint8_t input[JCE_PRED_INPUT_BYTES],
                    JcePredStepFn step_fn, void *user)
{
    if (!r || !step_fn) return false;
    if (tick < r->latest_tick) return false;

    /* Store input. */
    JcePredInput *in = &r->inputs[SLOT(tick)];
    in->tick = tick;
    if (input) memcpy(in->payload, input, JCE_PRED_INPUT_BYTES);
    else       memset(in->payload, 0,     JCE_PRED_INPUT_BYTES);
    in->active = true;

    /* Run step against previous state. */
    const uint8_t *prev = NULL;
    static uint8_t zero[JCE_PRED_STATE_BYTES] = {0};
    if (tick > 0) {
        JcePredState *p = &r->states[SLOT(tick - 1)];
        prev = (p->active && p->tick == tick - 1) ? p->payload : zero;
    } else {
        prev = zero;
    }
    JcePredState *out = &r->states[SLOT(tick)];
    out->tick = tick;
    step_fn(prev, in->payload, out->payload, user);
    out->active = true;
    r->latest_tick = tick;
    return true;
}

const JcePredState *jce_pred_get_state(const JcePredRing *r, uint32_t tick)
{
    if (!r) return NULL;
    const JcePredState *s = &r->states[SLOT(tick)];
    return (s->active && s->tick == tick) ? s : NULL;
}

const JcePredInput *jce_pred_get_input(const JcePredRing *r, uint32_t tick)
{
    if (!r) return NULL;
    const JcePredInput *i = &r->inputs[SLOT(tick)];
    return (i->active && i->tick == tick) ? i : NULL;
}

const JcePredState *jce_pred_latest(const JcePredRing *r)
{
    return r ? jce_pred_get_state(r, r->latest_tick) : NULL;
}

static uint32_t byte_diff(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint32_t d = 0;
    for (uint32_t i = 0; i < n; ++i) if (a[i] != b[i]) d++;
    return d;
}

uint32_t jce_pred_reconcile(JcePredRing *r, uint32_t auth_tick,
                              const uint8_t auth_state[JCE_PRED_STATE_BYTES],
                              JcePredStepFn step_fn, void *user)
{
    if (!r || !step_fn || !auth_state) return 0;
    JcePredState *local = &r->states[SLOT(auth_tick)];
    if (!local->active || local->tick != auth_tick) return 0;
    r->ack_tick = auth_tick;
    /* Compare. */
    uint32_t diff = byte_diff(local->payload, auth_state,
                               JCE_PRED_STATE_BYTES);
    if (diff <= r->tolerance_bytes) return 0;
    /* Overwrite with authoritative + replay. */
    memcpy(local->payload, auth_state, JCE_PRED_STATE_BYTES);
    uint32_t replayed = 0;
    for (uint32_t t = auth_tick + 1; t <= r->latest_tick; ++t) {
        JcePredInput *in = &r->inputs[SLOT(t)];
        JcePredState *prev = &r->states[SLOT(t - 1)];
        JcePredState *out  = &r->states[SLOT(t)];
        if (!in->active || in->tick != t || !prev->active) {
            /* Missing data → stop replay. */
            break;
        }
        out->tick = t;
        step_fn(prev->payload, in->payload, out->payload, user);
        out->active = true;
        replayed++;
    }
    return replayed;
}
