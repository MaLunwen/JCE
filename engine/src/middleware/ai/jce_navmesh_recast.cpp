/*
 * jce_navmesh_recast.cpp -- Recast/Detour navmesh backend impl.
 *
 * Standard Recast bake pipeline (see RecastDemo's Sample_SoloMesh):
 *   1. rasterize triangles into a heightfield
 *   2. filter walkable spans (low-hanging obstacles, ledge spans)
 *   3. build compact heightfield + erode by agent radius
 *   4. build distance field + watershed regions
 *   5. trace contours, build polymesh + detail mesh
 *   6. wrap in a single-tile dtNavMesh
 *
 * Query path (jce_recast_find_path) uses dtNavMeshQuery::findPath +
 * findStraightPath, returning a string-pulled XZ polyline.
 */

#include <jce/middleware/ai/jce_navmesh_recast.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <recastnavigation/Recast.h>
#include <recastnavigation/DetourNavMesh.h>
#include <recastnavigation/DetourNavMeshBuilder.h>
#include <recastnavigation/DetourNavMeshQuery.h>
#include <recastnavigation/DetourStatus.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>

#define LOG_TAG "navmesh-recast"

namespace {

constexpr int  kMaxPathPolys     = 256;
constexpr int  kMaxStraightVerts = 256;
constexpr float kSnapExtents[3]  = { 2.0f, 4.0f, 2.0f };

class SilentRcCtx : public rcContext {
public:
    SilentRcCtx() : rcContext(false) {}
};

} /* anonymous */

struct JceRecastNavMesh {
    dtNavMesh        *nav        = nullptr;
    dtNavMeshQuery   *query      = nullptr;
    dtQueryFilter     filter;
    JceRecastStats    stats      = {};
};

extern "C" void jce_recast_default_config(JceRecastConfig *out_cfg)
{
    if (!out_cfg) return;
    out_cfg->cell_size             = 0.30f;
    out_cfg->cell_height           = 0.20f;
    out_cfg->walkable_slope_deg    = 45.0f;
    out_cfg->walkable_height       = 2.0f;
    out_cfg->walkable_climb        = 0.40f;
    out_cfg->walkable_radius       = 0.40f;
    out_cfg->edge_max_len          = 12.0f;
    out_cfg->edge_max_error        = 1.3f;
    out_cfg->region_min_size       = 8;
    out_cfg->region_merge_size     = 20;
    out_cfg->max_verts_per_poly    = 6;
    out_cfg->detail_sample_dist    = 6.0f;
    out_cfg->detail_sample_max_err = 1.0f;
}

static void compute_aabb(const float *verts, uint32_t vcount,
                          float bmin[3], float bmax[3])
{
    bmin[0] = bmax[0] = verts[0];
    bmin[1] = bmax[1] = verts[1];
    bmin[2] = bmax[2] = verts[2];
    for (uint32_t i = 1; i < vcount; ++i) {
        const float *v = &verts[i * 3];
        bmin[0] = std::min(bmin[0], v[0]); bmax[0] = std::max(bmax[0], v[0]);
        bmin[1] = std::min(bmin[1], v[1]); bmax[1] = std::max(bmax[1], v[1]);
        bmin[2] = std::min(bmin[2], v[2]); bmax[2] = std::max(bmax[2], v[2]);
    }
}

