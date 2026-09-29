/*
 * jce_sr_light_probe.h  The baked light-probe SET, and sampling it where a
 * draw actually is.
 *
 * WHAT WAS WRONG.  sr_gather_lpg_cb walked every JceLightProbeGroup, picked
 * the ONE probe nearest the CAMERA, wrote its 9 coefficients into sr->gi_sh9,
 * and every lit surface in the frame received that one ambient.  Two
 * consequences, both visible:
 *
 *   * NO SPATIAL VARIATION.  A character in a dark doorway and a wall in open
 *     sun get the same indirect light, which is the entire thing light probes
 *     exist to prevent.  The bake stores per-probe irradiance and the renderer
 *     threw all but one away every frame.
 *   * IT MOVES WITH THE CAMERA, NOT WITH THE OBJECT.  Walk the camera past the
 *     midpoint between two probes and the ambient on every object in the scene
 *     switches at once -- a whole-frame pop with no object having moved.
 *
 * Unity samples per renderer and interpolates over a Delaunay tetrahedra-
 * lisation of the probe positions; Unreal's ILC and Godot's probe grid both
 * interpolate spatially too.  "One probe for the whole frame" is not a cheaper
 * version of that, it is a different thing.
 *
 * WHAT IS HERE.  A flat set of world-space probes with their SH9, and an
 * inverse-distance-weighted sample of the k nearest.  IDW rather than
 * tetrahedral: a tetrahedralisation is a real subsystem (and needs a rebuild
 * whenever a probe moves), while 1/d^2 falls off fast enough that a probe
 * three times farther contributes a ninth as much -- so the nearest probe
 * dominates, exactly at a probe the answer IS that probe, and the transition
 * between two is smooth instead of a switch.
 *
 * WHAT IT DOES NOT DO.  It does not know about walls: two probes either side
 * of one blend through it.  Tetrahedral interpolation only partly fixes that
 * too (Unity ships the same artefact), and fixing it properly needs
 * visibility information the bake does not currently store.
 *
 * THE PATHS THAT STILL GET THE FRAME-GLOBAL VALUE are the ones that never had
 * an anchor: the per-material funnel and the GPU-driven pass-2 draws call
 * sr_upload_gi_uniforms() with no position.  That is not new -- the DYNAMIC GI
 * path (GI L1.5) has lived with exactly that split since it was written, and
 * this makes the baked source use the same mechanism rather than adding a
 * second one.  sr->gi_sh9 stays as that fallback.
 *
 * Split out so the SET and the SAMPLE are pure -- no bgfx, no
 * JceSceneRenderer -- and can be asserted headlessly.  A sampler that decides
 * inside the gather can only be checked by rendering, and ambient that is
 * subtly wrong looks exactly like ambient that is right.
 *
 * Layer: Middleware/scene.  Internal to the scene renderer.
 */

#ifndef JCE_SR_LIGHT_PROBE_H
#define JCE_SR_LIGHT_PROBE_H

/* Deliberately NOT jce_sr_internal.h: nothing here touches bgfx or the
 * renderer struct, and that header pulls in <bgfx/c99/bgfx.h>, which a
 * headless test cannot compile against. */
#include <jce/os/core/jce_math.h>

#include <stdbool.h>

/* How many probes contribute to one sample.  Four is the smallest number that
 * can bracket a point in 3D, and matches the tetrahedron Unity interpolates
 * over -- so the failure mode when the probes happen to be coplanar is the
 * same one every engine has here, rather than a new one. */
#define SR_PROBE_SAMPLE_K 4

typedef struct JceSrProbeSet JceSrProbeSet;

JceSrProbeSet *sr_probe_set_create(void);
void           sr_probe_set_destroy(JceSrProbeSet *s);

/* Emptied at the start of every gather; the set is frame state, not a cache.
 * (A cache would have to notice a probe moving, a group being disabled, and a
 * scene being swapped -- three chances to serve last frame's lighting.) */
void sr_probe_set_clear(JceSrProbeSet *s);

/* Append one WORLD-SPACE probe.  False when the set is full or out of memory;
 * the caller keeps going, because a partial set still beats one probe. */
bool sr_probe_set_add(JceSrProbeSet *s, jce_vec3 pos, const float sh9[9][3]);

int  sr_probe_set_count(const JceSrProbeSet *s);

/* Inverse-distance-weighted blend of the SR_PROBE_SAMPLE_K nearest probes.
 *
 * Returns the blend weight in [0,1] -- 0 means "no probes, do not apply",
 * which is the same contract jce_gi_probes_sample_sh9() uses so the two
 * sources are interchangeable at the call site.  Non-zero always means the
 * output is fully populated: unlike the dynamic grid there is no "the data
 * has not arrived yet" state, a baked probe either exists or does not.
 *
 * `out` is untouched when the return value is 0. */
float sr_probe_set_sample(const JceSrProbeSet *s, jce_vec3 p,
                          float out[9][3]);

#endif /* JCE_SR_LIGHT_PROBE_H */
