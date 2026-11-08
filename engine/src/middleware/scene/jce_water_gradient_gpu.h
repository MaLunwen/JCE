#ifndef JCE_WATER_GRADIENT_GPU_H
#define JCE_WATER_GRADIENT_GPU_H
#include <jce/middleware/scene/jce_water_field.h>
#include <bgfx/c99/bgfx.h>

typedef struct JceWaterGradientGpu JceWaterGradientGpu;
/* Cache owns only render resources; field and its clock remain scene-owned.
 * False means retain the original vertex normal for this draw. */
bool jce_water_gradient_gpu_prepare(JceWaterGradientGpu **cache,
                                    const JceWaterField *field);
bgfx_texture_handle_t jce_water_gradient_gpu_texture(const JceWaterGradientGpu *cache);
void jce_water_gradient_gpu_destroy(JceWaterGradientGpu *cache);
#endif
