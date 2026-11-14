$input a_texcoord0, a_position, a_normal, a_indices, a_weight
$output v_texcoord0, v_normal

/* Normal/albedo-only pass: the same current bone blend as velocity, with the
 * view's camera and no previous-pose uniforms or unused clip varyings. */
#define JCE_GBUFFER_VELOCITY 0
#define JCE_DEPTH_ALPHA 1
#include "vs_gbuffer_skin_body.sh"
