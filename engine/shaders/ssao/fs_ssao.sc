$input v_texcoord0

#include <bgfx_shader.sh>

/* Inputs:
 *   s_depth   : non-linear depth (0..1)
 * Uniforms:
 *   u_ssao_params0 : (radius, bias, intensity, scale)
 *   u_ssao_params1 : (inv_view_proj_xx; we use plain depth-based AO)
 *   u_view_proj    : view-projection matrix (used only for normal recon)
 *   u_screen       : (1.0/width, 1.0/height, width, height)
 *   u_kernel[16]   : pre-computed sample directions (xyz hemisphere) */

SAMPLER2D(s_depth, 0);

uniform vec4 u_ssao_params0;
uniform vec4 u_ssao_params1;
uniform vec4 u_screen;
uniform vec4 u_kernel[16];

/* ── Contact shadows (green channel) ────────────────────────────────────
 *
 * A short view-space raymarch toward the sun, written into .g of this same
 * target.  It lives here, and not in a pass of its own, because the PBR
 * fragment shader has all 16 sampler stages occupied -- that is the WebGL2
 * budget the project charter requires, so the "one extra sampler" the obvious
 * design wants does not exist at any tier that must run on the web.  This pass
 * already has depth bound and already writes an RGBA8 target whose green
 * channel was a duplicate of red, so the march costs no sampler, no render
 * target and no view id.
 *
 *   u_cs_sun_view.xyz = direction TOWARD the sun, in view space (normalised)
 *   u_cs_sun_view.w   = steps to take; <= 0 disables the march entirely
 *   u_cs_params.xy    = projection scale (tan(fovY/2)*aspect, tan(fovY/2))
 *   u_cs_params.z     = ray length in world units
 *   u_cs_params.w     = 1 to jitter the march start, 0 for a fixed march
 */
uniform vec4 u_cs_sun_view;
uniform vec4 u_cs_params;

/* ── Cloud shadows (blue channel) ───────────────────────────────────────
 *
 * A top-down transmittance map baked on the CPU (jce_cloud_shadow.h), sampled
 * by WORLD XZ.  It rides in .b for the same reason contact shadows ride in .g:
 * the PBR fragment shader has all 16 sampler stages occupied, and both PBR and
 * terrain already bind this target.
 *
 * The route the design named -- the directional light cookie -- reaches PBR and
 * not terrain, and terrain is where a cloud shadow is most visible.
 *
 *   u_ssao_cloud.x  = world extent the map covers; 0 = no cloud shadow
 *   u_ssao_cloud.yz = map centre in world XZ
 *   u_ssao_cloud.w  = strength (0..1)
 */
SAMPLER2D(s_ssao_cloud, 1);
uniform vec4 u_ssao_cloud;

float cloud_shadow_at(vec2 uv, float raw_depth)
{
	if (u_ssao_cloud.x <= 0.0 || u_ssao_cloud.w <= 0.0) return 1.0;

	/* World position from depth.  u_invViewProj comes from the view transform
	 * the caller sets on this view; without it there is no way back from a
	 * depth sample to a world XZ, which is why the C side disables the lookup
	 * rather than reconstructing from an identity matrix. */
#if BGFX_SHADER_LANGUAGE_GLSL
	float ndc_z = raw_depth * 2.0 - 1.0;
#else
	float ndc_z = raw_depth;
#endif
	vec2 ndc_xy = uv * 2.0 - 1.0;
#if !BGFX_SHADER_LANGUAGE_GLSL
	/* Top-left texture origin: without this the world XZ is the mirrored
	 * pixel's, so the cloud shadow is inverted and does not track the ground. */
	ndc_xy.y = -ndc_xy.y;
#endif
	vec4 ndc = vec4(ndc_xy, ndc_z, 1.0);
	vec4 wp  = mul(u_invViewProj, ndc);
	vec3 world = wp.xyz / wp.w;

	vec2 muv = (world.xz - (u_ssao_cloud.yz - vec2_splat(0.5 * u_ssao_cloud.x)))
	         / u_ssao_cloud.x;
	/* Outside the map is FULL SUN, not the clamped edge texel.  Clamping
	 * smears the border across the entire rest of the world, so one dark texel
	 * at the edge becomes a hemisphere-wide shadow -- and that reads as
	 * weather, not as a sampling bug. */
	if (muv.x < 0.0 || muv.x > 1.0 || muv.y < 0.0 || muv.y > 1.0) return 1.0;

	float T = texture2D(s_ssao_cloud, muv).r;
	return mix(1.0, T, u_ssao_cloud.w);
}


