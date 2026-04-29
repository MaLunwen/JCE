$input v_pcolor, v_texcoord0

/*
 * fs_particle.sc -- soft circular sprite + colour modulation.
 *
 * Distance-to-centre fall-off gives a pleasing puff for fire/smoke
 * without needing a sprite texture.  Callers wanting textured
 * particles can swap in a sampled variant later.
 */

#include <bgfx_shader.sh>

void main()
{
    vec2 d = v_texcoord0 * 2.0 - vec2(1.0, 1.0);
    float r = dot(d, d);
    if (r > 1.0) discard;

    float falloff = 1.0 - r;
    falloff *= falloff;

    gl_FragColor = vec4(v_pcolor.rgb, v_pcolor.a * falloff);
}
