$input v_texcoord0

#include <bgfx_shader.sh>

/*
 * fs_volfog.sc — Analytic + raymarched homogeneous volumetric fog.
 *
 * Produces RGBA: rgb = scattered in-scattering color, a = transmittance.
 * Caller composites:  final = bg.rgb * a + rgb;
 *
 * Inputs:
 *   s_depth : non-linear depth buffer
 *
 * Uniforms:
 *   u_volfog_p0 : (density, scattering, near, far)
 *   u_volfog_p1 : (step_count, height_falloff, height_origin, max_distance)
 *   u_volfog_color : (r, g, b, ambient_lift)
 */

SAMPLER2D(s_depth, 0);

/* ── Shadow-aware march ─────────────────────────────────────────────────
 *
 * Cascade depth maps at stages 1-4.  This is a standalone fullscreen pass with
 * its own sampler space, so unlike the PBR shader there is room here.
 *
 * The lookup below is deliberately NOT the one in pbr/csm_shadow.sh, and not
 * because sharing would be hard.  That function does normal-offset bias and PCF
 * -- both defined in terms of a SURFACE.  A sample inside a participating
 * medium has no surface and no normal, so normal-offset bias has no direction
 * to offset along and PCF softens an edge that has no geometric meaning here.
 * The volumetric question is the simpler one: is this point in the light.  One
 * tap, one constant bias.
 */
SAMPLER2D(s_volfog_csm0, 1);
SAMPLER2D(s_volfog_csm1, 2);
SAMPLER2D(s_volfog_csm2, 3);
SAMPLER2D(s_volfog_csm3, 4);
SAMPLER2D(s_volfog_cloud_shadow, 6);

uniform vec4 u_volfog_p0;
uniform vec4 u_volfog_p1;
uniform vec4 u_volfog_color;
uniform vec4 u_volfog_noise;  // strength, inverse cell metres, XZ wind in cells
uniform vec4 u_volfog_volume; // world center XYZ, radius in metres (0 = global fog)
uniform vec4 u_volfog_lobes;  // forward strength, backward strength, override, reserved
uniform vec4 u_volfog_cloud_map; // extent, center X/Z, cloud base Y in metres
uniform vec4 u_volfog_cloud_ray; // published bake sun XZ/Y, reserved

/* xyz = direction TOWARD the sun (world, unit), w = Henyey-Greenstein g */
uniform vec4 u_volfog_sun;
/* rgb = sun radiance, a = cascade count (0 = shadow lookup off) */
uniform vec4 u_volfog_sun_color;
uniform mat4 u_volfog_csm_vp[4];
uniform vec4 u_volfog_csm_splits;
/* x = 1/shadow_map_size */
uniform vec4 u_volfog_csm_params;

float linearize(float d, float n, float f)
{
	return n * f / (f - d * (f - n));
}

vec3 reconstruct_world(vec2 uv, float d)
{
#if BGFX_SHADER_LANGUAGE_GLSL
	float ndc_z = d * 2.0 - 1.0;   /* GL: depth-buffer NDC z is [-1,1] */
#else
	float ndc_z = d;               /* D3D/Vulkan/Metal/WebGPU: NDC z is [0,1] */
#endif
	vec2 ndc_xy = uv * 2.0 - 1.0;
#if !BGFX_SHADER_LANGUAGE_GLSL
	/* Top-left texture origin: v grows DOWN, NDC y grows UP.  Branching the z
	 * convention alone is not enough -- without this the reconstructed world
	 * position is the one belonging to the VERTICALLY MIRRORED pixel while the
	 * depth is the true pixel's, so the entire march ray and its height
	 * falloff are flipped.  Nothing downstream cancels it: cam_ws/far_ws feed
	 * WORLD-space consumers, not a screen-space round trip.
	 *
	 * The same flip volfog_tap() below already applies to its cascade lookup,
	 * and csm_shadow.sh to the surfaces -- so this file disagreed with itself. */
	ndc_xy.y = -ndc_xy.y;
#endif
	vec4 ndc = vec4(ndc_xy, ndc_z, 1.0);
	vec4 wp = mul(u_invViewProj, ndc);
	return wp.xyz / wp.w;
}

float volfog_shadow_depth(int cascade, vec2 uv)
{
	if      (cascade == 0) return texture2D(s_volfog_csm0, uv).r;
	else if (cascade == 1) return texture2D(s_volfog_csm1, uv).r;
	else if (cascade == 2) return texture2D(s_volfog_csm2, uv).r;
	return texture2D(s_volfog_csm3, uv).r;
}

