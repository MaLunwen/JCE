/* Grass uses the same prepared roots and animated vertex stage in colour/depth.
 * Pure cell-range decisions remain GPU independent; production preparation and
 * submission are declared in jce_sr_internal.h, where EntityList is defined.
 * CPU acceptance does not prove GPU execution or visual parity.
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

/* Nonempty resident field admission only. Production preparation separately
 * distinguishes legitimate zero roots from failed nonempty requests. */
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

#endif /* JCE_SR_GRASS_SHADOW_H */
