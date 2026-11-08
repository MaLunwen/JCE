/* Shared neutral IBL/output pack; callers may fill the IBL-only x/y lanes. */
#ifndef JCE_SR_COLOUR_OUTPUT_H
#define JCE_SR_COLOUR_OUTPUT_H
#include <stdbool.h>
struct JceSceneRenderer;
void sr_apply_fog_uniforms(struct JceSceneRenderer *sr);

static inline void sr_pack_colour_output(bool linear_colour, bool tonemap,
    float params[4])
{
    params[0] = 0.0f;
    params[1] = 4.0f;
    params[2] = linear_colour ? (1.0f / 2.2f) : 1.0f;
    params[3] = tonemap ? 1.0f : 0.0f;
}
#endif
