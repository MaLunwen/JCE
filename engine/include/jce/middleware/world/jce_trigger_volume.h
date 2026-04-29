/*
 * jce_trigger_volume.h  Generic trigger volume system.
 *
 * Spatial volumes (AABB/Sphere/OBB) that fire enter/stay/exit callbacks
 * when tracked observer points cross them.  Generic: callers decide
 * what "observers" mean (player, NPCs, vehicles, projectiles, etc.) —
 * the system just operates on 3D points + opaque user IDs.
 *
 * Typical use cases:
 *   - Mission/quest activation zones
 *   - Audio environment changes (reverb regions)
 *   - Lighting / ToD overrides per region
 *   - Damage volumes (water, fire, void)
 *   - Loading-screen / streaming hints
 *   - Tutorial prompts on first entry
 *
 * Performance:
 *   - Point-vs-volume tests are O(volumes * observers); for small
 *     counts (typical: tens of zones × <100 observers) this is fine
 *     and avoids dependency on any spatial accel structure.
 *   - Callers can cull externally (e.g. only call update on observers
 *     near the camera) without losing correctness.
 *
 * Thread-safety: a JceTriggerVolumes instance is single-threaded.
 * Add/remove and update may be called only from the same thread.
 * Event callbacks are invoked synchronously inside _update().
 *
 * Example:
 *   JceTriggerVolumes *tv = jce_trigger_volumes_create(64, 256);
 *   JceTriggerVolumeId zone =
 *       jce_trigger_volumes_add_aabb(tv, (jce_vec3){0,0,0}, (jce_vec3){5,5,5});
 *   uint64_t player_id = 1;
 *   jce_trigger_volumes_set_observer(tv, player_id, player_pos);
 *   jce_trigger_volumes_set_callback(tv, on_event, user);  // ENTER/STAY/EXIT
 *   jce_trigger_volumes_update(tv);   // call once per frame
 *
 * Layer: world (Layer 3 — co-located with jce_world).
 */
#ifndef JCE_TRIGGER_VOLUME_H
#define JCE_TRIGGER_VOLUME_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceTriggerWorld JceTriggerWorld;

typedef enum {
    JCE_TRIGGER_AABB    = 0,
    JCE_TRIGGER_SPHERE  = 1,
    JCE_TRIGGER_OBB     = 2  /* center + half-extents + 3 axes */
} JceTriggerShape;

typedef struct {
    JceTriggerShape shape;
    /* AABB:  center + half_extents (axes ignored) */
    /* Sphere: center + half_extents.x = radius (rest ignored) */
    /* OBB:    center + half_extents + axis_x/y/z (must be unit + ortho) */
    jce_vec3 center;
    jce_vec3 half_extents;
    jce_vec3 axis_x;
    jce_vec3 axis_y;
    jce_vec3 axis_z;
} JceTriggerDesc;

typedef struct { uint32_t idx; uint32_t gen; } JceTriggerHandle;
typedef struct { uint32_t idx; uint32_t gen; } JceObserverHandle;

#define JCE_TRIGGER_INVALID  ((JceTriggerHandle){ UINT32_MAX, 0 })
#define JCE_OBSERVER_INVALID ((JceObserverHandle){ UINT32_MAX, 0 })

static inline bool jce_trigger_valid (JceTriggerHandle h)  { return h.idx != UINT32_MAX; }
static inline bool jce_observer_valid(JceObserverHandle h) { return h.idx != UINT32_MAX; }

/* ================================================================== */
/* Events                                                              */
/* ================================================================== */
typedef enum {
    JCE_TRIGGER_EVENT_ENTER = 0,
    JCE_TRIGGER_EVENT_STAY  = 1,
    JCE_TRIGGER_EVENT_EXIT  = 2
} JceTriggerEventType;

typedef struct {
    JceTriggerEventType type;
    JceTriggerHandle    trigger;
    JceObserverHandle   observer;
    uint64_t            trigger_user;   /* opaque, set on add */
    uint64_t            observer_user;  /* opaque, set on add */
    jce_vec3            point;          /* observer position at event time */
} JceTriggerEvent;

/* Event sink — caller-supplied callback invoked once per event during
 * jce_trigger_world_update().  Return value ignored. */
typedef void (*JceTriggerEventFn)(const JceTriggerEvent *ev, void *user);

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JCE_API JceTriggerWorld *jce_trigger_world_create(void);
JCE_API void             jce_trigger_world_destroy(JceTriggerWorld *w);

JCE_API void jce_trigger_world_set_event_fn(JceTriggerWorld *w,
                                            JceTriggerEventFn fn, void *user);

/* If true (default), STAY events fire each update for every (trigger,
 * observer) pair currently overlapping.  Set false to receive only
 * ENTER/EXIT — useful for level-load triggers. */
JCE_API void jce_trigger_world_set_stay_events(JceTriggerWorld *w, bool enabled);

/* Throttle STAY events to fire only every N updates per pair. N=1
 * (default) fires every update. N=0 disables STAY events (same as
 * passing false to set_stay_events). N>1 fires once every N updates,
 * which is sufficient for "still inside" gameplay queries while
 * cutting event-callback cost on busy worlds. */
JCE_API void jce_trigger_world_set_stay_event_period(JceTriggerWorld *w,
                                                     uint32_t every_n_updates);

/* ================================================================== */
/* Triggers                                                            */
/* ================================================================== */

JCE_API JceTriggerHandle jce_trigger_add(JceTriggerWorld *w,
                                         const JceTriggerDesc *desc,
                                         uint64_t user);
JCE_API void             jce_trigger_remove(JceTriggerWorld *w, JceTriggerHandle h);

/* Hot-edit volume shape/transform (e.g. moving platforms). */
JCE_API bool jce_trigger_set_desc(JceTriggerWorld *w,
                                  JceTriggerHandle h,
                                  const JceTriggerDesc *desc);

/* Disable without removing — overlap tests skip disabled triggers. */
JCE_API void jce_trigger_set_enabled(JceTriggerWorld *w,
                                     JceTriggerHandle h, bool enabled);

/* ================================================================== */
/* Observers                                                           */
/* ================================================================== */

JCE_API JceObserverHandle jce_observer_add(JceTriggerWorld *w,
                                           jce_vec3 initial_pos,
                                           uint64_t user);
JCE_API void              jce_observer_remove(JceTriggerWorld *w,
                                              JceObserverHandle h);

JCE_API void jce_observer_set_position(JceTriggerWorld *w,
                                       JceObserverHandle h,
                                       jce_vec3 pos);

/* ================================================================== */
/* Update                                                              */
/* ================================================================== */
/*
 * Advance the world: re-test every (trigger, observer) pair, fire
 * ENTER/STAY/EXIT events through the registered event_fn.
 *
 * Call once per frame after positions are updated.
 */
JCE_API void jce_trigger_world_update(JceTriggerWorld *w);

/* Stats. */
typedef struct {
    uint32_t triggers;
    uint32_t observers;
    uint32_t pairs_overlapping; /* current frame */
    uint32_t enter_events_last_update;
    uint32_t exit_events_last_update;
    uint32_t stay_events_last_update;
} JceTriggerStats;

JCE_API JceTriggerStats jce_trigger_world_stats(const JceTriggerWorld *w);

JCE_EXTERN_C_END
#endif /* JCE_TRIGGER_VOLUME_H */
