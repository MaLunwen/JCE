$input v_pcolor, v_texcoord0

/*
 * fs_particle.sc -- billboard sprite + colour modulation.
 *
 * Two paths, selected per draw by u_particle_misc.x:
 *   0 (legacy, byte-identical): procedural soft circular fall-off --
 *     a pleasing puff for fire/glows without needing a texture.
 *   1: sample s_particleTex and modulate by the particle colour
 *     (authored via the *.particles.json "texture" key).
 *
 * SOFT PARTICLES (Unity's QualitySettings.softParticlesEnabled), gated by
 * u_particle_misc.z = fade distance in world units; 0 disables the whole
 * block and the shader is then byte-identical to what it was.  A billboard
 * is a flat quad, so where it intersects the floor it draws a hard straight
 * seam that reads as a sheet of paper stuck through the ground -- the one
 * artefact that gives away every untreated smoke and dust effect.  Fading
 * alpha out over the last few centimetres before the opaque surface behind
 * removes the seam.
 *
 * The depth comes from the camera depth pre-pass (this engine's
 * _CameraDepthTexture), which the pipeline can now demand: soft particles is
 * one of the reasons sr_wants_depth_prepass() answers yes.  When no depth is
 * bound the host passes fade = 0, so the sampler is never read.
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_particleTex, 0);
SAMPLER2D(s_sceneDepth,  1);

uniform vec4 u_particle_misc;   /* .x = textured flag, .y = stretch allowed,
                                 * .z = soft fade distance (0 = off)        */
uniform vec4 u_particle_soft;   /* (1/viewport_w, 1/viewport_h, near, far)  */

/* Non-linear depth in [0,1] to a linear ratio of `far`.
 *
 * VERBATIM from fs_ssao.sc / fs_ssr.sc, including the backend split, and not
 * re-derived: the GL remap applied unconditionally is a bug this tree has
 * already paid for once (D3D/Vulkan/Metal hand you the [0,1] NDC z directly,
 * and using GL's *2-1 on it made every depth wrong). */
float jce_particle_linear_depth(float d)
{
	float near = u_particle_soft.z;
	float far  = u_particle_soft.w;
#if BGFX_SHADER_LANGUAGE_GLSL
	float z_n = d * 2.0 - 1.0;
	float z_e = (2.0 * near * far) / (far + near - z_n * (far - near));
#else
	float z_e = (near * far) / (far - d * (far - near));
#endif
	return z_e / far;
}

/* 1.0 away from geometry, falling to 0.0 as the billboard reaches it.
 * fragment_depth is gl_FragCoord.z, taken as a parameter because bgfx/shaderc
 * only exposes gl_FragCoord inside main() -- the same reason
 * fs_pbr_body.sh:664 passes it down. */
float jce_particle_soft_fade(vec2 frag_xy, float fragment_depth)
{
	float fade_dist = u_particle_misc.z;
	if (fade_dist <= 0.0) return 1.0;

	vec2  suv     = frag_xy * u_particle_soft.xy;
	float far     = u_particle_soft.w;
	float scene_z = jce_particle_linear_depth(texture2D(s_sceneDepth, suv).r) * far;
	float frag_z  = jce_particle_linear_depth(fragment_depth) * far;

	/* Behind the opaque surface the particle is already depth-rejected, so
	 * the difference is >= 0 for anything that got here; clamp anyway, since
	 * a depth target that was never written reads as far and would otherwise
	 * make the whole sprite vanish rather than merely not fade. */
	return clamp((scene_z - frag_z) / fade_dist, 0.0, 1.0);
}

void main()
{
	float soft = jce_particle_soft_fade(gl_FragCoord.xy, gl_FragCoord.z);

	if (u_particle_misc.x > 0.5) {
		vec4 t = texture2D(s_particleTex, v_texcoord0);
		vec4 c = v_pcolor * t;
		c.a *= soft;
		if (c.a < 0.004) discard;
		gl_FragColor = c;
	} else {
		vec2 d = v_texcoord0 * 2.0 - vec2(1.0, 1.0);
		float r = dot(d, d);
		if (r > 1.0) discard;

		float falloff = 1.0 - r;
		falloff *= falloff;

		gl_FragColor = vec4(v_pcolor.rgb, v_pcolor.a * falloff * soft);
	}
}
