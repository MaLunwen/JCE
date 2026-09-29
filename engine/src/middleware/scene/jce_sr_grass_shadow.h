/*
 * jce_sr_grass_shadow.h  Grass in the shadow cascades.
 *
 * JceGrassFieldComponent.cast_shadow was authored, serialized, and drawn in the
 * Inspector inside BeginDisabled(true) with an unwired badge and a "(v1:
 * reserved)" label -- so honest, but a field of grass never darkened anything,
 * and grass on grass reads as flat paint rather than as blades.
 *
 * THE PRIOR HYPOTHESIS WAS THAT vs_shadow_inst DID NOT EXIST.  It does:
 * engine/shaders/pbr/vs_shadow_inst.sc is built as `shadow_inst` in
 * jce_shaders.c and handed out by jce_renderer_get_program_shadow_inst().  The
 * sibling vegetation scatter has been driving it from a persistent instance
 * buffer since the 千万 S1 work, and grass already carries the identical
 * machinery -- grass_cache[].inst_vb holds every blade's TRS uploaded once per
 * param_hash, and cell_start[] already orders blades by spatial cell so a
 * contiguous [start, n) slice is addressable with zero CPU copy.  Nothing new
 * is needed on the GPU; grass simply never joined the path.
 *
 * SPLIT LIKE jce_sr_portal.h: the DECISION (does this field cast, and which
 * cell runs does this cascade need) is pure arithmetic with no bgfx and no
 * JceSceneRenderer, so it can be asserted headlessly.  A submit that decides
 * inside itself can only be checked by rendering, and a shadow map with the
 * wrong blades in it looks like a shadow map with the right blades in it.
 *
 * Layer: Middleware/scene.  Internal to the scene renderer.
 */

#ifndef JCE_SR_GRASS_SHADOW_H
#define JCE_SR_GRASS_SHADOW_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

/* A contiguous slice of the field's instance buffer: blades [start, start+count). */
typedef struct {
    uint32_t start;
    uint32_t count;
} JceGrassShadowRun;

/* Does this field contribute blades to the depth pass at all?
 *
 * `resident_blades` is grass_cache[].inst_vb_count: 0 means no persistent
 * instance buffer was built, which is what LOW/MED tiers and
 * JCE_PERSIST_GRASS=0 produce.  Those degrade to no grass shadows with the
 * colour pass untouched -- the same documented degrade the scatter takes.
 *
 * cast_shadow == false is the zero value AND today's behaviour, so every
 * scene that exists renders byte-identical shadow maps. */
bool jce_grass_shadow_field_casts(const JceGrassFieldComponent *g,
                                  bool comp_enabled, bool visible,
                                  uint32_t resident_blades);

/* Which cell runs this cascade needs, merged into contiguous slices.
 *
 * `cell_start` is the blade prefix array the colour pass already uses, so the
 * shadow set is drawn from the same ordering rather than a second one.  A cell
 * is included when its XZ box survives `planes` AND -- when fade_end > 0 -- its
 * closest point is within fade_end of `cam`, which is the same distance fade
 * the colour pass applies, so a blade cannot cast a shadow it is too far away
 * to be drawn with.
 *
 * Returns the number of runs written (<= max_runs).  Adjacent cells merge, so
 * the common "whole field visible" case is ONE run and one submit. */
uint32_t jce_grass_shadow_runs(const uint32_t *cell_start,
                               uint16_t gx, uint16_t gz,
                               float min_x, float min_z, float cell_size,
                               float ymin, float ymax,
                               const jce_vec4 *planes,
                               jce_vec3 cam, float fade_end,
                               JceGrassShadowRun *out, uint32_t max_runs);

/* Submit every casting grass field into one shadow cascade.
 *
 * Lives outside the entity walk for the same reason sr_submit_scatter_shadows
 * does: grass carries no ecull caster AABB, and it is the cache slots that
 * know which fields exist. */
struct JceSceneRenderer;
struct JceScene;
void sr_submit_grass_shadows(struct JceSceneRenderer *sr, struct JceScene *scene,
                             uint16_t cv, uint16_t shadow_inst_idx,
                             const jce_vec4 *planes, jce_vec3 cam);

#endif /* JCE_SR_GRASS_SHADOW_H */
