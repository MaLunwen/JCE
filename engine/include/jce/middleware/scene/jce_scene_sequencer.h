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

/* ── Event + camera-cut dispatch (FEATURE 8.4) ───────────────────────
 *
 * EVENT-track keys carry an authored handler name; CAMERA-CUT keys carry a
 * target camera/vcam entity.  When a key is crossed during
 * jce_scene_sequencer_update the integrator dispatches it: events through the
 * registered handler sink (the runtime routes this to jce_script_call_named so
 * a .seq EVENT key fires a Lua function), and camera-cuts through the
 * active-camera seam (the integrator raises the target vcam's priority so the
 * vcam system makes it the live camera; an optional sink also observes it). */

/* Invoked once per fired EVENT key.  `handler` is the key's authored name
 * (may be ""), `entity` its authored target id (0 if none), `time` the key
 * time.  Set by the runtime; cleared with a NULL fn. */
typedef void (*JceSeqEventHandlerFn)(const char *handler, uint64_t entity,
                                     float time, void *user);
JCE_API void jce_scene_sequencer_set_event_handler(JceSeqEventHandlerFn fn,
                                                   void *user);

/* Invoked once per crossed CAMERA-CUT key, AFTER the integrator has applied
 * the cut to the scene (raised the target vcam priority).  Lets the runtime
 * observe the active-camera change.  Optional; set by the runtime. */
typedef void (*JceSeqCameraCutFn)(JceScene *s, JceEntity target, float time,
                                  void *user);
JCE_API void jce_scene_sequencer_set_camera_cut_handler(JceSeqCameraCutFn fn,
                                                        void *user);

/* Resolve a SequencePlayer's authored (project-relative) .seq.json path to a
 * loadable host path before the integrator opens it.  Needed because the editor
 * runs with a CWD that is NOT the project root, so the raw relative path would
 * miss the file.  Returns true and fills `buf` on success.  Optional; when
 * unset (e.g. a shipped build whose CWD already is the asset root) the raw path
 * is used as-is.  Set by the runtime (mirrors JceRuntimeDesc.resolve_path_fn). */
typedef bool (*JceSeqResolvePathFn)(void *user, const char *path,
                                    char *buf, int cap);
JCE_API void jce_scene_sequencer_set_resolve_fn(JceSeqResolvePathFn fn,
                                                void *user);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_SEQUENCER_H */
