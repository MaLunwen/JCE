/*
 * jce_curve.h — authored value curves (the editor's Curve Editor, read back).
 *
 * WHY THIS EXISTS.  editor/src/panels/jce_panel_curve_editor.cpp is a
 * multi-channel curve editor with per-key in/out tangents and
 * linear/cubic/constant interpolation, and it has been writing
 * {tMin,tMax,vMin,vMax,active,channels:[{name,color,visible,keys:[...]}]}
 * to disk with NOTHING ANYWHERE READING IT -- no loader, no evaluator, no
 * component field, no script binding.  A designer could draw a curve and
 * there was no way for anything to use it.
 *
 * ONE EVALUATOR, NOT TWO.  jce_curve_eval_keys is the same function the
 * editor's preview calls: the panel's eval_channel now forwards to it rather
 * than keeping its own copy.  A second implementation would agree with the
 * first until the day it did not, and the failure mode -- the curve plays
 * differently from the way it was drawn -- is invisible in both halves.
 *
 * SEMANTICS, stated because they are choices and not conventions:
 *
 *   before the first key   the first key's value (clamp, not extrapolate)
 *   after the last key     the last key's value
 *   inside a segment       the LEFT key's `interp` decides.  CONSTANT holds
 *                          the left value; LINEAR lerps; CUBIC is a Hermite
 *                          using the left key's tan_out and the right key's
 *                          tan_in.
 *   tangents               VALUE PER UNIT t, scaled by the segment width
 *                          inside the Hermite -- so a tangent means the same
 *                          slope whatever the key spacing.
 *   a zero-width segment   the right key's value, so two keys at the same t
 *                          are a step rather than a division by zero.
 */
#ifndef JCE_CURVE_H
#define JCE_CURVE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_CURVE_LINEAR   = 0,
    JCE_CURVE_CUBIC    = 1,
    JCE_CURVE_CONSTANT = 2
} JceCurveInterp;

/* One authored key.  Layout matches the editor's own Key so the panel can
 * hand its array straight to the evaluator. */
typedef struct JceCurveKey {
    float t;
    float v;
    float tan_in;
    float tan_out;
    int   interp;      /* JceCurveInterp */
} JceCurveKey;

/* THE EVALUATOR, usable without loading anything.  `keys` must be sorted by
 * t ascending, which is what the editor maintains and what the loader
 * enforces.  An empty or NULL array evaluates to 0. */
JCE_API float JCE_CALL jce_curve_eval_keys(const JceCurveKey *keys, int count,
                                           float t);

/* A loaded multi-channel curve asset. */
typedef struct JceCurve JceCurve;

/* Parse the editor's JSON.  Returns NULL only when the bytes are not JSON at
 * all, or carry neither a "channels" array nor a top-level "keys" array.
 *
 * It accepts everything the Curve Editor's own loader accepts, on purpose: a
 * pre-channels document (top-level "keys") becomes channel 0, and keys out of
 * time order are SORTED rather than refused, because the panel sorts them too
 * -- ordering is a property of the format, not a rule one half enforces.  A
 * document the editor opens without complaint must never be invisible to the
 * game.  Unknown keys are ignored so a file from a newer editor still loads,
 * and an out-of-range `interp` reads as LINEAR rather than as whatever the
 * evaluator's default branch happens to be. */
JCE_API JceCurve *JCE_CALL jce_curve_parse(const char *json, size_t len);

/* Load through the engine's virtual file system (PAK + mounted dirs), so a
 * cooked build and a loose project resolve the same path.
 *
 * The filesystem is a PARAMETER, not a global: jce_prefab_instantiate takes
 * one for the same reason, and a process can hold more than one mount set
 * (the editor's project and a packaged game's PAK are not the same fs). */
typedef struct JceFileSystem JceFileSystem;
JCE_API JceCurve *JCE_CALL jce_curve_load(const JceFileSystem *fs,
                                          const char *virtual_path);

JCE_API void JCE_CALL jce_curve_destroy(JceCurve *c);

JCE_API int JCE_CALL jce_curve_channel_count(const JceCurve *c);

/* Channel name as authored, or NULL for an out-of-range index. */
JCE_API const char *JCE_CALL jce_curve_channel_name(const JceCurve *c, int ch);

/* Index of the channel with this name, or -1.  Names are how a script or a
 * component refers to a channel: an index would break the moment an author
 * reorders the channel list in the editor. */
JCE_API int JCE_CALL jce_curve_channel_index(const JceCurve *c,
                                             const char *name);

JCE_API int JCE_CALL jce_curve_key_count(const JceCurve *c, int ch);

/* The channel's keys, or NULL when the index is out of range or the channel
 * is empty.  Valid until jce_curve_destroy.
 *
 * This exists so the EDITOR does not need a second key reader: the panel
 * takes the keys from here and reads only its authoring extras (visible,
 * colour, view range) from the document itself.  Without it there were two
 * implementations of `t`/`v`/`tanIn`/`tanOut`/`interp` in a repository whose
 * most expensive recurring bug is exactly that. */
JCE_API const JceCurveKey *JCE_CALL jce_curve_channel_keys(const JceCurve *c,
                                                           int ch);

/* Evaluate one channel.  An out-of-range channel evaluates to 0, which is
 * the same answer an empty channel gives -- callers that need to tell those
 * apart ask jce_curve_channel_count first. */
JCE_API float JCE_CALL jce_curve_eval(const JceCurve *c, int ch, float t);

/* First and last authored key times of a channel, or 0 when it has none.
 * These are the KEYS' extent, not the editor's tMin/tMax view range: the
 * view is where the author was looking, not what they authored. */
JCE_API float JCE_CALL jce_curve_time_min(const JceCurve *c, int ch);
JCE_API float JCE_CALL jce_curve_time_max(const JceCurve *c, int ch);

JCE_EXTERN_C_END

#endif /* JCE_CURVE_H */
