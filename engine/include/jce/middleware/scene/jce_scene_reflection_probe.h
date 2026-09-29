/*
 * jce_scene_reflection_probe.h  Baking a probe from its own component.
 *
 * JceReflectionProbeComponent.resolution was authored, serialized and shown in
 * the Inspector, and the engine never read it -- because no engine file builds
 * a JceReflectionProbeBakeDesc at all.  Both builders are editor panels, and
 * they disagreed with each other about the same probe:
 *
 *   - the probe browser passed `p->resolution` (the component);
 *   - the Inspector passed a THIRD value out of a static keyed by the
 *     component POINTER, offering {128, 256, 512} against the component
 *     combo's {16 .. 1024} -- so the number a designer picked in the combo
 *     right above the Bake button was not the number that got baked;
 *   - and they named the artefact differently: "probe_%u.ktx" from the entity
 *     id versus "probe_%p.ktx" from the component pointer.  The pointer form
 *     is not stable across runs, so a scene's saved baked_cubemap_path could
 *     never be re-baked to the same file, and baking once from each panel left
 *     two cubemaps on disk with the last write winning in the component.
 *
 * This is the same shape ee842b83 fixed for probe deringing: the POLICY was in
 * an ImGui panel, so it did not ship in a game and the gate that scans
 * engine/src was right not to count it.  One entry point, one size, one name.
 *
 * Layer: Middleware/scene.
 */

#ifndef JCE_SCENE_REFLECTION_PROBE_H
#define JCE_SCENE_REFLECTION_PROBE_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_reflection_probe_bake.h>

JCE_EXTERN_C_BEGIN

/* Submit a bake for this probe, at the resolution the COMPONENT authors.
 *
 * Returns 0 without submitting when the entity has no probe, or when the probe
 * is in CUSTOM mode -- a Custom probe already has an authored source, so
 * baking over it would destroy the thing it was set to reflect.  (Unity greys
 * the Bake button out for the same reason.)
 *
 * `out_path` receives the artefact path the bake was given, derived from the
 * ENTITY so one probe has one name across runs and across panels.  The caller
 * records it into baked_cubemap_path -- on submit or on DONE, its choice.
 *
 * resolution == 0 keeps the engine's own default (256) and its 512 ceiling,
 * so a zeroed component bakes exactly the file it baked before. */
JCE_API JceReflectionProbeBakeHandle JCE_CALL
jce_scene_reflection_probe_bake(JceScene *s, JceEntity e,
                                bool include_dynamic,
                                char *out_path, int out_path_cap);

/* Start a bake that captures the REAL SCENE into the six faces, instead of
 * the procedural sky-and-ground gradient jce_scene_reflection_probe_bake
 * generates.  Same artefact name and same resolution rules -- the two entries
 * share them, because a second copy of "what is this file called and how big
 * is it" is the exact disagreement this module was created to collapse.
 *
 * Returns false when a capture cannot start (no probe, a Custom probe, a
 * capture or bake already running, no renderer yet).  A caller that gets
 * false should call jce_scene_reflection_probe_bake, which is the behaviour
 * that shipped before the capture existed.
 *
 * The capture is frame-driven: the host must call
 * jce_scene_probe_capture_poll() once per frame until it leaves RENDERING. */
JCE_API bool JCE_CALL
jce_scene_reflection_probe_capture(JceScene *s, JceEntity e,
                                   char *out_path, int out_path_cap);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_REFLECTION_PROBE_H */
