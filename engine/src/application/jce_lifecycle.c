/*
 * jce_lifecycle.c  Application lifecycle event registry implementation
 *                  (P3-B.3).
 *
 * Storage mirrors the PlayerLoop registry pattern: a single dynamic
 * array of slots sorted by ascending priority, with stable insertion
 * (equal priorities preserve registration order).  Backing allocator
 * is the process-default mimalloc-backed jce_allocator_t.  This is
 * NOT built on os/core/jce_event.c, whose dispatch contract differs
 * on ordering, removal, duplicates and lifetime; see the header.
 *
 * On folding the slot list together with runtime/jce_player_loop.c
 * (audited, and REJECTED — do not re-open):
 *   - Only upper_bound is algorithmically identical (12 lines).
 *     ensure_alloc and grow close over per-module file statics — slot
 *     type, log tag, allocator latch, id counter — that must stay
 *     separate: one shared id counter would make either shutdown()
 *     reset the other module's ids, and one shared allocator latch is
 *     new cross-layer global state.
 *   - The dispatch loops implement DIFFERENT contracts on purpose.
 *     jce_player_loop_run_phase dispatches by id-identity snapshot
 *     (the audit Round-3 F81 fix, pinned by tests/application/
 *     test_jce_player_loop.c); emit() below is positional over a
 *     snapshotted count.  A shared list must pick one, and either
 *     choice re-introduces F81 or silently changes this registry.
 *   - What is left is a 12-line bisection whose only type-generic C99
 *     form is (base, stride, field-offset) pointer arithmetic — the
 *     shape os/core/jce_hashmap.h already argues against for exactly
 *     this class of intrusive, hand-rolled table.
 *   - It is also a two-site pattern: those are the only two ordered
 *     priority-slot lists in engine/ (third_party excluded).
 *
 * Thread model: registration / unregistration / emit are MAIN-THREAD
 * ONLY (lifecycle events originate from the SDL event pump, which is
 * pumped on the main thread).  Documented in the header.
 */

#include <jce/application/jce_lifecycle.h>

#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_log.h>

#include <string.h>

#define LOG_TAG "lifecycle"

typedef struct LifecycleSlot {
    int32_t              priority;
    uint32_t             id;
    JceLifecycleCallback cb;
    void                *user;
} LifecycleSlot;

static LifecycleSlot  *s_slots;
static uint32_t        s_count;
static uint32_t        s_capacity;
static uint32_t        s_next_id;
static jce_allocator_t s_alloc;
static bool            s_alloc_ready;

static bool s_focused = true;   /* assume foreground at boot                 */
static bool s_paused;            /* assume running at boot                    */

static void ensure_alloc(void)
{
    if (!s_alloc_ready) {
        s_alloc       = jce_allocator_default();
        s_alloc_ready = true;
    }
}

static bool grow(void)
{
    const uint32_t new_cap = (s_capacity == 0u) ? 4u : (s_capacity * 2u);
    LifecycleSlot *grown   = (LifecycleSlot *)JCE_AREALLOC(
        s_alloc, s_slots, (size_t)new_cap * sizeof(LifecycleSlot));
    if (!grown) {
        LOG_ERROR(LOG_TAG, "listener table grow failed (cap=%u)", new_cap);
        return false;
    }
    s_slots    = grown;
    s_capacity = new_cap;
    return true;
}

/* First index whose priority is strictly greater than `priority`. */
static uint32_t upper_bound(int32_t priority)
{
    uint32_t lo = 0u, hi = s_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (s_slots[mid].priority <= priority)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

JceLifecycleHandle JCE_CALL
jce_lifecycle_register(JceLifecycleCallback cb, int32_t priority, void *user)
{
    JceLifecycleHandle h = { 0u };

    if (!cb) {
        LOG_WARN(LOG_TAG, "register rejected: NULL cb");
        return h;
    }

    ensure_alloc();

    if (s_count >= s_capacity && !grow())
        return h;

    const uint32_t pos = upper_bound(priority);
    if (pos < s_count) {
        memmove(&s_slots[pos + 1u], &s_slots[pos],
                (size_t)(s_count - pos) * sizeof(LifecycleSlot));
    }

    if (++s_next_id == 0u)
        s_next_id = 1u;

    s_slots[pos].priority = priority;
    s_slots[pos].id       = s_next_id;
    s_slots[pos].cb       = cb;
    s_slots[pos].user     = user;
    s_count++;

    h.id = s_next_id;
    return h;
}

void JCE_CALL
jce_lifecycle_unregister(JceLifecycleHandle h)
{
    if (h.id == 0u) return;

    for (uint32_t i = 0u; i < s_count; ++i) {
        if (s_slots[i].id != h.id) continue;

        const uint32_t tail = s_count - i - 1u;
        if (tail > 0u) {
            memmove(&s_slots[i], &s_slots[i + 1u],
                    (size_t)tail * sizeof(LifecycleSlot));
        }
        s_count--;
        return;
    }
}

void JCE_CALL
jce_lifecycle_emit(JceLifecycleEvent event)
{
    if ((unsigned)event >= (unsigned)JCE_LIFECYCLE_EVENT_COUNT)
        return;

    /* Update cached state BEFORE callbacks so getters reflect the
     * "new" state from inside listener code. */
    switch (event) {
    case JCE_LIFECYCLE_FOCUS_GAINED: s_focused = true;  break;
    case JCE_LIFECYCLE_FOCUS_LOST:   s_focused = false; break;
    case JCE_LIFECYCLE_PAUSE:        s_paused  = true;  break;
    case JCE_LIFECYCLE_RESUME:       s_paused  = false; break;
    default: break;
    }

    /* Snapshot count up front: a callback that registers another
     * listener must not fire this emit. */
    const uint32_t n = s_count;
    for (uint32_t i = 0u; i < n; ++i) {
        const LifecycleSlot s = s_slots[i];
        if (s.cb) s.cb(event, s.user);
    }
}

bool JCE_CALL jce_lifecycle_is_focused(void) { return s_focused; }
bool JCE_CALL jce_lifecycle_is_paused(void)  { return s_paused;  }

const char *JCE_CALL
jce_lifecycle_event_to_string(JceLifecycleEvent event)
{
    switch (event) {
    case JCE_LIFECYCLE_FOCUS_GAINED: return "FOCUS_GAINED";
    case JCE_LIFECYCLE_FOCUS_LOST:   return "FOCUS_LOST";
    case JCE_LIFECYCLE_PAUSE:        return "PAUSE";
    case JCE_LIFECYCLE_RESUME:       return "RESUME";
    case JCE_LIFECYCLE_LOW_MEMORY:   return "LOW_MEMORY";
    case JCE_LIFECYCLE_WILL_QUIT:    return "WILL_QUIT";
    case JCE_LIFECYCLE_DEVICE_LOST:  return "DEVICE_LOST";
    case JCE_LIFECYCLE_DEVICE_RESET: return "DEVICE_RESET";
    case JCE_LIFECYCLE_EVENT_COUNT:  break;
    }
    return "UNKNOWN";
}

void JCE_CALL
jce_lifecycle_shutdown(void)
{
    if (!s_alloc_ready) return;
    if (s_slots) JCE_AFREE(s_alloc, s_slots);
    s_slots    = NULL;
    s_count    = 0u;
    s_capacity = 0u;
    s_next_id  = 0u;
    s_focused  = true;
    s_paused   = false;
}
