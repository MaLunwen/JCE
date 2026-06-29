$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

/*
 * fs_pbr_tint.sc  —  per-instance-tint PBR fragment program (large-world-opt
 * P1 #7).
 *
 * Builds a THIRD compiled program from the SAME source of truth as fs_pbr /
 * fs_pbr_fwdplus (fs_pbr_body.sh): it only adds the `v_tint` varying to $input
 * and `#define JCE_INST_TINT` before the include.  The body, guarded by that
 * macro, multiplies the base albedo by v_tint exactly where a solo draw applies
 * u_baseColorFactor — so an instanced tinted copy is bit-identical to the same
 * mesh drawn solo with that colour, while STILL collapsing into one instanced
 * submit.  With the macro undefined (fs_pbr.sc) the body is byte-identical to
 * the historical shader, so the default + Forward+ programs are unchanged.
 *
 * $input MUST stay here (shaderc strips the leading $input/$output only from the
 * MAIN .sc file, never from #include'd files).
 */

#define JCE_INST_TINT
#include "fs_pbr_body.sh"
