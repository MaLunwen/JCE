$input a_position, a_normal, a_indices, a_weight
$output v_normal

/* Normal/albedo-only pass: the same current bone blend as velocity, with the
 * view's camera and no previous-pose uniforms or unused clip varyings. */
#define JCE_GBUFFER_VELOCITY 0
#include "vs_gbuffer_skin_body.sh"
