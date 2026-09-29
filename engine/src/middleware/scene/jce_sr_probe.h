/*
 * jce_sr_probe.h  Which cubemap a reflection probe reflects, and where it
 * reflects it.
 *
 * JceReflectionProbeComponent.mode selects the SOURCE, and that is a path
 * choice, not a capture: Unity's Custom mode reflects `customBakedTexture`,
 * UE's SLS_SpecifiedCubemap is the same split.  The gather took
 * baked_cubemap_path unconditionally, so the mode combo and the Custom HDR
 * file picker beside it decided nothing.
 *
 * Kept out of jce_sr_cull.c so the choice is one function with one caller
 * rather than a condition grown into a 2000-line gather.
 *
 * Layer: Middleware/scene.  Internal to the scene renderer.
 */

#ifndef JCE_SR_PROBE_H
#define JCE_SR_PROBE_H

/* Not jce_sr_internal.h: nothing here touches bgfx or JceSceneRenderer, and
 * that header pulls in <bgfx/c99/bgfx.h>, which a headless test cannot
 * compile against. */
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>

/* The path this probe should load, or NULL when it has nothing to load.
 *
 * CUSTOM with a usable custom_hdr_path takes it; everything else takes
 * baked_cubemap_path, which is what every existing scene resolves to.
 *
 * REALTIME resolves to the baked path and is NOT silently aliased to it: live
 * capture does not exist here (it is what near_clip / far_clip are waiting
 * on), so a REALTIME probe reflects its last bake, exactly as it does today.
 *
 * "Usable" means a cubemap CONTAINER extension.  That guard is not tidiness:
 * rprobe_cache holds 8 entries, a failed load marks its slot `failed` forever
 * and nothing evicts, so handing it a path the loader cannot read burns a slot
 * permanently -- and after eight of those, sr_rprobe_cache_get returns -1 for
 * EVERY probe in the scene, including the baked ones that were working. */
const char *sr_rprobe_source_path(const JceReflectionProbeComponent *c);

/* Does this probe reach `p`?
 *
 * WHAT WAS WRONG.  sr_gather_rprobe_cb picked the probe with the smallest
 * distance to the camera and applied it, with NO test that the camera was
 * anywhere near the probe's volume.  A single probe baked inside a room was
 * therefore the reflection source for the entire outdoor scene the moment it
 * was the nearest one -- every wet street and every car body showing that
 * room.  box_size was read (box projection re-aims the lookup) but it bounded
 * nothing, and blend_distance, the field whose whole job is to say how far
 * past the box the probe still reaches, was read by nothing at all.
 *
 * Unity, Unreal and Godot all bound a reflection probe by a volume; JCE not
 * doing so is the gap, and this is the test all three apply: inside the box,
 * full influence; within blend_distance outside it, still influencing.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO is WEIGHT that influence.  Unity fades
 * the probe against the next probe / the skybox across the blend band, which
 * needs two environment cubemaps bound at once.  fs_pbr_body.sh has sampler
 * stages 0..15 occupied (14 is already shared between s_cluster and s_iesLut),
 * so there is no stage for a second cubemap, and the intensity scalar cannot
 * stand in for one: fs_pbr treats a non-positive u_giParams.y as 1.0 on
 * purpose, so fading it toward zero snaps back to full rather than fading
 * out, and any small positive value fades toward BLACK rather than toward the
 * sky.  The band is therefore a hard boundary at box + blend_distance until
 * the probe pass can bind two sources.  That is a smaller lie than a probe
 * with no boundary at all.
 *
 * A NON-POSITIVE EXTENT ON ANY AXIS MEANS "no volume authored" and returns
 * true, which is exactly the old behaviour -- a probe that never got a box
 * must not silently lose the scene it was lighting.
 *
 * `center` is the probe's box centre in world space (transform.position +
 * box_offset), which the caller already computes. */
bool sr_rprobe_influences(const JceReflectionProbeComponent *c,
                          jce_vec3 center, jce_vec3 p);

#endif /* JCE_SR_PROBE_H */
