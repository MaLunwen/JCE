/*
 * jce_sr_terrain.c  Scene-renderer terrain module (split from
 * jce_scene_renderer.c).
 *
 * Terrain per-chunk LOD cache + per-chunk draw (colour + shadow).  Pure move
 * from the monolithic renderer: helpers shared across the split modules are
 * declared in jce_sr_internal.h; everything else stays file-static here.
 */

#include "jce_sr_internal.h"

/* ── Terrain per-chunk LOD cache (P1-terrain-lod) ─────────────────────
 *
 * sr_terrain_get_slot() find-or-loads a terrain by path and lazily fills the
 * per-chunk metadata (chunk count + each chunk's terrain-LOCAL AABB).  The
 * actual GPU chunk meshes are built on demand by sr_terrain_chunk_mesh() at
 * whatever LOD the camera-distance test asks for, and cached until the
 * required LOD changes (or the terrain is invalidated).  Nothing here merges
 * chunks: the renderer issues one draw per visible chunk. */

/* Compute chunk (cx,cz)'s terrain-local AABB by scanning its height samples.
 * Cheap (runs once per chunk at load) and gives a snug Y range for culling. */
static void sr_terrain_chunk_local_aabb(const JceTerrain *t, int cx, int cz,
                                        jce_vec3 *out_min, jce_vec3 *out_max)
{
    int w  = jce_terrain_width(t);
    int h  = jce_terrain_height(t);
    int cs = jce_terrain_chunk_size(t);
    float wsx = jce_terrain_world_size_x(t);
    float wsz = jce_terrain_world_size_z(t);
    float mh  = jce_terrain_max_height(t);
    const float *heights = jce_terrain_heights(t);

    int x0 = cx * cs, z0 = cz * cs;
    int x1 = x0 + cs, z1 = z0 + cs;
    if (x1 > w - 1) x1 = w - 1;
    if (z1 > h - 1) z1 = h - 1;

    float dx = (w > 1) ? wsx / (float)(w - 1) : 0.0f;
    float dz = (h > 1) ? wsz / (float)(h - 1) : 0.0f;

    float hmin = +FLT_MAX, hmax = -FLT_MAX;
    if (heights) {
        for (int zz = z0; zz <= z1; zz++)
        for (int xx = x0; xx <= x1; xx++) {
            float hv = heights[(size_t)zz * (size_t)w + (size_t)xx] * mh;
            if (hv < hmin) hmin = hv;
            if (hv > hmax) hmax = hv;
        }
    }
    if (hmin > hmax) { hmin = 0.0f; hmax = mh; }

    out_min->x = (float)x0 * dx; out_max->x = (float)x1 * dx;
    out_min->z = (float)z0 * dz; out_max->z = (float)z1 * dz;
    out_min->y = hmin;           out_max->y = hmax;
}

/* Free all per-chunk cache arrays + GPU meshes for one terrain slot. */
void sr_terrain_free_chunks(JceSceneRenderer *sr, int slot)
{
    if (slot < 0 || slot >= 16) return;
    if (sr->terrain_cache[slot].chunk_meshes) {
        for (int i = 0; i < sr->terrain_cache[slot].chunk_count; i++)
            if (sr->terrain_cache[slot].chunk_meshes[i])
                jce_mesh_destroy(sr->terrain_cache[slot].chunk_meshes[i]);
        JCE_FREE(sr->terrain_cache[slot].chunk_meshes);
        sr->terrain_cache[slot].chunk_meshes = NULL;
    }
    if (sr->terrain_cache[slot].chunk_lod) {
        JCE_FREE(sr->terrain_cache[slot].chunk_lod);
        sr->terrain_cache[slot].chunk_lod = NULL;
    }
    if (sr->terrain_cache[slot].chunk_min) {
        JCE_FREE(sr->terrain_cache[slot].chunk_min);
        sr->terrain_cache[slot].chunk_min = NULL;
    }
    if (sr->terrain_cache[slot].chunk_max) {
        JCE_FREE(sr->terrain_cache[slot].chunk_max);
        sr->terrain_cache[slot].chunk_max = NULL;
    }
    if (sr->terrain_cache[slot].chunk_splat_tex) {
        for (int i = 0; i < sr->terrain_cache[slot].chunk_count; i++)
            if (BGFX_HANDLE_IS_VALID(sr->terrain_cache[slot].chunk_splat_tex[i]))
                bgfx_destroy_texture(sr->terrain_cache[slot].chunk_splat_tex[i]);
        JCE_FREE(sr->terrain_cache[slot].chunk_splat_tex);
        sr->terrain_cache[slot].chunk_splat_tex = NULL;
    }
    sr->terrain_cache[slot].chunk_count = 0;
}

