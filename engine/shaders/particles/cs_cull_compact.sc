/*
 * cs_cull_compact.sc -- GPU-driven frustum cull with STREAM COMPACTION
 * (roadmap #18, Direction C).
 *
 * One thread per GPUScene record across the WHOLE color-pass instanced batch.
 * Tests the record's world-space AABB (centre + extent) against the 6 camera
 * frustum planes.  SURVIVORS atomically append their world matrix to the COMPACT
 * region of the visible buffer that belongs to their run: each record carries its
 * run's partition base slot (ids.x) and its run's counter index (ids.y); a
 * survivor does atomicAdd(b_counter[run_index], 1) to claim a dense local slot,
 * then writes its mat4 to b_visible[(run_base + local_slot) * 4].  Culled records
 * write NOTHING — they are simply skipped, so the compact region holds only
 * survivors and a later drawIndexedIndirect uses the counter as the instance
 * count (no degenerate zero-area instances are rasterised).
 *
 * This replaces the 1:1 cs_cull_frustum path (every slot written, culled ones
 * zeroed to degenerate triangles) with true compaction + indirect draw, removing
 * both the degenerate-slot raster waste AND the CPU per-run fixed-count submit.
 *
 * Pairing: cs_cull_reset zeroes b_counter first; cs_build_indirect reads the
 * final counters and fills the indirect-args buffer after this dispatch.  bgfx
 * inserts UAV barriers between same-view compute dispatches, so the counter and
 * compact writes are visible to the following passes.
 *
 * Scene records come from a TRANSIENT instance-data buffer (the per-frame ring
 * bgfx uploads with a single staging allocation), bound as a compute SRV.  The
 * SRV covers the whole ring from element 0, so the shader rebases each record by
 * u_cull_params.z (the allocation's vec4 offset in the ring).
 *
 * Buffer / record layout (must mirror jce_gpu_scene.c):
 *   b_scene   : RO, 7 vec4 per record (stride 112 B), ring-rebased by .z
 *       [0..3] world matrix columns col0..col3
 *       [4]    aabb_center.xyz, bounding-sphere radius (w, unused by cull)
 *       [5]    aabb_extent.xyz (half-extents), unused w
 *       [6]    ids: x=run_base (partition slot), y=run_index (counter slot)
 *   b_visible : RW, 4 vec4 per slot (a mat4 instance stream); compact within run
 *   b_counter : RW, uint per run; survivor count (atomicAdd target)
 *
 * Uniforms:
 *   u_cull_planes[6] : normalised frustum planes (xyz = normal, w = d)
 *   u_cull_params    : (record_count, visible_capacity, scene_vec4_offset, _)
 */

#include <bgfx_compute.sh>

BUFFER_RO(b_scene,   vec4, 0);
BUFFER_RW(b_visible, vec4, 1);
BUFFER_RW(b_counter, uint, 2);

uniform vec4 u_cull_planes[6];
uniform vec4 u_cull_params;

#define CULL_RECORD_VEC4 7u

NUM_THREADS(64, 1, 1)
void main()
{
	uint id    = gl_GlobalInvocationID.x;
	uint count = uint(u_cull_params.x);
	if (id >= count) {
		return;
	}

	/* Rebase into the transient ring: the SRV starts at element 0 of the whole
	 * ring, so add the allocation's vec4 offset. */
	uint scene_off = uint(u_cull_params.z);
	uint base = scene_off + id * CULL_RECORD_VEC4;

	vec4 c_r = b_scene[base + 4u];   /* center.xyz, radius */
	vec4 ext = b_scene[base + 5u];   /* half-extents.xyz   */
	vec4 ids = b_scene[base + 6u];   /* run_base, run_index */
	vec3 center = c_r.xyz;
	vec3 e      = ext.xyz;

	uint run_base  = uint(ids.x);
	uint run_index = uint(ids.y);

	/* AABB vs frustum: cull when the box's farthest extent toward a plane
	 * normal is still behind that plane, for ANY of the 6 planes. */
	bool visible = true;
	for (int p = 0; p < 6; ++p) {
		vec4 pl = u_cull_planes[p];
		float dist = dot(pl.xyz, center) + pl.w;
		float r = dot(abs(pl.xyz), e);
		if (dist + r < 0.0) {
			visible = false;
			break;
		}
	}

	if (!visible) {
		return;   /* culled records are skipped entirely (true compaction) */
	}

	/* Claim a dense slot inside this run's partition via the per-run counter. */
	uint slot;
	atomicFetchAndAdd(b_counter[run_index], 1u, slot);
	uint dst = (run_base + slot) * 4u;

	/* Guard against capacity (slot must stay inside the run partition, which is
	 * itself inside the visible buffer): the partition size == the run's record
	 * count, and survivors <= record count, so this never exceeds.  The capacity
	 * clamp is belt-and-suspenders for a corrupt run_base. */
	uint cap = uint(u_cull_params.y);
	if (run_base + slot >= cap) {
		return;
	}

	b_visible[dst + 0u] = b_scene[base + 0u];
	b_visible[dst + 1u] = b_scene[base + 1u];
	b_visible[dst + 2u] = b_scene[base + 2u];
	b_visible[dst + 3u] = b_scene[base + 3u];
}
