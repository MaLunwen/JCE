$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos

/*
 * fs_pbr_fwdplus.sc  —  OPT-IN Forward+ clustered PBR fragment program.
 *
 * Identical to fs_pbr.sc (same body, same $input) except it #defines
 * JCE_FORWARDPLUS before including the shared body, which:
 *   - swaps sampler stage 14 from s_iesLut to s_cluster (the SINGLE combined
 *     RGBA32F cluster data texture jce_forwardplus uploads),
 *   - declares u_clusterParams / u_clusterParams2 / u_clusterRegions,
 *   - replaces the 16-light brute-force point loop (and the spot loop) with a
 *     per-froxel clustered loop that lights only the lights in the fragment's
 *     froxel, lifting the JCE_MAX_POINT_LIGHTS cap,
 *   - drops the per-spot IES profile (documented MVP tradeoff — IES is niche
 *     and freeing stage 14 keeps WebGL2/GLES3 at ≤16 fragment samplers).
 *
 * The glob-based shader build (CompileShadersPBR) picks this fs_*.sc up
 * automatically and emits fs_pbr_fwdplus_<backend>.bin for dx11/spv/glsl/
 * essl/mtl.  Selected at runtime ONLY when the r.forwardplus cvar is on
 * (jce_renderer's program_pbr_fwdplus* handles); default off => fs_pbr.
 */

#define JCE_FORWARDPLUS 1
#include "fs_pbr_body.sh"
