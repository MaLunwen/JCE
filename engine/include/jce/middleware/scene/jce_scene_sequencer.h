/*
 * jce_scene_sequencer.h -- Sequencer ↔ scene integrator (P1-L).
 *
 * Bridges the entity-agnostic sequencer runtime (jce_sequencer.h) to live
 * scene components: a small canonical property catalogue (dotted names the
 * editor's Sequencer panel binds tracks to), float/color appliers that
 * route through the public jce_scene_get_* accessors, and the per-frame
 * driver jce_scene_sequencer_update() that walks every entity carrying a
 * JceSequencePlayerComponent (mirrors jce_scene_video_update).
 *
 * Layer: Scene (Layer 3) — public.
 */

#ifndef JCE_SCENE_SEQUENCER_H
#define JCE_SCENE_SEQUENCER_H

#include <jce/middleware/scene/jce_scene.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Canonical animatable properties.  Float props are addressed by
 * jce_seq_prop_apply_float; color props by jce_seq_prop_apply_color. */
typedef enum {
    JCE_SEQ_PROP_NONE = 0,
    /* float props */
    JCE_SEQ_PROP_POS_X,            /* transform.position.x        */
    JCE_SEQ_PROP_POS_Y,            /* transform.position.y        */
    JCE_SEQ_PROP_POS_Z,            /* transform.position.z        */
    JCE_SEQ_PROP_ROT_EULER_X,      /* transform.rotation.euler.x (deg) */
    JCE_SEQ_PROP_ROT_EULER_Y,      /* transform.rotation.euler.y (deg) */
    JCE_SEQ_PROP_ROT_EULER_Z,      /* transform.rotation.euler.z (deg) */
    JCE_SEQ_PROP_SCALE_X,          /* transform.scale.x           */
    JCE_SEQ_PROP_SCALE_Y,          /* transform.scale.y           */
    JCE_SEQ_PROP_SCALE_Z,          /* transform.scale.z           */
    JCE_SEQ_PROP_SCALE_UNIFORM,    /* transform.scale.uniform     */
    JCE_SEQ_PROP_LIGHT_INTENSITY,  /* light.intensity (dir→point→spot) */
    JCE_SEQ_PROP_CAMERA_FOV,       /* camera.fov (degrees)        */
    JCE_SEQ_PROP_AUDIO_VOLUME,     /* audio.volume                */
    JCE_SEQ_PROP_VOLUME_WEIGHT,    /* volume.weight               */
    /* color props */
    JCE_SEQ_PROP_LIGHT_COLOR,      /* light.color (dir→point→spot) */
    JCE_SEQ_PROP_MESH_BASE_COLOR,  /* meshrenderer.base_color      */

    JCE_SEQ_PROP_COUNT
} JceSeqPropId;

/* Canonical dotted name for a property id ("" for NONE/out of range). */
JCE_API const char  *jce_seq_prop_name(JceSeqPropId id);

/* Reverse lookup; returns JCE_SEQ_PROP_NONE for unknown/empty names. */
JCE_API JceSeqPropId jce_seq_prop_from_name(const char *name);

/* True for the color-typed properties (apply via jce_seq_prop_apply_color). */
JCE_API bool jce_seq_prop_is_color(JceSeqPropId id);

/* Does `e` carry the component the property routes to?  (Light props check
 * dir → point → spot in that order, matching the appliers.) */
JCE_API bool jce_seq_prop_supported(JceScene *s, JceEntity e, JceSeqPropId id);

/* Read the current value (editor preview snapshot / restore).  Returns
 * false when the prop is unsupported on `e`. */
JCE_API bool jce_seq_prop_get_float(JceScene *s, JceEntity e,
                                    JceSeqPropId id, float *out);
JCE_API bool jce_seq_prop_get_color(JceScene *s, JceEntity e,
                                    JceSeqPropId id, float out_rgb[3]);

/* Apply an evaluated track value to the entity's component. */
JCE_API void jce_seq_prop_apply_float(JceScene *s, JceEntity e,
                                      JceSeqPropId id, float v);
JCE_API void jce_seq_prop_apply_color(JceScene *s, JceEntity e,
                                      JceSeqPropId id, const float rgb[3]);

/* Per-frame SequencePlayer driver.
 *
 * Iterates every entity holding a JceSequencePlayerComponent: lazily loads
 * seq_path on first use (keyed on a path hash so in-place edits re-open),
 * resolves unbound tracks by bindEntityName once at open, advances the
 * sequencer by dt*speed, evaluates + applies every bound track, and counts
 * event-track triggers split across a loop wrap.  Call ONCE per frame from
 * the runtime step (NOT from the editor edit-mode pump — the editor
 * previews through the Sequencer panel instead). */
JCE_API void jce_scene_sequencer_update(JceScene *s, float dt);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_SEQUENCER_H */
