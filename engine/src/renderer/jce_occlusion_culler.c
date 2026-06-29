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
#define RESULT_NO_QUERY      (-3)  /* no bgfx query handle (pool exhausted) */

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

    /* Scene framebuffer the proxy depth-test runs against (set each frame by
     * jce_occlusion_culler_bind_target).  UINT16_MAX = backbuffer. */
    uint16_t  target_fbo;

    /* bgfx hard cap on live occlusion queries (BGFX_CAPS limits) + a one-shot
     * warning latch when the pool is exhausted. */
    uint32_t  query_cap;
    bool      queries_exhausted_warned;
    bool      table_full_warned;   /* one-shot latch for the (now-rare) full-table warn */

    /* Hash table. */
    OcclusionSlot  *slots;
    uint32_t        cap;       /* power-of-two capacity */
    uint32_t        used;

    /* Shader for proxy draws (color program — position + color layout). */
    bgfx_program_handle_t  proxy_prog;

    /* Per-frame cached matrices (set by begin_frame). */
    float  view_mtx[16];
    float  proj_mtx[16];
    float  cam_pos[3];     /* world-space camera position, derived from view_mtx */

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

/* Grow + rehash the table 2x.  The render path is now uncapped (a scene can have
 * far more entities than the initial cap), so the occlusion table must scale too
 * — otherwise overflow entities fail to alloc and spam "table full" every frame,
 * each doing an O(cap) failed probe.  bgfx query handles ride along unchanged. */
static void oc_grow(JceOcclusionCuller *oc)
{
    uint32_t new_cap = oc->cap ? oc->cap * 2u : 64u;
    OcclusionSlot *ns = (OcclusionSlot *)JCE_CALLOC(new_cap, sizeof(OcclusionSlot));
    if (!ns) return;   /* keep the old table; alloc may still fail this frame */
    for (uint32_t i = 0; i < new_cap; ++i) ns[i].entity_id = SLOT_EMPTY;
    for (uint32_t i = 0; i < oc->cap; ++i) {
        OcclusionSlot *s = &oc->slots[i];
        if (s->entity_id == SLOT_EMPTY) continue;
        uint32_t idx = hash_id(s->entity_id, new_cap);
        while (ns[idx].entity_id != SLOT_EMPTY) idx = (idx + 1) & (new_cap - 1);
        ns[idx] = *s;
    }
    JCE_FREE(oc->slots);
    oc->slots = ns;
    oc->cap   = new_cap;
}

