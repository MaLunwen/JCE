/*
 * jce_player_loop.c  Implementation of the 8-phase PlayerLoop (P3-B.1).
 *
 * Storage: per-phase dynamic array of slots sorted by ascending
 * priority.  Insertion uses a stable binary-search position so equal
 * priorities preserve registration order.  Backing allocator is the
 * process-default mimalloc-backed jce_allocator_t.
 *
 * Single-threaded by contract; see jce_player_loop.h.
 *
 * The near-identical slot list in application/jce_lifecycle.c is a
 * deliberate second instance, not un-deduplicated code: its dispatch
 * contract differs from run_phase's id-identity snapshot below, and
 * merging the two would undo the F81 fix.  Full verdict at the top of
 * that file.
 */

#include <jce/runtime/jce_player_loop.h>

#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_timer.h>

#include <string.h>

#define LOG_TAG "player_loop"
#define DEBUG_NAME_MAX 32u

typedef struct PhaseSlot {
    int32_t          priority;
    uint32_t         id;
    JcePlayerLoopFn  fn;
    void            *user;
    double           last_ms;
    bool             enabled;
    char             debug_name[DEBUG_NAME_MAX]; /* "" => not set */
} PhaseSlot;

typedef struct PhaseList {
    PhaseSlot *items;
    uint32_t   count;
    uint32_t   capacity;
} PhaseList;

static PhaseList        s_phases[JCE_PHASE_COUNT];
static uint32_t         s_next_id;     /* monotonically increasing, never reuses 0 */
static jce_allocator_t  s_alloc;
static bool             s_alloc_ready;

static void ensure_alloc(void)
{
    if (!s_alloc_ready) {
        s_alloc = jce_allocator_default();
        s_alloc_ready = true;
    }
}

static bool grow(PhaseList *p)
{
    const uint32_t new_cap = (p->capacity == 0u) ? 4u : (p->capacity * 2u);
    PhaseSlot *grown = (PhaseSlot *)JCE_AREALLOC(
        s_alloc, p->items, (size_t)new_cap * sizeof(PhaseSlot));
    if (!grown) {
        LOG_ERROR(LOG_TAG, "phase list grow failed (cap=%u)", new_cap);
        return false;
    }
    p->items    = grown;
    p->capacity = new_cap;
    return true;
}

/* Lower-bound: first index whose priority is strictly greater than
 * `priority`.  Equal priorities therefore append in registration order. */
