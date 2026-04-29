/*
 * jce_trigger_volume.c — Generic spatial trigger system.
 *
 * Strategy:
 *   - Triggers and observers stored in slot arrays with generation
 *     counters to detect stale handles.
 *   - Per-update: compute current overlap bitset, diff against
 *     previous frame's bitset to derive ENTER/EXIT, fire STAY for
 *     all currently-overlapping pairs (if enabled).
 *   - Bitset stored as a flat array of uint64_t, sized to
 *     ceil(triggers * observers / 64).  Packed as (t * obs + o).
 */
#include <jce/middleware/world/jce_trigger_volume.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================== */
/* Slot data                                                           */
/* ================================================================== */
typedef struct {
    JceTriggerDesc desc;
    uint64_t       user;
    uint32_t       gen;
    bool           alive;
    bool           enabled;
} TriggerSlot;

typedef struct {
    jce_vec3 position;
    uint64_t user;
    uint32_t gen;
    bool     alive;
} ObserverSlot;

struct JceTriggerWorld {
    TriggerSlot  *triggers;
    uint32_t      triggers_count;
    uint32_t      triggers_cap;
    /* Free-list of trigger slot indices for reuse. */
    uint32_t     *trig_free;
    uint32_t      trig_free_n, trig_free_cap;

    ObserverSlot *observers;
    uint32_t      observers_count;
    uint32_t      observers_cap;
    uint32_t     *obs_free;
    uint32_t      obs_free_n, obs_free_cap;

    /* Overlap bitset: previous frame.  Resized as counts grow. */
    uint64_t *prev_bits;
    uint32_t  bits_t_cap;   /* trigger dim of bitset matrix */
    uint32_t  bits_o_cap;   /* observer dim */

    JceTriggerEventFn event_fn;
    void             *event_user;
    bool              stay_events;
    uint32_t          stay_event_period;   /* 1 = every update, N = every Nth */
    uint32_t          update_index;

    /* Stats. */
    JceTriggerStats stats;
};

/* ================================================================== */
/* Geometry                                                            */
/* ================================================================== */
static bool point_in_aabb(jce_vec3 p, jce_vec3 c, jce_vec3 he)
{
    return fabsf(p.x - c.x) <= he.x &&
           fabsf(p.y - c.y) <= he.y &&
           fabsf(p.z - c.z) <= he.z;
}
static bool point_in_sphere(jce_vec3 p, jce_vec3 c, float r)
{
    jce_vec3 d = jce_v3_sub(p, c);
    return jce_v3_dot(d, d) <= r * r;
}
static bool point_in_obb(jce_vec3 p, const JceTriggerDesc *d)
{
    jce_vec3 rel = jce_v3_sub(p, d->center);
    float ax = jce_v3_dot(rel, d->axis_x);
    float ay = jce_v3_dot(rel, d->axis_y);
    float az = jce_v3_dot(rel, d->axis_z);
    return fabsf(ax) <= d->half_extents.x &&
           fabsf(ay) <= d->half_extents.y &&
           fabsf(az) <= d->half_extents.z;
}
static bool overlaps(const JceTriggerDesc *d, jce_vec3 p)
{
    switch (d->shape) {
    case JCE_TRIGGER_AABB:   return point_in_aabb(p, d->center, d->half_extents);
    case JCE_TRIGGER_SPHERE: return point_in_sphere(p, d->center, d->half_extents.x);
    case JCE_TRIGGER_OBB:    return point_in_obb(p, d);
    }
    return false;
}

/* ================================================================== */
/* Slot helpers                                                        */
/* ================================================================== */
static uint32_t alloc_trigger_slot(JceTriggerWorld *w)
{
    if (w->trig_free_n) return w->trig_free[--w->trig_free_n];
    if (w->triggers_count == w->triggers_cap) {
        uint32_t nc = w->triggers_cap ? w->triggers_cap * 2 : 16;
        w->triggers = (TriggerSlot *)JCE_REALLOC(w->triggers, sizeof(TriggerSlot) * nc);
        memset(w->triggers + w->triggers_cap, 0, sizeof(TriggerSlot) * (nc - w->triggers_cap));
        w->triggers_cap = nc;
    }
    return w->triggers_count++;
}
static void free_trigger_slot(JceTriggerWorld *w, uint32_t idx)
{
    if (w->trig_free_n == w->trig_free_cap) {
        uint32_t nc = w->trig_free_cap ? w->trig_free_cap * 2 : 16;
        w->trig_free = (uint32_t *)JCE_REALLOC(w->trig_free, sizeof(uint32_t) * nc);
        w->trig_free_cap = nc;
    }
    w->trig_free[w->trig_free_n++] = idx;
}
static uint32_t alloc_observer_slot(JceTriggerWorld *w)
{
    if (w->obs_free_n) return w->obs_free[--w->obs_free_n];
    if (w->observers_count == w->observers_cap) {
        uint32_t nc = w->observers_cap ? w->observers_cap * 2 : 16;
        w->observers = (ObserverSlot *)JCE_REALLOC(w->observers, sizeof(ObserverSlot) * nc);
        memset(w->observers + w->observers_cap, 0, sizeof(ObserverSlot) * (nc - w->observers_cap));
        w->observers_cap = nc;
    }
    return w->observers_count++;
}
static void free_observer_slot(JceTriggerWorld *w, uint32_t idx)
{
    if (w->obs_free_n == w->obs_free_cap) {
        uint32_t nc = w->obs_free_cap ? w->obs_free_cap * 2 : 16;
        w->obs_free = (uint32_t *)JCE_REALLOC(w->obs_free, sizeof(uint32_t) * nc);
        w->obs_free_cap = nc;
    }
    w->obs_free[w->obs_free_n++] = idx;
}