extern "C" JceRecastNavMesh *jce_recast_build(const float    *vertices,
                                                uint32_t        vertex_count,
                                                const uint32_t *indices,
                                                uint32_t        triangle_count,
                                                const JceRecastConfig *user_cfg)
{
    if (!vertices || vertex_count < 3 || !indices || triangle_count == 0) {
        LOG_ERROR(LOG_TAG, "invalid input mesh");
        return nullptr;
    }

    JceRecastConfig cfg;
    if (user_cfg) cfg = *user_cfg; else jce_recast_default_config(&cfg);

    auto t_start = std::chrono::steady_clock::now();

    /* Recast uses int triangle indices. */
    int *tri_indices = (int *)JCE_MALLOC(sizeof(int) * triangle_count * 3);
    if (!tri_indices) return nullptr;
    for (uint32_t i = 0; i < triangle_count * 3; ++i) {
        tri_indices[i] = (int)indices[i];
    }

    float bmin[3], bmax[3];
    compute_aabb(vertices, vertex_count, bmin, bmax);

    rcConfig rcfg;
    std::memset(&rcfg, 0, sizeof(rcfg));
    rcfg.cs                     = cfg.cell_size;
    rcfg.ch                     = cfg.cell_height;
    rcfg.walkableSlopeAngle     = cfg.walkable_slope_deg;
    rcfg.walkableHeight         = (int)std::ceil(cfg.walkable_height / rcfg.ch);
    rcfg.walkableClimb          = (int)std::floor(cfg.walkable_climb / rcfg.ch);
    rcfg.walkableRadius         = (int)std::ceil(cfg.walkable_radius / rcfg.cs);
    rcfg.maxEdgeLen             = (int)(cfg.edge_max_len / rcfg.cs);
    rcfg.maxSimplificationError = cfg.edge_max_error;
    rcfg.minRegionArea          = cfg.region_min_size * cfg.region_min_size;
    rcfg.mergeRegionArea        = cfg.region_merge_size * cfg.region_merge_size;
    rcfg.maxVertsPerPoly        = cfg.max_verts_per_poly;
    rcfg.detailSampleDist       = cfg.detail_sample_dist < 0.9f
                                  ? 0 : rcfg.cs * cfg.detail_sample_dist;
    rcfg.detailSampleMaxError   = rcfg.ch * cfg.detail_sample_max_err;
    rcVcopy(rcfg.bmin, bmin);
    rcVcopy(rcfg.bmax, bmax);
    rcCalcGridSize(rcfg.bmin, rcfg.bmax, rcfg.cs, &rcfg.width, &rcfg.height);

    SilentRcCtx ctx;

    /* 1. heightfield */
    rcHeightfield *hf = rcAllocHeightfield();
    if (!hf || !rcCreateHeightfield(&ctx, *hf, rcfg.width, rcfg.height,
                                     rcfg.bmin, rcfg.bmax, rcfg.cs, rcfg.ch)) {
        LOG_ERROR(LOG_TAG, "rcCreateHeightfield failed");
        if (hf) rcFreeHeightField(hf);
        JCE_FREE(tri_indices);
        return nullptr;
    }

    unsigned char *tri_areas = (unsigned char *)JCE_CALLOC(triangle_count, 1);
    if (!tri_areas) {
        rcFreeHeightField(hf);
        JCE_FREE(tri_indices);
        return nullptr;
    }
    rcMarkWalkableTriangles(&ctx, rcfg.walkableSlopeAngle,
                              vertices, (int)vertex_count,
                              tri_indices, (int)triangle_count, tri_areas);
    if (!rcRasterizeTriangles(&ctx, vertices, (int)vertex_count,
                                tri_indices, tri_areas, (int)triangle_count,
                                *hf, rcfg.walkableClimb)) {
        LOG_ERROR(LOG_TAG, "rcRasterizeTriangles failed");
        JCE_FREE(tri_areas);
        rcFreeHeightField(hf);
        JCE_FREE(tri_indices);
        return nullptr;
    }
    JCE_FREE(tri_areas);
    JCE_FREE(tri_indices);

    /* 2. filters */
    rcFilterLowHangingWalkableObstacles(&ctx, rcfg.walkableClimb, *hf);
    rcFilterLedgeSpans(&ctx, rcfg.walkableHeight, rcfg.walkableClimb, *hf);
    rcFilterWalkableLowHeightSpans(&ctx, rcfg.walkableHeight, *hf);

    /* 3. compact heightfield + erode */
    rcCompactHeightfield *chf = rcAllocCompactHeightfield();
    if (!chf || !rcBuildCompactHeightfield(&ctx, rcfg.walkableHeight,
                                            rcfg.walkableClimb, *hf, *chf)) {
        LOG_ERROR(LOG_TAG, "rcBuildCompactHeightfield failed");
        if (chf) rcFreeCompactHeightfield(chf);
        rcFreeHeightField(hf);
        return nullptr;
    }
    rcFreeHeightField(hf);

    if (!rcErodeWalkableArea(&ctx, rcfg.walkableRadius, *chf)) {
        LOG_ERROR(LOG_TAG, "rcErodeWalkableArea failed");
        rcFreeCompactHeightfield(chf);
        return nullptr;
    }

    /* 4. distance field + watershed */
    if (!rcBuildDistanceField(&ctx, *chf) ||
        !rcBuildRegions(&ctx, *chf, 0, rcfg.minRegionArea, rcfg.mergeRegionArea)) {
        LOG_ERROR(LOG_TAG, "rcBuildRegions failed");
        rcFreeCompactHeightfield(chf);
        return nullptr;
    }

    /* 5. contours, polymesh, detail mesh */
    rcContourSet *cset = rcAllocContourSet();
    if (!cset || !rcBuildContours(&ctx, *chf, rcfg.maxSimplificationError,
                                   rcfg.maxEdgeLen, *cset)) {
        LOG_ERROR(LOG_TAG, "rcBuildContours failed");
        if (cset) rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        return nullptr;
    }

    rcPolyMesh *pmesh = rcAllocPolyMesh();
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, rcfg.maxVertsPerPoly, *pmesh)) {
        LOG_ERROR(LOG_TAG, "rcBuildPolyMesh failed");
        if (pmesh) rcFreePolyMesh(pmesh);
        rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        return nullptr;
    }

    rcPolyMeshDetail *dmesh = rcAllocPolyMeshDetail();
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf,
                                          rcfg.detailSampleDist,
                                          rcfg.detailSampleMaxError, *dmesh)) {
        LOG_ERROR(LOG_TAG, "rcBuildPolyMeshDetail failed");
        if (dmesh) rcFreePolyMeshDetail(dmesh);
        rcFreePolyMesh(pmesh);
        rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        return nullptr;
    }
    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(cset);

    /* Mark every poly as walkable area + flag (Detour ignores areas
     * with flag 0). */
    for (int i = 0; i < pmesh->npolys; ++i) {
        if (pmesh->areas[i] == RC_WALKABLE_AREA) {
            pmesh->areas[i] = 0;
        }
        pmesh->flags[i] = 1;
    }

    /* 6. dtNavMesh */
    dtNavMeshCreateParams np;
    std::memset(&np, 0, sizeof(np));
    np.verts            = pmesh->verts;
    np.vertCount        = pmesh->nverts;
    np.polys            = pmesh->polys;
    np.polyAreas        = pmesh->areas;
    np.polyFlags        = pmesh->flags;
    np.polyCount        = pmesh->npolys;
    np.nvp              = pmesh->nvp;
    np.detailMeshes     = dmesh->meshes;
    np.detailVerts      = dmesh->verts;
    np.detailVertsCount = dmesh->nverts;
    np.detailTris       = dmesh->tris;
    np.detailTriCount   = dmesh->ntris;
    np.walkableHeight   = cfg.walkable_height;
    np.walkableRadius   = cfg.walkable_radius;
    np.walkableClimb    = cfg.walkable_climb;
    rcVcopy(np.bmin, pmesh->bmin);
    rcVcopy(np.bmax, pmesh->bmax);
    np.cs               = rcfg.cs;
    np.ch               = rcfg.ch;
    np.buildBvTree      = true;

    unsigned char *nav_data       = nullptr;
    int            nav_data_size  = 0;
    if (!dtCreateNavMeshData(&np, &nav_data, &nav_data_size)) {
        LOG_ERROR(LOG_TAG, "dtCreateNavMeshData failed");
        rcFreePolyMeshDetail(dmesh);
        rcFreePolyMesh(pmesh);
        return nullptr;
    }

    JceRecastNavMesh *out = new (std::nothrow) JceRecastNavMesh();
    if (!out) {
        dtFree(nav_data);
        rcFreePolyMeshDetail(dmesh);
        rcFreePolyMesh(pmesh);
        return nullptr;
    }

    out->nav = dtAllocNavMesh();
    if (!out->nav || dtStatusFailed(out->nav->init(nav_data, nav_data_size,
                                                    DT_TILE_FREE_DATA))) {
        LOG_ERROR(LOG_TAG, "dtNavMesh::init failed");
        if (out->nav) dtFreeNavMesh(out->nav); else dtFree(nav_data);
        delete out;
        rcFreePolyMeshDetail(dmesh);
        rcFreePolyMesh(pmesh);
        return nullptr;
    }

    out->query = dtAllocNavMeshQuery();
    if (!out->query || dtStatusFailed(out->query->init(out->nav, 2048))) {
        LOG_ERROR(LOG_TAG, "dtNavMeshQuery::init failed");
        if (out->query) dtFreeNavMeshQuery(out->query);
        dtFreeNavMesh(out->nav);
        delete out;
        rcFreePolyMeshDetail(dmesh);
        rcFreePolyMesh(pmesh);
        return nullptr;
    }

    out->filter.setIncludeFlags(0xffff);
    out->filter.setExcludeFlags(0);

    out->stats.polygon_count         = pmesh->npolys;
    out->stats.vertex_count          = pmesh->nverts;
    out->stats.detail_triangle_count = dmesh->ntris;

    rcFreePolyMeshDetail(dmesh);
    rcFreePolyMesh(pmesh);

    auto t_end = std::chrono::steady_clock::now();
    out->stats.build_time_ms = (int)std::chrono::duration_cast<
        std::chrono::milliseconds>(t_end - t_start).count();

    LOG_INFO(LOG_TAG, "built navmesh: %d polys, %d verts, %d ms",
                  out->stats.polygon_count, out->stats.vertex_count,
                  out->stats.build_time_ms);
    return out;
}

