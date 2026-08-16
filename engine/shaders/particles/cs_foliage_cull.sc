/*
 * cs_foliage_cull.sc -- GPU frustum + Hi-Z + distance-band cull with stream
 * compaction for the S1 foliage persistent instance buffer (千万 S2/S4/S5).
 *
 * One thread per foliage instance.  Reads the instance's world matrix DIRECTLY
 * from S1's persistent roots VB (b_roots, 4 vec4 = mat4 per instance), derives
 * its world-space AABB on the GPU from the shared mesh's LOCAL AABB (uniform),
 * tests it against the 6 camera frustum planes (+ optional Hi-Z occlusion),
 * classifies the survivor into a DISTANCE BAND (its LOD level), and atomically
 * appends its matrix into that band's PARTITION of b_visible.  One dispatch
 * culls + LOD-classifies every instance; the caller then issues ONE
 * drawIndexedIndirect per band at that band's reduced LOD index buffer with
 * startInstance = band * cap_band (partition base).
 *
 * 千万 S5 tiling: several dispatches (one per resident visible TILE) may append
 * into the SAME partitions/counters within a frame — the atomics make that safe;
 * the caller resets the counters once, dispatches per tile, then builds the
 * indirect args once.
 *
 * 千万 ② crossfade: an instance within `fade_w` of its band's FAR edge is
 * written TWICE — to its own band with coverage f (c3.w = +f) and to the next
 * band with the complementary coverage (c3.w = -f); the fragment side dithers
 * (IGN) so exactly one copy survives per pixel and the LOD switch is a smooth
 * dissolve instead of a pop.  Beyond the LAST band's far edge (u_cull_campos.w
 * = far draw distance) instances fade OUT the same way (no complement).
 * c3.w == 1.0 (the default written for solid instances) keeps the fragment
 * fast path — a buffer of plain world matrices renders byte-identically.
 *
 * Buffer layout (must mirror jce_sr_environment.c wiring):
 *   b_roots   : RO, 4 vec4 per instance — world matrix columns col0..col3
 *   b_visible : RW, 4 vec4 per slot — band-partitioned compacted survivors
 *               (band b owns slots [b*cap_band, (b+1)*cap_band))
 *   b_counter : RW, uint per band — survivor count (atomicAdd target)
 *
 * Uniforms:
 *   u_cull_planes[6] : normalised frustum planes (xyz = normal, w = d)
 *   u_fcull_params   : (instance_count, unused, force_visible, hiz_gate)
 *   u_fcull_aabb[2]  : [0]=local AABB centre.xyz, [1]=half-extents.xyz
 *   u_fcull_lod      : (band_step, band_count, cap_band, fade_w)
 *                      band_count <= 1 → everything lands in band 0 and the
 *                      distance/band/fade logic is skipped (byte-identical to
 *                      the pre-S4 single-partition cull).
 *   u_cull_campos    : (cam.xyz, far_draw_distance; <=0 → unlimited)
 */

#include <bgfx_compute.sh>

BUFFER_RO(b_roots,   vec4, 0);
BUFFER_RW(b_visible, vec4, 1);
BUFFER_RW(b_counter, uint, 2);

uniform vec4 u_cull_planes[6];
uniform vec4 u_fcull_params;
uniform vec4 u_fcull_aabb[2];
uniform vec4 u_fcull_lod;      /* {band_step, band_count, cap_band, fade_w} */
uniform vec4 u_cull_campos;    /* {cam.x, cam.y, cam.z, far_dist}           */

/* Hi-Z occlusion inputs (mirrors cs_cull_compact.sc). s_hiz = MAX-depth pyramid
 * from the depth prepass; u_cull_viewproj = the PREV-frame view-proj that produced
 * it (the cull runs before this frame's prepass); u_hiz_params = {level0_w,
 * level0_h, num_mips, 0}. Gated by u_fcull_params.w > 0.5. */
SAMPLER2D(s_hiz, 3);
uniform mat4 u_cull_viewproj;
uniform vec4 u_hiz_params;