/* Allocate + fill the per-chunk metadata for a freshly loaded terrain. */
static bool sr_terrain_init_chunks(JceSceneRenderer *sr, int slot)
{
    JceTerrain *terr = sr->terrain_cache[slot].terrain;
    if (!terr) return false;
    int ncx = jce_terrain_chunk_count_x(terr);
    int ncz = jce_terrain_chunk_count_z(terr);
    int n   = ncx * ncz;
    if (n <= 0 || n > SR_TERRAIN_MAX_CHUNKS) return false;

    JceMesh  **meshes = (JceMesh **)JCE_MALLOC(sizeof(JceMesh *) * (size_t)n);
    int8_t    *lods   = (int8_t  *)JCE_MALLOC(sizeof(int8_t)    * (size_t)n);
    jce_vec3  *mins   = (jce_vec3 *)JCE_MALLOC(sizeof(jce_vec3) * (size_t)n);
    jce_vec3  *maxs   = (jce_vec3 *)JCE_MALLOC(sizeof(jce_vec3) * (size_t)n);
    /* Per-chunk splat textures only for a tiled terrain (else NULL → the
     * monolithic splat_tex path is used). */
    bgfx_texture_handle_t *splats = jce_terrain_is_tiled(terr)
        ? (bgfx_texture_handle_t *)JCE_MALLOC(sizeof(bgfx_texture_handle_t) * (size_t)n)
        : NULL;
    if (!meshes || !lods || !mins || !maxs ||
        (jce_terrain_is_tiled(terr) && !splats)) {
        if (meshes) JCE_FREE(meshes);
        if (lods)   JCE_FREE(lods);
        if (mins)   JCE_FREE(mins);
        if (maxs)   JCE_FREE(maxs);
        if (splats) JCE_FREE(splats);
        return false;
    }
    for (int cz = 0; cz < ncz; cz++)
    for (int cx = 0; cx < ncx; cx++) {
        int idx = cz * ncx + cx;
        meshes[idx] = NULL;
        lods[idx]   = -1;
        if (splats) splats[idx].idx = UINT16_MAX;
        sr_terrain_chunk_local_aabb(terr, cx, cz, &mins[idx], &maxs[idx]);
    }
    sr->terrain_cache[slot].chunk_meshes    = meshes;
    sr->terrain_cache[slot].chunk_lod       = lods;
    sr->terrain_cache[slot].chunk_min       = mins;
    sr->terrain_cache[slot].chunk_max       = maxs;
    sr->terrain_cache[slot].chunk_splat_tex = splats;
    sr->terrain_cache[slot].chunk_count  = n;
    sr->terrain_cache[slot].chunk_nx     = ncx;
    sr->terrain_cache[slot].chunk_nz     = ncz;
    return true;
}

/* Build (or rebuild) chunk `idx`'s GPU mesh at `lod`, with a downward skirt
 * around its outer ring to hide T-junction cracks against neighbouring chunks
 * drawn at a different LOD.  Returns the cached mesh, or NULL on failure.
 * No-op (returns the cached mesh) when the chunk is already built at `lod`. */
