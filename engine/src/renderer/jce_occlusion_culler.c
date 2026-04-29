/*
 * jce_occlusion_culler.c  GPU-query occlusion culling.
 *
 * Two-pass coherence-based approach:
 *   Frame N  — entity_visible() checks last-frame query results,
 *              submit_query() queues bounding-box proxy draws.
 *   Frame N+1 — begin_frame() polls those results.
 *
 * Hash table: entity_id → slot (linear probing, power-of-two size).
 * Each slot holds a bgfx_occlusion_query_handle_t and the last result.
 *
 * Unit cube (8 vertices, 12 triangles) is stored as a global static
 * index buffer and reused each frame via transient vertex buffers.
 */

#include <jce/renderer/jce_occlusion_culler.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>

#include <string.h>
#include <math.h>

#define LOG_TAG "occlusion"

/* ── Unit-cube vertex / index data ──────────────────────────────────── */

/* 8 corners of a unit cube [-0.5, 0.5]^3 (position only). */
static const float k_cube_verts[8 * 3] = {
    -0.5f, -0.5f, -0.5f,
     0.5f, -0.5f, -0.5f,
     0.5f,  0.5f, -0.5f,
    -0.5f,  0.5f, -0.5f,
    -0.5f, -0.5f,  0.5f,
     0.5f, -0.5f,  0.5f,
     0.5f,  0.5f,  0.5f,
    -0.5f,  0.5f,  0.5f,
};

static const uint16_t k_cube_indices[36] = {
    0,1,2, 0,2,3,  /* -Z */
    4,6,5, 4,7,6,  /* +Z */
    0,4,5, 0,5,1,  /* -Y */
    2,6,7, 2,7,3,  /* +Y */
    0,7,4, 0,3,7,  /* -X */
    1,5,6, 1,6,2,  /* +X */
};

/* ── Slot (one per tracked entity) ───────────────────────────────────── */

#define SLOT_EMPTY           UINT64_MAX
#define RESULT_WARM_UP       (-2)  /* never queried before */
#define RESULT_IN_FLIGHT     (-1)  /* submitted, no result yet */

typedef struct {
    uint64_t                       entity_id;     /* SLOT_EMPTY = free     */
    bgfx_occlusion_query_handle_t  query;
    int32_t                        last_result;   /* pixel count or sentinel */
    bool                           submitted;     /* query submitted this frame */
} OcclusionSlot;

/* ── Occlusion culler struct ─────────────────────────────────────────── */

struct JceOcclusionCuller {
    bool   active;         /* false = GPU cap absent, always visible      */

    /* Config. */
    uint16_t  view_id;
    int32_t   min_pixels;

    /* Hash table. */
    OcclusionSlot  *slots;
    uint32_t        cap;       /* power-of-two capacity */
    uint32_t        used;

    /* Shader for proxy draws (color program — position + color layout). */
    bgfx_program_handle_t  proxy_prog;

    /* Per-frame cached matrices (set by begin_frame). */
    float  view_mtx[16];
    float  proj_mtx[16];

    /* Vertex layout for proxy (position only). */
    bgfx_vertex_layout_t  vl;

    /* Persistent index buffer for unit cube. */
    bgfx_index_buffer_handle_t  cube_ibh;

    /* Per-frame stats. */
    JceOcclusionStats  stats;
};

/* ── Hash helpers ─────────────────────────────────────────────────────── */

static uint32_t hash_id(uint64_t id, uint32_t cap)
{
    /* Murmur-inspired finalizer */
    uint64_t h = id ^ (id >> 30);
    h *= UINT64_C(0xbf58476d1ce4e5b9);
    h ^= (h >> 27);
    h *= UINT64_C(0x94d049bb133111eb);
    h ^= (h >> 31);
    return (uint32_t)(h & (uint64_t)(cap - 1));
}

static OcclusionSlot *find_or_alloc(JceOcclusionCuller *oc, uint64_t id)
{
    uint32_t idx = hash_id(id, oc->cap);
    for (uint32_t i = 0; i < oc->cap; ++i) {
        uint32_t       slot_i = (idx + i) & (oc->cap - 1);
        OcclusionSlot *s      = &oc->slots[slot_i];
        if (s->entity_id == id)        return s;
        if (s->entity_id == SLOT_EMPTY) {
            s->entity_id  = id;
            s->last_result = RESULT_WARM_UP;
            s->submitted   = false;
            /* Allocate a bgfx occlusion query for this slot. */
            s->query = bgfx_create_occlusion_query();
            ++oc->used;
            return s;
        }
    }
    return NULL; /* table full */
}

