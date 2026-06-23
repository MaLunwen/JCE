$input v_texcoord0

#include <bgfx_shader.sh>

/*
 * fs_ssr.sc — Screen-space reflections.
 *
 * Inputs:
 *   s_color  : current frame color (lit, post-tonemap optional)
 *   s_depth  : depth buffer (non-linear)
 *   s_normal : world-space normal (xyz in [-1,1] or encoded)
 *
 * Uniforms (xyzw):
 *   u_ssr_params0 : (max_distance, thickness, near, far)
 *   u_ssr_params1 : (step_count, max_steps_at_edge, fade_edges, intensity)
 *   u_invViewProj : 4x4 — to reconstruct world-space pos from depth
 *   u_view        : 4x4 — to project rays into view space
 *   u_proj        : 4x4
 *   u_screen      : (w, h, 1/w, 1/h)
 *
 * Approach: linear-z ray march in view space, project sample into
 * NDC each step, sample depth, compare for hit within `thickness`.
 * Fades at screen edges and behind-camera samples.
 */

SAMPLER2D(s_color,  0);
SAMPLER2D(s_depth,  1);
SAMPLER2D(s_normal, 2);

uniform vec4 u_ssr_params0;
uniform vec4 u_ssr_params1;
uniform vec4 u_screen;

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
	vec4 ndc = vec4(uv * 2.0 - 1.0, ndc_z, 1.0);
	vec4 wp = mul(u_invViewProj, ndc);
	return wp.xyz / wp.w;
}

void main()
{
	float max_dist  = u_ssr_params0.x;
	float thickness = u_ssr_params0.y;
	float nearp     = u_ssr_params0.z;
	float farp      = u_ssr_params0.w;
	float step_n   = u_ssr_params1.x;
	float intensity = u_ssr_params1.w;

	vec2 uv = v_texcoord0;
	float d = texture2D(s_depth, uv).r;
	if (d >= 1.0)
	{
		gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
	}
	else
	{
		vec3 wp = reconstruct_world(uv, d);
		vec4 vp = mul(u_view, vec4(wp, 1.0));
		vec3 view_pos = vp.xyz;

		/* Real world-space normal + roughness from the G-buffer
		 * (rgb = normal*0.5+0.5, a = roughness).  Cleared pixels (sky/skinned)
		 * have roughness 1 => the (1-roughness) factor below kills reflection. */
		vec4 nr = texture2D(s_normal, uv);
		float roughness = nr.w;
		vec3 nworld = normalize(nr.xyz * 2.0 - 1.0);
		vec3 view_n = normalize(mul(u_view, vec4(nworld, 0.0)).xyz);
		vec3 view_dir = normalize(view_pos);
		vec3 view_r = reflect(view_dir, view_n);

		vec3 ray_pos = view_pos + view_n * 0.05;
		vec3 step = view_r * (max_dist / step_n);

		vec4 hit_color = vec4(0.0, 0.0, 0.0, 0.0);
		float fade = 0.0;

		for (int i = 0; i < 64; i++)
		{
			if (float(i) >= step_n) break;
			ray_pos += step;
			vec4 clip = mul(u_proj, vec4(ray_pos, 1.0));
			vec3 ndc = clip.xyz / clip.w;
			vec2 sample_uv = ndc.xy * 0.5 + 0.5;
			if (sample_uv.x < 0.0 || sample_uv.x > 1.0 ||
				sample_uv.y < 0.0 || sample_uv.y > 1.0) break;
			float sd = texture2D(s_depth, sample_uv).r;
			float sd_lin = linearize(sd, nearp, farp);
			float ray_lin = -ray_pos.z;
			float diff = ray_lin - sd_lin;
			if (diff > 0.0 && diff < thickness)
			{
				vec2 edge = abs(sample_uv * 2.0 - 1.0);
				float ef = 1.0 - max(edge.x, edge.y);
				ef = clamp(ef * 2.0, 0.0, 1.0);
				fade = ef;
				hit_color = vec4(texture2D(s_color, sample_uv).rgb, 1.0);
				break;
			}
		}

		/* Fresnel (Schlick, dielectric F0=0.1 floor) × roughness gate: grazing
		 * reflections strengthen, head-on weaken, and matte (high-roughness)
		 * surfaces stop reflecting — physically plausible per-material SSR. */
		float cosTheta = clamp(dot(-view_dir, view_n), 0.0, 1.0);
		float fres = 0.1 + 0.9 * pow(1.0 - cosTheta, 4.0);
		float refl = fres * (1.0 - roughness);
		hit_color.rgb *= intensity * fade * refl;
		hit_color.a   *= fade * refl;
		gl_FragColor = hit_color;
	}
}
