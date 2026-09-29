$input v_texcoord0

#include <bgfx_shader.sh>

/*
 * fs_ssgi.sc -- screen-space global illumination: one diffuse bounce.
 *
 * The lit colour buffer already contains, at every pixel, the radiance that
 * surface emits toward the camera.  A diffuse receiver gathers that radiance
 * over its hemisphere; the screen is a partial, camera-side sample of it.
 * That is the whole idea, and also the whole limitation: light from a surface
 * that is off-screen or facing away contributes nothing, which is why this
 * ADDS to the probe/sky indirect term rather than replacing it.
 *
 * Inputs:
 *   s_color  : this frame's lit colour
 *   s_depth  : the depth pre-pass buffer (non-linear)
 *   s_normal : rgb = world normal * 0.5 + 0.5, a = roughness
 *
 * Uniforms:
 *   u_ssgi_params0 : (radius_world, thickness, near, far)
 *   u_ssgi_params1 : (ray_count, step_count, intensity, UNUSED)
 *                    .w was documented here as "max_bounce" and is written as
 *                    a literal 0.0 by jce_ssgi.c and read by nothing.  There is
 *                    no multi-bounce path: a second bounce would need the FIRST
 *                    bounce's result as an input texture, i.e. a second march
 *                    view and a second RT, and SSGI owns two view ids in total.
 *                    Named honestly rather than left as a parameter that does
 *                    not exist.
 *   u_screen       : (w, h, 1/w, 1/h)
 *   u_invViewProj / u_view / u_proj  -- filled by bgfx from the view transform
 *
 * Output: rgb = the bounced radiance already multiplied by the RECEIVER'S
 * ALBEDO, a = hit coverage.  The composite is then a plain additive blend.
 *
 * THE RECEIVER TERM USED TO BE ITS LIT COLOUR, and that was wrong in a
 * specific way: the correct term is albedo/PI * SUM(L_i cos), and lit colour
 * already contains the direct lighting -- so a brightly lit receiver gathered
 * more bounce than it should, brightest exactly where the error shows most.
 * It was the right stand-in only in the sense that it CANNOT BREAK THE FLOOR
 * (a black surface stays black, a shadowed one stays dark), which is why it
 * survived until there was a target to replace it with.
 *
 * There is one now: the depth pre-pass writes the material's base-colour
 * FACTOR into attachment 1 (jce_sr_cull.c, fs_gbuffer*.sc).  A flat colour is
 * a COARSE albedo and a CORRECT one, where lit colour was a detailed albedo
 * and a wrong one.  What it does not carry is per-texel albedo: binding every
 * material's map in a pre-pass that binds nothing today would make the
 * pre-pass as state-heavy as the colour pass, to refine a term that is
 * already a screen-space estimate.
 *
 * DETERMINISM.  The ray directions come from a hash of gl_FragCoord ONLY --
 * no frame counter, no time.  Two runs of the same frame are byte-identical,
 * which is what this tree's capture harness compares.  A temporally rotated
 * pattern would look better under TAA and would make every A/B comparison in
 * this repository meaningless.
 */

SAMPLER2D(s_color,  0);
SAMPLER2D(s_depth,  1);
SAMPLER2D(s_normal, 2);
SAMPLER2D(s_albedo, 3);   /* rgb = base colour; see the header */

uniform vec4 u_ssgi_params0;
uniform vec4 u_ssgi_params1;
uniform vec4 u_screen;

#define SSGI_PI 3.14159265359

float ssgi_linearize(float d, float n, float f)
{
	return n * f / (f - d * (f - n));
}

/* uv+depth -> WORLD, then the caller takes it to view space with u_view.
 *
 * COPIED FROM fs_ssr.sc, deliberately and to the letter.  The y flip is
 * `#if !BGFX_SHADER_LANGUAGE_GLSL` -- the opposite of what writing it from
 * scratch produces, which is how this tree collected twelve `uv*2-1` sign
 * defects, one of them in this very file before it was compared against its
 * neighbour.  The march below flips the SAME way for the same reason: the
 * two are inverses and must cancel, or the whole gather runs in a mirrored
 * view space while the normals come from the true pixel. */
vec3 ssgi_reconstruct_world(vec2 uv, float d)
{
#if BGFX_SHADER_LANGUAGE_GLSL
	float ndc_z = d * 2.0 - 1.0;   /* GL: depth-buffer NDC z is [-1,1] */
#else
	float ndc_z = d;               /* D3D/Vulkan/Metal/WebGPU: NDC z is [0,1] */
#endif
	vec2 ndc_xy = uv * 2.0 - 1.0;
#if !BGFX_SHADER_LANGUAGE_GLSL
	ndc_xy.y = -ndc_xy.y;
#endif
	vec4 ndc = vec4(ndc_xy, ndc_z, 1.0);
	vec4 wp = mul(u_invViewProj, ndc);
	return wp.xyz / wp.w;
}