JceMesh *sr_terrain_chunk_mesh(JceSceneRenderer *sr, int slot,
                               int cx, int cz, int lod)
{
    JceTerrain *terr = sr->terrain_cache[slot].terrain;
    if (!terr) return NULL;
    int ncx = sr->terrain_cache[slot].chunk_nx;
    int idx = cz * ncx + cx;
    if (idx < 0 || idx >= sr->terrain_cache[slot].chunk_count) return NULL;

    if (sr->terrain_cache[slot].chunk_meshes[idx] &&
        sr->terrain_cache[slot].chunk_lod[idx] == (int8_t)lod)
        return sr->terrain_cache[slot].chunk_meshes[idx];

    int core_v = 0, core_i = 0;
    jce_terrain_chunk_mesh_size(terr, cx, cz, lod, &core_v, &core_i);
    if (core_v <= 0 || core_i <= 0) return NULL;

    /* Skirt adds at most a full ring of duplicated edge verts plus two
     * triangles per edge segment.  nx == nz == sqrt(core_v) for square
     * chunks, but allocate the worst case: 4 edges of `core_v` verts. */
    int skirt_v_cap = 4 * core_v;
    int skirt_i_cap = 4 * core_v * 6;
    int cap_v = core_v + skirt_v_cap;
    int cap_i = core_i + skirt_i_cap;

    JceTerrainVertex *vb = (JceTerrainVertex *)
        JCE_MALLOC(sizeof(JceTerrainVertex) * (size_t)cap_v);
    uint32_t *ib = (uint32_t *)JCE_MALLOC(sizeof(uint32_t) * (size_t)cap_i);
    if (!vb || !ib) { if (vb) JCE_FREE(vb); if (ib) JCE_FREE(ib); return NULL; }

    int wrote_v = 0, wrote_i = 0;
    jce_terrain_chunk_build_mesh(terr, cx, cz, lod,
                                 vb, core_v, ib, core_i, &wrote_v, &wrote_i);
    if (wrote_v <= 0 || wrote_i <= 0) { JCE_FREE(vb); JCE_FREE(ib); return NULL; }

    /* The core grid is row-major nx*nz (see jce_terrain_chunk_build_mesh).
     * Derive nx/nz from the chunk extents at this LOD to walk its border. */
    int w  = jce_terrain_width(terr);
    int h  = jce_terrain_height(terr);
    int cs = jce_terrain_chunk_size(terr);
    int x0 = cx * cs, z0 = cz * cs;
    int x1 = x0 + cs, z1 = z0 + cs;
    if (x1 > w - 1) x1 = w - 1;
    if (z1 > h - 1) z1 = h - 1;
    int step = (lod <= 0) ? 1 : (1 << lod);
    int nx = (x1 - x0) / step + 1;
    int nz = (z1 - z0) / step + 1;

    float skirt = jce_terrain_max_height(terr) * SR_TERRAIN_SKIRT_FRAC;
    if (skirt < 0.01f) skirt = 0.01f;

    /* Append a skirt strip along one chunk border.  edge_ids[0..count-1] are
     * the core border vertex indices in order; for each we add a duplicated
     * vertex dropped by `skirt`, then stitch vertical quads between the core
     * ring and the apron.  `flip` selects the winding so all four edges face
     * outward consistently with the terrain's CW core winding. */
    {
        /* nx and nz are at most chunk_size+1; cap the border walk so an
         * unusually large chunk_size cannot overflow this scratch ring. */
        enum { SR_TC_EDGE_CAP = 4097 };
        int edge_ids[SR_TC_EDGE_CAP];
        for (int e4 = 0; e4 < 4; e4++) {
            int count, flip;
            switch (e4) {
            case 0: /* north (j=0)      */ count = nx; flip = 0; break;
            case 1: /* south (j=nz-1)   */ count = nx; flip = 1; break;
            case 2: /* west  (i=0)      */ count = nz; flip = 1; break;
            default:/* east  (i=nx-1)   */ count = nz; flip = 0; break;
            }
            if (count > SR_TC_EDGE_CAP) count = SR_TC_EDGE_CAP;
            for (int s = 0; s < count; s++) {
                switch (e4) {
                case 0:  edge_ids[s] = s;                       break;
                case 1:  edge_ids[s] = (nz - 1) * nx + s;       break;
                case 2:  edge_ids[s] = s * nx;                  break;
                default: edge_ids[s] = s * nx + (nx - 1);       break;
                }
            }
            int first_apron = wrote_v;
            for (int s = 0; s < count; s++) {
                int cvid = edge_ids[s];
                if (cvid < 0 || cvid >= core_v) continue;
                if (wrote_v >= cap_v) break;
                JceTerrainVertex a = vb[cvid];
                a.py -= skirt;
                vb[wrote_v++] = a;
            }
            for (int s = 0; s + 1 < count; s++) {
                if (wrote_i + 6 > cap_i) break;
                int top0 = edge_ids[s];
                int top1 = edge_ids[s + 1];
                int bot0 = first_apron + s;
                int bot1 = first_apron + s + 1;
                if (top0 < 0 || top1 < 0 ||
                    top0 >= core_v || top1 >= core_v ||
                    bot1 >= wrote_v) continue;
                if (flip) {
                    ib[wrote_i++] = (uint32_t)top0; ib[wrote_i++] = (uint32_t)bot0;
                    ib[wrote_i++] = (uint32_t)top1; ib[wrote_i++] = (uint32_t)top1;
                    ib[wrote_i++] = (uint32_t)bot0; ib[wrote_i++] = (uint32_t)bot1;
                } else {
                    ib[wrote_i++] = (uint32_t)top0; ib[wrote_i++] = (uint32_t)top1;
                    ib[wrote_i++] = (uint32_t)bot0; ib[wrote_i++] = (uint32_t)bot1;
                    ib[wrote_i++] = (uint32_t)bot0; ib[wrote_i++] = (uint32_t)top1;
                }
            }
        }
    }

    if (sr->terrain_cache[slot].chunk_meshes[idx]) {
        jce_mesh_destroy(sr->terrain_cache[slot].chunk_meshes[idx]);
        sr->terrain_cache[slot].chunk_meshes[idx] = NULL;
    }
    /* JceTerrainVertex layout matches JceMeshVertex exactly. */
    JceMesh *m = jce_mesh_create((const JceMeshVertex *)vb, (uint32_t)wrote_v,
                                 ib, (uint32_t)wrote_i);
    JCE_FREE(vb); JCE_FREE(ib);
    if (!m) return NULL;
    sr->terrain_cache[slot].chunk_meshes[idx] = m;
    sr->terrain_cache[slot].chunk_lod[idx]    = (int8_t)lod;
    return m;
}

