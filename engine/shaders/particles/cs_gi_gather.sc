/*
 * cs_gi_gather.sc -- GI L1 (Lumen-lite level 1): dynamic irradiance probe
 * grid, fed from the PREVIOUS frame's lit scene color + depth (the same
 * one-frame-late products the Hi-Z cull consumes — no new render passes).
 *
 * One thread per probe.  The grid follows the camera but probes are WORLD-
 * anchored via toroidal addressing: atlas slot (sx,sy,sz) always holds the
 * unique world cell in [origin, origin+dims) congruent to the slot modulo
 * dims, so camera motion never smears accumulated light — a slot whose world
 * cell CHANGED simply resets (meta texel remembers the cell it was built
 * for).
 *
 * Per probe: project into the prev frame; gather K jittered screen samples
 * around the projection; each sample reconstructs a world position from
 * depth and contributes its (de-gamma'd) radiance as a directional SH9
 * lobe — nearby surface hits attenuate by distance (single-bounce term),
 * far/sky samples contribute as sky radiance.  Coefficients use the SAME
 * convention as jce_lightmapper_bake_sh9 (basis * PI / N, cosine-kernel
 * folded, shader dots the raw basis), so fs_pbr's existing sh9_irradiance /
 * u_sh9 path consumes them unchanged.  Temporal hysteresis smooths noise.
 *
 * Atlas (rgba32f): x = sx + sz*gx, y = sy*11 + row; rows 0..8 = SH9 rgb,
 * row 9 = meta {blend_weight, cell.xyz}, row 10 = {sky_visibility (L2),
 * mean_depth, mean_depth^2 (L5 Chebyshev leak gate)} — row 10 is
 * GPU-internal (the CPU reads rows 0..9 only).
 * The CPU reads the
 * atlas back (small; rolling latency) and feeds u_sh9 through the existing
 * lit-submit funnel — fs_pbr needs no new sampler (all 16 stages are full).
 *
 * Uniforms (created before the program — GL order contract):
 *   u_gig_grid   : {origin cell x,y,z (integer-valued), spacing}
 *   u_gig_dims   : {gx, gy, gz, probe_count}
 *   u_gig_cam    : {cam.xyz, hysteresis alpha}
 *   u_gig_screen : {vp_w, vp_h, K samples, gather radius px}
 *   u_gig_misc   : {frame, sky_boost, gather_dist, gl_ndc}
 *   u_gig_vp     : prev frame view-proj
 *   u_gig_ivp    : inverse of prev frame view-proj
 */

#include <bgfx_compute.sh>

SAMPLER2D(s_gigColor, 0);
SAMPLER2D(s_gigDepth, 1);
IMAGE2D_RW(s_gigAtlas, rgba32f, 2);
SAMPLER2D(s_gigCsm,   3);   /* GI L3: one CSM cascade (unbound when off) */

uniform vec4 u_gig_grid;
uniform vec4 u_gig_dims;
uniform vec4 u_gig_cam;
uniform vec4 u_gig_screen;
uniform vec4 u_gig_misc;
uniform vec4 u_gig_sky;    /* {ambient rgb, floor amount; <=0 = off} (GI L2) */
uniform vec4 u_gig_sun;    /* {sun dir (travel), bounce amount; <=0 off} (L3) */
uniform vec4 u_gig_suncol; /* {sun rgb, 0}                                    */
uniform mat4 u_gig_csm;    /* light-space VP of the sampled cascade           */
uniform mat4 u_gig_vp;
uniform mat4 u_gig_ivp;

/* SH9 basis — MUST mirror jce_lightmapper.c sh9_eval / fs_pbr sh9_irradiance. */
void sh9_basis(vec3 d, out float b[9])
{
	float x = d.x, y = d.y, z = d.z;
	b[0] = 0.282095;
	b[1] = 0.488603 * y;
	b[2] = 0.488603 * z;
	b[3] = 0.488603 * x;
	b[4] = 1.092548 * x * y;
	b[5] = 1.092548 * y * z;
	b[6] = 0.315392 * (3.0 * z * z - 1.0);
	b[7] = 1.092548 * x * z;
	b[8] = 0.546274 * (x * x - y * y);
}

