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
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_defs.h>   /* JCE_ARCH_* for the navmesh ABI tag */

#include "os/core/jce_memory.h"

#include <recastnavigation/Recast.h>
#include <recastnavigation/DetourAlloc.h>
#include <recastnavigation/DetourNavMesh.h>
#include <recastnavigation/DetourNavMeshBuilder.h>
#include <recastnavigation/DetourNavMeshQuery.h>
#include <recastnavigation/DetourStatus.h>
#include <recastnavigation/DetourCommon.h>
#include <recastnavigation/DetourTileCache.h>
#include <recastnavigation/DetourTileCacheBuilder.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
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

    /* Dynamic-obstacle path (jce_recast_build_tiled).  nullptr for a static
     * navmesh built via jce_recast_build / jce_recast_load_file.  The cache
     * holds raw pointers to its three policy objects, so they live here and
     * are freed (after the cache) in jce_recast_destroy. */
    dtTileCache              *tile_cache  = nullptr;
    dtTileCacheAlloc         *tc_alloc    = nullptr;
    dtTileCacheCompressor    *tc_comp     = nullptr;
    dtTileCacheMeshProcess   *tc_proc     = nullptr;
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

/* Run the full Recast voxelisation + Detour build pipeline on a
 * triangle soup and return the raw, malloc'd dtNavMesh tile blob (via
 * dtAlloc).  The caller owns the returned blob and must free it with
 * dtFree (or hand it to dtNavMesh::init with DT_TILE_FREE_DATA).
 * Returns nullptr on failure; on success *out_size / *out_stats are
 * filled. */
static unsigned char *recast_build_navdata(const float    *vertices,
                                            uint32_t        vertex_count,
                                            const uint32_t *indices,
                                            uint32_t        triangle_count,
                                            const JceRecastConfig *user_cfg,
                                            int            *out_size,
                                            JceRecastStats *out_stats)
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

    if (out_stats) {
        out_stats->polygon_count         = pmesh->npolys;
        out_stats->vertex_count          = pmesh->nverts;
        out_stats->detail_triangle_count = dmesh->ntris;
        auto t_end = std::chrono::steady_clock::now();
        out_stats->build_time_ms = (int)std::chrono::duration_cast<
            std::chrono::milliseconds>(t_end - t_start).count();
    }

    rcFreePolyMeshDetail(dmesh);
    rcFreePolyMesh(pmesh);

    if (out_size) *out_size = nav_data_size;
    LOG_INFO(LOG_TAG, "built navmesh tile: %d polys, %d verts, %d bytes",
                  pmesh->npolys, pmesh->nverts, nav_data_size);
    return nav_data;
}

/* Wrap a raw dtNavMesh tile blob into a query-ready JceRecastNavMesh.
 * Takes ownership of `nav_data` (frees it on failure); on success the
 * dtNavMesh owns it via DT_TILE_FREE_DATA. */
static JceRecastNavMesh *make_navmesh_from_data(unsigned char *nav_data,
                                                int            nav_data_size)
{
    if (!nav_data || nav_data_size <= 0) {
        if (nav_data) dtFree(nav_data);
        return nullptr;
    }

    JceRecastNavMesh *out = new (std::nothrow) JceRecastNavMesh();
    if (!out) { dtFree(nav_data); return nullptr; }

    out->nav = dtAllocNavMesh();
    if (!out->nav || dtStatusFailed(out->nav->init(nav_data, nav_data_size,
                                                    DT_TILE_FREE_DATA))) {
        LOG_ERROR(LOG_TAG, "dtNavMesh::init failed");
        if (out->nav) dtFreeNavMesh(out->nav); else dtFree(nav_data);
        delete out;
        return nullptr;
    }

    out->query = dtAllocNavMeshQuery();
    if (!out->query || dtStatusFailed(out->query->init(out->nav, 2048))) {
        LOG_ERROR(LOG_TAG, "dtNavMeshQuery::init failed");
        if (out->query) dtFreeNavMeshQuery(out->query);
        dtFreeNavMesh(out->nav);
        delete out;
        return nullptr;
    }

    out->filter.setIncludeFlags(0xffff);
    out->filter.setExcludeFlags(0);

    /* Populate stats by summing tile headers — so a navmesh produced by
     * jce_recast_load_file (no live build) still reports poly/vert counts. */
    for (int i = 0; i < out->nav->getMaxTiles(); ++i) {
        const dtMeshTile *tile = ((const dtNavMesh *)out->nav)->getTile(i);
        if (tile && tile->header) {
            out->stats.polygon_count         += tile->header->polyCount;
            out->stats.vertex_count          += tile->header->vertCount;
            out->stats.detail_triangle_count += tile->header->detailTriCount;
        }
    }
    return out;
}

