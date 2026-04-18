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

    vec4 texel = texture2D(s_texColor, v_texcoord0);

    /* Wireframe-textured hue-Lambert mode (0 < w < 0.5):
       Normalise the texture colour by its luminance to extract the hue, then
       apply the same Lambert model as the standard path.  Result: same
       shadowing/brightness as plain wireframe, but tinted by the albedo hue. */
    if (u_lightDir.w > 0.0 && u_lightDir.w < 0.5) {
        float lum = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
        vec3  hue = (lum > 0.001) ? texel.rgb * (1.0 / lum) : vec3(1.0, 1.0, 1.0);
        vec3 N = normalize(v_normal);
        vec3 L = normalize(u_lightDir.xyz);
        float ambient = u_lightColor.w;
        float diffuse = max(dot(N, L), 0.0);
        vec3  light   = u_lightColor.xyz * (ambient + diffuse);
        gl_FragColor = vec4(clamp(hue * light, 0.0, 1.0), 1.0);
        return;
    }

    /* Raw textured mode: output texture color directly without lighting. */
    if (u_lightDir.w > 0.5) {
        gl_FragColor = texel;
        return;
    }

    vec3 N = normalize(v_normal);
    vec3 L = normalize(u_lightDir.xyz);

    float ambient  = u_lightColor.w;
    float diffuse  = max(dot(N, L), 0.0);

    vec3 light = u_lightColor.xyz * (ambient + diffuse);

    gl_FragColor = vec4(texel.rgb * light, texel.a);
}