/* ================================================================== */
/* Bitset                                                              */
/* ================================================================== */
static inline size_t bits_words(uint32_t t_cap, uint32_t o_cap)
{
    uint64_t total = (uint64_t)t_cap * (uint64_t)o_cap;
    return (size_t)((total + 63u) / 64u);
}
static void ensure_bits_capacity(JceTriggerWorld *w, uint32_t t_cap, uint32_t o_cap)
{
    if (t_cap == w->bits_t_cap && o_cap == w->bits_o_cap) return;
    /* Re-shape bitset.  Old bits dropped — they only matter for delta
     * across one frame, and pair indices change with capacity, so a
     * full reset on capacity change is correct (next frame re-discovers). */
    JCE_FREE(w->prev_bits);
    size_t words = bits_words(t_cap, o_cap);
    if (words == 0) {
        w->prev_bits = NULL;
    } else {
        w->prev_bits = (uint64_t *)JCE_MALLOC(words * sizeof(uint64_t));
        memset(w->prev_bits, 0, words * sizeof(uint64_t));
    }
    w->bits_t_cap = t_cap;
    w->bits_o_cap = o_cap;
}
static inline bool get_bit(const uint64_t *b, uint64_t pair)
{
    if (!b) return false;
    return (b[pair >> 6] >> (pair & 63u)) & 1ull;
}
static inline void set_bit(uint64_t *b, uint64_t pair)
{
    b[pair >> 6] |= (1ull << (pair & 63u));
}

/* ================================================================== */
/* API                                                                 */
/* ================================================================== */

JceTriggerWorld *jce_trigger_world_create(void)
{
    JceTriggerWorld *w = (JceTriggerWorld *)JCE_MALLOC(sizeof(*w));
    memset(w, 0, sizeof(*w));
    w->stay_events = true;
    w->stay_event_period = 1u;
    return w;
}

void jce_trigger_world_destroy(JceTriggerWorld *w)
{
    if (!w) return;
    JCE_FREE(w->triggers); JCE_FREE(w->trig_free);
    JCE_FREE(w->observers); JCE_FREE(w->obs_free);
    JCE_FREE(w->prev_bits);
    JCE_FREE(w);
}

void jce_trigger_world_set_event_fn(JceTriggerWorld *w, JceTriggerEventFn fn, void *user)
{ w->event_fn = fn; w->event_user = user; }

void jce_trigger_world_set_stay_events(JceTriggerWorld *w, bool e)
{ if (w) w->stay_events = e; }

void jce_trigger_world_set_stay_event_period(JceTriggerWorld *w, uint32_t n)
{ if (w) w->stay_event_period = n; }

JceTriggerHandle jce_trigger_add(JceTriggerWorld *w, const JceTriggerDesc *desc, uint64_t user)
{
    if (!w || !desc) return JCE_TRIGGER_INVALID;
    uint32_t idx = alloc_trigger_slot(w);
    TriggerSlot *t = &w->triggers[idx];
    t->desc = *desc;
    t->user = user;
    t->gen += 1;
    t->alive = true;
    t->enabled = true;
    JceTriggerHandle h = { idx, t->gen };
    return h;
}

static TriggerSlot *resolve_trigger(JceTriggerWorld *w, JceTriggerHandle h)
{
    if (h.idx >= w->triggers_count) return NULL;
    TriggerSlot *t = &w->triggers[h.idx];
    if (!t->alive || t->gen != h.gen) return NULL;
    return t;
}
static ObserverSlot *resolve_observer(JceTriggerWorld *w, JceObserverHandle h)
{
    if (h.idx >= w->observers_count) return NULL;
    ObserverSlot *o = &w->observers[h.idx];
    if (!o->alive || o->gen != h.gen) return NULL;
    return o;
}

void jce_trigger_remove(JceTriggerWorld *w, JceTriggerHandle h)
{
    TriggerSlot *t = resolve_trigger(w, h);
    if (!t) return;
    t->alive = false;
    t->enabled = false;
    free_trigger_slot(w, h.idx);
    /* Force bitset rebuild — pair indices stable but state stale is fine;
     * the slot is now inactive and skipped in the loop. */
}