extern "C" JceRecastNavMesh *jce_recast_build(const float    *vertices,
                                                uint32_t        vertex_count,
                                                const uint32_t *indices,
                                                uint32_t        triangle_count,
                                                const JceRecastConfig *user_cfg)
{
    JceRecastStats stats = {};
    int nav_data_size = 0;
    unsigned char *nav_data = recast_build_navdata(vertices, vertex_count,
                                                   indices, triangle_count,
                                                   user_cfg, &nav_data_size,
                                                   &stats);
    if (!nav_data) return nullptr;

    JceRecastNavMesh *out = make_navmesh_from_data(nav_data, nav_data_size);
    if (!out) return nullptr;   /* nav_data already freed by helper */
    out->stats = stats;

    LOG_INFO(LOG_TAG, "built navmesh: %d polys, %d verts, %d ms",
                  out->stats.polygon_count, out->stats.vertex_count,
                  out->stats.build_time_ms);
    return out;
}

/* On-disk format: 16-byte header + raw dtNavMesh tile blob.
 *   u32 magic ('JNAV') | u32 version (1) | u32 tile_size | u32 reserved */
#define JCE_RECAST_FILE_MAGIC   0x56414E4Au   /* 'J' 'N' 'A' 'V' (LE) */
#define JCE_RECAST_FILE_VERSION 1u

/* Compact ABI tag for the raw (host-layout) Detour blob.  The blob is the
 * verbatim output of dtCreateNavMeshData — pointer size, struct padding and
 * byte order are host-specific, so a navmesh baked on one architecture must
 * not be fed to dtNavMesh::init on another.  We stamp this tag into the
 * reserved header word and refuse a mismatch on load so the caller rebuilds
 * instead of crashing on a corrupt blob.  Cross-endian files are additionally
 * caught by the magic word's byte order.  Legacy files store 0 here and are
 * accepted host-assumed for back-compat (audit Round-3 P3-D). */
static uint32_t jce_recast_layout_tag(void)
{
    uint32_t arch =
#if JCE_ARCH_X64
        1u;
#elif JCE_ARCH_ARM64
        2u;
#elif JCE_ARCH_X86
        3u;
#elif JCE_ARCH_ARM
        4u;
#else
        0xFEu;
#endif
    const uint16_t probe  = 1u;
    const uint32_t little = (*(const uint8_t *)&probe) ? 1u : 0u;
    /* byte 0: arch | byte 1: sizeof(void*) | byte 2: endianness | byte 3: 1
     * (tag-scheme version; keeps the whole word non-zero so it is always
     * distinguishable from a legacy 0). */
    return (arch & 0xFFu)
         | ((uint32_t)(sizeof(void *) & 0xFFu) << 8)
         | (little << 16)
         | (1u << 24);
}

extern "C" bool jce_recast_build_to_file(const char     *path,
                                          const float    *vertices,
                                          uint32_t        vertex_count,
                                          const uint32_t *indices,
                                          uint32_t        triangle_count,
                                          const JceRecastConfig *cfg,
                                          JceRecastStats *out_stats)
{
    if (!path || !path[0]) {
        LOG_ERROR(LOG_TAG, "build_to_file: empty path");
        return false;
    }

    JceRecastStats stats = {};
    int nav_data_size = 0;
    unsigned char *nav_data = recast_build_navdata(vertices, vertex_count,
                                                   indices, triangle_count,
                                                   cfg, &nav_data_size, &stats);
    if (!nav_data) return false;

    uint32_t header[4] = {
        JCE_RECAST_FILE_MAGIC, JCE_RECAST_FILE_VERSION,
        (uint32_t)nav_data_size, jce_recast_layout_tag()
    };
    /* Serialise header + nav blob into one buffer and write it through the
       engine host-FS wrapper (portable, creates parent dirs) instead of raw
       stdio, so navmesh I/O works on every platform backend. */
    const size_t total = sizeof(header) + (size_t)nav_data_size;
    unsigned char *blob = (unsigned char *)JCE_MALLOC(total);
    if (!blob) {
        LOG_ERROR(LOG_TAG, "build_to_file: OOM (%zu bytes) for '%s'", total, path);
        dtFree(nav_data);
        return false;
    }
    std::memcpy(blob, header, sizeof(header));
    std::memcpy(blob + sizeof(header), nav_data, (size_t)nav_data_size);
    dtFree(nav_data);

    bool ok = jce_fs_host_write_all(path, blob, (uint64_t)total);
    JCE_FREE(blob);

    if (!ok) {
        LOG_ERROR(LOG_TAG, "build_to_file: write failed '%s'", path);
        return false;
    }
    if (out_stats) *out_stats = stats;
    LOG_INFO(LOG_TAG, "wrote navmesh '%s' (%d bytes, %d polys)",
                  path, nav_data_size, stats.polygon_count);
    return true;
}