float linearize_depth(float d)
{
	/* Convert non-linear depth in [0,1] to a roughly linear value in
	 * [0,1] using a fixed near/far range encoded in params1. */
	float near = u_ssao_params1.x;
	float far  = u_ssao_params1.y;
	/* Depth-buffer NDC z convention is backend-dependent: OpenGL is [-1,1]
	 * (needs the *2-1 remap); D3D/Vulkan/Metal are already [0,1].  Using the
	 * GL remap unconditionally made AO depths wrong on D3D/VK → over-occlusion
	 * → the scene rendered dimmer than OpenGL.  Mirrors fs_ssr.sc. */
#if BGFX_SHADER_LANGUAGE_GLSL
	float z_n  = d * 2.0 - 1.0;
	float z_e  = (2.0 * near * far) / (far + near - z_n * (far - near));
#else
	/* D3D/Vulkan/Metal depth buffer is the [0,1] NDC z directly; the GL eye-z
	 * formula above algebraically reduces to this for that range. */
	float z_e  = (near * far) / (far - d * (far - near));
#endif
	return z_e / far;
}

vec3 reconstruct_normal(vec2 uv, vec2 ts)
{
	float dC = linearize_depth(texture2D(s_depth, uv).r);
	float dR = linearize_depth(texture2D(s_depth, uv + vec2(ts.x, 0.0)).r);
	float dD = linearize_depth(texture2D(s_depth, uv + vec2(0.0, ts.y)).r);
	vec3 pC = vec3(uv, dC);
	vec3 pR = vec3(uv + vec2(ts.x, 0.0), dR);
	vec3 pD = vec3(uv + vec2(0.0, ts.y), dD);
	return normalize(cross(pR - pC, pD - pC));
}

float hash(vec2 p)
{
	p = fract(p * vec2(123.34, 456.21));
	p += dot(p, p + 45.32);
	return fract(p.x * p.y);
}

/* Loop bound is a COMPILE-TIME CONSTANT and the tier controls how many steps
 * are taken via the early exit.  A uniform bound is the tempting alternative
 * and it is a portability trap: it compiles on desktop GL and is exactly where
 * GLES drivers have historically refused to unroll or silently done something
 * else.  Must match JCE_CONTACT_SHADOW_MAX_STEPS. */
#define CS_MAX_STEPS 16

/* View-space position from screen UV + linear depth ratio.  Right-handed,
 * camera looking down -Z, so view_z is positive distance and P.z is -view_z. */
vec3 cs_view_pos(vec2 uv, float lin01)
{
	float view_z = lin01 * u_ssao_params1.y;          /* * far */
	vec2  ndc    = uv * 2.0 - 1.0;
#if !BGFX_SHADER_LANGUAGE_GLSL
	/* Top-left texture origin.  u_cs_sun_view is the TRUE view-space sun
	 * direction (CPU-computed), so the march space has to be true as well: a
	 * mirrored P with an unmirrored L walks the ray the wrong way in y and the
	 * contact shadows fall upward. */
	ndc.y = -ndc.y;
#endif
	return vec3(ndc * u_cs_params.xy * view_z, -view_z);
}

/* The inverse of the above: view-space point back to screen UV. */
vec2 cs_view_to_uv(vec3 p)
{
	vec2 ndc = (p.xy / max(-p.z, 1e-4)) / u_cs_params.xy;
#if !BGFX_SHADER_LANGUAGE_GLSL
	ndc.y = -ndc.y;   /* exact inverse of cs_view_pos above */
#endif
	return ndc * 0.5 + 0.5;
}

