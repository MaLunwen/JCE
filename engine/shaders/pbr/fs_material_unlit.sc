$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos
/* Public MeshRenderer material factors and UV transform, without lighting. */
#include "fs_pbr_decl.sh"
void main()
{
    vec2 uv = v_texcoord0 * u_uvTransform.xy + u_uvTransform.zw;
    vec4 color = texture2D(s_albedo,uv)*u_baseColorFactor;
    if(u_emissiveFactor.w == 1.0 && color.a < u_pbrParams.w) discard;
    gl_FragColor = color;
}
