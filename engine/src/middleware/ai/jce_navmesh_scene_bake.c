/*
 * jce_navmesh_scene_bake.c  Scene → Recast navmesh.
 *
 * Walks scene colliders, generates a triangle soup, and forwards to
 * jce_recast_build().  Box/Sphere/Capsule colliders are tessellated
 * here; MeshCollider integration is intentionally out of scope for
 * this first cut (would require asset-system mesh resolution).
 */

#include <jce/middleware/ai/jce_navmesh_scene_bake.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "navmesh-bake"

typedef struct {
    float   *verts;          /* xyz triples */
    uint32_t  vert_count;
    uint32_t  vert_cap;
    uint32_t *idx;           /* triangle indices */
    uint32_t  tri_count;
    uint32_t  tri_cap;
} TriSoup;

static bool soup_reserve_verts(TriSoup *s, uint32_t additional)
{
    uint32_t need = s->vert_count + additional;
    if (need <= s->vert_cap) return true;
    uint32_t cap = s->vert_cap ? s->vert_cap * 2u : 256u;
    while (cap < need) cap *= 2u;
    float *p = (float *)JCE_REALLOC(s->verts, cap * 3u * sizeof(float));
    if (!p) return false;
    s->verts   = p;
    s->vert_cap = cap;
    return true;
}

static bool soup_reserve_tris(TriSoup *s, uint32_t additional)
{
    uint32_t need = s->tri_count + additional;
    if (need <= s->tri_cap) return true;
    uint32_t cap = s->tri_cap ? s->tri_cap * 2u : 256u;
    while (cap < need) cap *= 2u;
    uint32_t *p = (uint32_t *)JCE_REALLOC(s->idx, cap * 3u * sizeof(uint32_t));
    if (!p) return false;
    s->idx     = p;
    s->tri_cap = cap;
    return true;
}

static uint32_t push_v(TriSoup *s, float x, float y, float z)
{
    soup_reserve_verts(s, 1);
    uint32_t i = s->vert_count;
    s->verts[i*3+0] = x;
    s->verts[i*3+1] = y;
    s->verts[i*3+2] = z;
    s->vert_count++;
    return i;
}

static void push_tri(TriSoup *s, uint32_t a, uint32_t b, uint32_t c)
{
    soup_reserve_tris(s, 1);
    s->idx[s->tri_count*3+0] = a;
    s->idx[s->tri_count*3+1] = b;
    s->idx[s->tri_count*3+2] = c;
    s->tri_count++;
}

/* World transform of a vec3 by (translation, rotation, scale).  Box
 * colliders honour transform; we approximate rotation by quaternion. */
static void xform_point(jce_vec3 in, const JceTransform *t, float out[3])
{
    /* scale */
    float sx = in.x * t->scale.x;
    float sy = in.y * t->scale.y;
    float sz = in.z * t->scale.z;
    /* rotate q*v*conj(q) */
    float qx = t->rotation.x, qy = t->rotation.y, qz = t->rotation.z, qw = t->rotation.w;
    float ix =  qw*sx + qy*sz - qz*sy;
    float iy =  qw*sy + qz*sx - qx*sz;
    float iz =  qw*sz + qx*sy - qy*sx;
    float iw = -qx*sx - qy*sy - qz*sz;
    float rx = ix*qw + iw*(-qx) + iy*(-qz) - iz*(-qy);
    float ry = iy*qw + iw*(-qy) + iz*(-qx) - ix*(-qz);
    float rz = iz*qw + iw*(-qz) + ix*(-qy) - iy*(-qx);
    out[0] = rx + t->position.x;
    out[1] = ry + t->position.y;
    out[2] = rz + t->position.z;
}