bool jce_trigger_set_desc(JceTriggerWorld *w, JceTriggerHandle h, const JceTriggerDesc *desc)
{
    TriggerSlot *t = resolve_trigger(w, h);
    if (!t || !desc) return false;
    t->desc = *desc;
    return true;
}

void jce_trigger_set_enabled(JceTriggerWorld *w, JceTriggerHandle h, bool enabled)
{
    TriggerSlot *t = resolve_trigger(w, h);
    if (t) t->enabled = enabled;
}

JceObserverHandle jce_observer_add(JceTriggerWorld *w, jce_vec3 pos, uint64_t user)
{
    if (!w) return JCE_OBSERVER_INVALID;
    uint32_t idx = alloc_observer_slot(w);
    ObserverSlot *o = &w->observers[idx];
    o->position = pos;
    o->user = user;
    o->gen += 1;
    o->alive = true;
    JceObserverHandle h = { idx, o->gen };
    return h;
}

void jce_observer_remove(JceTriggerWorld *w, JceObserverHandle h)
{
    ObserverSlot *o = resolve_observer(w, h);
    if (!o) return;
    o->alive = false;
    free_observer_slot(w, h.idx);
}

void jce_observer_set_position(JceTriggerWorld *w, JceObserverHandle h, jce_vec3 p)
{
    ObserverSlot *o = resolve_observer(w, h);
    if (o) o->position = p;
}

/* ================================================================== */
/* Update                                                              */
/* ================================================================== */
void jce_trigger_world_update(JceTriggerWorld *w)
{
    if (!w) return;
    JCE_PROFILE_ZONE_N("World::Triggers::update");

    w->update_index++;
    const bool stay_fires_this_frame = w->stay_events
        && w->stay_event_period > 0u
        && (w->update_index % w->stay_event_period) == 0u;

    /* Reshape bitset to current slot capacities.We use *capacity*
     * (not count) so dead slots have consistent pair indices. */
    ensure_bits_capacity(w, w->triggers_cap, w->observers_cap);
    size_t nwords = bits_words(w->bits_t_cap, w->bits_o_cap);
    uint64_t *cur = nwords ? (uint64_t *)JCE_MALLOC(nwords * sizeof(uint64_t)) : NULL;
    if (nwords) memset(cur, 0, nwords * sizeof(uint64_t));

    uint32_t enter = 0, exit = 0, stay = 0, overlapping = 0;

    for (uint32_t ti = 0; ti < w->triggers_count; ++ti) {
        TriggerSlot *t = &w->triggers[ti];
        if (!t->alive || !t->enabled) continue;
        for (uint32_t oi = 0; oi < w->observers_count; ++oi) {
            ObserverSlot *o = &w->observers[oi];
            if (!o->alive) continue;
            uint64_t pair = (uint64_t)ti * (uint64_t)w->bits_o_cap + (uint64_t)oi;
            bool was = get_bit(w->prev_bits, pair);
            bool is_ = overlaps(&t->desc, o->position);
            if (is_) {
                set_bit(cur, pair);
                overlapping++;
            }
            if (is_ != was) {
                if (w->event_fn) {
                    JceTriggerEvent ev;
                    ev.type = is_ ? JCE_TRIGGER_EVENT_ENTER : JCE_TRIGGER_EVENT_EXIT;
                    ev.trigger = (JceTriggerHandle){ ti, t->gen };
                    ev.observer = (JceObserverHandle){ oi, o->gen };
                    ev.trigger_user = t->user;
                    ev.observer_user = o->user;
                    ev.point = o->position;
                    w->event_fn(&ev, w->event_user);
                }
                if (is_) enter++; else exit++;
            } else if (is_ && stay_fires_this_frame) {
                if (w->event_fn) {
                    JceTriggerEvent ev;
                    ev.type = JCE_TRIGGER_EVENT_STAY;
                    ev.trigger = (JceTriggerHandle){ ti, t->gen };
                    ev.observer = (JceObserverHandle){ oi, o->gen };
                    ev.trigger_user = t->user;
                    ev.observer_user = o->user;
                    ev.point = o->position;
                    w->event_fn(&ev, w->event_user);
                }
                stay++;
            }
        }
    }

    JCE_FREE(w->prev_bits);
    w->prev_bits = cur;

    w->stats.triggers = w->triggers_count - w->trig_free_n;
    w->stats.observers = w->observers_count - w->obs_free_n;
    w->stats.pairs_overlapping = overlapping;
    w->stats.enter_events_last_update = enter;
    w->stats.exit_events_last_update = exit;
    w->stats.stay_events_last_update = stay;
    JCE_PROFILE_PLOT_I("world.triggers.pairs_overlapping", (int64_t)overlapping);
    JCE_PROFILE_ZONE_END;
}

JceTriggerStats jce_trigger_world_stats(const JceTriggerWorld *w)
{
    if (!w) { JceTriggerStats z = {0}; return z; }
    return w->stats;
}