vec4 volfog_shadow_clip(int cascade, vec3 world_pos)
{
	if      (cascade == 0) return mul(u_volfog_csm_vp[0], vec4(world_pos, 1.0));
	else if (cascade == 1) return mul(u_volfog_csm_vp[1], vec4(world_pos, 1.0));
	else if (cascade == 2) return mul(u_volfog_csm_vp[2], vec4(world_pos, 1.0));
	return mul(u_volfog_csm_vp[3], vec4(world_pos, 1.0));
}

float volfog_split(int cascade)
{
	if      (cascade == 0) return u_volfog_csm_splits.x;
	else if (cascade == 1) return u_volfog_csm_splits.y;
	else if (cascade == 2) return u_volfog_csm_splits.z;
	return u_volfog_csm_splits.w;
}

/* Is this point in sunlight?  1 = lit, 0 = shadowed.
 *
 * Returns 1 (LIT) for anything the cascades do not cover.  That direction is
 * chosen: an uncovered sample defaulting to SHADOWED would darken the fog
 * beyond the last cascade, drawing a soft dark wall across the distance that
 * moves with the camera and reads as deliberate depth cueing. */
/* One cascade tap.  Returns -1 when the sample falls outside this cascade's
 * light-space square, which is the caller's signal to try a wider one -- the
 * same sentinel csm_shadow.sh uses, for the same reason. */
float volfog_tap(int cascade, vec3 world_pos)
{
	vec4 clip = volfog_shadow_clip(cascade, world_pos);
	vec3 ndc  = clip.xyz / clip.w;
	vec2 suv  = ndc.xy * 0.5 + 0.5;
#if !BGFX_SHADER_LANGUAGE_GLSL
	suv.y = 1.0 - suv.y;
#endif
#if BGFX_SHADER_LANGUAGE_GLSL
	float z = ndc.z * 0.5 + 0.5;
#else
	float z = ndc.z;
#endif
	if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 ||
	    z < 0.0 || z > 1.0)
		return -1.0;

	/* Bias scales WITH the cascade, because the cascades do not share a texel
	 * size: cascade 3's shadow texels are an order of magnitude coarser in
	 * world terms than cascade 0's, and one constant bias for all four is
	 * either acne in the near cascade or peter-panning in the far one. The
	 * same mix(1,2,...) ramp csm_shadow.sh applies, for the same reason.
	 *
	 * There is no slope-scaled term because there is no slope: the receiver
	 * is a point in the air. */
	float lerp = clamp(float(cascade) * (1.0 / 3.0), 0.0, 1.0);
	float bias = max(u_volfog_csm_params.x, 1.0 / 4096.0) * 2.0
	           * mix(1.0, 2.0, lerp);

	/* ONE tap, and the four-tap version that replaced it was reverted.
	 *
	 * The reasoning for softening was sound: this returns a hard 0 or 1, while
	 * the lit SURFACE at the same world position is filtered with a 5x5 PCF on
	 * the tiers that enable volumetric fog -- so the shadow on the ground is
	 * soft and the shaft of light standing above it has a one-texel edge.
	 *
	 * Measured, it does not pay. A four-tap rotated quad cost +0.955 ms on a
	 * 5.55 ms frame -- SEVENTEEN PERCENT -- and moved the edge energy inside
	 * the fog's own contribution by 0.3%, which is the fourth significant
	 * figure (21.836 -> 21.838 at one pose, 21.894 -> 21.871 at another; the
	 * two disagree about the SIGN). Four times the texture fetches in a
	 * 32-step march is exactly where that cost comes from.
	 *
	 * The edge is real and this is still where it lives; a cheaper softening
	 * (a single tap into a pre-blurred cascade, or a temporal resolve) would
	 * be the way to buy it. Paying a sixth of the frame for a change nobody
	 * can measure is not. */
	return (z - bias > volfog_shadow_depth(cascade, suv)) ? 0.0 : 1.0;
}