/* Threadgroup-aggregated compaction, PER BAND: each 64-thread group tallies its
 * survivors per band in shared memory, ONE thread claims each band's contiguous
 * range with a single global atomicAdd, and survivors write at group_base +
 * local offset within their band's partition.  Global atomics stay ~N/64. */
#define JCE_FCULL_MAX_BANDS 8
SHARED uint s_cnt[JCE_FCULL_MAX_BANDS];
SHARED uint s_base[JCE_FCULL_MAX_BANDS];

NUM_THREADS(64, 1, 1)
void main()
{
	uint id    = gl_GlobalInvocationID.x;
	uint lid   = gl_LocalInvocationID.x;
	uint count = uint(u_fcull_params.x);
	uint bands = uint(max(u_fcull_lod.y, 1.0));
	if (bands > uint(JCE_FCULL_MAX_BANDS)) bands = uint(JCE_FCULL_MAX_BANDS);
	uint capb  = uint(u_fcull_lod.z);

	if (lid < uint(JCE_FCULL_MAX_BANDS)) {
		s_cnt[lid] = 0u;
	}
	barrier();

	bool  visible   = false;
	uint  band      = 0u;
	float fade      = 1.0;   /* primary copy coverage */
	bool  dual      = false; /* also write the complement into band+1 */
	uint  slot_a    = 0u;    /* local slot, primary band  */
	uint  slot_b    = 0u;    /* local slot, next band     */
	vec4 c0 = vec4(0.0, 0.0, 0.0, 0.0);
	vec4 c1 = c0, c2 = c0, c3 = c0;

	if (id < count) {
		uint base = id * 4u;
		c0 = b_roots[base + 0u];   /* world matrix columns (column-major) */
		c1 = b_roots[base + 1u];
		c2 = b_roots[base + 2u];
		c3 = b_roots[base + 3u];

		vec3 lc = u_fcull_aabb[0].xyz;  /* local AABB centre       */
		vec3 le = u_fcull_aabb[1].xyz;  /* local AABB half-extents */

		/* world centre = M * vec4(lc, 1) ; M built from columns c0..c3. */
		vec3 center = c0.xyz * lc.x + c1.xyz * lc.y + c2.xyz * lc.z + c3.xyz;

		/* Transformed-AABB half-extent: e'_i = Σ_j |M_ij| * e_j. */
		vec3 e = abs(c0.xyz) * le.x + abs(c1.xyz) * le.y + abs(c2.xyz) * le.z;

		/* AABB vs frustum: cull when the box's farthest extent toward a plane
		 * normal is still behind that plane, for ANY of the 6 planes. */
		visible = true;
		for (int p = 0; p < 6; ++p) {
			vec4 pl = u_cull_planes[p];
			float dist = dot(pl.xyz, center) + pl.w;
			float r = dot(abs(pl.xyz), e);
			if (dist + r < 0.0) {
				visible = false;
				break;
			}
		}

		/* Distance band (千万 S4) + far cull + crossfade (千万 ②).  Only when
		 * banding is on (bands > 1); the single-band callers skip all of it. */
		if (visible && bands > 1u) {
			float step_m = max(u_fcull_lod.x, 1.0e-3);
			float fade_w = u_fcull_lod.w;
			float far_d  = u_cull_campos.w;
			float dband  = distance(center, u_cull_campos.xyz);

			if (far_d > 0.0 && dband >= far_d) {
				visible = false;   /* beyond the draw distance */
			} else {
				float fb = dband / step_m;
				band = uint(min(fb, float(bands - 1u)));
				/* distance to this band's FAR edge (last band's far edge is
				 * the draw distance itself → fade OUT, no complement). */
				float edge = (band + 1u < bands) ? float(band + 1u) * step_m
				           : ((far_d > 0.0) ? far_d : 3.4e38);
				float to_edge = edge - dband;
				if (fade_w > 0.0 && to_edge < fade_w) {
					fade = clamp(to_edge / fade_w, 0.0, 1.0);
					dual = (band + 1u < bands);
				}
			}
		}

		/* Hi-Z occlusion (gate u_fcull_params.w > 0.5). INLINED — see
		 * cs_cull_compact.sc for the GLSL-crash rationale.  Conservative:
		 * keeps VISIBLE on near-straddle / off-screen / oversized footprints. */
		if (visible && u_fcull_params.w > 0.5) {
			vec2  mn_uv = vec2(1.0e9, 1.0e9);
			vec2  mx_uv = vec2(-1.0e9, -1.0e9);
			float mn_z  = 1.0e9;
			bool  keep_visible = false;
			for (int cc = 0; cc < 8; ++cc) {
				vec3 corner = center + vec3(
					(cc & 1) != 0 ?  e.x : -e.x,
					(cc & 2) != 0 ?  e.y : -e.y,
					(cc & 4) != 0 ?  e.z : -e.z);
				vec4 clip = mul(u_cull_viewproj, vec4(corner, 1.0));
				if (clip.w <= 1.0e-6) { keep_visible = true; break; }
				vec3 ndc = clip.xyz / clip.w;
#if BGFX_SHADER_LANGUAGE_GLSL
				float win_z = ndc.z * 0.5 + 0.5;
#else
				float win_z = ndc.z;
#endif
				if (win_z < 0.0) { keep_visible = true; break; }
				vec2 uv = ndc.xy * 0.5 + 0.5;
#if !BGFX_SHADER_LANGUAGE_GLSL
				/* Top-left texture origin: the Hi-Z pyramid inherits the depth
				 * texture's row order, so NDC y (up) must be flipped into UV v
				 * (down) here, exactly as csm_shadow.sh does for the cascades. */
				uv.y = 1.0 - uv.y;
#endif
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
					if (mn_z > occ) visible = false;
				}
			}
		}

		/* DIAG lane (u_fcull_params.z, env JCE_FCULL_FORCE): backend triage.
		 *   z=1: every in-range instance survives (band 0, solid).
		 *   z=2: additionally synthesize the matrix (never reads b_roots). */
		if (u_fcull_params.z > 0.5) {
			visible = true; band = 0u; fade = 1.0; dual = false;
			if (u_fcull_params.z > 1.5) {
				c0 = vec4(1.0, 0.0, 0.0, 0.0);
				c1 = vec4(0.0, 1.0, 0.0, 0.0);
				c2 = vec4(0.0, 0.0, 1.0, 0.0);
				c3 = vec4(float(id % 64u) * 2.0 - 64.0, 0.5,
				          float(id / 64u) * 2.0, 1.0);
			}
		}

		/* Claim per-band local slots via the shared counters. */
		if (visible) {
			atomicFetchAndAdd(s_cnt[band], 1u, slot_a);
			if (dual) {
				atomicFetchAndAdd(s_cnt[band + 1u], 1u, slot_b);
			}
		}
	}
	barrier();

	/* One global atomic per band per group: reserve contiguous ranges. */
	if (lid == 0u) {
		for (uint b = 0u; b < bands; ++b) {
			if (s_cnt[b] > 0u) {
				atomicFetchAndAdd(b_counter[b], s_cnt[b], s_base[b]);
			} else {
				s_base[b] = 0u;
			}
		}
	}
	barrier();

	if (!visible) {
		return;
	}

	/* Primary copy: coverage +fade (1.0 = solid fast path). */
	uint slot = s_base[band] + slot_a;
	if (slot < capb) {
		uint dst = (band * capb + slot) * 4u;
		b_visible[dst + 0u] = c0;
		b_visible[dst + 1u] = c1;
		b_visible[dst + 2u] = c2;
		b_visible[dst + 3u] = vec4(c3.xyz, fade);
	}

	/* Complement copy into the next band: coverage encoded as -fade so the
	 * fragment dither keeps EXACTLY the pixels the primary discards. */
	if (dual) {
		uint slot2 = s_base[band + 1u] + slot_b;
		if (slot2 < capb) {
			uint dst2 = ((band + 1u) * capb + slot2) * 4u;
			b_visible[dst2 + 0u] = c0;
			b_visible[dst2 + 1u] = c1;
			b_visible[dst2 + 2u] = c2;
			b_visible[dst2 + 3u] = vec4(c3.xyz, -max(fade, 1.0e-4));
		}
	}
}
