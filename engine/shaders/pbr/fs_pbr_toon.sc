$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos

/*
 * fs_pbr_toon.sc  —  per-character soft-cel toon PBR fragment program.
 *
 * Identical to fs_pbr.sc (same body, same $input) except it #defines JCE_TOON
 * before including the shared body, which quantizes the directional diffuse
 * into bands and adds a world-space rim.  Ambient/IBL/SH9 are shared so the
 * cel character still receives the scene's environment lighting.  Selected
 * only when a MeshRenderer.toon==true entity is drawn AND sr_toon_allowed
 * (quality>=HIGH + valid handle); default skinned path stays fs_pbr.
 *
 * $input MUST stay in this .sc (shaderc strips $input only from the MAIN file).
 */

#define JCE_TOON 1
#include "fs_pbr_body.sh"