extern "C" void jce_recast_destroy(JceRecastNavMesh *nm)
{
    if (!nm) return;
    if (nm->query) dtFreeNavMeshQuery(nm->query);
    if (nm->nav)   dtFreeNavMesh(nm->nav);
    delete nm;
}

extern "C" int jce_recast_find_path(const JceRecastNavMesh *nm,
                                      float start_x, float start_z,
                                      float goal_x,  float goal_z,
                                      float *out_xz, int max_pairs)
{
    if (!nm || !nm->query || !out_xz || max_pairs <= 0) return 0;

    const float start_pos[3] = { start_x, 0.0f, start_z };
    const float goal_pos[3]  = { goal_x,  0.0f, goal_z  };

    dtPolyRef start_ref = 0, goal_ref = 0;
    float     start_nearest[3], goal_nearest[3];

    nm->query->findNearestPoly(start_pos, kSnapExtents, &nm->filter,
                                &start_ref, start_nearest);
    nm->query->findNearestPoly(goal_pos,  kSnapExtents, &nm->filter,
                                &goal_ref,  goal_nearest);
    if (!start_ref || !goal_ref) return 0;

    dtPolyRef path_polys[kMaxPathPolys];
    int       path_poly_count = 0;
    dtStatus  st = nm->query->findPath(start_ref, goal_ref,
                                         start_nearest, goal_nearest,
                                         &nm->filter,
                                         path_polys, &path_poly_count,
                                         kMaxPathPolys);
    if (dtStatusFailed(st) || path_poly_count == 0) return 0;

    float         straight[kMaxStraightVerts * 3];
    unsigned char straight_flags[kMaxStraightVerts];
    dtPolyRef     straight_refs[kMaxStraightVerts];
    int           straight_count = 0;

    st = nm->query->findStraightPath(start_nearest, goal_nearest,
                                       path_polys, path_poly_count,
                                       straight, straight_flags, straight_refs,
                                       &straight_count, kMaxStraightVerts, 0);
    if (dtStatusFailed(st) || straight_count == 0) return 0;

    /* Drop the start vertex (matches grid backend convention: the
     * agent is already there).  If only the start was returned,
     * fall back to the goal as a single waypoint. */
    int begin = (straight_count > 1) ? 1 : 0;
    int n     = std::min(straight_count - begin, max_pairs);
    for (int i = 0; i < n; ++i) {
        out_xz[i * 2 + 0] = straight[(begin + i) * 3 + 0];
        out_xz[i * 2 + 1] = straight[(begin + i) * 3 + 2];
    }
    return n;
}