float volfog_sunlight(vec3 world_pos, float view_z)
{
	float count = u_volfog_sun_color.a;
	if (count < 0.5) return 1.0;
	float shadow_far = volfog_split(3);
	float fade_start = shadow_far * 0.85;
	/* Past the existing fade, valid CSM visibility is already exactly lit.
	 * Skip only work whose result that fade discards; keep all march samples.
	 * Invalid counts/reach and a rounded-to-zero fade span keep the old path. */
	if (count >= 1.0 && count <= 4.0 && shadow_far > 0.0 &&
	    fade_start < shadow_far && view_z >= shadow_far)
		return 1.0;
	int last = int(count) - 1;

	int cascade = last;
	for (int c = 0; c < 4; c++)
	{
		if (float(c) >= count) break;
		if (view_z < volfog_split(c)) { cascade = c; break; }
	}

	float vis = volfog_tap(cascade, world_pos);

	/* FALLTHROUGH.  A depth-bucketed sample can land outside its own cascade's
	 * light-space square; stepping to wider cascades is what stops it being
	 * wrongly declared lit. csm_shadow.sh has carried this for the surfaces
	 * since it was written, and its own comment names the symptom -- "a
	 * triangular bright wedge, which reads as a lighting choice rather than a
	 * bug". The fog had no equivalent, so the wedge existed in the air and
	 * nowhere else. */
	if (vis < 0.0 && cascade < last) { cascade = cascade + 1; vis = volfog_tap(cascade, world_pos); }
	if (vis < 0.0 && cascade < last) { cascade = cascade + 1; vis = volfog_tap(cascade, world_pos); }
	if (vis < 0.0 && cascade < last) { cascade = cascade + 1; vis = volfog_tap(cascade, world_pos); }
	if (vis < 0.0) vis = 1.0;

	/* IN-CASCADE BLEND BAND.
	 *
	 * This is the knife-cut. The cascade was selected by a hard bucket on
	 * view-space Z, so the boundary is a PLANE whose normal is the camera
	 * forward axis, anchored at the eye -- it sweeps across the world when the
	 * camera merely turns on the spot. Crossing it changes both the map
	 * sampled and the effective bias, and `vis` is binary, so the change is
	 * full-amplitude: a hard-edged band of lit fog against shadowed fog, or
	 * the reverse, moving with the view.
	 *
	 * The lit SURFACE at the same world position crosses the identical plane
	 * smoothly, because csm_shadow.sh blends over the last 22% of the split
	 * span on exactly the tiers that enable volumetric fog. So the fog in
	 * front of a wall banded where the wall itself did not, which is what made
	 * it read as a fog artefact rather than a shadow one.
	 *
	 * Same blend fraction, same span, one extra tap and only inside the band. */
	float blend_frac = clamp(u_volfog_csm_params.y, 0.0, 0.35);
	if (blend_frac > 0.0 && cascade < last)
	{
		float split_start = (cascade == 0) ? 0.0 : volfog_split(cascade - 1);
		float split_end   = volfog_split(cascade);
		float span        = max(split_end - split_start, 0.001);
		float band        = max(span * blend_frac, 0.001);
		float w = smoothstep(split_end - band, split_end, view_z);
		if (w > 0.0)
		{
			float nxt = volfog_tap(cascade + 1, world_pos);
			if (nxt >= 0.0) vis = mix(vis, nxt, w);
		}
	}

	/* DISTANCE FADE.  Past the last cascade the tap returns 1 (lit), and a
	 * bare step from "shadowed" to "lit" at that distance is the same knife
	 * cut one cascade further out. Fade over the last 15% of the shadow reach,
	 * matching csm_shadow.sh's own fade, so the outer edge is a gradient
	 * instead of an edge. */
	if (shadow_far > 0.0)
		vis = mix(vis, 1.0, smoothstep(fade_start, shadow_far, view_z));

	return vis;
}

/* Henyey-Greenstein phase function.
 *
 * This is what makes fog brighten when you look toward the sun and stay dim
 * when you look away.  Without it the march is isotropic and every direction
 * scatters equally, which is the look of uniform haze -- the god rays are
 * geometrically present but carry no directional weight, so they read as a
 * washed-out grey wedge rather than as light. */
float volfog_phase(float cos_theta, float g)
{
	float g2 = g * g;
	float denom = 1.0 + g2 - 2.0 * g * cos_theta;
	/* denom -> 0 as g -> 1 in the forward direction; the max() keeps the
	 * forward lobe finite instead of returning inf and painting a NaN pixel. */
	return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(denom, 1e-4), 1.5));
}

/* One period-matched world-space lattice octave, with a fixed vertical
 * shear so the same horizontal contour does not extend through every height. */
float volfog_density_hash(vec2 cell)
{
	vec2 p = fract(mod(cell, vec2_splat(64.0)) * vec2(0.1031, 0.11369));
	p += vec2_splat(dot(p, p.yx + vec2_splat(19.19)));
	return fract(p.x * p.y);
}