static OcclusionSlot *find_slot(JceOcclusionCuller *oc, uint64_t id)
{
    uint32_t idx = hash_id(id, oc->cap);
    for (uint32_t i = 0; i < oc->cap; ++i) {
        uint32_t       slot_i = (idx + i) & (oc->cap - 1);
        OcclusionSlot *s      = &oc->slots[slot_i];
        if (s->entity_id == id)        return s;
        if (s->entity_id == SLOT_EMPTY) return NULL;
    }
    return NULL;
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

JceOcclusionCuller *jce_occlusion_culler_create(
    const JceOcclusionConfig *config,
    const JceShaderSet       *shaders)
{
    if (!config || !shaders) return NULL;

    JceOcclusionCuller *oc = (JceOcclusionCuller *)JCE_CALLOC(1, sizeof(*oc));
    if (!oc) return NULL;

    /* Check GPU capability via bgfx caps. */
    const bgfx_caps_t *bgfx_caps = bgfx_get_caps();
    if (!bgfx_caps || !(bgfx_caps->supported & BGFX_CAPS_OCCLUSION_QUERY)) {
        LOG_WARN(LOG_TAG,
            "occlusion queries not supported on this GPU — culler inactive");
        oc->active = false;
        return oc; /* still valid, just always-visible */
    }

    /* Round up max_entities to next power-of-two. */
    uint32_t cap = 64;
    while (cap < config->max_entities) cap <<= 1;

    oc->slots = (OcclusionSlot *)JCE_CALLOC(cap, sizeof(OcclusionSlot));
    if (!oc->slots) { JCE_FREE(oc); return NULL; }
    for (uint32_t i = 0; i < cap; ++i)
        oc->slots[i].entity_id = SLOT_EMPTY;

    oc->cap        = cap;
    oc->used       = 0;
    oc->view_id    = config->view_id;
    oc->min_pixels = config->min_pixels;
    oc->active     = true;

    /* Proxy shader: reuse the 'color' program (simple VS+FS). */
    oc->proxy_prog.idx = shaders->color.idx;

    /* Vertex layout: position only (float3). */
    bgfx_vertex_layout_begin(&oc->vl, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&oc->vl, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&oc->vl);

    /* Persistent unit-cube index buffer. */
    const bgfx_memory_t *ibm = bgfx_copy(k_cube_indices, sizeof(k_cube_indices));
    oc->cube_ibh = bgfx_create_index_buffer(ibm, BGFX_BUFFER_NONE);

    LOG_INFO(LOG_TAG,
        "occlusion culler created (cap=%u, view=%u)", cap, (unsigned)config->view_id);
    return oc;
}

void jce_occlusion_culler_destroy(JceOcclusionCuller *oc)
{
    if (!oc) return;
    if (oc->active) {
        /* Free all query handles. */
        for (uint32_t i = 0; i < oc->cap; ++i) {
            OcclusionSlot *s = &oc->slots[i];
            if (s->entity_id != SLOT_EMPTY &&
                BGFX_HANDLE_IS_VALID(s->query)) {
                bgfx_destroy_occlusion_query(s->query);
            }
        }
        if (BGFX_HANDLE_IS_VALID(oc->cube_ibh))
            bgfx_destroy_index_buffer(oc->cube_ibh);
        JCE_FREE(oc->slots);
    }
    JCE_FREE(oc);
}

/* ── Per-frame ────────────────────────────────────────────────────────── */

void jce_occlusion_culler_begin_frame(JceOcclusionCuller *oc,
                                       const float        *view_mtx,
                                       const float        *proj_mtx)
{
    if (!oc) return;
    memset(&oc->stats, 0, sizeof(oc->stats));
    if (!oc->active) return;

    if (view_mtx) memcpy(oc->view_mtx, view_mtx, 64);
    if (proj_mtx) memcpy(oc->proj_mtx, proj_mtx, 64);

    /* Poll all query results from the previous frame. */
    for (uint32_t i = 0; i < oc->cap; ++i) {
        OcclusionSlot *s = &oc->slots[i];
        if (s->entity_id == SLOT_EMPTY) continue;
        if (!s->submitted) continue;

        int32_t num_pixels = 0;
        bgfx_occlusion_query_result_t res =
            bgfx_get_result(s->query, &num_pixels);

        if (res == BGFX_OCCLUSION_QUERY_RESULT_VISIBLE ||
            res == BGFX_OCCLUSION_QUERY_RESULT_NORESULT) {
            /* Either visible or result not ready yet — keep as in-flight. */
            s->last_result = (res == BGFX_OCCLUSION_QUERY_RESULT_NORESULT)
                                 ? RESULT_IN_FLIGHT
                                 : num_pixels;
        } else {
            /* INVISIBLE */
            s->last_result = 0;
        }

        s->submitted = false;
    }
}

bool jce_occlusion_culler_entity_visible(JceOcclusionCuller *oc,
                                          uint64_t            entity_id,
                                          jce_vec3            center,
                                          float               radius)
{
    (void)center; (void)radius;
    if (!oc || !oc->active) return true;
    ++oc->stats.total_entities;

    OcclusionSlot *s = find_slot(oc, entity_id);
    if (!s) {
        /* Unknown entity or table full — allocate on submit_query(). */
        ++oc->stats.warm_up;
        return true;
    }

    if (s->last_result == RESULT_WARM_UP) {
        ++oc->stats.warm_up;
        return true;
    }
    if (s->last_result == RESULT_IN_FLIGHT) {
        /* Result not ready — treat as visible to avoid flicker. */
        ++oc->stats.no_result;
        return true;
    }
    if (s->last_result <= oc->min_pixels) {
        ++oc->stats.occluded;
        return false;
    }

    ++oc->stats.visible;
    return true;
}

/* Submit a bounding-sphere proxy draw and attach an occlusion query.
   Uses bgfx transient vertex buffer for the unit cube (no permanent alloc
   per entity). */
void jce_occlusion_culler_submit_query(JceOcclusionCuller *oc,
                                        uint64_t            entity_id,
                                        jce_vec3            center,
                                        float               radius)
{
    if (!oc || !oc->active) return;

    OcclusionSlot *s = find_or_alloc(oc, entity_id);
    if (!s) {
        LOG_WARN(LOG_TAG, "occlusion table full — increase max_entities");
        return;
    }

    /* Diameter → scale of the unit cube. */
    float dia = radius * 2.0f;

    /* Build world transform: scale by dia, translate to center. */
    float mtx[16];
    memset(mtx, 0, sizeof(mtx));
    mtx[0]  = dia;
    mtx[5]  = dia;
    mtx[10] = dia;
    mtx[12] = center.x;
    mtx[13] = center.y;
    mtx[14] = center.z;
    mtx[15] = 1.0f;

    /* Transient vertex buffer for the unit cube (position only). */
    if (bgfx_get_avail_transient_vertex_buffer(8, &oc->vl) < 8)
        return; /* transient buffer pool exhausted */
    bgfx_transient_vertex_buffer_t tvb;
    bgfx_alloc_transient_vertex_buffer(&tvb, 8, &oc->vl);
    memcpy(tvb.data, k_cube_verts, sizeof(k_cube_verts));

    bgfx_encoder_t *enc = bgfx_encoder_begin(false);
    if (!enc) return;

    bgfx_encoder_set_transform(enc, mtx, 1);
    bgfx_encoder_set_transient_vertex_buffer(enc, 0, &tvb, 0, 8);
    bgfx_encoder_set_index_buffer(enc, oc->cube_ibh, 0, 36);

    /* Depth test only — no color write, front faces only. */
    uint64_t state = BGFX_STATE_WRITE_Z
                   | BGFX_STATE_DEPTH_TEST_LEQUAL
                   | BGFX_STATE_CULL_CW;

    bgfx_encoder_set_state(enc, state, 0);
    bgfx_encoder_set_condition(enc, s->query, true /* occlude when invisible */);
    bgfx_encoder_submit(enc, oc->view_id, oc->proxy_prog, 0,
                        BGFX_DISCARD_ALL);

    bgfx_encoder_end(enc);

    s->submitted = true;
}

JceOcclusionStats jce_occlusion_culler_get_stats(const JceOcclusionCuller *oc)
{
    JceOcclusionStats empty = {0};
    return oc ? oc->stats : empty;
}

bool jce_occlusion_culler_is_active(const JceOcclusionCuller *oc)
{
    return oc && oc->active;
}