/* A stable per-pixel rotation.  Interleaved gradient noise: cheap, no texture,
 * and a pure function of the pixel -- see DETERMINISM above. */
float ssgi_ign(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main()
{
	vec2  uv    = v_texcoord0;
	float depth = texture2D(s_depth, uv).r;

	/* Sky / cleared depth: nothing receives a bounce there. */
	if (depth >= 0.9999)
	{
		gl_FragColor = vec4_splat(0.0);
		return;
	}

	vec4  nrm4 = texture2D(s_normal, uv);
	vec3  N_ws = normalize(nrm4.xyz * 2.0 - 1.0);
	vec3  N    = normalize(mul(u_view, vec4(N_ws, 0.0)).xyz);

	float radius    = u_ssgi_params0.x;
	float thickness = u_ssgi_params0.y;
	float znear     = u_ssgi_params0.z;
	float zfar      = u_ssgi_params0.w;
	float rays      = u_ssgi_params1.x;
	float steps     = u_ssgi_params1.y;
	float intensity = u_ssgi_params1.z;

	vec3  Pw     = ssgi_reconstruct_world(uv, depth);
	vec3  P      = mul(u_view, vec4(Pw, 1.0)).xyz;
	vec3  gather = vec3_splat(0.0);
	float hits   = 0.0;

	float rot = ssgi_ign(gl_FragCoord.xy) * 2.0 * SSGI_PI;

	/* FIXED, AND SMALL.  A dynamic bound is a divergent loop on every backend
	 * here and the ES 3.0 floor will not unroll it at all -- so the bounds are
	 * compile-time.  4x8 and not 8x16 because the compiler UNROLLS them: at
	 * 8x16 fxc ground for minutes on one 195 MB process per backend, and the
	 * unrolled body would have issued up to 384 texture fetches per pixel.
	 * This engine's stated baseline is an integrated GPU; 4 rays x 8 steps is
	 * 32 fetches, the same order as the SSR march next door. */
	for (int r = 0; r < 4; ++r)
	{
		if (float(r) >= rays) break;

		/* Cosine-weighted hemisphere around N, deterministic per pixel. */
		float a  = rot + float(r) * (2.0 * SSGI_PI / max(rays, 1.0));
		float u1 = fract(ssgi_ign(gl_FragCoord.xy + vec2(float(r) * 7.0, float(r) * 13.0)));
		float sr = sqrt(u1);
		vec3  T  = normalize(abs(N.z) < 0.99 ? cross(N, vec3(0.0, 0.0, 1.0))
		                                     : cross(N, vec3(1.0, 0.0, 0.0)));
		vec3  B  = cross(N, T);
		vec3  dir = normalize(T * (sr * cos(a)) + B * (sr * sin(a))
		                      + N * sqrt(max(1.0 - u1, 0.0)));

		vec3  step_v = dir * (radius / max(steps, 1.0));
		vec3  march  = P;
		for (int s = 0; s < 8; ++s)
		{
			if (float(s) >= steps) break;
			march += step_v;

			vec4 clip = mul(u_proj, vec4(march, 1.0));
			if (clip.w <= 0.0) break;
			vec2 suv = (clip.xy / clip.w) * 0.5 + 0.5;
#if !BGFX_SHADER_LANGUAGE_GLSL
			suv.y = 1.0 - suv.y;   /* pairs with ssgi_reconstruct_world */
#endif
			if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;

			float sd  = texture2D(s_depth, suv).r;
			if (sd >= 0.9999) break;                 /* marched into the sky */
			float lin_scene = ssgi_linearize(sd, znear, zfar);
			float lin_ray   = -march.z;

			float delta = lin_ray - lin_scene;
			if (delta > 0.0 && delta < thickness)
			{
				/* The hit surface must be facing the receiver, or we are
				 * gathering light off its back -- the classic screen-space
				 * bleed-through-a-wall artefact. */
				vec3 hitN = normalize(texture2D(s_normal, suv).xyz * 2.0 - 1.0);
				vec3 hitNv = normalize(mul(u_view, vec4(hitN, 0.0)).xyz);
				if (dot(hitNv, -dir) > 0.0)
				{
					float ndl = max(dot(N, dir), 0.0);
					gather += texture2D(s_color, suv).rgb * ndl;
					hits   += 1.0;
				}
				break;
			}
		}
	}

	float inv = 1.0 / max(rays, 1.0);
	vec3  bounce = gather * inv * intensity;

	/* THE RECEIVER'S ALBEDO, from the pre-pass's albedo target.
	 *
	 * This used to be the receiver's LIT COLOUR, which counts its direct
	 * lighting twice: a brightly lit surface then gathered more bounce than
	 * it should, brightest exactly where the error is most visible.  The
	 * target carries the material's base-colour FACTOR, not its texture --
	 * a flat colour is a coarse albedo and a correct one, where lit colour
	 * was a detailed albedo and a wrong one. */
	vec3 receiver = texture2D(s_albedo, uv).rgb;
	gl_FragColor = vec4(receiver * bounce, hits * inv);
}
