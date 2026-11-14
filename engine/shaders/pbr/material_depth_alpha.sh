/* Match stock PBR MASK coverage, including authored factor and UV transform.
 * Opaque depth programs never include this sampler or inherit its state. */
SAMPLER2D(s_albedo, 0);
uniform vec4 u_baseColorFactor;
uniform vec4 u_pbrParams;
uniform vec4 u_uvTransform;
void jce_material_depth_alpha(vec2 source_uv)
{
    vec2 uv = source_uv * u_uvTransform.xy + u_uvTransform.zw;
    float alpha = texture2D(s_albedo, uv).a * u_baseColorFactor.a;
    if (alpha < u_pbrParams.w) discard;
}
