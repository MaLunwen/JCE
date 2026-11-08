/* Compatibility seam: CPU particle uploads use the renderer's shared owner. */
#ifndef JCE_SR_PARTICLE_BATCH_H
#define JCE_SR_PARTICLE_BATCH_H
#include "renderer/jce_instance_upload.h"
#include <jce/renderer/jce_particles.h>
#include <bgfx/c99/bgfx.h>
typedef JceInstanceUpload SrParticleUpload;
#define SR_PARTICLE_INSTANCE_BYTES 64u
bool sr_particle_upload_begin(const JceRenderer *,
                              uint32_t requested, uint32_t budget_mb, SrParticleUpload *);
bool sr_particle_upload_bind(SrParticleUpload *, uint32_t actual);

typedef struct {
    bgfx_transient_vertex_buffer_t vertices;
    bgfx_transient_index_buffer_t indices;
    uint32_t count;
    uint32_t capacity;
} SrParticleExpandedUpload;

bool sr_particle_expanded_begin(const JceRenderer *, uint32_t requested,
                                SrParticleExpandedUpload *);
void sr_particle_expanded_visit(const JceParticleView *, void *upload);
bool sr_particle_expanded_bind(const SrParticleExpandedUpload *);
#endif