static void emit_box(TriSoup *s, jce_vec3 center, jce_vec3 half_ext,
                     const JceTransform *xf)
{
    /* 8 corners. */
    jce_vec3 corners[8] = {
        { center.x - half_ext.x, center.y - half_ext.y, center.z - half_ext.z },
        { center.x + half_ext.x, center.y - half_ext.y, center.z - half_ext.z },
        { center.x + half_ext.x, center.y - half_ext.y, center.z + half_ext.z },
        { center.x - half_ext.x, center.y - half_ext.y, center.z + half_ext.z },
        { center.x - half_ext.x, center.y + half_ext.y, center.z - half_ext.z },
        { center.x + half_ext.x, center.y + half_ext.y, center.z - half_ext.z },
        { center.x + half_ext.x, center.y + half_ext.y, center.z + half_ext.z },
        { center.x - half_ext.x, center.y + half_ext.y, center.z + half_ext.z },
    };
    uint32_t base = s->vert_count;
    for (int i = 0; i < 8; ++i) {
        float w[3];
        xform_point(corners[i], xf, w);
        push_v(s, w[0], w[1], w[2]);
    }
    /* 12 triangles, CCW outward. */
    static const uint32_t tris[12][3] = {
        {0,1,2},{0,2,3},   /* bottom (-Y) */
        {4,6,5},{4,7,6},   /* top    (+Y) */
        {0,4,5},{0,5,1},   /* front  (-Z) */
        {1,5,6},{1,6,2},   /* right  (+X) */
        {2,6,7},{2,7,3},   /* back   (+Z) */
        {3,7,4},{3,4,0},   /* left   (-X) */
    };
    for (int i = 0; i < 12; ++i)
        push_tri(s, base+tris[i][0], base+tris[i][1], base+tris[i][2]);
}

static void emit_sphere(TriSoup *s, jce_vec3 center, float radius,
                        const JceTransform *xf, uint32_t segments)
{
    if (segments < 6) segments = 6;
    const uint32_t lat = segments / 2u;
    const uint32_t lng = segments;
    uint32_t base = s->vert_count;
    for (uint32_t i = 0; i <= lat; ++i) {
        float v = (float)i / (float)lat;
        float phi = (v - 0.5f) * 3.14159265f;
        float cy = sinf(phi);
        float cr = cosf(phi);
        for (uint32_t j = 0; j <= lng; ++j) {
            float u = (float)j / (float)lng;
            float th = u * 6.2831853f;
            jce_vec3 p = {
                center.x + radius * cr * cosf(th),
                center.y + radius * cy,
                center.z + radius * cr * sinf(th)
            };
            float w[3];
            xform_point(p, xf, w);
            push_v(s, w[0], w[1], w[2]);
        }
    }
    for (uint32_t i = 0; i < lat; ++i) {
        for (uint32_t j = 0; j < lng; ++j) {
            uint32_t a = base + i*(lng+1) + j;
            uint32_t b = base + (i+1)*(lng+1) + j;
            push_tri(s, a, b, a+1);
            push_tri(s, b, b+1, a+1);
        }
    }
}

static void emit_capsule(TriSoup *s, jce_vec3 center, float radius,
                         float height, const JceTransform *xf,
                         uint32_t segments)
{
    /* Approximation: a sphere stretched along Y by (height/2 - radius).
     * For pathfinding obstacle representation this is a fine
     * tessellation — Recast voxelises so exact geometry is irrelevant. */
    if (height < 2.0f * radius) height = 2.0f * radius;
    float half_cyl = (height - 2.0f * radius) * 0.5f;

    /* Two hemispheres at ±half_cyl. */
    jce_vec3 top    = { center.x, center.y + half_cyl, center.z };
    jce_vec3 bot    = { center.x, center.y - half_cyl, center.z };
    emit_sphere(s, top, radius, xf, segments);
    emit_sphere(s, bot, radius, xf, segments);

    /* Cylindrical band. */
    if (segments < 6) segments = 6;
    uint32_t base = s->vert_count;
    for (uint32_t j = 0; j <= segments; ++j) {
        float u = (float)j / (float)segments;
        float th = u * 6.2831853f;
        jce_vec3 p_lo = { center.x + radius * cosf(th),
                          center.y - half_cyl,
                          center.z + radius * sinf(th) };
        jce_vec3 p_hi = { center.x + radius * cosf(th),
                          center.y + half_cyl,
                          center.z + radius * sinf(th) };
        float w[3];
        xform_point(p_lo, xf, w); push_v(s, w[0], w[1], w[2]);
        xform_point(p_hi, xf, w); push_v(s, w[0], w[1], w[2]);
    }
    for (uint32_t j = 0; j < segments; ++j) {
        uint32_t a = base + j*2u;
        uint32_t b = base + (j+1)*2u;
        push_tri(s, a, b,   a+1);
        push_tri(s, b, b+1, a+1);
    }
}