/* Find (or lazily load) the terrain cache slot for `path`, initialising the
 * per-chunk metadata on first load.  Returns the slot index, or -1 on failure
 * (no free slot, load failed, or chunk init failed). */
int sr_terrain_find_or_load_slot(JceSceneRenderer *sr, const char *path)
{
    if (!sr || !path || !path[0]) return -1;
    int slot = -1, free_slot = -1;
    for (int i = 0; i < 16; i++) {
        if (sr->terrain_cache[i].used &&
            strncmp(sr->terrain_cache[i].path, path,
                    sizeof sr->terrain_cache[i].path) == 0) {
            slot = i; break;
        }
        if (!sr->terrain_cache[i].used && free_slot < 0) free_slot = i;
    }
    if (slot >= 0)
        return sr->terrain_cache[slot].failed ? -1 : slot;
    if (free_slot < 0) return -1;

    slot = free_slot;
    memset(&sr->terrain_cache[slot], 0, sizeof sr->terrain_cache[slot]);
    jce_strlcpy(sr->terrain_cache[slot].path, path,
                sizeof sr->terrain_cache[slot].path);
    sr->terrain_cache[slot].used = true;
    sr->terrain_cache[slot].splat_tex.idx = UINT16_MAX;

    /* PAK-first: deployed bundles overlay sr->pak, so a bundled terrain
     * meta+bin loads with zero host filesystem access.  If the path isn't in
     * the PAK, fall back to the host-resolved path (editor / loose files). */
    JceTerrain *terr = jce_terrain_load_from_pak(sr->pak, path);
    char        resolved[1024];
    const char *load_path = path;
    if (!terr) {
        if (sr->has_cbs && sr->cbs.resolve_path &&
            sr->cbs.resolve_path(path, resolved, (int)sizeof(resolved),
                                 sr->cbs.userdata)) {
            load_path = resolved;
        }
        terr = jce_terrain_load_file(load_path);
    }
    if (!terr) {
        sr->terrain_cache[slot].failed = true;
        LOG_WARN(LOG_TAG, "terrain load failed: '%s' (from '%s')",
                 load_path, path);
        return -1;
    }
    sr->terrain_cache[slot].terrain = terr;
    if (!sr_terrain_init_chunks(sr, slot)) {
        sr->terrain_cache[slot].failed = true;
        return -1;
    }
    return slot;
}

