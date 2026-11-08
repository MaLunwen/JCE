#ifndef JCE_WATER_FOAM_GPU_H
#define JCE_WATER_FOAM_GPU_H
#include "jce_water_foam.h"
#include <bgfx/c99/bgfx.h>
typedef struct JceWaterFoamGpu JceWaterFoamGpu;
bool jce_water_foam_gpu_prepare(JceWaterFoamGpu **owner, const JceWaterFoamState *state);
bgfx_texture_handle_t jce_water_foam_gpu_texture(const JceWaterFoamGpu *cache);
void jce_water_foam_gpu_destroy(JceWaterFoamGpu *cache);
#endif
