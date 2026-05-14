/*
 * jce_anim_curve_serial.h  JSON persistence for JceAnimCurve.
 *
 * Saves curves to small JSON files that artists / scripters can hand-
 * edit if they want.  Format:
 *
 *   {
 *     "preWrap":  "clamp",        // or "repeat" / "pingpong"
 *     "postWrap": "clamp",
 *     "keys": [
 *       { "t": 0.0, "v": 0.0, "in": 0.0, "out": 1.0 },
 *       { "t": 0.5, "v": 1.0, "in": 0.0, "out": 0.0 },
 *       ...
 *     ]
 *   }
 *
 * Returns true on success.  Load creates a new curve with the file's
 * key count (caller owns).
 *
 * Layer: middleware / animation (Layer 3) — public.
 */

#ifndef JCE_ANIM_CURVE_SERIAL_H
#define JCE_ANIM_CURVE_SERIAL_H

#include <jce/middleware/animation/jce_anim_curve.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

JCE_API bool          jce_anim_curve_save_json(const JceAnimCurve *c,
                                                 const char *path);

JCE_API JceAnimCurve *jce_anim_curve_load_json(const char *path);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_CURVE_SERIAL_H */