extern "C" JceRecastNavMesh *jce_recast_load_file(const char *path)
{
    if (!path || !path[0]) return nullptr;
    /* Read the whole file through the engine host-FS wrapper (portable) and
       parse the header in memory, replacing raw stdio. */
    uint64_t file_size = 0;
    unsigned char *blob =
        (unsigned char *)jce_fs_host_read_all(path, &file_size);
    if (!blob) {
        LOG_WARN(LOG_TAG, "load_file: cannot read '%s'", path);
        return nullptr;
    }
    uint32_t header[4] = { 0, 0, 0, 0 };
    if (file_size < sizeof(header)) {
        LOG_WARN(LOG_TAG, "load_file: bad header '%s'", path);
        jce_fs_buffer_free(blob);
        return nullptr;
    }
    std::memcpy(header, blob, sizeof(header));
    if (header[0] != JCE_RECAST_FILE_MAGIC ||
        header[1] != JCE_RECAST_FILE_VERSION ||
        header[2] == 0) {
        LOG_WARN(LOG_TAG, "load_file: bad header '%s'", path);
        jce_fs_buffer_free(blob);
        return nullptr;
    }
    /* The Detour blob is host-ABI specific; refuse a navmesh baked for a
     * different architecture so we rebuild rather than feed dtNavMesh::init a
     * mismatched blob (audit Round-3 P3-D).  header[3]==0 = legacy untagged,
     * accepted host-assumed. */
    if (header[3] != 0u && header[3] != jce_recast_layout_tag()) {
        LOG_WARN(LOG_TAG, "load_file: navmesh '%s' baked for a different "
                 "architecture (tag 0x%08x != 0x%08x) — rebuild required",
                 path, header[3], jce_recast_layout_tag());
        jce_fs_buffer_free(blob);
        return nullptr;
    }
    int tile_size = (int)header[2];
    if (file_size < sizeof(header) + (uint64_t)tile_size) {
        LOG_WARN(LOG_TAG, "load_file: truncated '%s'", path);
        jce_fs_buffer_free(blob);
        return nullptr;
    }
    /* dtNavMesh::init takes ownership of a dtAlloc'd blob (DT_TILE_FREE_DATA). */
    unsigned char *nav_data =
        (unsigned char *)dtAlloc((size_t)tile_size, DT_ALLOC_PERM);
    if (!nav_data) { jce_fs_buffer_free(blob); return nullptr; }
    std::memcpy(nav_data, blob + sizeof(header), (size_t)tile_size);
    jce_fs_buffer_free(blob);

    JceRecastNavMesh *nm = make_navmesh_from_data(nav_data, tile_size);
    if (nm) {
        LOG_INFO(LOG_TAG, "loaded navmesh '%s' (%d bytes)", path, tile_size);
    }
    return nm;
}

