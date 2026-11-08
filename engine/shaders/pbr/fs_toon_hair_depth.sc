$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos

#include <bgfx_shader.sh>

SAMPLER2D(s_albedo, 0);
uniform vec4 u_baseColorFactor;
uniform vec4 u_pbrParams;
uniform vec4 u_emissiveFactor;
uniform vec4 u_uvTransform;

void main()
{
    if (u_emissiveFactor.w == 1.0) {
        vec2 uv = v_texcoord0 * u_uvTransform.xy + u_uvTransform.zw;
        if (texture2D(s_albedo, uv).a * u_baseColorFactor.a < u_pbrParams.w)
            discard;
    }
    gl_FragColor = vec4(max(v_viewdepth, 0.0), 0.0, 0.0, 1.0);
}