/* Iter context that the entity callback writes into. */
typedef struct {
    TriSoup        *soup;
    const JceNavmeshSceneBakeDesc *desc;
    JceScene       *scene;
} BakeCtx;

static void per_entity(JceScene *s, JceEntity e, void *ud)
{
    BakeCtx *ctx = (BakeCtx *)ud;

    JceTransform *t = jce_scene_get_transform(s, e);
    if (!t) return;

    /* Box collider. */
    JceBoxColliderComponent *box = jce_scene_get_box_collider(s, e);
    if (box) {
        jce_vec3 c = { box->center[0], box->center[1], box->center[2] };
        jce_vec3 he = { box->size[0]*0.5f, box->size[1]*0.5f, box->size[2]*0.5f };
        emit_box(ctx->soup, c, he, t);
    }
    JceSphereColliderComponent *sph = jce_scene_get_sphere_collider(s, e);
    if (sph) {
        jce_vec3 c = { sph->center[0], sph->center[1], sph->center[2] };
        emit_sphere(ctx->soup, c, sph->radius, t, ctx->desc->sphere_segments);
    }
    JceCapsuleColliderComponent *cap = jce_scene_get_capsule_collider(s, e);
    if (cap) {
        jce_vec3 c = { cap->center[0], cap->center[1], cap->center[2] };
        emit_capsule(ctx->soup, c, cap->radius, cap->height, t,
                     ctx->desc->sphere_segments);
    }
}

JceRecastNavMesh *jce_navmesh_bake_scene(JceScene *scene,
                                          const JceNavmeshSceneBakeDesc *desc)
{
    if (!scene || !desc) return NULL;

    JceNavmeshSceneBakeDesc d = *desc;
    if (d.sphere_segments == 0u) d.sphere_segments = 12u;

    TriSoup soup = {0};
    BakeCtx ctx  = { &soup, &d, scene };
    jce_scene_each_entity(scene, per_entity, &ctx);

    if (soup.tri_count == 0u) {
        LOG_WARN(LOG_TAG, "no collider geometry found in scene");
        JCE_FREE(soup.verts);
        JCE_FREE(soup.idx);
        return NULL;
    }

    LOG_INFO(LOG_TAG, "bake input: %u vertices, %u triangles",
             soup.vert_count, soup.tri_count);

    JceRecastNavMesh *nm = jce_recast_build(soup.verts, soup.vert_count,
                                            soup.idx,   soup.tri_count,
                                            &d.recast);
    JCE_FREE(soup.verts);
    JCE_FREE(soup.idx);

    if (!nm) {
        LOG_ERROR(LOG_TAG, "Recast build failed");
        return NULL;
    }

    /* Off-mesh links currently not supported by jce_recast_build's
     * config surface; logged for visibility so callers know they were
     * received but not applied yet. */
    if (d.link_count > 0u && d.links) {
        LOG_INFO(LOG_TAG, "bake received %u off-mesh links — runtime "
                 "application requires Recast off-mesh extension (TODO)",
                 d.link_count);
    }

    return nm;
}