/* ── Terrain per-chunk draw (P1-terrain-lod) ──────────────────────────
 *
 * Replaces the old "merge every chunk into one ~16M-vert mesh" draw.  For each
 * terrain entity we walk its chunk grid and, per chunk:
 *   1. transform the chunk's local AABB by the entity world matrix,
 *   2. frustum-cull it against the camera (skip if fully outside),
 *   3. pick a LOD from the camera→chunk-centre distance,
 *   4. build/cache the chunk mesh (with skirts) at that LOD,
 *   5. re-bind transform + terrain textures/params and submit one draw.
 * Step 5 is repeated per chunk because jce_mesh_submit_terrain discards all
 * bound state (BGFX_DISCARD_ALL) after each submit. */

/* Build a box-filtered RGBA8 mip chain for the splat map into ONE bgfx memory
 * block (mip0..mipN, sequential, as bgfx expects for a hasMips upload).
 * Large-world #4: an un-mipped W×H splat (~64 MB at 4097²) aliases badly on
 * distant terrain and can never be dropped under VRAM pressure; a real mip chain
 * lets trilinear sampling pick the right level and the mip-bias hook evict the
 * top mips.  Averages the 4 packed layer weights per 2×2 (weights stay ~summed). */
static const bgfx_memory_t *sr_splat_build_mips(const uint32_t *splat, int w, int h)
{
    int    mw = w, mh = h, levels = 1;
    size_t total = (size_t)w * h * 4u;
    while (mw > 1 || mh > 1) {
        mw = mw > 1 ? mw >> 1 : 1; mh = mh > 1 ? mh >> 1 : 1;
        total += (size_t)mw * mh * 4u; ++levels;
    }
    const bgfx_memory_t *mem = bgfx_alloc((uint32_t)total);
    if (!mem) return NULL;
    uint8_t *dst = mem->data;
    memcpy(dst, splat, (size_t)w * h * 4u);            /* mip 0 = source */
    const uint8_t *prev = dst; int pw = w, ph = h;
    uint8_t *cur = dst + (size_t)w * h * 4u;
    mw = w > 1 ? w >> 1 : 1; mh = h > 1 ? h >> 1 : 1;
    for (int l = 1; l < levels; ++l) {
        for (int y = 0; y < mh; ++y)
            for (int x = 0; x < mw; ++x) {
                int x0 = x * 2, y0 = y * 2;
                int x1 = (x0 + 1 < pw) ? x0 + 1 : x0;
                int y1 = (y0 + 1 < ph) ? y0 + 1 : y0;
                for (int c = 0; c < 4; ++c) {
                    int s = prev[(y0 * pw + x0) * 4 + c] + prev[(y0 * pw + x1) * 4 + c]
                          + prev[(y1 * pw + x0) * 4 + c] + prev[(y1 * pw + x1) * 4 + c];
                    cur[(y * mw + x) * 4 + c] = (uint8_t)(s >> 2);
                }
            }
        prev = cur; pw = mw; ph = mh; cur += (size_t)mw * mh * 4u;
        mw = mw > 1 ? mw >> 1 : 1; mh = mh > 1 ? mh >> 1 : 1;
    }
    return mem;
}