float gig_hash(uint v)
{
	v ^= v >> 16u; v *= 2246822519u; v ^= v >> 13u;
	v *= 3266489917u; v ^= v >> 16u;
	return float(v & 0x00ffffffu) / 16777216.0;
}

NUM_THREADS(64, 1, 1)
void main()
{
	uint p = gl_GlobalInvocationID.x;
	uint count = uint(u_gig_dims.w);
	if (p >= count) {
		return;
	}
	uint gx = uint(u_gig_dims.x);
	uint gy = uint(u_gig_dims.y);
	uint gz = uint(u_gig_dims.z);
	uint sx = p % gx;
	uint sz = (p / gx) % gz;
	uint sy = p / (gx * gz);

	/* World cell for this toroidal slot: the unique cell in
	 * [origin, origin+dims) with cell mod dims == slot. */
	vec3 origin = u_gig_grid.xyz;
	vec3 dims   = vec3(float(gx), float(gy), float(gz));
	vec3 slot   = vec3(float(sx), float(sy), float(sz));
	vec3 rel    = mod(mod(slot - origin, dims) + dims, dims);
	vec3 cell   = origin + rel;
	float spacing = u_gig_grid.w;
	vec3 ppos = (cell + vec3_splat(0.5)) * spacing;

	int ax = int(sx + sz * gx);
	int ay = int(sy) * 11;   /* 11 rows: 9 SH + meta + sky-vis (L2) */

	/* Stale slot (world cell changed since it was written) -> restart. */
	vec4 meta = imageLoad(s_gigAtlas, ivec2(ax, ay + 9));
	bool fresh = (abs(meta.y - cell.x) < 0.5 && abs(meta.z - cell.y) < 0.5 &&
	              abs(meta.w - cell.z) < 0.5);
	float old_w = fresh ? meta.x : 0.0;
	/* GI L2: temporally-smoothed sky openness (row 10). */
	float old_vis = fresh ? imageLoad(s_gigAtlas, ivec2(ax, ay + 10)).x : 0.0;

	/* Gather from the prev frame around the probe's projection. */
	float acc[9 * 3];
	for (int i = 0; i < 27; ++i) acc[i] = 0.0;
	float hits = 0.0;
	float sky_hits = 0.0;
	/* GI L4: nearest geometry hit per +/-axis (register-only leak gate
	 * for the neighbour multi-bounce below — reject a neighbour whose
	 * direction showed a surface between this probe and it). */
	float axis_hit[6];
	for (int ah = 0; ah < 6; ++ah) axis_hit[ah] = 1.0e9;
	/* GI L5: DDGI depth moments E[d], E[d^2] over geometry hits, EMA'd
	 * into row 10 .yz — the neighbour bounce below reads them for a
	 * temporally-stable Chebyshev visibility gate (soft, unlike the
	 * per-frame axis_hit hard reject). */
	float dist_sum = 0.0, dist_sqsum = 0.0, dhits = 0.0;

	vec4 clip = mul(u_gig_vp, vec4(ppos, 1.0));
	if (clip.w > 1.0e-4) {
		vec2 base_uv = (clip.xy / clip.w) * 0.5 + 0.5;
#if !BGFX_SHADER_LANGUAGE_GLSL
		/* Top-left texture origin: NDC y grows UP, UV v grows DOWN.  Pairs with
		 * the uv->NDC step in the loop below -- change one, change both.  They
		 * round-tripped, so the geometry was self-consistent in mirror space
		 * and the damage was that s_gigDepth / s_gigColor were sampled from the
		 * mirrored half of the frame. */
		base_uv.y = 1.0 - base_uv.y;
#endif
		float K = max(u_gig_screen.z, 1.0);
		vec2 rpx = vec2(u_gig_screen.w / max(u_gig_screen.x, 1.0),
		                u_gig_screen.w / max(u_gig_screen.y, 1.0));
		float gather_d = u_gig_misc.z;
		for (int k = 0; k < 16; ++k) {
			if (float(k) >= K) break;
			uint seed = p * 97u + uint(k) * 31u + uint(u_gig_misc.x) * 7u;
			float a = gig_hash(seed) * 6.2831853;
			float r = sqrt(gig_hash(seed ^ 0x9e3779b9u));
			vec2 uv = base_uv + vec2(cos(a), sin(a)) * r * rpx;
			if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
				continue;
			float d = texture2DLod(s_gigDepth, uv, 0).x;
			float ndc_z = (u_gig_misc.w > 0.5) ? d * 2.0 - 1.0 : d;
			vec2 gndc = uv * 2.0 - 1.0;
#if !BGFX_SHADER_LANGUAGE_GLSL
			gndc.y = -gndc.y;   /* exact inverse of the base_uv flip above */
#endif
			vec4 wp4 = mul(u_gig_ivp, vec4(gndc, ndc_z, 1.0));
			vec3 radiance = texture2DLod(s_gigColor, uv, 0).rgb;
			/* Scene color is gamma-encoded by the manual-gamma pipeline;
			 * linearise for energy accumulation. */
			radiance = radiance * radiance;   /* cheap pow(2.2)~pow(2) */

			vec3 dir;
			float atten;
			if (d >= 0.9999) {
				sky_hits += 1.0;
				/* Sky: direction toward the far point, full weight. */
				if (abs(wp4.w) < 1.0e-6) continue;
				vec3 wp = wp4.xyz / wp4.w;
				vec3 dv = wp - ppos;
				float len = length(dv);
				if (len < 1.0e-4) continue;
				dir = dv / len;
				atten = u_gig_misc.y;             /* sky boost */
			} else {
				if (abs(wp4.w) < 1.0e-6) continue;
				vec3 wp = wp4.xyz / wp4.w;
				vec3 dv = wp - ppos;
				float len = length(dv);
				if (len < 1.0e-3 || len > gather_d) continue;
				dir = dv / len;
				atten = 1.0 - len / gather_d;     /* near-surface bounce */
				vec3 ad = abs(dir);
				int aidx = (ad.x >= ad.y && ad.x >= ad.z) ? 0
				         : ((ad.y >= ad.z) ? 1 : 2);
				float acomp = (aidx == 0) ? dir.x : ((aidx == 1) ? dir.y : dir.z);
				int abin = aidx * 2 + ((acomp < 0.0) ? 1 : 0);
				axis_hit[abin] = min(axis_hit[abin], len);
				dist_sum += len; dist_sqsum += len * len; dhits += 1.0;
			}
			float basis[9];
			sh9_basis(dir, basis);
			/* Lightmapper convention: PI / N projection weight. */
			float w = 3.14159265 * atten / K;
			for (int c = 0; c < 9; ++c) {
				acc[c * 3 + 0] += radiance.r * basis[c] * w;
				acc[c * 3 + 1] += radiance.g * basis[c] * w;
				acc[c * 3 + 2] += radiance.b * basis[c] * w;
			}
			hits += 1.0;
		}
	}

	/* GI L4: DDGI-lite probe-to-probe multi-bounce.  Diffuse each valid
	 * neighbour's converged SH one cell per frame -> spatial infinite
	 * bounce that FILLS off-screen/occluded probes (temporal feedback
	 * alone leaves them dark).  Normalised weighted mean (gain in
	 * u_gig_suncol.w, 0=off): fixed point SH = gather/(1-gain), stable
	 * for gain<1 independent of valid-neighbour count.  axis_hit gates
	 * a neighbour whose direction showed geometry within 0.75 cell. */
	vec3  bounce_rgb[9];
	for (int bj = 0; bj < 9; ++bj) bounce_rgb[bj] = vec3_splat(0.0);
	float bw_sum = 0.0;
	float bgain = u_gig_suncol.w;
	if (bgain > 0.0) {
		for (int nb = 0; nb < 6; ++nb) {
			if (axis_hit[nb] < spacing * 0.75) continue;   /* wall between */
			int axn = nb >> 1; float sgn = ((nb & 1) == 0) ? 1.0 : -1.0;
			vec3 ncell = cell;
			int nsx = int(sx), nsy = int(sy), nsz = int(sz);
			if (axn == 0) { ncell.x += sgn; nsx = int(mod(float(nsx) + sgn + float(gx), float(gx))); }
			else if (axn == 1) { ncell.y += sgn; nsy = int(mod(float(nsy) + sgn + float(gy), float(gy))); }
			else { ncell.z += sgn; nsz = int(mod(float(nsz) + sgn + float(gz), float(gz))); }
			int nax = nsx + nsz * int(gx);
			int nay = nsy * 11;
			vec4 nmeta = imageLoad(s_gigAtlas, ivec2(nax, nay + 9));
			if (nmeta.x <= 0.0) continue;
			if (abs(nmeta.y - ncell.x) > 0.5 ||
			    abs(nmeta.z - ncell.y) > 0.5 ||
			    abs(nmeta.w - ncell.z) > 0.5) continue;   /* torus seam */
			float nw = nmeta.x;
			/* L5 Chebyshev: attenuate by the NEIGHBOUR's occlusion moments
			 * (isotropic mean geometry depth).  If its mean occluder is
			 * closer than one cell, light to us is likely blocked; the
			 * variance term softens the transition (DDGI's real leak fix).
			 * mean==0 = never saw geometry = open space = fully visible. */
			vec2 nm = imageLoad(s_gigAtlas, ivec2(nax, nay + 10)).yz;
			if (nm.x > 0.0) {
				float nvar = max(nm.y - nm.x * nm.x, 1.0e-4);
				float ndd  = spacing - nm.x;
				if (ndd > 0.0) nw *= nvar / (nvar + ndd * ndd);
			}
			if (nw <= 1.0e-4) continue;
			for (int nc = 0; nc < 9; ++nc)
				bounce_rgb[nc] += imageLoad(s_gigAtlas, ivec2(nax, nay + nc)).xyz * nw;
			bw_sum += nw;
		}
		if (bw_sum > 0.0) {
			float bk = bgain / bw_sum;
			for (int bc = 0; bc < 9; ++bc) bounce_rgb[bc] *= bk;
		}
	}
	float has_bounce = (bw_sum > 0.0) ? 1.0 : 0.0;

	/* Temporal blend: probes with no view this frame keep their history,
	 * UNLESS a neighbour bounce can fill them (off-screen support). */
	float alpha = (hits > 0.5) ? u_gig_cam.w : 0.0;
	if (old_w <= 0.0 && hits > 0.5) alpha = 1.0;
	if (hits <= 0.5 && has_bounce > 0.5) alpha = max(alpha, u_gig_cam.w);
	float w_target = min(hits / max(u_gig_screen.z, 1.0), 1.0);
	w_target = max(w_target, has_bounce * min(bw_sum, 1.0) * 0.5);
	float new_w = mix(old_w, w_target, alpha);

	/* GI L2: sky openness = fraction of this probe's screen samples that
	 * were SKY, temporally smoothed.  The floor term below scales the scene
	 * ambient by it — an open-field probe gets the full ambient floor, a
	 * probe under cover (its samples all hit geometry) gets none, so the
	 * indoor/outdoor transition follows the probes instead of one global
	 * ambient.  Same EMA as the SH so the two stay in phase. */
	float new_vis = mix(old_vis,
	                    (hits > 0.5) ? sky_hits / max(hits, 1.0) : old_vis,
	                    alpha);

	/* Sky ambient floor folded into the blend TARGET (not the store) so the
	 * EMA never double-accumulates it.  c0-only: a uniform ambient lobe;
	 * 3.5449 = 1/Y0 so E(n) evaluates back to ambient*amount*vis exactly. */
	float floor_amt = u_gig_sky.w;
	vec3  sky_c0 = (floor_amt > 0.0)
	    ? u_gig_sky.xyz * u_gig_sky.xyz * floor_amt * new_vis * 3.5449
	    : vec3_splat(0.0);   /* xyz*xyz: ambient is gamma-ish, linearise */

	/* GI L3: sun ground-bounce.  Sample the CSM cascade at the PROBE — a
	 * sunlit probe stands over sunlit ground, which reflects the sun back
	 * up; inject that as a lobe arriving FROM BELOW (projection direction
	 * (0,-1,0), matching the gather's toward-the-emitter convention).
	 * Shadowed probes get nothing, so the warm fill follows real sun
	 * occlusion (the wall test: bright side warm, shadow side neutral).
	 * Folded into the blend TARGET like the sky floor. */
	float sun_sh[9];
	for (int c2 = 0; c2 < 9; ++c2) sun_sh[c2] = 0.0;
	vec3 sun_rgb = vec3_splat(0.0);
	if (u_gig_sun.w > 0.0) {
		vec4 sc = mul(u_gig_csm, vec4(ppos, 1.0));
		if (sc.w > 1.0e-4) {
			vec3 sndc = sc.xyz / sc.w;
			vec2 suv  = sndc.xy * 0.5 + 0.5;
#if !BGFX_SHADER_LANGUAGE_GLSL
			suv.y = 1.0 - suv.y;   /* matches csm_shadow.sh's cascade lookup */
#endif
			float sz  = (u_gig_misc.w > 0.5) ? sndc.z * 0.5 + 0.5 : sndc.z;
			if (suv.x > 0.0 && suv.x < 1.0 && suv.y > 0.0 && suv.y < 1.0 &&
			    sz > 0.0 && sz < 1.0) {
				float smd = texture2DLod(s_gigCsm, suv, 0).x;
				if (sz <= smd + 0.01) {         /* probe in direct sun */
					float cosg = max(-u_gig_sun.y, 0.0); /* ground N.L  */
					/* 0.3 = assumed ground albedo. */
					sun_rgb = u_gig_suncol.xyz *
					          (0.3 * cosg * u_gig_sun.w);
					/* Clamped-cosine (Lambert) hemisphere lobe about (0,-1,0),
					 * NOT a raw directional delta: a delta rings NEGATIVE on the
					 * opposite hemisphere (the l=2 zonal at full PI weight nuked
					 * up-facing GI to ~0).  Analytic ZH band weights A0=PI,
					 * A1=2PI/3, A2=PI/4 give a proper one-sided ground bounce —
					 * bright below, ~0 above, never negative. */
					float sb[9];
					sh9_basis(vec3(0.0, -1.0, 0.0), sb);
					float zh1 = 2.0943951, zh2 = 0.7853982;
					sun_sh[0] = sb[0] * 3.14159265;
					sun_sh[1] = sb[1] * zh1; sun_sh[2] = sb[2] * zh1;
					sun_sh[3] = sb[3] * zh1;
					sun_sh[4] = sb[4] * zh2; sun_sh[5] = sb[5] * zh2;
					sun_sh[6] = sb[6] * zh2; sun_sh[7] = sb[7] * zh2;
					sun_sh[8] = sb[8] * zh2;
				}
			}
		}
	}

	for (int c = 0; c < 9; ++c) {
		vec4 prev = imageLoad(s_gigAtlas, ivec2(ax, ay + c));
		vec3 shp = fresh ? prev.xyz : vec3_splat(0.0);
		vec3 shn = vec3(acc[c * 3 + 0], acc[c * 3 + 1], acc[c * 3 + 2]);
		if (c == 0) shn += sky_c0;
		shn += sun_rgb * sun_sh[c];
		shn += bounce_rgb[c];   /* GI L4 multi-bounce */
		imageStore(s_gigAtlas, ivec2(ax, ay + c),
		           vec4(mix(shp, shn, alpha), 0.0));
	}
	imageStore(s_gigAtlas, ivec2(ax, ay + 9), vec4(new_w, cell));
	/* GI L5: EMA the depth moments alongside sky-vis; only step them
	 * when geometry was actually seen (a sky-only probe keeps its
	 * moments so it never reports itself as a near occluder). */
	float mean_d  = (dhits > 0.5) ? dist_sum   / dhits : 0.0;
	float mean_d2 = (dhits > 0.5) ? dist_sqsum / dhits : 0.0;
	vec2  old_m   = fresh ? imageLoad(s_gigAtlas, ivec2(ax, ay + 10)).yz
	                      : vec2(mean_d, mean_d2);
	float amd     = (dhits > 0.5) ? alpha : 0.0;
	float new_md  = mix(old_m.x, mean_d,  amd);
	float new_md2 = mix(old_m.y, mean_d2, amd);
	imageStore(s_gigAtlas, ivec2(ax, ay + 10), vec4(new_vis, new_md, new_md2, 0.0));
}
