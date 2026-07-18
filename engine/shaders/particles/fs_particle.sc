$input v_pcolor, v_texcoord0

/*
 * fs_particle.sc -- billboard sprite + colour modulation.
 *
 * Two paths, selected per draw by u_particle_misc.x:
 *   0 (legacy, byte-identical): procedural soft circular fall-off --
 *     a pleasing puff for fire/glows without needing a texture.
 *   1: sample s_particleTex and modulate by the particle colour
 *     (authored via the *.particles.json "texture" key).
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_particleTex, 0);

uniform vec4 u_particle_misc;   /* .x = textured flag */

void main()
{
    if (u_particle_misc.x > 0.5) {
        vec4 t = texture2D(s_particleTex, v_texcoord0);
        vec4 c = v_pcolor * t;
        if (c.a < 0.004) discard;
        gl_FragColor = c;
    } else {
        vec2 d = v_texcoord0 * 2.0 - vec2(1.0, 1.0);
        float r = dot(d, d);
        if (r > 1.0) discard;

        float falloff = 1.0 - r;
        falloff *= falloff;

        gl_FragColor = vec4(v_pcolor.rgb, v_pcolor.a * falloff);
    }
}
