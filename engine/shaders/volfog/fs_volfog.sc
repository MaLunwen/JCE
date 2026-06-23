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

uniform vec4 u_volfog_p0;
uniform vec4 u_volfog_p1;
uniform vec4 u_volfog_color;

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
	float scene_z = (d >= 1.0) ? max_dist : linearize(d, nearp, farp);
	scene_z = min(scene_z, max_dist);

	vec3 cam_ws  = reconstruct_world(uv, 0.0);
	vec3 far_ws  = reconstruct_world(uv, 1.0);
	vec3 ray_dir = normalize(far_ws - cam_ws);

	float ray_len = scene_z;
	float dt = ray_len / step_n;

	float transmittance = 1.0;
	vec3  inscatter     = vec3(0.0, 0.0, 0.0);

	for (int i = 0; i < 64; i++)
	{
		if (float(i) >= step_n) break;
		float t = (float(i) + 0.5) * dt;
		vec3 sample_ws = cam_ws + ray_dir * t;
		float h = sample_ws.y - height_origin;
		float local_density = density * exp(-max(h, 0.0) * height_falloff);
		float sigma_t = local_density;
		float seg = exp(-sigma_t * dt);
		vec3  Lin = u_volfog_color.rgb * (scattering + u_volfog_color.a) *
		            local_density * dt;
		inscatter += Lin * transmittance;
		transmittance *= seg;
		if (transmittance < 0.005) break;
	}

	gl_FragColor = vec4(inscatter, transmittance);
}