extern "C" void jce_recast_destroy(JceRecastNavMesh *nm)
{
    if (!nm) return;
    /* Free the tile cache before the navmesh: the cache holds compressed tile
     * blobs (its own memory) but does not own the dtNavMesh tiles.  The policy
     * objects must be freed after the cache (it dereferences them on teardown). */
    if (nm->tile_cache) dtFreeTileCache(nm->tile_cache);
    delete nm->tc_proc;
    delete nm->tc_comp;
    delete nm->tc_alloc;
    if (nm->query)      dtFreeNavMeshQuery(nm->query);
    if (nm->nav)        dtFreeNavMesh(nm->nav);
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

/* Resolve the surface height of `pos` on polygon `ref`.  Returns the
 * getPolyHeight result on success; otherwise leaves *out_y at its
 * caller-supplied fallback.  When `ref` is 0 (no polyRef for this point),
 * snaps `pos` to the nearest walkable polygon and probes that instead, so
 * a waypoint always gets a real navmesh Y rather than the straight-path
 * interpolation. */
static void recast_resolve_waypoint_y(const JceRecastNavMesh *nm,
                                      dtPolyRef ref,
                                      const float pos[3],
                                      float *out_y)
{
    float h = *out_y;
    if (ref) {
        if (dtStatusSucceed(nm->query->getPolyHeight(ref, pos, &h))) {
            *out_y = h;
            return;
        }
    }
    /* No usable ref (or getPolyHeight failed) — find the poly under this
     * point in 3D and probe its height. */
    dtPolyRef near_ref = 0;
    float     nearest[3];
    if (dtStatusSucceed(nm->query->findNearestPoly(pos, kSnapExtents,
                                                    &nm->filter,
                                                    &near_ref, nearest))
        && near_ref) {
        if (dtStatusSucceed(nm->query->getPolyHeight(near_ref, pos, &h))) {
            *out_y = h;
            return;
        }
        /* getPolyHeight can fail if pos is outside the poly's xz bounds;
         * the snapped nearest point's Y is the best remaining estimate. */
        *out_y = nearest[1];
    }
}

extern "C" int jce_recast_find_path_3d(const JceRecastNavMesh *nm,
                                       float start_x, float start_y, float start_z,
                                       float goal_x,  float goal_y,  float goal_z,
                                       float *out_xyz, int max_pts)
{
    if (!nm || !nm->query || !out_xyz || max_pts <= 0) return 0;

    /* Honour the caller's Y — findNearestPoly searches a 3D box around the
     * query point, so feeding the real Y picks the correct floor in a
     * multi-level navmesh (the 2D variant forces Y=0 and would snap to the
     * wrong floor). */
    const float start_pos[3] = { start_x, start_y, start_z };
    const float goal_pos[3]  = { goal_x,  goal_y,  goal_z  };

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

    /* Drop the start vertex (matches the 2D convention: the agent is
     * already there).  If only the start was returned, fall back to the
     * goal as a single waypoint. */
    int begin = (straight_count > 1) ? 1 : 0;
    int n     = std::min(straight_count - begin, max_pts);
    for (int i = 0; i < n; ++i) {
        const int   src = begin + i;
        const float pos[3] = { straight[src * 3 + 0],
                               straight[src * 3 + 1],
                               straight[src * 3 + 2] };
        /* Start from the straight-path height as the fallback, then refine
         * to the true surface height of the polygon this waypoint lies on.
         * straight_refs[k] is the poly entered at point k (0 for the goal
         * vertex per Detour's contract, which the resolver handles). */
        float y = pos[1];
        recast_resolve_waypoint_y(nm, straight_refs[src], pos, &y);

        out_xyz[i * 3 + 0] = pos[0];
        out_xyz[i * 3 + 1] = y;
        out_xyz[i * 3 + 2] = pos[2];
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

extern "C" int jce_recast_debug_edges(const JceRecastNavMesh *nm,
                                        JceRecastEdgeFn fn, void *user)
{
    if (!nm || !nm->nav || !fn) return 0;
    int emitted = 0;
    for (int t = 0; t < nm->nav->getMaxTiles(); ++t) {
        const dtMeshTile *tile = ((const dtNavMesh *)nm->nav)->getTile(t);
        if (!tile || !tile->header) continue;
        for (int p = 0; p < tile->header->polyCount; ++p) {
            const dtPoly *poly = &tile->polys[p];
            if (poly->getType() != DT_POLYTYPE_GROUND) continue;
            for (int j = 0; j < (int)poly->vertCount; ++j) {
                const float *a = &tile->verts[poly->verts[j] * 3];
                const float *b = &tile->verts[poly->verts[(j + 1) % poly->vertCount] * 3];
                fn(user, a, b, poly->neis[j] == 0);
                ++emitted;
            }
        }
    }
    return emitted;
}

extern "C" float jce_recast_agent_height(const JceRecastNavMesh *nm)
{
    /* Straight off the tile header.  dtCreateNavMeshData writes the build's
     * walkableHeight there, so the number survives into the .navmesh.bin and
     * a loaded mesh can still say what it was carved for -- which is the
     * whole point: the sidecar .json holds the bake SETTINGS, but the runtime
     * loads only the .bin.
     *
     * The first tile with a header answers for all of them: every tile of one
     * build carries the same walkableHeight, because it comes from the single
     * rcConfig that build used. */
    if (!nm || !nm->nav) return 0.0f;
    const dtNavMesh *m = (const dtNavMesh *)nm->nav;
    for (int i = 0; i < m->getMaxTiles(); ++i) {
        const dtMeshTile *t = m->getTile(i);
        if (t && t->header) return t->header->walkableHeight;
    }
    return 0.0f;
}

extern "C" void jce_recast_get_stats(const JceRecastNavMesh *nm,
                                       JceRecastStats *out_stats)
{
    if (!out_stats) return;
    if (!nm) { std::memset(out_stats, 0, sizeof(*out_stats)); return; }
    *out_stats = nm->stats;
}

/* =================================================================== *
 *  Dynamic obstacles (DetourTileCache)
 *
 *  Builds the same triangle soup as recast_build_navdata, but slices it
 *  into a grid of compressed tile-cache tiles managed by a live
 *  dtTileCache.  Obstacles are stamped into the cache at runtime, which
 *  marks the touched tiles dirty; dtTileCache::update() re-cuts those
 *  tiles (carving the obstacle footprint out of the walkable area) and
 *  re-inserts them into the dtNavMesh, so subsequent queries detour.
 *
 *  This mirrors RecastDemo's Sample_TempObstacles.  Tiles are 48 cells
 *  square (the RecastDemo default) and we use a no-compression
 *  pass-through compressor so we carry no extra third-party dependency
 *  (fastlz lives in RecastDemo's Contrib, not the installed lib).
 * =================================================================== */

namespace {

constexpr int kTileCacheTileSize  = 48;   /* cells per tile edge */
constexpr int kTileCacheMaxLayers = 32;   /* max stacked layers per (tx,ty) */
constexpr int kTileCacheExpansion = 4;    /* border in cells for layer build */

/* Pass-through "compressor": the tile cache requires a compressor, but we
 * keep the layer data verbatim (no external codec dependency).  This trades
 * a little memory for zero extra deps and is the same trick used when fastlz
 * is unavailable. */
struct JceNoopTileCacheCompressor : public dtTileCacheCompressor {
    int maxCompressedSize(const int bufferSize) override {
        /* Worst case: the data plus a tiny margin (we never grow it). */
        return bufferSize;
    }
    dtStatus compress(const unsigned char *buffer, const int bufferSize,
                      unsigned char *compressed, const int /*maxCompressedSize*/,
                      int *compressedSize) override {
        std::memcpy(compressed, buffer, (size_t)bufferSize);
        *compressedSize = bufferSize;
        return DT_SUCCESS;
    }
    dtStatus decompress(const unsigned char *compressed, const int compressedSize,
                        unsigned char *buffer, const int maxBufferSize,
                        int *bufferSize) override {
        if (compressedSize > maxBufferSize) return DT_FAILURE;
        std::memcpy(buffer, compressed, (size_t)compressedSize);
        *bufferSize = compressedSize;
        return DT_SUCCESS;
    }
};

/* Linear bump allocator for the tile cache's transient layer/contour/mesh
 * scratch.  reset() rewinds it between tile rebuilds. */
struct JceLinearTileCacheAlloc : public dtTileCacheAlloc {
    unsigned char *buffer = nullptr;
    size_t         capacity = 0;
    size_t         top = 0;
    size_t         high = 0;

    explicit JceLinearTileCacheAlloc(size_t cap)
        : capacity(cap)
    {
        buffer = (unsigned char *)dtAlloc(cap, DT_ALLOC_PERM);
    }
    ~JceLinearTileCacheAlloc() override {
        if (buffer) dtFree(buffer);
    }
    void reset() override { high = std::max(high, top); top = 0; }

    void *alloc(const size_t size) override {
        if (!buffer) return nullptr;
        /* 16-byte align each block. */
        size_t aligned = (top + 15u) & ~((size_t)15u);
        if (aligned + size > capacity) return nullptr;  /* overflow: drop */
        unsigned char *p = buffer + aligned;
        top = aligned + size;
        return p;
    }
    void free(void * /*ptr*/) override { /* bump allocator: no-op — raw-alloc-ok: virtual override of allocator iface, not the global free */ }
};

/* Mesh processor: stamp walkable flags on every poly so dtNavMeshQuery's
 * default include-filter (0xffff) accepts them — without this every poly has
 * flag 0 and the query finds no path. */
struct JceTileCacheMeshProcess : public dtTileCacheMeshProcess {
    void process(struct dtNavMeshCreateParams *params,
                 unsigned char *polyAreas,
                 unsigned short *polyFlags) override {
        for (int i = 0; i < params->polyCount; ++i) {
            if (polyAreas[i] == DT_TILECACHE_WALKABLE_AREA) {
                polyAreas[i] = 0;        /* match the static path's area 0 */
                polyFlags[i] = 1;        /* walkable flag (filter accepts) */
            } else if (polyAreas[i] != 0) {
                polyFlags[i] = 1;
            }
        }
    }
};

/* Build the compressed tile-cache tiles for a single (tx,ty) tile column and
 * add them to the cache.  Runs the per-tile slice of the Recast pipeline
 * (rasterise → filter → compact → erode → rcBuildHeightfieldLayers) then
 * compresses each layer via dtBuildTileCacheLayer.  Returns the number of
 * layers added (0 = empty tile, which is fine for open space at the border). */
static int build_tile_cache_tiles(dtTileCache *tc,
                                  rcContext   *ctx,
                                  const rcConfig &base_cfg,
                                  const float *vertices, int vertex_count,
                                  const int *tri_indices, int triangle_count,
                                  int tx, int ty,
                                  const float *bmin, const float *bmax)
{
    rcConfig tcfg = base_cfg;

    /* Tile-local bounds, expanded by the layer-build border so neighbour
     * spans rasterise correctly. */
    tcfg.bmin[0] = bmin[0] + (float)(tx * tcfg.tileSize) * tcfg.cs;
    tcfg.bmin[1] = bmin[1];
    tcfg.bmin[2] = bmin[2] + (float)(ty * tcfg.tileSize) * tcfg.cs;
    tcfg.bmax[0] = bmin[0] + (float)((tx + 1) * tcfg.tileSize) * tcfg.cs;
    tcfg.bmax[1] = bmax[1];
    tcfg.bmax[2] = bmin[2] + (float)((ty + 1) * tcfg.tileSize) * tcfg.cs;
    tcfg.bmin[0] -= tcfg.borderSize * tcfg.cs;
    tcfg.bmin[2] -= tcfg.borderSize * tcfg.cs;
    tcfg.bmax[0] += tcfg.borderSize * tcfg.cs;
    tcfg.bmax[2] += tcfg.borderSize * tcfg.cs;

    rcHeightfield *hf = rcAllocHeightfield();
    if (!hf || !rcCreateHeightfield(ctx, *hf, tcfg.width, tcfg.height,
                                    tcfg.bmin, tcfg.bmax, tcfg.cs, tcfg.ch)) {
        if (hf) rcFreeHeightField(hf);
        return -1;
    }

    unsigned char *tri_areas = (unsigned char *)JCE_CALLOC(triangle_count, 1);
    if (!tri_areas) { rcFreeHeightField(hf); return -1; }
    rcMarkWalkableTriangles(ctx, tcfg.walkableSlopeAngle,
                            vertices, vertex_count,
                            tri_indices, triangle_count, tri_areas);
    if (!rcRasterizeTriangles(ctx, vertices, vertex_count,
                              tri_indices, tri_areas, triangle_count,
                              *hf, tcfg.walkableClimb)) {
        JCE_FREE(tri_areas);
        rcFreeHeightField(hf);
        return -1;
    }
    JCE_FREE(tri_areas);

    rcFilterLowHangingWalkableObstacles(ctx, tcfg.walkableClimb, *hf);
    rcFilterLedgeSpans(ctx, tcfg.walkableHeight, tcfg.walkableClimb, *hf);
    rcFilterWalkableLowHeightSpans(ctx, tcfg.walkableHeight, *hf);

    rcCompactHeightfield *chf = rcAllocCompactHeightfield();
    if (!chf || !rcBuildCompactHeightfield(ctx, tcfg.walkableHeight,
                                           tcfg.walkableClimb, *hf, *chf)) {
        if (chf) rcFreeCompactHeightfield(chf);
        rcFreeHeightField(hf);
        return -1;
    }
    rcFreeHeightField(hf);

    if (tcfg.walkableRadius > 0 &&
        !rcErodeWalkableArea(ctx, tcfg.walkableRadius, *chf)) {
        rcFreeCompactHeightfield(chf);
        return -1;
    }

    rcHeightfieldLayerSet *lset = rcAllocHeightfieldLayerSet();
    if (!lset || !rcBuildHeightfieldLayers(ctx, *chf, tcfg.borderSize,
                                           tcfg.walkableHeight, *lset)) {
        if (lset) rcFreeHeightfieldLayerSet(lset);
        rcFreeCompactHeightfield(chf);
        return -1;
    }
    rcFreeCompactHeightfield(chf);

    JceNoopTileCacheCompressor comp;
    int n_added = 0;
    for (int i = 0; i < lset->nlayers && i < kTileCacheMaxLayers; ++i) {
        const rcHeightfieldLayer *layer = &lset->layers[i];

        dtTileCacheLayerHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.magic   = DT_TILECACHE_MAGIC;
        hdr.version = DT_TILECACHE_VERSION;
        hdr.tx      = tx;
        hdr.ty      = ty;
        hdr.tlayer  = i;
        rcVcopy(hdr.bmin, layer->bmin);
        rcVcopy(hdr.bmax, layer->bmax);
        hdr.width  = (unsigned char)layer->width;
        hdr.height = (unsigned char)layer->height;
        hdr.minx   = (unsigned char)layer->minx;
        hdr.maxx   = (unsigned char)layer->maxx;
        hdr.miny   = (unsigned char)layer->miny;
        hdr.maxy   = (unsigned char)layer->maxy;
        hdr.hmin   = (unsigned short)layer->hmin;
        hdr.hmax   = (unsigned short)layer->hmax;

        unsigned char *tile_data = nullptr;
        int            tile_size = 0;
        dtStatus st = dtBuildTileCacheLayer(&comp, &hdr,
                                            layer->heights, layer->areas,
                                            layer->cons,
                                            &tile_data, &tile_size);
        if (dtStatusFailed(st) || !tile_data) {
            continue;   /* skip this layer; others may still build */
        }

        dtCompressedTileRef tref = 0;
        st = tc->addTile(tile_data, tile_size, DT_COMPRESSEDTILE_FREE_DATA, &tref);
        if (dtStatusFailed(st)) {
            dtFree(tile_data);   /* cache rejected it; we still own the blob */
            continue;
        }
        ++n_added;
    }
    rcFreeHeightfieldLayerSet(lset);
    return n_added;
}

} /* anonymous (tile cache) */

extern "C" JceRecastNavMesh *jce_recast_build_tiled(const float    *vertices,
                                                    uint32_t        vertex_count,
                                                    const uint32_t *indices,
                                                    uint32_t        triangle_count,
                                                    const JceRecastConfig *user_cfg)
{
    if (!vertices || vertex_count < 3 || !indices || triangle_count == 0) {
        LOG_ERROR(LOG_TAG, "build_tiled: invalid input mesh");
        return nullptr;
    }

    JceRecastConfig cfg;
    if (user_cfg) cfg = *user_cfg; else jce_recast_default_config(&cfg);

    auto t_start = std::chrono::steady_clock::now();

    int *tri_indices = (int *)JCE_MALLOC(sizeof(int) * triangle_count * 3);
    if (!tri_indices) return nullptr;
    for (uint32_t i = 0; i < triangle_count * 3; ++i)
        tri_indices[i] = (int)indices[i];

    float bmin[3], bmax[3];
    compute_aabb(vertices, vertex_count, bmin, bmax);

    /* Shared Recast config (per-tile bounds are derived per column). */
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
    rcfg.tileSize               = kTileCacheTileSize;
    rcfg.borderSize             = rcfg.walkableRadius + kTileCacheExpansion;
    rcfg.width                  = rcfg.tileSize + rcfg.borderSize * 2;
    rcfg.height                 = rcfg.tileSize + rcfg.borderSize * 2;
    rcfg.detailSampleDist       = cfg.detail_sample_dist < 0.9f
                                  ? 0 : rcfg.cs * cfg.detail_sample_dist;
    rcfg.detailSampleMaxError   = rcfg.ch * cfg.detail_sample_max_err;
    rcVcopy(rcfg.bmin, bmin);
    rcVcopy(rcfg.bmax, bmax);

    int grid_w = 0, grid_h = 0;
    rcCalcGridSize(rcfg.bmin, rcfg.bmax, rcfg.cs, &grid_w, &grid_h);
    const int n_tiles_x = (grid_w + rcfg.tileSize - 1) / rcfg.tileSize;
    const int n_tiles_y = (grid_h + rcfg.tileSize - 1) / rcfg.tileSize;

    /* Tile/poly bit budget (mirrors RecastDemo). */
    int tile_bits = (int)dtIlog2(dtNextPow2((unsigned int)
                        (n_tiles_x * n_tiles_y * kTileCacheMaxLayers)));
    if (tile_bits > 14) tile_bits = 14;
    int poly_bits = 22 - tile_bits;
    const int max_tiles = 1 << tile_bits;
    const int max_polys_per_tile = 1 << poly_bits;

    JceRecastNavMesh *out = new (std::nothrow) JceRecastNavMesh();
    if (!out) { JCE_FREE(tri_indices); return nullptr; }

    /* dtNavMesh sized for the whole tiled grid. */
    out->nav = dtAllocNavMesh();
    if (!out->nav) { JCE_FREE(tri_indices); jce_recast_destroy(out); return nullptr; }

    dtNavMeshParams nmp;
    std::memset(&nmp, 0, sizeof(nmp));
    rcVcopy(nmp.orig, bmin);
    nmp.tileWidth  = rcfg.tileSize * rcfg.cs;
    nmp.tileHeight = rcfg.tileSize * rcfg.cs;
    nmp.maxTiles   = max_tiles;
    nmp.maxPolys   = max_polys_per_tile;
    if (dtStatusFailed(out->nav->init(&nmp))) {
        LOG_ERROR(LOG_TAG, "build_tiled: dtNavMesh::init failed");
        JCE_FREE(tri_indices);
        jce_recast_destroy(out);
        return nullptr;
    }

    /* dtTileCache + its three policy objects.  The alloc/compressor/proc must
     * outlive the cache (it holds raw pointers to them and dereferences them
     * during update() and teardown), so they are owned by the JceRecastNavMesh
     * and freed in jce_recast_destroy after the cache. */
    out->tile_cache = dtAllocTileCache();
    if (!out->tile_cache) {
        LOG_ERROR(LOG_TAG, "build_tiled: dtAllocTileCache failed");
        JCE_FREE(tri_indices);
        jce_recast_destroy(out);
        return nullptr;
    }

    dtTileCacheParams tcp;
    std::memset(&tcp, 0, sizeof(tcp));
    rcVcopy(tcp.orig, bmin);
    tcp.cs                     = rcfg.cs;
    tcp.ch                     = rcfg.ch;
    tcp.width                  = rcfg.tileSize;
    tcp.height                 = rcfg.tileSize;
    tcp.walkableHeight         = cfg.walkable_height;
    tcp.walkableRadius         = cfg.walkable_radius;
    tcp.walkableClimb          = cfg.walkable_climb;
    tcp.maxSimplificationError = cfg.edge_max_error;
    tcp.maxTiles               = n_tiles_x * n_tiles_y * kTileCacheMaxLayers;
    tcp.maxObstacles           = 128;

    /* Scratch big enough for the largest layer's intermediate buffers.  A
     * tile is at most (tileSize+border*2)^2 cells; the layer build needs a
     * handful of byte-per-cell arrays plus contour/mesh scratch, so size
     * generously (RecastDemo uses a similar fixed pool). */
    const size_t cells = (size_t)rcfg.width * (size_t)rcfg.height;
    JceLinearTileCacheAlloc *talloc =
        new (std::nothrow) JceLinearTileCacheAlloc(cells * 64u + (1u << 16));
    out->tc_alloc = talloc;
    out->tc_comp  = new (std::nothrow) JceNoopTileCacheCompressor();
    out->tc_proc  = new (std::nothrow) JceTileCacheMeshProcess();
    if (!talloc || !out->tc_comp || !out->tc_proc || !talloc->buffer) {
        LOG_ERROR(LOG_TAG, "build_tiled: OOM for tile-cache policies");
        JCE_FREE(tri_indices);
        jce_recast_destroy(out);   /* frees cache + whichever policies exist */
        return nullptr;
    }

    if (dtStatusFailed(out->tile_cache->init(&tcp, out->tc_alloc,
                                             out->tc_comp, out->tc_proc))) {
        LOG_ERROR(LOG_TAG, "build_tiled: dtTileCache::init failed");
        JCE_FREE(tri_indices);
        jce_recast_destroy(out);
        return nullptr;
    }

    SilentRcCtx ctx;

    /* Build + register every tile column's compressed layers. */
    int total_layers = 0;
    for (int ty = 0; ty < n_tiles_y; ++ty) {
        for (int tx = 0; tx < n_tiles_x; ++tx) {
            int added = build_tile_cache_tiles(out->tile_cache, &ctx, rcfg,
                                               vertices, (int)vertex_count,
                                               tri_indices, (int)triangle_count,
                                               tx, ty, bmin, bmax);
            if (added > 0) total_layers += added;
        }
    }
    JCE_FREE(tri_indices);

    if (total_layers == 0) {
        LOG_ERROR(LOG_TAG, "build_tiled: produced no walkable tiles");
        jce_recast_destroy(out);
        return nullptr;
    }

    /* Build the initial dtNavMesh from the freshly added tiles (no obstacles
     * yet).  update() with no pending requests just flushes the build. */
    for (int ty = 0; ty < n_tiles_y; ++ty)
        for (int tx = 0; tx < n_tiles_x; ++tx)
            out->tile_cache->buildNavMeshTilesAt(tx, ty, out->nav);

    /* Query object over the now-populated navmesh. */
    out->query = dtAllocNavMeshQuery();
    if (!out->query || dtStatusFailed(out->query->init(out->nav, 2048))) {
        LOG_ERROR(LOG_TAG, "build_tiled: dtNavMeshQuery::init failed");
        jce_recast_destroy(out);
        return nullptr;
    }
    out->filter.setIncludeFlags(0xffff);
    out->filter.setExcludeFlags(0);

    /* Stats: sum tile headers (same as make_navmesh_from_data). */
    out->stats = {};
    for (int i = 0; i < out->nav->getMaxTiles(); ++i) {
        const dtMeshTile *tile = ((const dtNavMesh *)out->nav)->getTile(i);
        if (tile && tile->header) {
            out->stats.polygon_count         += tile->header->polyCount;
            out->stats.vertex_count          += tile->header->vertCount;
            out->stats.detail_triangle_count += tile->header->detailTriCount;
        }
    }
    auto t_end = std::chrono::steady_clock::now();
    out->stats.build_time_ms = (int)std::chrono::duration_cast<
        std::chrono::milliseconds>(t_end - t_start).count();

    LOG_INFO(LOG_TAG, "built tiled navmesh: %dx%d tiles, %d layers, "
             "%d polys, %d ms",
             n_tiles_x, n_tiles_y, total_layers,
             out->stats.polygon_count, out->stats.build_time_ms);
    return out;
}

/* Drain the tile cache's pending obstacle requests, rebuilding every touched
 * tile so the dtNavMesh reflects the change before we return.  dtTileCache
 * processes a bounded batch per update() call, so we loop until upToDate. */
static void recast_flush_tile_cache(JceRecastNavMesh *nm)
{
    if (!nm || !nm->tile_cache || !nm->nav) return;
    bool up_to_date = false;
    /* Bounded loop: each update() drains up to MAX_REQUESTS(64); a few
     * hundred iterations is far more than any realistic obstacle batch. */
    for (int guard = 0; guard < 4096 && !up_to_date; ++guard) {
        dtStatus st = nm->tile_cache->update(0.0f, nm->nav, &up_to_date);
        if (dtStatusFailed(st)) break;
    }

    /* Recompute stats: obstacle carving changes poly/vert counts. */
    nm->stats.polygon_count = 0;
    nm->stats.vertex_count = 0;
    nm->stats.detail_triangle_count = 0;
    for (int i = 0; i < nm->nav->getMaxTiles(); ++i) {
        const dtMeshTile *tile = ((const dtNavMesh *)nm->nav)->getTile(i);
        if (tile && tile->header) {
            nm->stats.polygon_count         += tile->header->polyCount;
            nm->stats.vertex_count          += tile->header->vertCount;
            nm->stats.detail_triangle_count += tile->header->detailTriCount;
        }
    }
}

extern "C" JceRecastObstacleRef jce_recast_add_obstacle(JceRecastNavMesh *nm,
                                                        float pos_x, float pos_y, float pos_z,
                                                        float radius, float height)
{
    if (!nm || !nm->tile_cache || radius <= 0.0f || height <= 0.0f) return 0;
    const float pos[3] = { pos_x, pos_y, pos_z };
    dtObstacleRef ref = 0;
    dtStatus st = nm->tile_cache->addObstacle(pos, radius, height, &ref);
    if (dtStatusFailed(st) || ref == 0) {
        LOG_WARN(LOG_TAG, "add_obstacle (cylinder) failed");
        return 0;
    }
    recast_flush_tile_cache(nm);
    return (JceRecastObstacleRef)ref;
}

extern "C" JceRecastObstacleRef jce_recast_add_box_obstacle(JceRecastNavMesh *nm,
                                                            float min_x, float min_y, float min_z,
                                                            float max_x, float max_y, float max_z)
{
    if (!nm || !nm->tile_cache) return 0;
    const float bmin[3] = { min_x, min_y, min_z };
    const float bmax[3] = { max_x, max_y, max_z };
    dtObstacleRef ref = 0;
    dtStatus st = nm->tile_cache->addBoxObstacle(bmin, bmax, &ref);
    if (dtStatusFailed(st) || ref == 0) {
        LOG_WARN(LOG_TAG, "add_obstacle (box) failed");
        return 0;
    }
    recast_flush_tile_cache(nm);
    return (JceRecastObstacleRef)ref;
}

extern "C" bool jce_recast_remove_obstacle(JceRecastNavMesh *nm,
                                           JceRecastObstacleRef ref)
{
    if (!nm || !nm->tile_cache || ref == 0) return false;
    dtStatus st = nm->tile_cache->removeObstacle((dtObstacleRef)ref);
    if (dtStatusFailed(st)) {
        LOG_WARN(LOG_TAG, "remove_obstacle failed");
        return false;
    }
    recast_flush_tile_cache(nm);
    return true;
}

extern "C" bool jce_recast_has_tile_cache(const JceRecastNavMesh *nm)
{
    return nm && nm->tile_cache != nullptr;
}
