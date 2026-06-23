/*
 * jce_anim_compress.h — lossy animation keyframe compression (curve reduction).
 *
 * A PURE, dependency-light keyframe REDUCER: given one animation track's sorted
 * timestamps + per-key values, it removes interior keyframes that can be
 * reconstructed (by the runtime's linear / shortest-arc interpolation) within a
 * caller error tolerance — the standard Ramer–Douglas–Peucker curve fit Unity /
 * Unreal apply at import.  Endpoints are always kept; a track of <= 2 keys is
 * returned unchanged.
 *
 * The API is deliberately STRUCT-FREE — it works on the raw (timestamps,
 * values) arrays an importer already holds (the engine's JceAnimChannel stores
 * exactly these), so it has no dependency on the internal clip layout and is
 * trivially unit-testable.  Compaction is IN PLACE: kept keys are moved to the
 * front of both arrays and the new key count is returned; the caller sets its
 * channel's count to that and the unused tail is simply ignored / freed.
 *
 * Lossy by nature, so it is OPT-IN (jce_anim_compress_set_enabled, default OFF):
 * importers call jce_anim_compress_track per channel only when enabled, keeping
 * the default load path byte-identical.
 *
 * Layer: L4 (middleware/animation).  Consumed via <jce/api_animation.h>.
 */

#ifndef JCE_ANIM_COMPRESS_H
#define JCE_ANIM_COMPRESS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Value layout of a track being compressed. */
typedef enum JceAnimCompressTarget {
    JCE_ANIM_COMPRESS_VEC3 = 0,  /* values is jce_vec3[count] (translation/scale) */
    JCE_ANIM_COMPRESS_QUAT = 1   /* values is jce_quat[count] (rotation)          */
} JceAnimCompressTarget;

/* Error tolerances.  A keyframe is dropped only when every key it would elide
 * reconstructs within these bounds.  Larger = smaller clips, more drift. */
typedef struct JceAnimCompressParams {
    float vec3_tolerance;       /* max position/scale error (world units)        */
    float quat_tolerance_rad;   /* max rotation error (radians)                  */
} JceAnimCompressParams;

/* Sane defaults: ~1 mm translation/scale, ~0.5° rotation. */
JCE_API void JCE_CALL
jce_anim_compress_params_default(JceAnimCompressParams *out);

/*
 * Reduce one track in place.  `timestamps` is count floats (sorted ascending);
 * `values` is count VEC3 or QUAT entries per `target`.  Removes interior keys
 * reconstructible within `params` (NULL -> defaults), compacts the survivors to
 * the front of BOTH arrays, and returns the new key count (<= count).  Tracks of
 * <= 2 keys, or NULL args, are returned unchanged.
 */
JCE_API uint32_t JCE_CALL
jce_anim_compress_track(float                       *timestamps,
                        void                        *values,
                        uint32_t                     count,
                        JceAnimCompressTarget        target,
                        const JceAnimCompressParams *params);

/* ── Global opt-in (import setting) ───────────────────────────────────────
 * Importers gate their per-channel jce_anim_compress_track calls on this so the
 * default load path is byte-identical until a cook / editor turns it on. */
JCE_API void JCE_CALL jce_anim_compress_set_enabled(bool enabled);
JCE_API bool JCE_CALL jce_anim_compress_is_enabled(void);
JCE_API void JCE_CALL jce_anim_compress_set_params(const JceAnimCompressParams *p);
JCE_API void JCE_CALL jce_anim_compress_get_params(JceAnimCompressParams *out);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_COMPRESS_H */
