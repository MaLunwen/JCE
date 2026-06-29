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
		gl_FragColor = vec4(occ, occ, occ, 1.0);
	}
}