/* Lazily build + cache chunk (cx,cz)'s per-tile splat texture (with the same
 * box-filtered mip chain as the monolithic path) for a TILED terrain. Assumes
 * chunk_size == tile_dim so chunk index == tile (cx,cz). Returns an invalid
 * handle on failure (caller falls back to white). */
static bgfx_texture_handle_t sr_terrain_tile_splat_tex(JceSceneRenderer *sr,
                                                       int slot, int cx, int cz,
                                                       int tile_dim)
{
    bgfx_texture_handle_t inv = { UINT16_MAX };
    bgfx_texture_handle_t *cache = sr->terrain_cache[slot].chunk_splat_tex;
    if (!cache) return inv;
    int ncx = sr->terrain_cache[slot].chunk_nx;
    int idx = cz * ncx + cx;
    if (idx < 0 || idx >= sr->terrain_cache[slot].chunk_count) return inv;
    if (BGFX_HANDLE_IS_VALID(cache[idx])) return cache[idx];

    int    span = tile_dim + 1;
    size_t n    = (size_t)span * (size_t)span;
    uint32_t *sp = (uint32_t *)JCE_MALLOC(n * sizeof(uint32_t));
    bgfx_texture_handle_t st = inv;
    if (sp && jce_terrain_tile_copy(sr->terrain_cache[slot].terrain,
                                    cx, cz, NULL, sp)) {
        const bgfx_memory_t *mem = sr_splat_build_mips(sp, span, span);
        if (mem)
            st = bgfx_create_texture_2d((uint16_t)span, (uint16_t)span, true, 1,
                    BGFX_TEXTURE_FORMAT_RGBA8,
                    BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem, 0);
    }
    if (sp) JCE_FREE(sp);
    cache[idx] = st;
    return st;
}

