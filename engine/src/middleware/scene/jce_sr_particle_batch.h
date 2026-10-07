/* Private CPU instance upload arena; no compute or particle simulation policy. */
#ifndef JCE_SR_PARTICLE_BATCH_H
#define JCE_SR_PARTICLE_BATCH_H
#include "jce_sr_particle_batch_plan.h"
#include <bgfx/c99/bgfx.h>

typedef struct {
    bgfx_instance_data_buffer_t transient;
    const bgfx_memory_t *memory;
    float *data;
    uint32_t count, offset;
    bool dynamic;
} SrParticleUpload;

bool sr_particle_upload_begin(SrParticleArena *, uint32_t frame,
                              uint32_t requested, uint32_t budget_mb, SrParticleUpload *);
void sr_particle_upload_bind(SrParticleArena *, SrParticleUpload *, uint32_t actual);
void sr_particle_arena_destroy(SrParticleArena *);
#endif