extern "C" int jce_recast_path_fn(void *user,
                                    float start_x, float start_z,
                                    float goal_x,  float goal_z,
                                    float *out_xz, int max_pairs)
{
    return jce_recast_find_path((const JceRecastNavMesh *)user,
                                  start_x, start_z, goal_x, goal_z,
                                  out_xz, max_pairs);
}

extern "C" bool jce_recast_snap_to_navmesh(const JceRecastNavMesh *nm,
                                             float wx, float wz,
                                             float *out_x, float *out_y, float *out_z)
{
    if (!nm || !nm->query) return false;
    const float pos[3] = { wx, 0.0f, wz };
    dtPolyRef ref = 0;
    float nearest[3];
    nm->query->findNearestPoly(pos, kSnapExtents, &nm->filter, &ref, nearest);
    if (!ref) return false;
    if (out_x) *out_x = nearest[0];
    if (out_y) *out_y = nearest[1];
    if (out_z) *out_z = nearest[2];
    return true;
}

extern "C" void jce_recast_get_stats(const JceRecastNavMesh *nm,
                                       JceRecastStats *out_stats)
{
    if (!out_stats) return;
    if (!nm) { std::memset(out_stats, 0, sizeof(*out_stats)); return; }
    *out_stats = nm->stats;
}