static uint32_t upper_bound(const PhaseList *p, int32_t priority)
{
    uint32_t lo = 0u, hi = p->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (p->items[mid].priority <= priority)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

JcePlayerLoopHandle JCE_CALL
jce_player_loop_register(JcePlayerLoopPhase phase,
                         int32_t            priority,
                         JcePlayerLoopFn    fn,
                         void              *user)
{
    JcePlayerLoopHandle h = { 0u };

    if ((unsigned)phase >= (unsigned)JCE_PHASE_COUNT || !fn) {
        LOG_WARN(LOG_TAG, "register rejected: bad phase or NULL fn");
        return h;
    }

    ensure_alloc();
    PhaseList *p = &s_phases[phase];

    if (p->count >= p->capacity && !grow(p))
        return h;

    const uint32_t pos = upper_bound(p, priority);
    if (pos < p->count) {
        memmove(&p->items[pos + 1u], &p->items[pos],
                (size_t)(p->count - pos) * sizeof(PhaseSlot));
    }

    /* Skip id 0 (reserved as "invalid handle"). */
    if (++s_next_id == 0u)
        s_next_id = 1u;

    p->items[pos].priority = priority;
    p->items[pos].id       = s_next_id;
    p->items[pos].fn       = fn;
    p->items[pos].user     = user;
    p->items[pos].last_ms  = 0.0;
    p->items[pos].enabled  = true;
    p->items[pos].debug_name[0] = '\0';
    p->count++;

    h.id = s_next_id;
    return h;
}

void JCE_CALL
jce_player_loop_unregister(JcePlayerLoopHandle h)
{
    if (h.id == 0u) return;

    for (int phase = 0; phase < (int)JCE_PHASE_COUNT; ++phase) {
        PhaseList *p = &s_phases[phase];
        for (uint32_t i = 0u; i < p->count; ++i) {
            if (p->items[i].id != h.id) continue;

            const uint32_t tail = p->count - i - 1u;
            if (tail > 0u) {
                memmove(&p->items[i], &p->items[i + 1u],
                        (size_t)tail * sizeof(PhaseSlot));
            }
            p->count--;
            return;
        }
    }
}

/* Phase-local lookup by id.  Ids are globally unique and never reused, so a
 * captured id matches at most the one slot it was taken from (or nothing, if
 * that slot was unregistered). */
static PhaseSlot *find_slot_in_phase(PhaseList *p, uint32_t id)
{
    for (uint32_t i = 0u; i < p->count; ++i)
        if (p->items[i].id == id) return &p->items[i];
    return NULL;
}

void JCE_CALL
jce_player_loop_run_phase(JcePlayerLoopPhase phase, float dt)
{
    if ((unsigned)phase >= (unsigned)JCE_PHASE_COUNT) return;

    PhaseList *p = &s_phases[phase];
    const uint32_t n = p->count;
    if (n == 0u) return;

    /* Dispatch by IDENTITY, not by position.  A callback may register or
     * unregister a same-phase slot mid-loop, which memmoves the dense array;
     * iterating by index would then skip a shifted-in sibling or re-run a
     * shifted-up slot (audit Round-3 P2).  We snapshot the ids present at
     * entry and run each by id-lookup, which yields exactly-once dispatch:
     *   - ids registered during this call are absent from the snapshot, so
     *     they do not fire this frame (the documented contract); and
     *   - ids unregistered during this call are no longer found, so they do
     *     not fire.
     * last_ms is written back via the same id, so a mid-loop insert can never
     * misattribute a duration to the wrong slot. */
    uint32_t  ids_stack[64];
    uint32_t *ids = ids_stack;
    bool      ids_heap = false;
    if (n > (uint32_t)(sizeof(ids_stack) / sizeof(ids_stack[0]))) {
        uint32_t *h = (uint32_t *)JCE_AREALLOC(s_alloc, NULL,
                                               (size_t)n * sizeof(uint32_t));
        if (h) { ids = h; ids_heap = true; }
    }
    /* When the heap snapshot fails (OOM) we cap to the stack buffer; better to
     * dispatch the first 64 than to crash. */
    const uint32_t cap = ids_heap
        ? n
        : (n < (uint32_t)(sizeof(ids_stack) / sizeof(ids_stack[0]))
               ? n
               : (uint32_t)(sizeof(ids_stack) / sizeof(ids_stack[0])));
    for (uint32_t i = 0u; i < cap; ++i)
        ids[i] = p->items[i].id;

    const uint64_t freq = jce_time_perf_freq();
    for (uint32_t k = 0u; k < cap; ++k) {
        PhaseSlot *s = find_slot_in_phase(p, ids[k]);
        if (!s || !s->fn || !s->enabled) continue;   /* unregistered/disabled */
        const JcePlayerLoopFn fn   = s->fn;
        void *const           user = s->user;
        /* Single timestamp diff per call: cheap enough to keep
         * unconditionally compiled in (baseline target: ~30 ns / call). */
        const uint64_t t0 = jce_time_perf_counter();
        fn(dt, user);
        const uint64_t t1 = jce_time_perf_counter();
        /* Re-find by id: fn() may have realloc'd/memmoved the array. */
        s = find_slot_in_phase(p, ids[k]);
        if (s) {
            s->last_ms = (freq > 0u)
                ? ((double)(t1 - t0) * 1000.0 / (double)freq)
                : 0.0;
        }
    }

    if (ids_heap) JCE_AFREE(s_alloc, ids);
}

uint32_t JCE_CALL
jce_player_loop_phase_count(JcePlayerLoopPhase phase)
{
    if ((unsigned)phase >= (unsigned)JCE_PHASE_COUNT) return 0u;
    return s_phases[phase].count;
}

void JCE_CALL
jce_player_loop_iterate(JcePlayerLoopIterFn cb, void *user)
{
    if (!cb) return;
    for (int phase = 0; phase < (int)JCE_PHASE_COUNT; ++phase) {
        const PhaseList *p = &s_phases[phase];
        for (uint32_t i = 0u; i < p->count; ++i) {
            const PhaseSlot *s = &p->items[i];
            JcePlayerLoopEntry e;
            e.phase      = (JcePlayerLoopPhase)phase;
            e.priority   = s->priority;
            e.debug_name = (s->debug_name[0] != '\0') ? s->debug_name : NULL;
            /* memcpy avoids undefined behavior from a direct
             * function->object pointer cast; the panel only displays
             * the value, never invokes it through this handle. */
            {
                void *fn_as_obj = NULL;
                memcpy(&fn_as_obj, &s->fn, sizeof(fn_as_obj));
                e.fn = fn_as_obj;
            }
            e.user       = s->user;
            e.id         = s->id;
            e.last_ms    = s->last_ms;
            e.enabled    = s->enabled;
            cb(&e, user);
        }
    }
}

static PhaseSlot *find_slot_by_id(uint32_t id)
{
    if (id == 0u) return NULL;
    for (int phase = 0; phase < (int)JCE_PHASE_COUNT; ++phase) {
        PhaseList *p = &s_phases[phase];
        for (uint32_t i = 0u; i < p->count; ++i) {
            if (p->items[i].id == id) return &p->items[i];
        }
    }
    return NULL;
}

void JCE_CALL
jce_player_loop_set_enabled(uint32_t id, bool enabled)
{
    PhaseSlot *s = find_slot_by_id(id);
    if (s) s->enabled = enabled;
}

void JCE_CALL
jce_player_loop_set_debug_name(uint32_t id, const char *name)
{
    PhaseSlot *s = find_slot_by_id(id);
    if (!s) return;
    if (!name) { s->debug_name[0] = '\0'; return; }
    size_t i = 0u;
    while (i < DEBUG_NAME_MAX - 1u && name[i] != '\0') {
        s->debug_name[i] = name[i];
        ++i;
    }
    s->debug_name[i] = '\0';
}

const char *JCE_CALL
jce_player_loop_phase_to_string(JcePlayerLoopPhase phase)
{
    switch (phase) {
        case JCE_PHASE_INITIALIZATION: return "initialization";
        case JCE_PHASE_EARLY_UPDATE:   return "early_update";
        case JCE_PHASE_FIXED_UPDATE:   return "fixed_update";
        case JCE_PHASE_UPDATE:         return "update";
        case JCE_PHASE_LATE_UPDATE:    return "late_update";
        case JCE_PHASE_PRE_RENDER:     return "pre_render";
        case JCE_PHASE_POST_RENDER:    return "post_render";
        case JCE_PHASE_END_OF_FRAME:   return "end_of_frame";
        case JCE_PHASE_COUNT:          break;
    }
    return "unknown";
}

void JCE_CALL
jce_player_loop_shutdown(void)
{
    if (!s_alloc_ready) return;
    for (int phase = 0; phase < (int)JCE_PHASE_COUNT; ++phase) {
        PhaseList *p = &s_phases[phase];
        if (p->items) JCE_AFREE(s_alloc, p->items);
        p->items    = NULL;
        p->count    = 0u;
        p->capacity = 0u;
    }
    s_next_id = 0u;
}
