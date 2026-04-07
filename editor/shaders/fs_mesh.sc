$input v_normal, v_texcoord0, v_worldpos

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

uniform vec4 u_lightDir;   /* xyz = direction (toward light), w = unused */
uniform vec4 u_lightColor; /* xyz = color, w = ambient strength */

void main()
{
    vec3 N = normalize(v_normal);
    vec3 L = normalize(u_lightDir.xyz);

    float ambient  = u_lightColor.w;
    float diffuse  = max(dot(N, L), 0.0);

    vec3 light = u_lightColor.xyz * (ambient + diffuse);
    vec4 texel = texture2D(s_texColor, v_texcoord0);

    gl_FragColor = vec4(texel.rgb * light, texel.a);
}
