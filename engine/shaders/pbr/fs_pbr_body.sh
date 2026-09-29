/*
 * fs_pbr_body.sh  —  the PBR fragment body, now assembled from two halves.
 *
 * WHY TWO.  Generated graph-material code must read the samplers and uniforms
 * declared in the first half and be visible to main() in the second, and HLSL
 * does not accept a function used before it is defined.  So the declarations
 * and main() became separate files and this one includes both in order.
 *
 * fs_pbr.sc and fs_pbr_fwdplus.sc include THIS file and are unchanged: the
 * split is textual and was verified by compiling every entry point that uses
 * it before and after and comparing the blobs byte for byte.
 */
#include "fs_pbr_decl.sh"
#include "fs_pbr_main.sh"