void sr_draw_terrain_chunks(JceSceneRenderer *sr, JceScene *scene,
                            EntityList *list,
                            const JceCamera *camera, uint16_t view_id,
                            int slot, JceTerrainComponent *tc,
                            const jce_mat4 *model,
                            const JcePbrMaterial *pbr,
                            const bgfx_texture_handle_t layer_tex[4])
{
    if (!sr || slot < 0 || slot >= 16) return;
    JceTerrain *terr = sr->terrain_cache[slot].terrain;
    if (!terr || sr->terrain_cache[slot].chunk_count <= 0) return;

    /* Lazy splat texture upload (once per terrain). */
    if (!sr->terrain_cache[slot].splat_uploaded) {
        int tw = jce_terrain_width(terr);
        int th = jce_terrain_height(terr);
        const uint32_t *splat = jce_terrain_splat(terr);
        if (splat && tw > 0 && th > 0) {
            /* hasMips=true + a full CPU-built mip chain: trilinear sampling then
             * picks the right level for distant chunks (no shimmer) and the mips
             * are droppable under VRAM pressure. */
            const bgfx_memory_t *mem = sr_splat_build_mips(splat, tw, th);
            if (mem)
                sr->terrain_cache[slot].splat_tex =
                    bgfx_create_texture_2d((uint16_t)tw, (uint16_t)th, true, 1,
                        BGFX_TEXTURE_FORMAT_RGBA8,
                        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem, 0);
        }
        sr->terrain_cache[slot].splat_uploaded = true;
    }
    bgfx_texture_handle_t splat_h = sr->terrain_cache[slot].splat_tex;
    if (!BGFX_HANDLE_IS_VALID(splat_h)) splat_h = sr->white_tex;

    float tparams[4] = {
        (tc && tc->tile_scale > 0.0f) ? tc->tile_scale : 10.0f,
        (tc && tc->splat_enabled) ? 1.0f : 0.0f,
        0.0f, 0.0f
    };
    /* large-world #4 per-tile splat: a tiled terrain has no monolithic splat
     * map. When its tile grid aligns with the chunk grid (tile_dim==chunk_size,
     * so chunk i ↔ tile i) we bind a per-tile splat texture per chunk (in the
     * loop below) and remap the global UV into that tile via u_terrainTileUV.
     * Otherwise chunks can't be mapped to tiles, so fall back to a clean base
     * layer instead of blending an all-white (equal-weight) splat into mud. */
    int tg_x = 0, tg_z = 0, tg_dim = 0;
    jce_terrain_tile_grid(terr, &tg_x, &tg_z, &tg_dim);
    bool per_tile_splat = jce_terrain_is_tiled(terr) && tg_dim > 0 &&
                          tg_dim == jce_terrain_chunk_size(terr) &&
                          tc && tc->splat_enabled;
    if (jce_terrain_is_tiled(terr) && !per_tile_splat) tparams[1] = 0.0f;

    /* Frustum planes from the camera (independent of the entity-level cull
     * toggle so terrain always benefits from per-chunk culling). */
    jce_vec4 planes[6];
    bool have_planes = false;
    if (camera) {
        const jce_mat4 v  = jce_camera_view(camera);
        const jce_mat4 p  = jce_camera_proj(camera, 16.0f / 9.0f,
                                            sr->homogeneous_depth);
        const jce_mat4 vp = jce_m4_multiply(&p, &v);
        sr_extract_frustum_planes(&vp, planes);
        have_planes = true;
    }
    jce_vec3 cam_pos = camera ? jce_camera_get_position(camera)
                              : jce_v3(0, 0, 0);

    /* Streaming prefetch (large-world #4): page in the tiles around the camera
     * before the chunk loop samples them, so crossing a tile boundary doesn't
     * first-touch-stall.  Camera → terrain-local via the model translation (exact
     * for placed-by-translation terrains, the common case; the lazy load in
     * terrain_h is the safety net for scaled/rotated ones).  Radius ≈ 2 tiles. */
    if (camera && jce_terrain_is_tiled(terr)) {
        int   ptx = 0, ptz = 0, ptd = 0;
        jce_terrain_tile_grid(terr, &ptx, &ptz, &ptd);
        float wsx = jce_terrain_world_size_x(terr);
        if (ptx > 0 && wsx > 0.0f) {
            float local_x = cam_pos.x - model->col[3].x;
            float local_z = cam_pos.z - model->col[3].z;
            jce_terrain_prefetch(terr, local_x, local_z, 2.0f * (wsx / (float)ptx));
        }
    }

    int ncx = sr->terrain_cache[slot].chunk_nx;
    int ncz = sr->terrain_cache[slot].chunk_nz;

    sr->stat_terrain_chunks_total   += ncx * ncz;

    for (int cz = 0; cz < ncz; cz++)
    for (int cx = 0; cx < ncx; cx++) {
        int idx = cz * ncx + cx;
        jce_vec3 lmn = sr->terrain_cache[slot].chunk_min[idx];
        jce_vec3 lmx = sr->terrain_cache[slot].chunk_max[idx];

        jce_vec3 wmn, wmx;
        sr_transform_aabb(model, lmn, lmx, &wmn, &wmx);

        if (have_planes && !sr_aabb_in_frustum(planes, wmn, wmx)) {
            sr->stat_terrain_chunks_culled++;
            continue;
        }

        /* LOD from camera distance to the chunk's world-space centre. */
        jce_vec3 ctr = { 0.5f * (wmn.x + wmx.x),
                         0.5f * (wmn.y + wmx.y),
                         0.5f * (wmn.z + wmx.z) };
        float dx = ctr.x - cam_pos.x;
        float dy = ctr.y - cam_pos.y;
        float dz = ctr.z - cam_pos.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        int lod = (int)(dist / SR_TERRAIN_LOD_DIST);
        if (lod < 0) lod = 0;
        if (lod > SR_TERRAIN_MAX_LOD) lod = SR_TERRAIN_MAX_LOD;

        JceMesh *cm = sr_terrain_chunk_mesh(sr, slot, cx, cz, lod);
        if (!cm) {
            /* Fall back to LOD 0 if the requested LOD collapsed (tiny chunk). */
            if (lod != 0) cm = sr_terrain_chunk_mesh(sr, slot, cx, cz, 0);
            if (!cm) continue;
        }

        /* Per-chunk state (re-bound every submit; BGFX_DISCARD_ALL clears it). */
        sr_inline_bind_pbr_global(sr, pbr, view_id, scene, list);
        bgfx_set_transform(model->raw[0], 1);
        bgfx_set_texture(0,  sr->s_terrain_layer0, layer_tex[0], UINT32_MAX);
        bgfx_set_texture(4,  sr->s_terrain_layer3, layer_tex[3], UINT32_MAX);
        bgfx_set_texture(14, sr->s_terrain_layer1, layer_tex[1], UINT32_MAX);
        bgfx_set_texture(15, sr->s_terrain_layer2, layer_tex[2], UINT32_MAX);
        bgfx_set_uniform(sr->u_terrain_params, tparams, 1);

        if (per_tile_splat) {
            /* Bind this chunk's tile splat texture + remap the global terrain UV
             * into the tile's local [0..1]. */
            bgfx_texture_handle_t st =
                sr_terrain_tile_splat_tex(sr, slot, cx, cz, tg_dim);
            bgfx_set_texture(13, sr->s_terrain_splat,
                BGFX_HANDLE_IS_VALID(st) ? st : sr->white_tex, UINT32_MAX);
            float tile_uv[4] = { (float)cx / (float)tg_x,
                                 (float)cz / (float)tg_z,
                                 (float)tg_x, (float)tg_z };
            bgfx_set_uniform(sr->u_terrain_tile_uv, tile_uv, 1);
        } else {
            bgfx_set_texture(13, sr->s_terrain_splat, splat_h, UINT32_MAX);
            float tile_uv[4] = { 0.0f, 0.0f, 1.0f, 1.0f };  /* identity remap */
            bgfx_set_uniform(sr->u_terrain_tile_uv, tile_uv, 1);
        }

        jce_mesh_submit_terrain(cm, sr->renderer, view_id);
        sr->stat_terrain_chunks_drawn++;
    }
}

