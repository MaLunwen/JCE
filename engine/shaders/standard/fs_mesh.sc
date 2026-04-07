$input v_normal, v_texcoord0, v_worldpos

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

uniform vec4 u_lightDir;   /* xyz = direction (toward light), w = flat-color flag (<0 = flat) */
uniform vec4 u_lightColor; /* xyz = color, w = ambient strength */

void main()
{
    /* Flat color mode: when u_lightDir.w < 0, output u_lightColor.xyz directly. */
    if (u_lightDir.w < 0.0) {
        gl_FragColor = vec4(u_lightColor.xyz, 1.0);
        return;
    }

    vec3 N = normalize(v_normal);
    vec3 L = normalize(u_lightDir.xyz);

    float ambient  = u_lightColor.w;
    float diffuse  = max(dot(N, L), 0.0);

    vec3 light = u_lightColor.xyz * (ambient + diffuse);
    vec4 texel = texture2D(s_texColor, v_texcoord0);

    gl_FragColor = vec4(texel.rgb * light, texel.a);
}