float volfog_density_noise(vec3 world_pos, float strength)
{
	vec2 p = (world_pos.xz + vec2(0.17, -0.11) * world_pos.y)
	       * u_volfog_noise.y - u_volfog_noise.zw;
	vec2 cell = floor(p);
	vec2 f = fract(p);
	vec2 w = f * f * (3.0 - 2.0 * f);
	float low = mix(volfog_density_hash(cell),
	                volfog_density_hash(cell + vec2(1.0, 0.0)), w.x);
	float high = mix(volfog_density_hash(cell + vec2(0.0, 1.0)),
	                 volfog_density_hash(cell + vec2(1.0, 1.0)), w.x);
	return 1.0 + strength * (2.0 * mix(low, high, w.y) - 1.0);
}

float volfog_cloud_transmittance(vec3 world_pos)
{
	/* The projected map integrates the complete layer. Only rays below
	 * the layer use it, with their intercept on the bake's Y=0 plane. */
	if (world_pos.y >= u_volfog_cloud_map.w)
		return 1.0;
	vec2 ground_xz = world_pos.xz - world_pos.y * u_volfog_cloud_ray.xy;
	vec2 uv = (ground_xz - u_volfog_cloud_map.yz) / u_volfog_cloud_map.x + 0.5;
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
		return 1.0;

#if BGFX_SHADER_LANGUAGE_HLSL
	float transmittance = texture2DLod(s_volfog_cloud_shadow, uv, 0.0).r;
#else
	float transmittance = texture2D(s_volfog_cloud_shadow, uv).r;
#endif
	/* Soften the transition to the region where a partial slab is unknown. */
	float weight = smoothstep(0.0, 24.0, u_volfog_cloud_map.w - world_pos.y);
	return mix(1.0, clamp(transmittance, 0.0, 1.0), weight);
}

vec2 volfog_clip_sphere(vec3 ray_start, vec3 ray_dir, float ray_end, vec4 sphere)
{
	vec3 to_center = sphere.xyz - ray_start;
	float along = dot(to_center, ray_dir);
	vec3 closest = to_center - ray_dir * along;
	float remaining = sphere.w * sphere.w - dot(closest, closest);
	if (remaining <= 0.0)
		return vec2(0.0, 0.0);
	float half_chord = sqrt(remaining);
	return vec2(max(along - half_chord, 0.0), min(along + half_chord, ray_end));
}

