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

/* ONE STRUCT HOLDING THE MODULE, not eight loose names.
 *
 * This is the shape find_duplicate_symbols.py's global-state detector has
 * been teaching since the text shape cache (six statics folded to one) and
 * the morph GPU module: eight names become one.  Independent of the
 * DEVICE_LOST work in this commit -- it is here because the file was open
 * and the detector has asked for it three times in other modules.
 *
 * Process-global for the reason the header gives at length -- this registry
 * has no instance to hang off, jce_engine_destroy frees the event bus well
 * before it calls jce_lifecycle_shutdown(), and emit must stay usable in
 * between. */
static struct {
    LifecycleSlot   *slots;
    uint32_t         count;
    uint32_t         capacity;
    uint32_t         next_id;
    jce_allocator_t  alloc;
    bool             alloc_ready;

    bool             focused;     /* assume foreground at boot */
    bool             paused;      /* assume running at boot    */
} g_lc = { NULL, 0u, 0u, 0u, {0}, false, true, false };


static void ensure_alloc(void)
{
    if (!g_lc.alloc_ready) {
        g_lc.alloc       = jce_allocator_default();
        g_lc.alloc_ready = true;
    }
}

static bool grow(void)
{
    const uint32_t new_cap = (g_lc.capacity == 0u) ? 4u : (g_lc.capacity * 2u);
    LifecycleSlot *grown   = (LifecycleSlot *)JCE_AREALLOC(
        g_lc.alloc, g_lc.slots, (size_t)new_cap * sizeof(LifecycleSlot));
    if (!grown) {
        LOG_ERROR(LOG_TAG, "listener table grow failed (cap=%u)", new_cap);
        return false;
    }
    g_lc.slots    = grown;
    g_lc.capacity = new_cap;
    return true;
}

/* First index whose priority is strictly greater than `priority`. */
static uint32_t upper_bound(int32_t priority)
{
    uint32_t lo = 0u, hi = g_lc.count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (g_lc.slots[mid].priority <= priority)
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

    if (g_lc.count >= g_lc.capacity && !grow())
        return h;

    const uint32_t pos = upper_bound(priority);
    if (pos < g_lc.count) {
        memmove(&g_lc.slots[pos + 1u], &g_lc.slots[pos],
                (size_t)(g_lc.count - pos) * sizeof(LifecycleSlot));
    }

    if (++g_lc.next_id == 0u)
        g_lc.next_id = 1u;

    g_lc.slots[pos].priority = priority;
    g_lc.slots[pos].id       = g_lc.next_id;
    g_lc.slots[pos].cb       = cb;
    g_lc.slots[pos].user     = user;
    g_lc.count++;

    h.id = g_lc.next_id;
    return h;
}

void JCE_CALL
jce_lifecycle_unregister(JceLifecycleHandle h)
{
    if (h.id == 0u) return;

    for (uint32_t i = 0u; i < g_lc.count; ++i) {
        if (g_lc.slots[i].id != h.id) continue;

        const uint32_t tail = g_lc.count - i - 1u;
        if (tail > 0u) {
            memmove(&g_lc.slots[i], &g_lc.slots[i + 1u],
                    (size_t)tail * sizeof(LifecycleSlot));
        }
        g_lc.count--;
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
    case JCE_LIFECYCLE_FOCUS_GAINED: g_lc.focused = true;  break;
    case JCE_LIFECYCLE_FOCUS_LOST:   g_lc.focused = false; break;
    case JCE_LIFECYCLE_PAUSE:        g_lc.paused  = true;  break;
    case JCE_LIFECYCLE_RESUME:       g_lc.paused  = false; break;
    default: break;
    }

    /* Snapshot count up front: a callback that registers another
     * listener must not fire this emit. */
    const uint32_t n = g_lc.count;
    for (uint32_t i = 0u; i < n; ++i) {
        const LifecycleSlot s = g_lc.slots[i];
        if (s.cb) s.cb(event, s.user);
    }
}


bool JCE_CALL jce_lifecycle_is_focused(void) { return g_lc.focused; }
bool JCE_CALL jce_lifecycle_is_paused(void)  { return g_lc.paused;  }

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
    if (!g_lc.alloc_ready) return;
    if (g_lc.slots) JCE_AFREE(g_lc.alloc, g_lc.slots);
    g_lc.slots    = NULL;
    g_lc.count    = 0u;
    g_lc.capacity = 0u;
    g_lc.next_id  = 0u;
    g_lc.focused  = true;
    g_lc.paused   = false;
}
