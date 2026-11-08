/* Private terrain snow admission, displacement and culling contract.
 * Bounds are sent through existing u_terrainSnowGeometry.zw to all shaders. */
#ifndef JCE_SR_SNOW_LIMITS_H
#define JCE_SR_SNOW_LIMITS_H
#define SR_SNOW_DEPTH_MIN_M 0.01f
#define SR_SNOW_DEPTH_MAX_M 0.40f
#define SR_SNOW_RIM_MAX_M (SR_SNOW_DEPTH_MAX_M * 0.25f)
static inline int sr_snow_depth_valid(float depth)
{
    return depth >= SR_SNOW_DEPTH_MIN_M && depth <= SR_SNOW_DEPTH_MAX_M;
}
#endif
