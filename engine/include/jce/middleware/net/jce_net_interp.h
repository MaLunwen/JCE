/*
 * jce_net_interp.h  Snapshot ring buffer + render-time interpolation.
 *
 * Client-side mechanism for taking the last N server snapshots and
 * sampling a smooth, time-aligned entity state for rendering.  Mirrors
 * Unity NetCode / Mirror's interpolation pipeline:
 *
 *   1. Server emits snapshots at, say, 30 Hz.
 *   2. Client buffers them into a ring keyed by `server_tick`.
 *   3. Render time is biased back by an interpolation delay (typically
 *      2 server tick intervals) so the renderer always has two
 *      neighbouring snapshots to blend between.
 *   4. `jce_net_interp_sample(ring, render_time, entity_id, &out)`
 *      returns the lerp-blended state.
 *
 * The ring takes ownership of pushed snapshots' entity arrays via
 * deep copy, so callers can free their decoded buffers after push.
 *
 * Layer: middleware / net (Layer 4) — public.
 */

#ifndef JCE_NET_INTERP_H
#define JCE_NET_INTERP_H

#include <jce/middleware/net/jce_net_replication.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceNetSnapshotRing JceNetSnapshotRing;

/* Create / destroy.  Capacity 0 → default (16). */
JCE_API JceNetSnapshotRing *jce_net_snapshot_ring_create(uint32_t capacity);
JCE_API void                jce_net_snapshot_ring_destroy(JceNetSnapshotRing *r);

/* Push a fully-decoded snapshot (ownership not transferred — the ring
 * deep-copies).  The supplied server_time should be the simulation
 * timestamp the server stamped on this snapshot (in seconds).
 * Snapshots older than the oldest retained one are dropped.
 * Returns true on success. */
JCE_API bool jce_net_snapshot_ring_push(JceNetSnapshotRing *r,
                                         double                 server_time,
                                         const JceNetEntitySnap *entities,
                                         uint32_t               entity_count);

JCE_API uint32_t jce_net_snapshot_ring_count(const JceNetSnapshotRing *r);
JCE_API double   jce_net_snapshot_ring_oldest_time(const JceNetSnapshotRing *r);
JCE_API double   jce_net_snapshot_ring_newest_time(const JceNetSnapshotRing *r);

/* Sample a single entity at `render_time`.  Finds the two snapshots
 * surrounding render_time, lerps each field set in the older snap's
 * mask, and writes to `out`.  Returns:
 *
 *   true  : entity found in at least one neighbouring snapshot.
 *   false : no snapshot bracketing render_time, or entity absent.
 *
 * When render_time falls outside the ring's range, behaviour depends
 * on `extrapolate`:
 *   false → snap to nearest endpoint (hold-last)
 *   true  → linearly extrapolate from the two latest snapshots */
JCE_API bool jce_net_interp_sample(const JceNetSnapshotRing *r,
                                    double                    render_time,
                                    uint32_t                  entity_id,
                                    bool                      extrapolate,
                                    JceNetEntitySnap         *out);

JCE_EXTERN_C_END

#endif /* JCE_NET_INTERP_H */