/* If entity e is a terrain, submit its chunks as depth-only shadow casters
 * into `view_id` and return true (so the shadow loop skips the generic mesh
 * path).  Mirrors sr_try_submit_mesh_renderer_model_shadow.  Reuses any chunk
 * mesh already cached by the colour pass; otherwise builds it at the coarse
 * shadow LOD.  Returns false for non-terrain entities. */
bool sr_try_submit_terrain_shadow(JceSceneRenderer *sr, JceScene *scene,
                                  JceEntity e, uint16_t view_id)
{
    if (!jce_scene_has_terrain(scene, e)) return false;
    JceTerrainComponent *tc = jce_scene_get_terrain(scene, e);
    if (!tc || !tc->visible || !tc->terrain_path[0]) return false;
    int slot = sr_terrain_find_or_load_slot(sr, tc->terrain_path);
    if (slot < 0 || sr->terrain_cache[slot].chunk_count <= 0) return false;
    if (!jce_scene_has_transform(scene, e)) return false;

    jce_mat4 model = jce_scene_get_world_matrix(scene, e);
    int ncx = sr->terrain_cache[slot].chunk_nx;
    int ncz = sr->terrain_cache[slot].chunk_nz;

    for (int cz = 0; cz < ncz; cz++)
    for (int cx = 0; cx < ncx; cx++) {
        int cidx = cz * ncx + cx;
        JceMesh *cm = sr->terrain_cache[slot].chunk_meshes[cidx];
        if (!cm) cm = sr_terrain_chunk_mesh(sr, slot, cx, cz,
                                            SR_TERRAIN_SHADOW_LOD);
        if (!cm) continue;
        bgfx_set_transform(model.raw[0], 1);
        jce_mesh_submit_shadow(cm, sr->renderer, view_id);
    }
    return true;
}