void main()
{
	float density        = u_volfog_p0.x;
	float scattering     = u_volfog_p0.y;
	float nearp          = u_volfog_p0.z;
	float farp           = u_volfog_p0.w;
	float step_n         = u_volfog_p1.x;
	float height_falloff = u_volfog_p1.y;
	float height_origin  = u_volfog_p1.z;
	float max_dist       = u_volfog_p1.w;

	vec2 uv = v_texcoord0;
	float d = texture2D(s_depth, uv).r;
	// The projection W diagonal distinguishes orthographic from perspective.
	bool orthographic = abs(u_proj[3][3]) > 0.5;
	float scene_z = (d >= 1.0) ? farp
	              : (orthographic ? mix(nearp, farp, d) : linearize(d, nearp, farp));

	vec3 cam_ws  = reconstruct_world(uv, 0.0);
	vec3 far_ws  = reconstruct_world(uv, 1.0);
	/* Keep the UNNORMALISED span: its length is the one number needed to turn
	 * a view-axis depth into an along-ray distance, and it is already being
	 * computed by the normalize() below. */
	vec3  ray_vec  = far_ws - cam_ws;
	float ray_span = max(length(ray_vec), 1e-6);
	vec3  ray_dir  = ray_vec / ray_span;

	bool cloud_shadow_on = u_volfog_cloud_map.x > 0.0;
	float sun_on = u_volfog_sun_color.a;
	/* Cascade splits are view-space Z, but t measures distance ALONG THE RAY.
	 * At the screen edge those differ by the angle between the ray and the view
	 * axis -- up to ~25% at a 60-degree fov -- which would select a cascade one
	 * step too far out in the corners and put a visible seam there.  Row 2 of
	 * the view matrix is the (negated) forward axis, so this projects the ray
	 * onto it once instead of per step. */
	/* cos(theta) between this pixel's ray and the view axis -- WITHOUT
	 * touching the view matrix.
	 *
	 * The previous form read it out of u_view by raw element indexing, which
	 * is the one thing vs_foliage.sc's comment already forbids in this tree:
	 * m[i][j] is column-i/row-j on GLSL but row-i/col-j on HLSL, so those
	 * three taps read the forward axis on OpenGL and the TRANSPOSE -- the
	 * world Z axis expressed in camera coordinates -- on D3D. Dotting a
	 * world-space direction against a camera-space vector is a category error
	 * whose result happens to be near 1 at some yaws and near 0 at others, so
	 * it looked plausible from any one camera. Divided into the ray length
	 * below it became a 1000x multiplier (the max() floor) wherever it passed
	 * near zero, saturating the fog to a flat wash with a razor-sharp
	 * boundary -- pose-dependent, D3D-only, and indistinguishable from "the
	 * fog is too dense" unless you look at more than one yaw.
	 *
	 * This form cannot be transposed because it reads no matrix at all: the
	 * near->far segment spans exactly (farp - nearp) along the view axis BY
	 * CONSTRUCTION, and ray_span along the ray, so their ratio is cos(theta)
	 * exactly. Same value on every backend, no handedness, no sign to get
	 * wrong.
	 *
	 * Clamped to (0, 1]: cos cannot exceed 1, and the floor keeps a degenerate
	 * projection (farp == nearp) from dividing by zero. */
	float view_z_scale = clamp((farp - nearp) / ray_span, 1e-3, 1.0);
	float cos_theta = dot(ray_dir, u_volfog_sun.xyz);
	float phase = volfog_phase(cos_theta, u_volfog_sun.w);
	BRANCH if (u_volfog_lobes.z > 0.5)
	{
		phase *= u_volfog_lobes.x;
		if (u_volfog_lobes.y > 0.0)
			phase += u_volfog_lobes.y * volfog_phase(-cos_theta, u_volfog_sun.w);
		if (u_volfog_lobes.x <= 0.0 && u_volfog_lobes.y <= 0.0)
			sun_on = 0.0;
	}
	/* scene_z is view-space Z -- distance along the VIEW AXIS -- but `t` below
	 * steps along a NORMALISED ray, so it measures arc length. The two differ
	 * by 1/cos(angle to the view axis), which is exactly the ratio computed
	 * three lines above, whose own comment already puts it at "up to ~25% at a
	 * 60-degree fov". That correction was applied to cascade selection and not
	 * to the length of the ray being integrated, so the corners of every frame
	 * marched a ray up to a quarter too short and came out under-fogged -- an
	 * error that grows smoothly from the centre outward, which is to say a
	 * vignette nobody authored and which no single-pixel probe would reveal.
	 *
	 * cam_ws is the NEAR-PLANE point, not the eye: reconstruct_world(uv, 0.0)
	 * evaluates at d = 0. So the span to integrate begins at nearp, not at 0.
	 * The term is small (nearp is typically 0.1 m) but it is free and its
	 * absence would make this line right for the wrong reason.
	 *
	 * view_z_scale is cos(theta), clamped to (0, 1] where it is computed. */
	float scene_end = min(max(scene_z - nearp, 0.0) / view_z_scale, max_dist);
	vec2 fog_bounds = vec2(0.0, scene_end);
	BRANCH if (u_volfog_volume.w > 0.0)
		fog_bounds = volfog_clip_sphere(cam_ws, ray_dir, scene_end, u_volfog_volume);
	if (fog_bounds.y <= fog_bounds.x)
	{
		gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	float fog_start = fog_bounds.x;
	float ray_len = fog_bounds.y - fog_start;
	float dt = ray_len / step_n;
	/* Fade lattice contrast as the march stops resolving individual cells.
	 * A zero weight also skips the noise and enables the uniform shortcut. */
	float noise_strength = u_volfog_noise.x *
	                       (1.0 - smoothstep(0.5, 1.5, dt * u_volfog_noise.y));
	bool noise_on = noise_strength > 0.0;

	float transmittance = 1.0;
	vec3  inscatter     = vec3(0.0, 0.0, 0.0);

	/* A zero height falloff makes the medium uniform along this ray. Keep
	 * the same scalar/vector multiplication order, samples and recurrence;
	 * only their repeated density/segment/bulk work moves out of the loop. */
	bool uniform_medium = height_falloff == 0.0 && !noise_on;
	float uniform_seg = 1.0;
	vec3 uniform_Lin = vec3(0.0, 0.0, 0.0);
	if (uniform_medium)
	{
		float uniform_sigma_t = density;
		uniform_seg = exp(-uniform_sigma_t * dt);
		uniform_Lin = u_volfog_color.rgb * (scattering + u_volfog_color.a) *
		              density * dt;
	}

	/* Do NOT unroll. fxc unrolls a bounded loop by default, and once the body
	 * grew a cascade-blend tap and a fallthrough ladder the unrolled march --
	 * 64 copies of five shadow taps -- stopped fitting ("unable to unroll loop
	 * ... or unrolled loop is too large"). A dynamic loop is also the right
	 * shape here: step_n is a uniform, so most of those 64 iterations break out
	 * immediately and unrolling them was paying for work that never runs.
	 *
	 * HLSL only. GLSL/SPIR-V/Metal have no equivalent attribute and their
	 * compilers do not eagerly unroll this. */
	/* Per-pixel start offset, so neighbouring pixels do not sample the fog at
	 * the SAME depths.
	 *
	 * Without it every pixel steps at 0.5*dt, 1.5*dt, ... and a 32-step march
	 * through a shadowed volume quantises the shaft boundary to those depths:
	 * the edge of a god ray becomes a staircase whose treads are one step
	 * long, and because dt is derived from the ray length the treads MOVE as
	 * the camera turns. Offsetting the start by a per-pixel fraction of one
	 * step turns that staircase into noise at the same amplitude, and fog is
	 * the one thing in a frame smooth enough to hide noise.
	 *
	 * Interleaved gradient noise, the same generator fs_sky.sc jitters its
	 * cloud march with -- one place in the engine, one sequence, so two
	 * volumetric passes cannot produce two different dither patterns over the
	 * same pixel. Seeded on gl_FragCoord ALONE and not on time: a per-frame
	 * seed would trade a static staircase for a crawling one, which is worse
	 * without a temporal resolve to average it. */
	float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy,
	                                        vec2(0.06711056, 0.00583715))));

