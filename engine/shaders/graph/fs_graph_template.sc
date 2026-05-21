/*
 * fs_graph_template.sc
 *
 * Codegen template for the JCE shader graph (editor/src/shadergraph/).
 *
 * The block between /JCE_BEGIN_MATERIAL/ and /JCE_END_MATERIAL/ is the
 * substitution region: jce_shadergraph_codegen replaces it with graph-driven
 * GLSL that assigns the five "material output" locals consumed by main():
 *
 *     vec4  mat_base_color   (linear RGB, premultiplied factor; .a = alpha)
 *     vec3  mat_normal_ts    (tangent-space normal in [-1, 1])
 *     float mat_metallic     (final, post-factor; main() clamps roughness only)
 *     float mat_roughness    (final, post-factor)
 *     vec3  mat_emissive     (linear RGB, post-factor)
 *
 * The default body below mirrors the texture/factor reads in fs_pbr.sc so
 * the un-substituted template still compiles standalone (Phase C smoke test).
 *
 * Lighting model is intentionally minimal (one directional light + ambient)
 * so the template is small and predictable; the runtime PBR shader
 * (fs_pbr.sc) remains the production path. Phase D may revisit this once
 * generated shaders are wired into the live render pipeline.
 *
 * DO NOT edit the hook markers; codegen string-matches on them verbatim.
 */
$input v_texcoord0, v_worldpos, v_normal

#include <bgfx_shader.sh>

uniform vec4 u_baseColorFactor;
uniform vec4 u_pbrParams;       // x=metallic, y=roughness, z=aoStrength, w=alphaCutoff
uniform vec4 u_emissiveFactor;  // xyz=emissive, w=alphaMode
uniform vec4 u_ambientColor;    // xyz=ambient color, w=intensity
uniform vec4 u_dirLights[4];    // [0]=dir.xyz/intensity, [1]=color.xyz/unused

SAMPLER2D(s_albedo,     0);
SAMPLER2D(s_metalRough, 1);
SAMPLER2D(s_normalMap,  2);
SAMPLER2D(s_emissive,   4);

void main()
{
    vec2 mat_uv = v_texcoord0;

    /*JCE_BEGIN_MATERIAL*/
    vec4  mat_base_color = u_baseColorFactor * texture2D(s_albedo, mat_uv);
    vec3  mat_normal_ts  = texture2D(s_normalMap, mat_uv).xyz * 2.0 - vec3_splat(1.0);
    vec4  mat_mr_sample  = texture2D(s_metalRough, mat_uv);
    float mat_metallic   = u_pbrParams.x * mat_mr_sample.b;
    float mat_roughness  = clamp(u_pbrParams.y * mat_mr_sample.g, 0.04, 1.0);
    vec3  mat_emissive   = u_emissiveFactor.xyz * texture2D(s_emissive, mat_uv).rgb;
    /*JCE_END_MATERIAL*/

    vec3 N = normalize(v_normal + mat_normal_ts * 0.0001);

    vec3 L         = normalize(-u_dirLights[0].xyz);
    float ndotl    = max(dot(N, L), 0.0);
    vec3 lightCol  = u_dirLights[1].xyz * u_dirLights[0].w;

    vec3 diffuse  = mat_base_color.rgb * (1.0 - mat_metallic) * lightCol * ndotl;
    float spec    = pow(ndotl, mix(2.0, 64.0, 1.0 - mat_roughness));
    vec3 specular = lightCol * spec * mat_metallic;
    vec3 ambient  = u_ambientColor.xyz * u_ambientColor.w * mat_base_color.rgb;

    vec3 color = ambient + diffuse + specular + mat_emissive;
    gl_FragColor = vec4(color, mat_base_color.a);
}
