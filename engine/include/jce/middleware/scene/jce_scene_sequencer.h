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
 * jce_seq_prop_apply_float; color props by jce_seq_prop_apply_color.
 *
 * ADDING ONE IS AN ABI STATEMENT, and the reason is the SENTINEL rather than
 * the new values.  JCE_SEQ_PROP_COUNT moves whenever this list grows, so
 * anything that sized storage by it -- k_prop_names here, the editor's picker
 * loop, an SDK consumer's table -- disagrees with the library until it is
 * rebuilt, and an array indexed past its end does not fail loudly.  The ABI
 * snapshot calls that an incompatible change and it is right to: appending to
 * the list is exactly what moves the sentinel.
 *
 * This engine ships its headers and its library together, so the answer here
 * is "rebuild", recorded rather than assumed.  The wire format is unaffected:
 * a .seq file stores the dotted NAME (`bindProp`, and the legacy
 * "<id>/<prop>" string), never the integer, so authored sequences survive any
 * renumbering of this enum. */
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

    /* ── UI (APPENDED 2026-09-21) ────────────────────────────────────
     * The fifteen properties above cannot address a single UI element, so
     * every canvas animation in this tree is a script.  These are the
     * vocabulary for the three things a menu does -- FADE, SLIDE, POP --
     * plus the spin the RectTransform already supports.
     *
     * Appended rather than grouped with the float/color blocks above
     * because the ids are internal and appending keeps every existing one
     * fixed without having to prove that nothing depends on the order.
     * (The .seq wire format stores the dotted NAME, so it would have
     * survived either way; this way the claim needs no proof.)
     *
     * `uirect.*` addresses whichever UI graphic the entity carries, in the
     * same component order the layout walk uses -- Image, Text, Slider,
     * Toggle, InputField, ScrollView, ProgressBar, Dropdown, then Button
     * last.  One order, one function: uc_entity_rect_mut. */
    /* float props */
    JCE_SEQ_PROP_CANVASGROUP_ALPHA,   /* canvasgroup.alpha (0..1, subtree) */
    JCE_SEQ_PROP_UIRECT_ANCHORED_X,   /* uirect.anchored_position.x (px)   */
    JCE_SEQ_PROP_UIRECT_ANCHORED_Y,   /* uirect.anchored_position.y (px)   */
    JCE_SEQ_PROP_UIRECT_SCALE_X,      /* uirect.scale.x                    */
    JCE_SEQ_PROP_UIRECT_SCALE_Y,      /* uirect.scale.y                    */
    JCE_SEQ_PROP_UIRECT_SCALE_UNIFORM,/* uirect.scale.uniform              */
    JCE_SEQ_PROP_UIRECT_ROTATION,     /* uirect.rotation (deg, clockwise)  */
    JCE_SEQ_PROP_UIIMAGE_ALPHA,       /* uiimage.color.a                   */
    JCE_SEQ_PROP_UITEXT_ALPHA,        /* uitext.color.a                    */
    /* color props */
    JCE_SEQ_PROP_UIIMAGE_COLOR,       /* uiimage.color (rgb)               */
    JCE_SEQ_PROP_UITEXT_COLOR,        /* uitext.color (rgb)                */
    /* APPENDED 2026-09-21.  The three the catalogue's own "why not parity"
     * note named: a radial wipe, a driven progress bar, and text that grows.
     *
     * uitext.font_size CARRIES A ZERO TRAP, the same shape as uirect.scale:
     * 0 does not mean "no text", it means "use the default", and the default
     * is 14 in uc_draw_text while the dropdown and input-field widgets use 16.
     * So a track keyed 0 -> 24 starts at 14px, not 0.  Author from a non-zero
     * value.  Not special-cased in the applier for the reason the scale pair
     * is not: one field meaning two things depending on who wrote it is a
     * worse defect than the authoring surprise, and the surprise is tested.
     *
     * uislider.value HAS A SECOND WRITER -- the canvas writes it back while a
     * drag is in flight.  A timeline driving an INTERACTABLE slider is two
     * writers on one field per frame; author it non-interactable while
     * driven.  Stated here because nothing in the types says so. */
    JCE_SEQ_PROP_UIIMAGE_FILL,        /* uiimage.fill_amount (0..1)        */
    JCE_SEQ_PROP_UISLIDER_VALUE,      /* uislider.value (min..max)         */
    JCE_SEQ_PROP_UITEXT_FONT_SIZE,    /* uitext.font_size (px; 0 = default)*/

    JCE_SEQ_PROP_COUNT
} JceSeqPropId;

/* A NOTE THAT BELONGS WITH THE PROPERTY AND NOT IN A CHANGELOG.
 *
 * JceRectTransform.scale treats 0 as UNSCALED, not as zero size -- every
 * RectTransform serialised before that field existed loads it as zeros, and a
 * literal 0 would draw nothing, which reads as the element ceasing to exist
 * rather than as a default.  The sequencer does NOT special-case that: one
 * field with two meanings, depending on who wrote it, is the failure this
 * engine keeps paying for.
 *
 * The consequence for an author is concrete: a pop authored 0 -> 1 plays
 * FULL SIZE at t=0.  Author it from a small non-zero value (0.01) instead.
 * test_jce_seq_ui_props.c pins this so it cannot be "fixed" on one side
 * only. */

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
