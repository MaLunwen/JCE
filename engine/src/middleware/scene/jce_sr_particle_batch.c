#include "jce_sr_particle_batch.h"
bool sr_particle_upload_begin(const JceRenderer *renderer,
                              uint32_t requested, uint32_t budget_mb, SrParticleUpload *out)
{
    return jce_instance_upload_begin(jce_renderer_instance_upload_owner(renderer),
        jce_renderer_instance_upload_epoch(), requested, SR_PARTICLE_INSTANCE_BYTES, budget_mb, out);
}
bool sr_particle_upload_bind(SrParticleUpload *upload, uint32_t actual)
{
    if (!upload) return false;
    return jce_instance_upload_bind(upload->owner, upload, actual);
}

typedef struct {
    float center[3];
    float uv[2];
    float stretch_size[4];
    float color[4];
} SrParticleExpandedVertex;

/* At most 880 KiB of transient data per view. Leave room in both pools for
 * later geometry and UI. The 16-bit indices are local to this transient slice. */
#define SR_PARTICLE_EXPANDED_LIMIT 4096u
#define SR_PARTICLE_EXPANDED_INDEX_RESERVE_BYTES (64u * 1024u)

bool sr_particle_expanded_begin(const JceRenderer *renderer, uint32_t requested,
                                SrParticleExpandedUpload *upload)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || !requested) return false;

    bgfx_vertex_layout_t layout;
    bgfx_vertex_layout_begin(&layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD1, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_COLOR0, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&layout);
    const uint32_t stride = bgfx_vertex_layout_get_stride(&layout);
    if (stride != sizeof(SrParticleExpandedVertex)) return false;

    const JceInstanceUploadOwner *owner = jce_renderer_instance_upload_owner(renderer);
    uint32_t vertex_reserve_bytes = owner ? owner->transient_reserve_bytes : 1048576u;
    const uint32_t max_vertex_reserve = caps->limits.maxTransientVbSize / 2u;
    if (vertex_reserve_bytes > max_vertex_reserve) vertex_reserve_bytes = max_vertex_reserve;
    const uint32_t vertex_reserve = (vertex_reserve_bytes + stride - 1u) / stride;
    uint32_t index_reserve = SR_PARTICLE_EXPANDED_INDEX_RESERVE_BYTES / sizeof(uint16_t);
    const uint32_t max_index_reserve = caps->limits.maxTransientIbSize / 2u / sizeof(uint16_t);
    if (index_reserve > max_index_reserve) index_reserve = max_index_reserve;

    if (requested > SR_PARTICLE_EXPANDED_LIMIT) requested = SR_PARTICLE_EXPANDED_LIMIT;
    const uint32_t avail_vertices =
        bgfx_get_avail_transient_vertex_buffer(requested * 4u + vertex_reserve, &layout);
    const uint32_t avail_indices =
        bgfx_get_avail_transient_index_buffer(requested * 6u + index_reserve, false);
    if (avail_vertices <= vertex_reserve || avail_indices <= index_reserve) return false;

    const uint32_t vertex_quads = (avail_vertices - vertex_reserve) / 4u;
    const uint32_t index_quads = (avail_indices - index_reserve) / 6u;
    if (requested > vertex_quads) requested = vertex_quads;
    if (requested > index_quads) requested = index_quads;
    if (!requested ||
        !bgfx_alloc_transient_buffers(&upload->vertices, &layout, requested * 4u,
                                      &upload->indices, requested * 6u, false))
        return false;

    upload->count = 0;
    upload->capacity = requested;
    return true;
}

void sr_particle_expanded_visit(const JceParticleView *p, void *context)
{
    SrParticleExpandedUpload *upload = context;
    if (upload->count >= upload->capacity) return;

    const uint32_t quad = upload->count++;
    SrParticleExpandedVertex *vertices =
        (SrParticleExpandedVertex *)upload->vertices.data + quad * 4u;
    uint16_t *indices = (uint16_t *)upload->indices.data + quad * 6u;
    const uint16_t first = (uint16_t)(quad * 4u);
    const float size = p->size < 0.0001f ? 0.0001f : p->size;
    SrParticleExpandedVertex vertex = {
        {p->position.x, p->position.y, p->position.z},
        {0.0f, 0.0f},
        {p->velocity.x * p->stretch, p->velocity.y * p->stretch,
         p->velocity.z * p->stretch, size},
        {p->color.x, p->color.y, p->color.z, p->color.w},
    };
    static const float corners[4][2] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f},
    };
    static const uint16_t quad_indices[] = {0, 1, 2, 0, 2, 3};
    for (uint32_t i = 0; i < 4u; i++) {
        vertex.uv[0] = corners[i][0];
        vertex.uv[1] = corners[i][1];
        vertices[i] = vertex;
    }
    for (uint32_t i = 0; i < 6u; i++)
        indices[i] = (uint16_t)(first + quad_indices[i]);
}

bool sr_particle_expanded_bind(const SrParticleExpandedUpload *upload)
{
    if (!upload->count) return false;
    bgfx_set_transient_vertex_buffer(0, &upload->vertices, 0, upload->count * 4u);
    bgfx_set_transient_index_buffer(&upload->indices, 0, upload->count * 6u);
    return true;
}
