/*
 * cs_foliage_indirect.sc -- fill one drawIndexedIndirect element PER BAND from
 * the foliage cull's per-band survivor counters (千万 S2/S4/S5).
 *
 * After cs_foliage_cull has compacted survivors into b_visible's band
 * partitions and tallied b_counter[band], this pass writes band_count
 * drawIndexedIndirect elements so the renderer issues ONE
 * bgfx_submit_indirect per band with the EXACT survivor instance count:
 *
 *   element b:
 *     numIndices    = band b's reduced-LOD index count (u_find_counts lanes)
 *     numInstances  = b_counter[b]
 *     startIndex    = 0
 *     startVertex   = 0
 *     startInstance = b * cap_band   (the band's partition base — the draw
 *                     binds the WHOLE visible buffer as its instance stream)
 *
 * Single-band callers (the primitive S2 path) use band_count = 1: one element,
 * startInstance 0 — byte-identical to the historical single-run behaviour.
 *
 * Buffer layout (must mirror jce_sr_environment.c / jce_gpu_scene.c):
 *   b_counter  : RO, uint per band (survivor counts from cs_foliage_cull)
 *   b_indirect : WO, the bgfx indirect buffer (band_count elements)
 *
 * Uniforms:
 *   u_find_params    : (band_count, cap_band, _, _)
 *   u_find_counts[2] : 8 lanes — per-band numIndices
 */

#include <bgfx_compute.sh>

BUFFER_RO(b_counter,  uint,  0);
BUFFER_WO(b_indirect, uvec4, 1);

uniform vec4 u_find_params;
uniform vec4 u_find_counts[2];

NUM_THREADS(8, 1, 1)
void main()
{
	uint b     = gl_GlobalInvocationID.x;
	uint bands = uint(max(u_find_params.x, 1.0));
	if (b >= bands || b >= 8u) {
		return;
	}
	uint capb = uint(u_find_params.y);

	float lanes[8];
	lanes[0] = u_find_counts[0].x; lanes[1] = u_find_counts[0].y;
	lanes[2] = u_find_counts[0].z; lanes[3] = u_find_counts[0].w;
	lanes[4] = u_find_counts[1].x; lanes[5] = u_find_counts[1].y;
	lanes[6] = u_find_counts[1].z; lanes[7] = u_find_counts[1].w;

	uint numIndices   = uint(lanes[b]);
	uint numInstances = min(b_counter[b], capb);   /* clamp to the partition */

	drawIndexedIndirect(
		b_indirect,        /* target indirect buffer                  */
		b,                 /* element index (one per band)            */
		numIndices,        /* index count of band b's LOD level       */
		numInstances,      /* survivor instance count in the partition */
		0u,                /* startIndex                              */
		0u,                /* startVertex                             */
		b * capb           /* startInstance = partition base          */
		);
}
