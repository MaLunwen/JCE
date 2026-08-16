/*
 * cs_meshlet_cull.sc -- Nanite-lite V2: per-MESHLET GPU cull for one hero
 * mesh instance (cluster granularity instead of instance granularity).
 *
 * One thread per meshlet.  Reads the mesh's static meshlet records
 * (b_meshlets, 6 vec4 each — built by jce_skinned_mesh_set_meshlets):
 *   [0] = {asfloat(index_offset), asfloat(index_count), 0, 0}
 *   [1] = {sphere cx, cy, cz, r}          (mesh-LOCAL space)
 *   [2] = {cone axis xyz, cone cutoff}    (meshopt convention)
 *   [3] = {own_error, parent_error, 0, 0} (V3 cluster-LOD DAG, object units)
 *   [4] = {own-group sphere cx,cy,cz,r}   (own-error test distance)
 *   [5] = {parent-group sphere cx,cy,cz,r}(parent-error test distance)
 * transforms the bounding sphere/cone by the entity's model matrix, then:
 *   - V3 DAG-cut-tests (screen-space error): on the cut iff own_error is
 *     small enough at this distance and parent_error is not (leaves carry
 *     {0, +BIG} so V1 assets / k<=0 degenerate to leaves-only)
 *   - frustum-tests the world sphere against the 6 camera planes
 *   - cone-backface-tests: cullable when
 *       dot(center - cam, axis) >= cutoff * |center - cam| + radius
 * and writes drawIndexedIndirect element m UNCONDITIONALLY:
 *   visible → {index_count, 1, index_offset, 0, 0}
 *   culled  → {0, 0, 0, 0, 0}   (zero-index draws are ~free)
 * No counters, no atomics, no reset pass — every element is (re)written each
 * dispatch, so per-entity indirect buffers stay self-consistent.  The caller
 * issues ONE bgfx_submit_indirect(view, prog, buf, 0, meshlet_count) with the
 * meshlet-grouped index buffer bound — surviving clusters draw, the rest are
 * zero-cost degenerates (universal across backends; no count-buffer needed).
 *
 * Uniforms (creation precedes program load — GL order contract):
 *   u_mlcull_params : (meshlet_count, max_scale, force_visible, 0)
 *   u_mlcull_model  : entity world matrix (column-major)
 *   u_cull_planes[6]: camera frustum planes (shared with the foliage cull)
 *   u_cull_campos   : camera world position (shared)
 */

#include <bgfx_compute.sh>

BUFFER_RO(b_meshlets, vec4,  0);
BUFFER_WO(b_indirect, uvec4, 1);

uniform vec4 u_mlcull_params;
uniform mat4 u_mlcull_model;
uniform vec4 u_cull_planes[6];
uniform vec4 u_cull_campos;

/* Hi-Z occlusion inputs (V3.1 — mirrors cs_foliage_cull.sc).  s_hiz = the
 * MAX-depth pyramid built from the depth prepass; u_cull_viewproj = the
 * PREV-frame view-proj that produced it; u_hiz_params = {w, h, num_mips, 0}.
 * Gated by the hiz bit in u_mlcull_params.w (see below). */
SAMPLER2D(s_hiz, 2);
uniform mat4 u_cull_viewproj;
uniform vec4 u_hiz_params;