static OcclusionSlot *find_or_alloc(JceOcclusionCuller *oc, uint64_t id)
{
    /* Keep load < 0.75 so linear probing stays fast and the table never fills. */
    if ((oc->used + 1u) * 4u >= oc->cap * 3u) oc_grow(oc);
    uint32_t idx = hash_id(id, oc->cap);
    for (uint32_t i = 0; i < oc->cap; ++i) {
        uint32_t       slot_i = (idx + i) & (oc->cap - 1);
        OcclusionSlot *s      = &oc->slots[slot_i];
        if (s->entity_id == id)        return s;
        if (s->entity_id == SLOT_EMPTY) {
            s->entity_id  = id;
            s->last_result = RESULT_WARM_UP;
            s->submitted   = false;
            /* Allocate a bgfx occlusion query for this slot.  bgfx has a HARD
             * cap of BGFX_CONFIG_MAX_OCCLUSION_QUERIES live queries (256 by
             * default!) — far below a dense view's entity count.  Past that,
             * bgfx_create_occlusion_query() returns an INVALID handle.  Keep the
             * slot (so we don't re-attempt allocation every frame and thrash) but
             * flag it: an entity with no real query is ALWAYS treated visible —
             * never false-culled.  Occlusion thus applies to the first ~256
             * tracked entities; the rest render unconditionally (safe). */
            s->query = bgfx_create_occlusion_query();
            if (!BGFX_HANDLE_IS_VALID(s->query)) {
                s->last_result = RESULT_NO_QUERY;
                if (!oc->queries_exhausted_warned) {
                    oc->queries_exhausted_warned = true;
                    LOG_WARN(LOG_TAG,
                        "bgfx occlusion-query pool exhausted (cap %u) — extra "
                        "entities render unconditionally (no false-cull)",
                        (unsigned)oc->query_cap);
                }
            }
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

    /* bgfx's live occlusion-query pool is small (BGFX_CONFIG_MAX_OCCLUSION_
     * QUERIES, 256 by default).  Record it so the first allocation past the cap
     * warns once and those entities fall back to always-visible (see
     * find_or_alloc).  0 (older bgfx) → use the documented default. */
    oc->query_cap = bgfx_caps->limits.maxOcclusionQueries;
    if (oc->query_cap == 0) oc->query_cap = 256;

    oc->slots = (OcclusionSlot *)JCE_CALLOC(cap, sizeof(OcclusionSlot));
    if (!oc->slots) { JCE_FREE(oc); return NULL; }
    for (uint32_t i = 0; i < cap; ++i)
        oc->slots[i].entity_id = SLOT_EMPTY;

    oc->cap        = cap;
    oc->used       = 0;
    oc->view_id    = config->view_id;
    oc->min_pixels = config->min_pixels;
    oc->active     = true;
    oc->target_fbo = UINT16_MAX;  /* backbuffer until bind_target() says else */

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

    if (view_mtx) {
        memcpy(oc->view_mtx, view_mtx, 64);
        /* Camera world position = -R^T * t  (R = upper-left 3x3, t = column 3)
         * of the column-major view matrix.  entity_visible() / submit_query()
         * use it to skip the (unreliable) occlusion query for objects the camera
         * sits inside or near. */
        const float *v = view_mtx;
        oc->cam_pos[0] = -(v[0] * v[12] + v[1] * v[13] + v[2]  * v[14]);
        oc->cam_pos[1] = -(v[4] * v[12] + v[5] * v[13] + v[6]  * v[14]);
        oc->cam_pos[2] = -(v[8] * v[12] + v[9] * v[13] + v[10] * v[14]);
    }
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
    if (!oc || !oc->active) return true;
    ++oc->stats.total_entities;

    /* Camera inside/near the object's bounding volume → the occlusion query is
     * unreliable: the proxy AABB gets clipped by the near plane and/or self-
     * occludes against the geometry's OWN depth (worst for a large FLAT ground
     * the camera stands on, whose thin proxy is coincident with its surface).
     * The fragment count then hovers around min_pixels and the entity flickers
     * visible/hidden as the camera approaches — the reported terrain "屏闪".
     * Force visible there; a huge ground is itself a primary occluder and is
     * never meaningfully occluded anyway. */
    {
        const float dx = oc->cam_pos[0] - center.x;
        const float dy = oc->cam_pos[1] - center.y;
        const float dz = oc->cam_pos[2] - center.z;
        const float near_r = radius + 2.0f;   /* + near-plane safety margin */
        if (dx * dx + dy * dy + dz * dz <= near_r * near_r) {
            ++oc->stats.warm_up;   /* reads as "not occluded" */
            return true;
        }
    }

    OcclusionSlot *s = find_slot(oc, entity_id);
    if (!s) {
        /* Unknown entity or table full — allocate on submit_query(). */
        ++oc->stats.warm_up;
        return true;
    }

    if (s->last_result == RESULT_NO_QUERY) {
        /* No bgfx query handle for this entity (pool exhausted) — always draw,
         * never cull.  Counted as warm_up so it reads as "not occluded". */
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

/* Submit an AABB proxy draw and attach an occlusion query.
   Uses bgfx transient vertex buffer for the unit cube (no permanent alloc
   per entity).  The cube is non-uniformly scaled by the entity's world AABB
   half-extents (×2 = full extents) and translated to the AABB centre, so the
   proxy matches the real geometry's silhouette — not a fat/thin uniform box. */
void jce_occlusion_culler_submit_query(JceOcclusionCuller *oc,
                                        uint64_t            entity_id,
                                        jce_vec3            center,
                                        jce_vec3            half_extents)
{
    if (!oc || !oc->active) return;

    /* Mirror entity_visible's camera-proximity skip: spend no query (nor a scarce
     * query-pool slot) on an object forced visible because the camera is inside/
     * near it.  Use the largest half-extent as the radius so this matches the
     * value the caller passes to entity_visible. */
    {
        float r = half_extents.x;
        if (half_extents.y > r) r = half_extents.y;
        if (half_extents.z > r) r = half_extents.z;
        const float dx = oc->cam_pos[0] - center.x;
        const float dy = oc->cam_pos[1] - center.y;
        const float dz = oc->cam_pos[2] - center.z;
        const float near_r = r + 2.0f;
        if (dx * dx + dy * dy + dz * dz <= near_r * near_r) return;
    }

    OcclusionSlot *s = find_or_alloc(oc, entity_id);
    if (!s) {
        /* Only reachable if oc_grow hit OOM (the table auto-grows otherwise).
         * Latch so it can't flood; the entity renders unconditionally (safe). */
        if (!oc->table_full_warned) {
            oc->table_full_warned = true;
            LOG_WARN(LOG_TAG, "occlusion table grow failed (OOM) — extra entities "
                              "render unconditionally");
        }
        return;
    }
    /* No bgfx query handle (pool exhausted) → nothing to issue; the entity is
     * always-visible via entity_visible's RESULT_NO_QUERY branch. */
    if (!BGFX_HANDLE_IS_VALID(s->query)) return;

    /* Full per-axis extents → non-uniform scale of the unit cube [-0.5,0.5].
     * Clamp to a tiny floor so a perfectly flat axis (e.g. a ground plane with
     * zero Y extent) still produces a rasterisable, queryable proxy. */
    float ex = half_extents.x * 2.0f; if (ex < 0.002f) ex = 0.002f;
    float ey = half_extents.y * 2.0f; if (ey < 0.002f) ey = 0.002f;
    float ez = half_extents.z * 2.0f; if (ez < 0.002f) ez = 0.002f;

    /* Build world transform: non-uniform scale by full extents, translate to
     * the AABB centre. */
    float mtx[16];
    memset(mtx, 0, sizeof(mtx));
    mtx[0]  = ex;
    mtx[5]  = ey;
    mtx[10] = ez;
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

    /* Depth TEST only — NO depth write, NO color write, NO face cull.
     *  - WRITE_Z would corrupt the scene depth that the color pass wrote (and
     *    that later proxies test against), and would let a box occlude itself.
     *  - No color write: this is a pure visibility probe.
     *  - No face cull: when the camera is near or inside the proxy box (common
     *    for a large building's AABB) front-face-only culling would drop every
     *    triangle → zero fragments → the entity wrongly reads as occluded.  No
     *    culling guarantees the box always rasterises so the query is honest. */
    uint64_t state = BGFX_STATE_DEPTH_TEST_LEQUAL;

    bgfx_encoder_set_state(enc, state, 0);
    /* ISSUE a real hardware occlusion query (counts fragments that pass the
     * depth test against the scene depth buffer).  The previous code used
     * set_condition()+submit(), which is CONDITIONAL RENDERING — it never issued
     * a query, so bgfx_get_result() always returned NORESULT and the culler was
     * permanently inert (every entity stuck "visible").  submit_occlusion_query
     * is the API that actually records the result begin_frame() polls. */
    bgfx_encoder_submit_occlusion_query(enc, oc->view_id, oc->proxy_prog,
                                        s->query, 0, BGFX_DISCARD_ALL);

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

uint16_t jce_occlusion_culler_get_view_id(const JceOcclusionCuller *oc)
{
    return oc ? oc->view_id : 254;
}

void jce_occlusion_culler_bind_target(JceOcclusionCuller *oc,
                                      uint16_t fbo_idx,
                                      uint16_t x, uint16_t y,
                                      uint16_t w, uint16_t h)
{
    if (!oc || !oc->active) return;

    oc->target_fbo = fbo_idx;

    /* Point the proxy view at the SAME framebuffer the color/depth pass uses so
     * the depth test reads the depth the scene actually wrote.  An invalid idx
     * (UINT16_MAX) binds the backbuffer — correct for the direct-to-backbuffer
     * path. */
    bgfx_frame_buffer_handle_t fb = { fbo_idx };
    bgfx_set_view_frame_buffer(oc->view_id, fb);

    /* Match the color pass viewport so the rasterised proxies cover the same
     * pixels the depth was written at (different rect → depth test misaligned).
     * w/h == 0 leaves the rect untouched (caller already set it). */
    if (w > 0 && h > 0)
        bgfx_set_view_rect(oc->view_id, x, y, w, h);

    /* Do NOT set a view clear: the proxy pass must PRESERVE the depth the color
     * pass wrote (it tests against it).  The proxy draws are depth-test-only,
     * no color write. */
}