#if BGFX_SHADER_LANGUAGE_HLSL
	[loop]
#endif
	for (int i = 0; i < 64; i++)
	{
		if (float(i) >= step_n) break;
		float t = fog_start + (float(i) + ign) * dt;
		vec3 sample_ws = cam_ws + ray_dir * t;
		float local_density = density;
		float seg = uniform_seg;
		vec3 Lin = uniform_Lin;
		/* The condition is draw-uniform. Keep the unused exponentials behind
		 * it: HLSL flattening otherwise evaluates both paths before selection. */
#if BGFX_SHADER_LANGUAGE_HLSL
		[branch]
#endif
		if (!uniform_medium)
		{
			float h = sample_ws.y - height_origin;
			local_density = density * exp(-max(h, 0.0) * height_falloff);
			BRANCH if (noise_on)
				local_density *= volfog_density_noise(sample_ws, noise_strength);
			float sigma_t = local_density;
			seg = exp(-sigma_t * dt);

			/* Bulk ambient term: what the fog scatters from the sky regardless of
			 * the sun. Kept separate so a shadow does not black out the fog. */
			Lin = u_volfog_color.rgb * (scattering + u_volfog_color.a) *
			      local_density * dt;
		}

		/* Sun in-scattering, shadowed and phase-weighted.
		 *
		 * The 1/PI is a UNIT conversion, not a taste knob.  u_volfog_sun_color
		 * is the same quantity the surface shaders receive, and those treat it
		 * as irradiance: pbr_common.sh computes `kD * albedo / PI` before
		 * multiplying by it.  Feeding it to a medium as though it were radiance
		 * makes the fog PI times brighter than the surfaces standing in it --
		 * a white Lambertian sheet facing the sun returns intensity/PI, so
		 * without this the fog out-scatters a white sheet under its own light
		 * source, which no medium with a single-scattering albedo <= 1 can do.
		 *
		 * It clips to white rather than reading as bright, and it clips WORST
		 * looking toward the sun, where the phase function peaks -- so the
		 * symptom is "the horizon is always fogged" rather than "the fog is too
		 * strong", and the density slider is the wrong place to look. */
		if (sun_on > 0.5)
		{
			float vis = volfog_sunlight(sample_ws, nearp + t * view_z_scale);
			if (vis > 0.0)
			{
				BRANCH if (cloud_shadow_on)
					vis *= volfog_cloud_transmittance(sample_ws);
				Lin += u_volfog_sun_color.rgb * u_volfog_color.rgb
				     * (phase * scattering * local_density * dt * vis
				        * (1.0 / 3.14159265));
			}
		}
		inscatter += Lin * transmittance;
		transmittance *= seg;
		if (transmittance < 0.005) break;
	}

	gl_FragColor = vec4(inscatter, transmittance);
}
