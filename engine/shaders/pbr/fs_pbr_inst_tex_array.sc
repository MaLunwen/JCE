$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

/*
 * fs_pbr_inst_tex_array.sc — texture-diverse instanced PBR fragment program.
 *
 * Built from the SAME source of truth as fs_pbr (fs_pbr_body.sh) with
 * JCE_TEX_ARRAY: s_albedo is a SAMPLER2DARRAY and the base-colour sample reads
 * this instance's own layer via v_tint.x (the vertex shader packs the layer
 * index there — the array variant carries no colour tint, so v_tint is free).
 * That lets albedo-TEXTURE-diverse copies of one mesh collapse into one
 * instanced submit.  baseColor / metallic / roughness stay shared uniforms (the
 * batch groups on them), so the $input stays at the same 8 varyings as fs_pbr.
 *
 * essl1/GLES2 is excluded from the GLES2 allowlist (no sampler2DArray), so the
 * LOW/WebGL1 tier keeps the non-array fs_pbr path. $input MUST stay here.
 */

#define JCE_TEX_ARRAY
#include "fs_pbr_body.sh"
