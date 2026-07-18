$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

/*
 * fs_pbr_fade.sc — PBR fragment program with LOD cross-fade dither (千万 ②).
 *
 * Byte-identical lighting to fs_pbr.sc (same fs_pbr_body.sh), plus a
 * screen-door prologue: v_tint.a carries the band-transition coverage from
 * vs_pbr_inst_fade (+f primary / -f complement / 1 solid); interleaved
 * gradient noise picks a complementary pixel subset for the two copies so a
 * LOD switch reads as a smooth dissolve.  See JCE_FADE_DITHER in the body.
 */
#define JCE_FADE_DITHER 1
#include "fs_pbr_body.sh"
