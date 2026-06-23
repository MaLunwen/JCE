$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos

/*
 * fs_pbr.sc  —  DEFAULT PBR fragment program (brute-force point-light path).
 *
 * The entire shader body lives in fs_pbr_body.sh so the Forward+ variant
 * (fs_pbr_fwdplus.sc) can build a SECOND compiled program from the same
 * source of truth by only adding `#define JCE_FORWARDPLUS` before the
 * include.  This file defines NO variant macro, so the body compiles
 * EXACTLY as the historical fs_pbr.sc did: s_iesLut(14) + IES sampling +
 * the 16-light brute-force loop, byte-identical lighting.  See the body for
 * the per-#ifdef breakdown.
 *
 * $input MUST stay here (shaderc strips the leading $input/$output only from
 * the MAIN .sc file, never from #include'd files).
 */

#include "fs_pbr_body.sh"