float cs_march(vec2 uv, float center_lin)
{
	float steps = u_cs_sun_view.w;
	float ray   = u_cs_params.z;
	if (steps < 0.5 || ray <= 0.0) return 1.0;

	vec3 P = cs_view_pos(uv, center_lin);
	vec3 L = u_cs_sun_view.xyz;

	float step_len = ray / steps;
	/* Jitter only where a temporal resolve exists to average it; otherwise the
	 * march is fixed and its banding is at least stable. */
	float offset = (u_cs_params.w > 0.5) ? hash(uv * u_screen.zw) : 0.5;

	/* A thickness window, not a half-space test.  Without an upper bound every
	 * pixel whose sample lands behind ANY closer geometry reports occluded, so
	 * the far side of every silhouette grows a dark halo -- a screen-space ray
	 * knows nothing about what is behind the first depth layer, and treating
	 * "behind" as "occluding" is the classic way that ignorance becomes a
	 * visible artefact. */
	float thickness = ray * 2.0;

	for (int i = 0; i < CS_MAX_STEPS; i++)
	{
		if (float(i) >= steps) break;

		vec3  S    = P + L * (step_len * (float(i) + offset));
		vec2  suv  = cs_view_to_uv(S);
		if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;

		float scene_z = linearize_depth(texture2D(s_depth, suv).r)
		              * u_ssao_params1.y;
		float ray_z   = -S.z;
		float delta   = ray_z - scene_z;

		if (delta > 0.002 && delta < thickness)
		{
			/* Fade with distance along the ray: an occluder found at the very
			 * end of a short ray is the least certain hit, and hard-shadowing
			 * on it makes the march's own length visible as a rim. */
			float t = (float(i) + offset) / steps;
			return clamp(t * t, 0.0, 1.0);
		}
	}
	return 1.0;
}

void main()
{
	vec2 uv = v_texcoord0;
	vec2 ts = u_screen.xy;
	float radius    = u_ssao_params0.x;
	float bias      = u_ssao_params0.y;
	float intensity = u_ssao_params0.z;

	float center_d = linearize_depth(texture2D(s_depth, uv).r);
	if (center_d > 0.999)
	{
		/* Sky: no AO and no contact shadow -- there is nothing at this pixel
		 * for a shadow to land on.  Cloud shadow is 1 too: the sky is already
		 * drawn with the clouds in it, and darkening it here would shadow the
		 * clouds with themselves. */
		gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);
	}
	else
	{
		vec3 n = reconstruct_normal(uv, ts);
		float occ = 0.0;
		float jitter = hash(uv * u_screen.zw) * 6.2831853;
		float cs = cos(jitter), sn = sin(jitter);

		/* Use normal to bias jitter angle (cheap normal-aware mix). */
		float nbias = n.x * 0.3 + n.y * 0.5;
		cs = cos(jitter + nbias);
		sn = sin(jitter + nbias);

		for (int i = 0; i < 16; i++)
		{
			vec3 sd = u_kernel[i].xyz;
			vec2 r2 = vec2(sd.x * cs - sd.y * sn, sd.x * sn + sd.y * cs);
			vec2 sp = uv + r2 * radius * ts * 256.0;
			float sd_depth = linearize_depth(texture2D(s_depth, sp).r);
			float diff = center_d - sd_depth;
			float w = smoothstep(0.0, 1.0, radius / abs(diff + 0.0001));
			if (diff > bias) occ += w;
		}
		occ = 1.0 - (occ / 16.0) * intensity;
		occ = clamp(occ, 0.0, 1.0);

		/* .r = ambient occlusion (multiplies indirect), .g = contact shadow
		 * (multiplies the sun).  Two different lights, two channels, one pass. */
		float contact = cs_march(uv, center_d);
		/* .r ambient occlusion, .g contact shadow, .b cloud shadow.  Three
		 * visibility terms attenuating three different lights, one pass, no
		 * extra sampler in the shader that consumes them. */
		float cloud = cloud_shadow_at(uv, texture2D(s_depth, uv).r);
		gl_FragColor = vec4(occ, contact, cloud, 1.0);
	}
}
