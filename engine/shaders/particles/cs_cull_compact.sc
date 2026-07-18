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

/* Hi-Z occlusion inputs (large-world #5). s_hiz is the MAX-depth pyramid built
 * from the depth prepass; u_cull_viewproj is the PREV-frame view-proj that
 * produced it (the cull runs before this frame's prepass); u_hiz_params =
 * {level0_w, level0_h, num_mips, 0}. Gated by u_cull_params.w. */
SAMPLER2D(s_hiz, 3);
uniform vec4 u_cull_planes[6];
uniform vec4 u_cull_params;
uniform mat4 u_cull_viewproj;
uniform vec4 u_hiz_params;

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

	/* Hi-Z occlusion (large-world #5, opt-in via u_cull_params.w > 0.5). INLINED
	 * — NOT a helper taking s_hiz as an argument: that GLSL form was rejected by
	 * the GL driver and crashed editor startup (revert d7d785c4). An instance is
	 * OCCLUDED iff the nearest point of its screen footprint is behind the
	 * farthest occluder (max Hi-Z) over that footprint. Conservative (keeps
	 * VISIBLE) on near-straddle / off-screen / oversized-footprint. Depth is
	 * window-space [0,1] on every backend (the pyramid stores the depth texture);
	 * only NDC-z reconstruction differs by backend (mirrors fs_volfog.sc). */
	if (visible && u_cull_params.w > 0.5) {
		vec2  mn_uv = vec2(1.0e9, 1.0e9);
		vec2  mx_uv = vec2(-1.0e9, -1.0e9);
		float mn_z  = 1.0e9;
		bool  keep_visible = false;   /* conservative bail → never occlude */
		for (int cc = 0; cc < 8; ++cc) {
			vec3 corner = center + vec3(
				(cc & 1) != 0 ?  e.x : -e.x,
				(cc & 2) != 0 ?  e.y : -e.y,
				(cc & 4) != 0 ?  e.z : -e.z);
			vec4 clip = mul(u_cull_viewproj, vec4(corner, 1.0));
			if (clip.w <= 1.0e-6) { keep_visible = true; break; }  /* near straddle */
			vec3 ndc = clip.xyz / clip.w;
#if BGFX_SHADER_LANGUAGE_GLSL
			float win_z = ndc.z * 0.5 + 0.5;   /* GL NDC z [-1,1] → window [0,1] */
#else
			float win_z = ndc.z;               /* D3D/Vulkan/Metal NDC z is [0,1] */
#endif
			if (win_z < 0.0) { keep_visible = true; break; }      /* in front of near */
			vec2 uv = ndc.xy * 0.5 + 0.5;
			mn_uv = min(mn_uv, uv);
			mx_uv = max(mx_uv, uv);
			mn_z  = min(mn_z, win_z);
		}
		if (!keep_visible &&
		    !(mx_uv.x < 0.0 || mn_uv.x > 1.0 || mx_uv.y < 0.0 || mn_uv.y > 1.0)) {
			float span = max((mx_uv.x - mn_uv.x) * u_hiz_params.x,
			                 (mx_uv.y - mn_uv.y) * u_hiz_params.y);
			float mip  = ceil(log2(max(span, 1.0)));
			if (mip < u_hiz_params.z) {
				float occ = texture2DLod(s_hiz, vec2(mn_uv.x, mn_uv.y), mip).r;
				occ = max(occ, texture2DLod(s_hiz, vec2(mx_uv.x, mn_uv.y), mip).r);
				occ = max(occ, texture2DLod(s_hiz, vec2(mn_uv.x, mx_uv.y), mip).r);
				occ = max(occ, texture2DLod(s_hiz, vec2(mx_uv.x, mx_uv.y), mip).r);
				if (mn_z > occ) visible = false;   /* nearest point behind occluder */
			}
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