NUM_THREADS(64, 1, 1)
void main()
{
	uint m     = gl_GlobalInvocationID.x;
	uint count = uint(u_mlcull_params.x);
	if (m >= count) {
		return;
	}

	vec4 d0 = b_meshlets[m * 6u + 0u];
	vec4 d1 = b_meshlets[m * 6u + 1u];
	vec4 d2 = b_meshlets[m * 6u + 2u];
	vec4 d3 = b_meshlets[m * 6u + 3u];   /* {own_error, parent_error, 0, 0} */
	vec4 d4 = b_meshlets[m * 6u + 4u];   /* own-group sphere    (LOCAL xyzr) */
	vec4 d5 = b_meshlets[m * 6u + 5u];   /* parent-group sphere (LOCAL xyzr) */
	uint index_offset = floatBitsToUint(d0.x);
	uint index_count  = floatBitsToUint(d0.y);

	/* World-space bounding sphere: centre through the model matrix; radius
	 * scaled by the caller-provided max axis scale (conservative). */
	float mscale = max(u_mlcull_params.y, 1.0e-6);
	vec3  center = mul(u_mlcull_model, vec4(d1.xyz, 1.0)).xyz;
	float radius = d1.w * mscale;

	/* V3 cluster-LOD DAG cut (u_cull_campos.w = k, world error allowance per
	 * unit distance = tol_px * 2*tan(fov/2)/viewport_h).  A cluster is on the
	 * cut iff its OWN error is acceptable and its PARENT's is not — each
	 * error measured at its GROUP sphere, which a child's parent-test SHARES
	 * with its parent's own-test, making the two decisions exactly
	 * complementary (no holes, no double-draw) regardless of where the
	 * threshold lands.  Errors are monotone up every path (cook enforces),
	 * and leaves carry {0, +BIG}, so k <= 0 (or a V1 asset without the
	 * errors lane) degenerates to leaves-only == plain V2 cull. */
	float k = u_cull_campos.w;
	vec3  oc = mul(u_mlcull_model, vec4(d4.xyz, 1.0)).xyz;
	float od = max(length(oc - u_cull_campos.xyz) - d4.w * mscale, 1.0e-3);
	vec3  pc = mul(u_mlcull_model, vec4(d5.xyz, 1.0)).xyz;
	float pd = max(length(pc - u_cull_campos.xyz) - d5.w * mscale, 1.0e-3);
	bool visible = (d3.x * mscale <= k * od) &&
	               (d3.y * mscale >  k * pd);

	vec3  vdir  = center - u_cull_campos.xyz;
	float vdist = length(vdir);

	/* u_mlcull_params.w packs {diag, hiz}: w = diag + 8*hiz_ready.  Diag
	 * bisect (JCE_MLCULL_DIAG): 1 = frustum only, 2 = cone only, 0 = all
	 * tests.  Bisect lanes replace the DAG cut with the LEAF set
	 * (own_error == 0) AND skip Hi-Z: the isolated test is then the ONLY
	 * source of visibility differences — neither the distance-dependent
	 * cut nor the prev-frame pyramid can confound an A/B. */
	/* V4 shadow mode (+16): a SHADOW-cascade dispatch keeps the frustum
	 * test but drops the cone (a cluster backfacing the LIGHT still casts
	 * shadow) and Hi-Z (no light-space pyramid).  w = diag + 8*hiz + 16*shadow. */
	float pw     = u_mlcull_params.w;
	bool  shadow = pw >= 15.5; if (shadow) pw -= 16.0;
	bool  hiz    = pw >= 7.5;
	float diag   = hiz ? pw - 8.0 : pw;
	if (diag > 0.5) {
		visible = (d3.x <= 0.0);
	}

	/* Frustum: sphere vs 6 planes. */
	for (int p = 0; p < 6; ++p) {
		vec4 pl = u_cull_planes[p];
		if (diag < 1.5 && dot(pl.xyz, center) + pl.w < -radius) {
			visible = false;
		}
	}

	/* Cone backface (meshopt convention).  Axis rotates by the model matrix
	 * (w=0); cutoff is scale-invariant.  cutoff >= 1 encodes "never cull"
	 * (flat/degenerate clusters). */
	if (visible && !shadow && (diag < 0.5 || diag > 1.5) && d2.w < 0.99) {
		vec3  axis = normalize(mul(u_mlcull_model, vec4(d2.xyz, 0.0)).xyz);
		if (vdist > 1.0e-6 &&
		    dot(vdir, axis) >= d2.w * vdist + radius) {
			visible = false;
		}
	}

	/* Hi-Z occlusion (V3.1; gated by the hiz bit, skipped in diag lanes).
	 * Project the cluster's world-sphere AABB corners with the PREV-frame
	 * view-proj, take the footprint's mip, and cull when the nearest corner
	 * depth is beyond the pyramid's MAX depth.  Conservative: keeps VISIBLE
	 * on near-plane straddle / off-screen / oversized footprints.  INLINED
	 * — see cs_cull_compact.sc for the GLSL-crash rationale. */
	if (visible && hiz && !shadow && diag < 0.5) {
		vec2  mn_uv = vec2(1.0e9, 1.0e9);
		vec2  mx_uv = vec2(-1.0e9, -1.0e9);
		float mn_z  = 1.0e9;
		bool  keep_visible = false;
		for (int cc = 0; cc < 8; ++cc) {
			vec3 corner = center + vec3(
				(cc & 1) != 0 ?  radius : -radius,
				(cc & 2) != 0 ?  radius : -radius,
				(cc & 4) != 0 ?  radius : -radius);
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

	/* DIAG lane (JCE_FCULL_FORCE): keep every LEAF (own_error == 0) — the
	 * full-resolution geometry exactly once (A/B + backend triage; keeping
	 * ALL DAG levels would double-draw every surface). */
	if (u_mlcull_params.z > 0.5) {
		visible = (d3.x <= 0.0);
	}

	drawIndexedIndirect(
		b_indirect,
		m,                              /* element index (one per meshlet) */
		visible ? index_count : 0u,     /* zero-index draw = culled        */
		visible ? 1u : 0u,
		index_offset,                   /* startIndex into the meshlet IB  */
		0u,                             /* startVertex                     */
		0u                              /* startInstance                   */
		);
}
