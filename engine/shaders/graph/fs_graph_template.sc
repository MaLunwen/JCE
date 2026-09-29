$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos

/*
 * fs_graph_template.sc
 *
 * Codegen template for the JCE shader graph (editor/src/shadergraph/).
 *
 * A GRAPH SHADER IS A DIFFERENT MATERIAL, NOT A DIFFERENT RENDERER.
 *
 * This template used to carry its own lighting: one directional light, a
 * pow() specular lobe and a flat ambient term.  It was honest about it -- the
 * comment said "intentionally minimal" -- but the consequence was that a
 * Shader Graph material was lit differently from every other surface in the
 * same scene.  No shadows.  No IBL.  No fog.  No clustered point lights, no
 * area lights, no wetness, no snow, no tone mapping.  Two spheres side by
 * side, one plain and one graph-authored, did not look like they were in the
 * same world, and nothing about the graph could fix that.  Unity, Unreal and
 * Godot all feed a graph's outputs into the same lit shader the rest of the
 * scene uses; that is what makes a graph usable for anything but an unlit
 * effect.
 *
 * So the graph now produces the five material values and fs_pbr_main.sh does
 * the lighting -- the SAME file fs_pbr.sc and fs_pbr_fwdplus.sc use.  The
 * three entry points differ only in what they #define before including it.
 *
 * WHY THE SPLIT INCLUDE.  Generated code has to read the samplers and
 * uniforms declared in fs_pbr_decl.sh and be visible to main() in
 * fs_pbr_main.sh, and HLSL will not accept a function used before it is
 * defined.  So the declarations come first, then this file's
 * jce_graph_material, then main().
 *
 * The block between /JCE_BEGIN_MATERIAL/ and /JCE_END_MATERIAL/ is the
 * substitution region: jce_shadergraph_codegen replaces it with graph-driven
 * GLSL that assigns the five locals below.  The default body reproduces the
 * stock PBR material reads, so the un-substituted template still compiles and
 * renders exactly like fs_pbr.
 *
 * DO NOT edit the hook markers; codegen string-matches on them verbatim.
 */

#define JCE_GRAPH_MATERIAL
#include "fs_pbr_decl.sh"
/* Helper functions the procedural node snippets call.  A node's snippet is one
 * expression; anything needing a loop or a hash is a function here rather than
 * an unreadable inline giant. */
#include "graph_nodes.sh"

/* The graph's material function.
 *
 * `mat_uv` is the name the generated statements expect -- it is the material's
 * tiled/offset UV, the same one every stock map is sampled with.  world_pos
 * and normal_ws are passed for nodes that need position or the geometric
 * normal; unused ones cost nothing.
 *
 * mat_screen_uv is passed rather than read here because gl_FragCoord is not a
 * variable in bgfx's HLSL translation -- it is something the entry point owns,
 * and a helper function cannot see it.  A Screen UV node that read it directly
 * compiled on GLSL and SPIR-V and failed on D3D with "undeclared identifier",
 * which is the shape of bug that ships to one backend.
 *
 * The five outs are exactly the PBR Output node's five input sockets. */
void jce_graph_material(vec2 mat_uv, vec3 mat_world_pos, vec3 mat_normal_ws,
                        vec2 mat_screen_uv,
                        out vec4 out_base_color, out vec3 out_normal_ts,
                        out float out_metallic, out float out_roughness,
                        out vec3 out_emissive)
{
    /*JCE_BEGIN_MATERIAL*/
    vec3  mat_normal_ts  = texture2D(s_normalMap, mat_uv).xyz * 2.0 - vec3_splat(1.0);
    vec4  mat_base_color = u_baseColorFactor * texture2D(s_albedo, mat_uv);
    vec4  mat_mr_sample  = texture2D(s_metalRough, mat_uv);
    float mat_metallic   = u_pbrParams.x * mat_mr_sample.b;
    float mat_roughness  = clamp(u_pbrParams.y * mat_mr_sample.g, 0.04, 1.0);
    vec3  mat_emissive   = u_emissiveFactor.xyz * texture2D(s_emissive, mat_uv).rgb;
    /*JCE_END_MATERIAL*/

    out_base_color = mat_base_color;
    out_normal_ts  = mat_normal_ts;
    out_metallic   = mat_metallic;
    out_roughness  = mat_roughness;
    out_emissive   = mat_emissive;

    /* Referenced so a graph that drives neither still compiles with them in
     * the signature; the optimiser removes both. */
    out_base_color.rgb += (mat_world_pos + mat_normal_ws) * 0.0;
}

#include "fs_pbr_main.sh"
