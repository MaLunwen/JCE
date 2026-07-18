/*
 * jce_sr_environment.c  Scene-renderer environment module (split from
 * jce_scene_renderer.c).
 *
 * Vegetation scatter, water surface (Gerstner + FFT), tilemap chunked draw,
 * sky pass, cloth pass, async IBL bake + skybox scan, and the
 * time-of-day / weather / decals drivers.  Pure move from the monolithic
 * renderer: driver / cache entry points are declared in jce_sr_internal.h,
 * everything else stays file-static here.  No behaviour change.
 */

#include "jce_sr_internal.h"

#include <jce/resource/jce_image_decode.h>  /* density-mask load (large-world #8a) */
#include <jce/renderer/jce_render_pipeline.h> /* foliage Hi-Z perf default */
#include <math.h>    /* sqrtf: authored dome sun-dir normalization */
#include <string.h>

/* Resolve optional CPU-side images exactly like other runtime assets: a loose
 * callback override wins for development, then the embedded project PAK is
 * decoded in memory. Calling jce_image_decode_file() directly here made grass
 * density masks disappear once an executable was copied away from its cooked
 * directory, silently turning masked fields into full-density carpets. */
static bool sr_decode_image_asset(JceSceneRenderer *sr, const char *asset_path,
                                  JceImage *out)
{
    if (!sr || !asset_path || !asset_path[0] || !out) return false;

    if (sr->has_cbs && sr->cbs.resolve_path) {
        char host_path[1024];
        if (sr->cbs.resolve_path(asset_path, host_path,
                                 (int)sizeof(host_path), sr->cbs.userdata) &&
            jce_image_decode_file(host_path, out))
            return true;
    }

    if (sr->pak && jce_image_decode_pak(sr->pak, asset_path, out))
        return true;

    if (jce_image_decode_file(asset_path, out))
        return true;

    LOG_WARN(LOG_TAG, "image asset unavailable from runtime sources: %s",
             asset_path);
    return false;
}

/* ── Vegetation scatter draw (P0 foliage, roadmap 2.2) ─────────────────
 *
 * Deterministically scatters a referenced mesh over the bound terrain
 * (jce_foliage_scatter) and draws each instance with the shared model-draw
 * path (jce_model_draw — correct materials, proven path).  The scatter is
 * cached per entity and rebuilt only when a component parameter changes, so
 * the steady-state cost is N model submits.  GPU instancing (one batched
 * submit) is a perf follow-up; this first slice prioritises correctness. */

/* FNV-1a over the scatter-shaping fields; a change forces a re-scatter. */
static uint32_t sr_foliage_param_hash(const JceVegetationScatterComponent *vs)
{
    /* Shared FNV-1a (jce_hash.h) over the scatter-shaping fields; a change
     * forces a re-scatter.  Byte-wise append (vs the old word-wise mix) only
     * changes the cache-key value, which self-heals on the next scatter. */
    uint32_t h = jce_fnv1a32_append(JCE_FNV1A32_INIT, &vs->seed, sizeof vs->seed);
    h = jce_fnv1a32_append(h, &vs->density,       sizeof vs->density);
    h = jce_fnv1a32_append(h, &vs->area_x,        sizeof vs->area_x);
    h = jce_fnv1a32_append(h, &vs->area_z,        sizeof vs->area_z);
    h = jce_fnv1a32_append(h, &vs->max_slope_deg, sizeof vs->max_slope_deg);
    h = jce_fnv1a32_append(h, &vs->scale_min,     sizeof vs->scale_min);
    h = jce_fnv1a32_append(h, &vs->scale_max,     sizeof vs->scale_max);
    /* density mask path — a change re-scatters (large-world #8a). */
    h = jce_fnv1a32_append(h, vs->density_mask_path, (uint32_t)strlen(vs->density_mask_path));
    /* painted density grid — each brush stroke changes it -> re-scatter. */
    h = jce_fnv1a32_append(h, &vs->density_paint_active, sizeof vs->density_paint_active);
    if (vs->density_paint_active)
        h = jce_fnv1a32_append(h, vs->density_paint, (uint32_t)sizeof vs->density_paint);
    return h;
}

/* Free everything a VegetationScatter foliage slot owns (CPU instance
 * arrays + persistent/GPU-cull/tile GPU buffers) and zero it.  Used on
 * eviction, renderer destroy, and the scene-swap entity-cache reset. */
void sr_foliage_slot_free(JceSceneRenderer *sr, int slot)
{
    JCE_FREE(sr->foliage_cache[slot].insts);
    JCE_FREE(sr->foliage_cache[slot].roots);
    JCE_FREE(sr->foliage_cache[slot].lod_sorted);   /* 千万 S4 LOD-band buffer */
    if (sr->foliage_cache[slot].inst_vb_count) { /* 千万 S1 persistent instance VB */
        LOG_DEBUG(LOG_TAG, "[vbdbg] slot_free slot=%d idx=%u n=%u",
                 slot, sr->foliage_cache[slot].inst_vb.idx,
                 sr->foliage_cache[slot].inst_vb_count);
        bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].inst_vb);
    }
    if (sr->foliage_cache[slot].cull_cap) {      /* 千万 S2 GPU-cull buffers */
        bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].cull_visible);
        bgfx_destroy_dynamic_index_buffer(sr->foliage_cache[slot].cull_counter);
        bgfx_destroy_indirect_buffer(sr->foliage_cache[slot].cull_indirect);
    }
    if (sr->foliage_cache[slot].cull_lod_bands) {   /* 千万 S4 partitioned bufs */
        bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].cull_lod_visible);
        bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].cull_lod_counter);
        bgfx_destroy_indirect_buffer(sr->foliage_cache[slot].cull_lod_indirect);
    }
    if (sr->foliage_cache[slot].tiles) {            /* 千万 S5 tile residency */
        int nt = sr->foliage_cache[slot].tiles_x * sr->foliage_cache[slot].tiles_z;
        for (int t = 0; t < nt; ++t)
            if (sr->foliage_cache[slot].tiles[t].resident)
                bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].tiles[t].vb);
        JCE_FREE(sr->foliage_cache[slot].tiles);
    }
    memset(&sr->foliage_cache[slot], 0, sizeof sr->foliage_cache[slot]);
}

/* Find (or evict into) a foliage cache slot for entity `e`. */
static int sr_foliage_find_slot(JceSceneRenderer *sr, JceEntity e)
{
    int n = (int)(sizeof sr->foliage_cache / sizeof sr->foliage_cache[0]);
    int free_slot = -1;
    for (int i = 0; i < n; ++i) {
        if (sr->foliage_cache[i].used && sr->foliage_cache[i].entity == e)
            return i;
        if (free_slot < 0 && !sr->foliage_cache[i].used) free_slot = i;
    }
    if (free_slot >= 0) return free_slot;
    /* All slots taken: evict a deterministic one (rebuilds next time). */
    int victim = (int)((uint32_t)e % (uint32_t)n);
    sr_foliage_slot_free(sr, victim);
    return victim;
}

/* Build a TRS model matrix (uniform scale, Y rotation, translation),
 * column-major to match the renderer's jce_mat4 convention. */
static jce_mat4 sr_foliage_instance_matrix(const JceFoliageInstance *fi)
{
    const float s = fi->scale, c = cosf(fi->rot_y), sn = sinf(fi->rot_y);
    jce_mat4 m = jce_m4_identity();
    m.col[0].x =  s * c;  m.col[0].y = 0; m.col[0].z = -s * sn; m.col[0].w = 0;
    m.col[1].x =  0;      m.col[1].y = s; m.col[1].z =  0;      m.col[1].w = 0;
    m.col[2].x =  s * sn; m.col[2].y = 0; m.col[2].z =  s * c;  m.col[2].w = 0;
    m.col[3].x = fi->pos[0]; m.col[3].y = fi->pos[1]; m.col[3].z = fi->pos[2];
    m.col[3].w = 1.0f;
    return m;
}

/* Pack a float rgba [0..1] into bgfx 0xAABBGGRR. */
static uint32_t sr_line_abgr(const float c[4])
{
    float r = c[0] < 0.0f ? 0.0f : (c[0] > 1.0f ? 1.0f : c[0]);
    float g = c[1] < 0.0f ? 0.0f : (c[1] > 1.0f ? 1.0f : c[1]);
    float b = c[2] < 0.0f ? 0.0f : (c[2] > 1.0f ? 1.0f : c[2]);
    float a = c[3] < 0.0f ? 0.0f : (c[3] > 1.0f ? 1.0f : c[3]);
    uint32_t ri = (uint32_t)(r * 255.0f + 0.5f);
    uint32_t gi = (uint32_t)(g * 255.0f + 0.5f);
    uint32_t bi = (uint32_t)(b * 255.0f + 0.5f);
    uint32_t ai = (uint32_t)(a * 255.0f + 0.5f);
    return (ai << 24) | (bi << 16) | (gi << 8) | ri;
}

/* Transform a point by a column-major jce_mat4 (w = 1). */
static jce_vec3 sr_line_xf(const jce_mat4 *m, float x, float y, float z)
{
    jce_vec3 r;
    r.x = m->col[0].x*x + m->col[1].x*y + m->col[2].x*z + m->col[3].x;
    r.y = m->col[0].y*x + m->col[1].y*y + m->col[2].y*z + m->col[3].y;
    r.z = m->col[0].z*x + m->col[1].z*y + m->col[2].z*z + m->col[3].z;
    return r;
}

/* Shared camera-facing TRIANGLE RIBBON (PT_LINES produces nothing in the editor
 * pre-postfx offscreen; triangles via the color program DO render).  Width-
 * expanded quads, colour + width lerped along the polyline, optionally looped.
 * Points are transformed by `model` (identity => already world).  No new shader.
 * Shared by the Line renderer (loopable, local/world) and the Trail renderer
 * (open, world-space captured points). */
static void sr_draw_ribbon(JceSceneRenderer *sr, const JceCamera *camera,
                           uint16_t view_id, const jce_mat4 *model,
                           const float (*points)[3], int count, bool loop,
                           float w0, float w1,
                           const float c0[4], const float c1[4])
{
    if (count < 2) return;
    int pts = count;
    if (pts > JCE_LINE_MAX_POINTS) pts = JCE_LINE_MAX_POINTS;
    int segs = loop ? pts : (pts - 1);
    if (segs < 1) return;

    uint32_t vcount = (uint32_t)segs * 4u;   /* quad per segment */
    uint32_t icount = (uint32_t)segs * 6u;

    bgfx_vertex_layout_t layout;
    bgfx_vertex_layout_begin(&layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_COLOR0,   4, BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_end(&layout);

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &layout, vcount, &tib, icount, false))
        return;

    struct SrLineVtx { float x, y, z; uint32_t abgr; };
    struct SrLineVtx *v = (struct SrLineVtx *)tvb.data;
    uint16_t *idx = (uint16_t *)tib.data;

    jce_vec3 eye = camera ? jce_camera_get_position(camera)
                          : jce_v3(0.0f, 0.0f, 1.0e4f);

    uint32_t vi = 0, ii = 0;
    for (int s = 0; s < segs; ++s) {
        int   ia = s;
        int   ib = (s + 1) % pts;
        float ta = (pts > 1) ? (float)ia / (float)(pts - 1) : 0.0f;
        float tb = (loop && s == segs - 1)
                     ? 1.0f
                     : ((pts > 1) ? (float)ib / (float)(pts - 1) : 0.0f);

        jce_vec3 wa = sr_line_xf(model, points[ia][0], points[ia][1], points[ia][2]);
        jce_vec3 wb = sr_line_xf(model, points[ib][0], points[ib][1], points[ib][2]);

        jce_vec3 dir = jce_v3_sub(wb, wa);
        float    dl  = jce_v3_len(dir);
        if (dl < 1e-6f) continue;
        dir = jce_v3_scale(dir, 1.0f / dl);
        jce_vec3 mid = jce_v3_scale(jce_v3_add(wa, wb), 0.5f);
        jce_vec3 vdir = jce_v3_sub(mid, eye);
        float    vl = jce_v3_len(vdir);
        vdir = (vl > 1e-6f) ? jce_v3_scale(vdir, 1.0f / vl) : jce_v3(0,0,1);
        jce_vec3 side = jce_v3_cross(dir, vdir);
        float    sl = jce_v3_len(side);
        if (sl < 1e-6f) { side = jce_v3(0,1,0); sl = 1.0f; }
        side = jce_v3_scale(side, 1.0f / sl);

        float wda = w0 + (w1 - w0) * ta;
        float wdb = w0 + (w1 - w0) * tb;
        if (wda < 0.05f) wda = 0.05f;
        if (wdb < 0.05f) wdb = 0.05f;
        jce_vec3 sa = jce_v3_scale(side, wda * 0.5f);
        jce_vec3 sb = jce_v3_scale(side, wdb * 0.5f);

        float ca[4], cb[4];
        for (int c = 0; c < 4; ++c) {
            float d = c1[c] - c0[c];
            ca[c] = c0[c] + d * ta;
            cb[c] = c0[c] + d * tb;
        }
        uint32_t col_a = sr_line_abgr(ca), col_b = sr_line_abgr(cb);

        jce_vec3 p0 = jce_v3_add(wa, sa), p1 = jce_v3_sub(wa, sa);
        jce_vec3 p2 = jce_v3_add(wb, sb), p3 = jce_v3_sub(wb, sb);
        uint32_t base = vi;
        v[vi].x=p0.x; v[vi].y=p0.y; v[vi].z=p0.z; v[vi].abgr=col_a; ++vi;
        v[vi].x=p1.x; v[vi].y=p1.y; v[vi].z=p1.z; v[vi].abgr=col_a; ++vi;
        v[vi].x=p2.x; v[vi].y=p2.y; v[vi].z=p2.z; v[vi].abgr=col_b; ++vi;
        v[vi].x=p3.x; v[vi].y=p3.y; v[vi].z=p3.z; v[vi].abgr=col_b; ++vi;
        idx[ii++]=(uint16_t)base;     idx[ii++]=(uint16_t)(base+1); idx[ii++]=(uint16_t)(base+2);
        idx[ii++]=(uint16_t)(base+1); idx[ii++]=(uint16_t)(base+3); idx[ii++]=(uint16_t)(base+2);
    }
    if (ii == 0) return;

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, vcount);
    bgfx_set_transient_index_buffer(&tib, 0, ii);
    jce_mat4 ident = jce_m4_identity();
    bgfx_set_transform(ident.raw[0], 1);   /* positions are already world */

    /* Depth-TEST against opaque geometry (so a ribbon behind a tree/rock is
     * occluded) but do NOT write depth: these are translucent ribbons (wind
     * streaks, lightning, trails).  A blended primitive that writes Z punches a
     * depth hole that rejects any transparent content drawn AFTER it at greater
     * depth — the wind lines then appear to "cut through" / erase the rain,
     * smoke and particles behind them.  Matches the reference WindLines material
     * (transparent:true, depthWrite:false). */
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA | BGFX_STATE_BLEND_ALPHA;
    bgfx_set_state(state, 0);   /* no cull: ribbon is double-sided */

    JceShaderHandle sh = jce_renderer_get_program_color(sr->renderer);
    bgfx_program_handle_t prog;
    prog.idx = sh.idx;
    if (BGFX_HANDLE_IS_VALID(prog))
        bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

/* Line Renderer — colour/width-lerped ribbon, loopable; local points ride the
 * entity transform, world-space points draw directly. */
void sr_draw_line_renderer(JceSceneRenderer *sr, JceScene *scene,
                           EntityList *list, JceEntity e,
                           const JceCamera *camera, uint16_t view_id)
{
    (void)list;
    JceLineRendererComponent *lr = jce_scene_get_line_renderer(scene, e);
    if (!lr || lr->position_count < 2) return;
    jce_mat4 model = lr->use_world_space ? jce_m4_identity()
                                         : jce_scene_get_world_matrix(scene, e);
    sr_draw_ribbon(sr, camera, view_id, &model, lr->positions, lr->position_count,
                   lr->loop, lr->width_start, lr->width_end,
                   lr->color_start, lr->color_end);
}

/* Trail Renderer — the runtime-captured world-space point trail drawn as an open
 * ribbon.  Capture/growth happens in the runtime (rt_update_trails); here we
 * just draw whatever points the buffer currently holds (also previews a scene's
 * serialized trail in the editor). */
void sr_draw_trail_renderer(JceSceneRenderer *sr, JceScene *scene,
                            EntityList *list, JceEntity e,
                            const JceCamera *camera, uint16_t view_id)
{
    (void)list;
    JceTrailRendererComponent *tr = jce_scene_get_trail_renderer(scene, e);
    if (!tr || tr->point_count < 2) return;
    jce_mat4 ident = jce_m4_identity();   /* trail points are already world */
    sr_draw_ribbon(sr, camera, view_id, &ident, tr->points, tr->point_count,
                   false, tr->width_start, tr->width_end,
                   tr->color_start, tr->color_end);
}

/* 千万 S2: ensure the per-scatter GPU-cull buffers (compacted survivor mat4s,
 * survivor counter, indirect arg) exist and hold >= n slots.  Persistent, grown
 * geometrically — the cull rewrites them each frame so a fresh buffer carries no
 * stale data.  cull_cap==0 means "never created" (the zero-initialised handles
 * are not valid sentinels, so creation/teardown gate on cull_cap, like S1's
 * inst_vb gates on inst_vb_count).  Returns false on a failed create. */
static bool sr_foliage_ensure_cull_buffers(JceSceneRenderer *sr, int slot, uint32_t n)
{
    if (sr->foliage_cache[slot].cull_cap >= n && sr->foliage_cache[slot].cull_cap > 0)
        return true;

    const bool first = (sr->foliage_cache[slot].cull_cap == 0);
    uint32_t cap = sr->foliage_cache[slot].cull_cap ? sr->foliage_cache[slot].cull_cap : 1024u;
    while (cap < n) cap *= 2u;
    cap = (cap + 63u) & ~63u;   /* round to the 64-thread cull group */

    static bgfx_vertex_layout_t vis_layout; static bool vis_init = false;
    static bgfx_vertex_layout_t cnt_layout; static bool cnt_init = false;
    if (!vis_init) {
        bgfx_vertex_layout_begin(&vis_layout, BGFX_RENDERER_TYPE_NOOP);
        bgfx_vertex_layout_add(&vis_layout, BGFX_ATTRIB_TEXCOORD7, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&vis_layout, BGFX_ATTRIB_TEXCOORD6, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&vis_layout, BGFX_ATTRIB_TEXCOORD5, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&vis_layout, BGFX_ATTRIB_TEXCOORD4, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_end(&vis_layout); vis_init = true;
    }
    if (!cnt_init) {
        bgfx_vertex_layout_begin(&cnt_layout, BGFX_RENDERER_TYPE_NOOP);
        bgfx_vertex_layout_add(&cnt_layout, BGFX_ATTRIB_TEXCOORD0, 1, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_end(&cnt_layout); cnt_init = true;
    }

    if (!first)   /* regrow: free the old visible buffer (counter/indirect persist) */
        bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].cull_visible);
    sr->foliage_cache[slot].cull_visible = bgfx_create_dynamic_vertex_buffer(
        cap, &vis_layout,
        BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_COMPUTE_FORMAT_32X4 | BGFX_BUFFER_COMPUTE_TYPE_FLOAT);
    if (sr->foliage_cache[slot].cull_visible.idx == UINT16_MAX) {
        sr->foliage_cache[slot].cull_cap = 0;
        return false;
    }
    sr->foliage_cache[slot].cull_cap = cap;

    if (first) {
        /* 1024 elements (4KB), not 64 (256B): bgfx's shared dynamic-VB pool
         * mis-handles the tiny 256B COMPUTE block adjacent to the big visible
         * buffer (layout-sensitive heap corruption reproduced via the
         * benchmark scatter rig; order-swapping the creates crashes HARDER).
         * A pool-granular allocation sidesteps the buggy small-block path;
         * the shader only ever touches counter[0..bands-1]. */
        /* DYNAMIC INDEX buffer, not vertex: co-locating this tiny COMPUTE
         * counter in the dynamic-VERTEX pool next to the big visible buffer
         * reproducibly corrupted the heap (benchmark scatter rig: crash on
         * exit; creation-order swap crashed harder — a bgfx VB-pool layout
         * bug).  The index-buffer pool is separate, and INDEX32 is the
         * canonical carrier for 32-bit UINT compute data. */
        sr->foliage_cache[slot].cull_counter = bgfx_create_dynamic_index_buffer(
            64,
            BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ_WRITE |
            BGFX_BUFFER_COMPUTE_FORMAT_32X1 | BGFX_BUFFER_COMPUTE_TYPE_UINT);
        sr->foliage_cache[slot].cull_indirect = bgfx_create_indirect_buffer(1);
        if (sr->foliage_cache[slot].cull_counter.idx == UINT16_MAX ||
            sr->foliage_cache[slot].cull_indirect.idx == UINT16_MAX) {
            sr->foliage_cache[slot].cull_cap = 0;
            return false;
        }
    }
    return true;
}

/* 千万 S4/S5 LOD-in-cull: ensure the band-PARTITIONED cull buffers — one
 * visible buffer of bands × cap_band mat4 slots, one counter buffer (uint per
 * band) and one indirect buffer (element per band).  cap_band is the per-band
 * partition capacity (survivors past it are skipped — graceful overflow).
 * Recreated when the partition capacity or band count changes. */
static bool sr_foliage_ensure_lod_cull_buffers(JceSceneRenderer *sr, int slot,
                                               uint32_t cap_band, uint32_t bands)
{
    if (bands == 0u) return false;
    if (bands > JCE_FOLIAGE_LOD_BANDS) bands = JCE_FOLIAGE_LOD_BANDS;
    cap_band = (cap_band + 63u) & ~63u;
    if (cap_band == 0u) cap_band = 64u;

    if (sr->foliage_cache[slot].cull_lod_cap == cap_band &&
        sr->foliage_cache[slot].cull_lod_bands == (uint8_t)bands)
        return true;   /* already sized for this partition + band count */

    if (sr->foliage_cache[slot].cull_lod_bands) {
        bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].cull_lod_visible);
        bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].cull_lod_counter);
        bgfx_destroy_indirect_buffer(sr->foliage_cache[slot].cull_lod_indirect);
        sr->foliage_cache[slot].cull_lod_bands = 0;
        sr->foliage_cache[slot].cull_lod_cap   = 0;
    }

    static bgfx_vertex_layout_t vl; static bool vl_i = false;
    static bgfx_vertex_layout_t cl; static bool cl_i = false;
    if (!vl_i) {
        bgfx_vertex_layout_begin(&vl, BGFX_RENDERER_TYPE_NOOP);
        bgfx_vertex_layout_add(&vl, BGFX_ATTRIB_TEXCOORD7, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&vl, BGFX_ATTRIB_TEXCOORD6, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&vl, BGFX_ATTRIB_TEXCOORD5, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&vl, BGFX_ATTRIB_TEXCOORD4, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_end(&vl); vl_i = true;
    }
    if (!cl_i) {
        bgfx_vertex_layout_begin(&cl, BGFX_RENDERER_TYPE_NOOP);
        bgfx_vertex_layout_add(&cl, BGFX_ATTRIB_TEXCOORD0, 1, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_end(&cl); cl_i = true;
    }

    sr->foliage_cache[slot].cull_lod_visible = bgfx_create_dynamic_vertex_buffer(
        cap_band * bands, &vl,
        BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_COMPUTE_FORMAT_32X4 | BGFX_BUFFER_COMPUTE_TYPE_FLOAT);
    sr->foliage_cache[slot].cull_lod_counter = bgfx_create_dynamic_vertex_buffer(
        64, &cl,
        BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_COMPUTE_FORMAT_32X1 | BGFX_BUFFER_COMPUTE_TYPE_UINT);
    sr->foliage_cache[slot].cull_lod_indirect =
        bgfx_create_indirect_buffer(JCE_FOLIAGE_LOD_BANDS);
    if (sr->foliage_cache[slot].cull_lod_visible.idx  == UINT16_MAX ||
        sr->foliage_cache[slot].cull_lod_counter.idx  == UINT16_MAX ||
        sr->foliage_cache[slot].cull_lod_indirect.idx == UINT16_MAX) {
        if (sr->foliage_cache[slot].cull_lod_visible.idx != UINT16_MAX)
            bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].cull_lod_visible);
        if (sr->foliage_cache[slot].cull_lod_counter.idx != UINT16_MAX)
            bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].cull_lod_counter);
        if (sr->foliage_cache[slot].cull_lod_indirect.idx != UINT16_MAX)
            bgfx_destroy_indirect_buffer(sr->foliage_cache[slot].cull_lod_indirect);
        sr->foliage_cache[slot].cull_lod_cap = 0;
        return false;
    }
    sr->foliage_cache[slot].cull_lod_cap   = cap_band;
    sr->foliage_cache[slot].cull_lod_bands = (uint8_t)bands;
    return true;
}

/* 千万 S4: distance-LOD bands for a MODEL scatter — partition the static
 * instances by camera distance into contiguous bands (counting sort) and submit
 * each band at its own LOD level, so distant heavy meshes render reduced-LOD
 * geometry (GPU vertex cost ∝ detail × distance, not flat full-detail × N).  The
 * partition is cached and only recomputed when the camera moves past a quarter of
 * the LOD step (O(1) steady-state, like S1).  band b → LOD level b (0 = base,
 * clamped to the model's available LODs by the mesh submit).  Returns false (→
 * caller draws the single LOD0 submit) when the model has no reduced LODs. */
static bool sr_foliage_draw_model_lod(JceSceneRenderer *sr, int slot,
                                      const JceModel *model, uint16_t view_id,
                                      const JceCamera *camera, uint32_t n)
{
    if (!camera) return false;
    uint32_t max_lod = jce_model_max_lod(model);  /* reduced levels available */
    if (max_lod == 0u) return false;              /* no reduced LOD → single LOD0 submit */
    uint32_t bands = max_lod + 1u;                /* LOD0 + the reduced levels */
    if (bands > JCE_FOLIAGE_LOD_BANDS) bands = JCE_FOLIAGE_LOD_BANDS;

    static float s_step = -1.0f;
    if (s_step < 0.0f) { const char *e = getenv("JCE_FOLIAGE_LOD_STEP");
                         s_step = (e && atof(e) > 0.0) ? (float)atof(e) : 40.0f; }

    const jce_vec3 cp = jce_camera_get_position(camera);
    const jce_mat4 *roots = sr->foliage_cache[slot].roots;

    const float dx = cp.x - sr->foliage_cache[slot].lod_cam_pos.x;
    const float dy = cp.y - sr->foliage_cache[slot].lod_cam_pos.y;
    const float dz = cp.z - sr->foliage_cache[slot].lod_cam_pos.z;
    const float eps = s_step * 0.25f;
    const bool moved = (dx*dx + dy*dy + dz*dz) > eps * eps;

    if (!sr->foliage_cache[slot].lod_valid || moved ||
        sr->foliage_cache[slot].lod_sorted_cap < n) {
        if (sr->foliage_cache[slot].lod_sorted_cap < n) {
            jce_mat4 *nb = (jce_mat4 *)JCE_REALLOC(
                sr->foliage_cache[slot].lod_sorted, (size_t)n * sizeof(jce_mat4));
            if (!nb) return false;
            sr->foliage_cache[slot].lod_sorted     = nb;
            sr->foliage_cache[slot].lod_sorted_cap = n;
        }
        uint32_t counts[JCE_FOLIAGE_LOD_BANDS];
        for (uint32_t b = 0; b < bands; ++b) counts[b] = 0u;
        for (uint32_t i = 0; i < n; ++i) {
            const float ox = roots[i].col[3].x - cp.x;
            const float oy = roots[i].col[3].y - cp.y;
            const float oz = roots[i].col[3].z - cp.z;
            uint32_t b = (uint32_t)(sqrtf(ox*ox + oy*oy + oz*oz) / s_step);
            if (b >= bands) b = bands - 1u;
            counts[b]++;
        }
        uint32_t acc = 0u, cursor[JCE_FOLIAGE_LOD_BANDS];
        for (uint32_t b = 0; b < bands; ++b) {
            sr->foliage_cache[slot].lod_band_start[b] = acc;
            cursor[b] = acc;
            acc += counts[b];
        }
        sr->foliage_cache[slot].lod_band_start[bands] = acc;
        for (uint32_t i = 0; i < n; ++i) {
            const float ox = roots[i].col[3].x - cp.x;
            const float oy = roots[i].col[3].y - cp.y;
            const float oz = roots[i].col[3].z - cp.z;
            uint32_t b = (uint32_t)(sqrtf(ox*ox + oy*oy + oz*oz) / s_step);
            if (b >= bands) b = bands - 1u;
            sr->foliage_cache[slot].lod_sorted[cursor[b]++] = roots[i];
        }
        sr->foliage_cache[slot].lod_bands   = (uint8_t)bands;
        sr->foliage_cache[slot].lod_cam_pos = cp;
        sr->foliage_cache[slot].lod_valid   = true;
    }

    const uint32_t b_used = sr->foliage_cache[slot].lod_bands;
    for (uint32_t b = 0; b < b_used; ++b) {
        const uint32_t start = sr->foliage_cache[slot].lod_band_start[b];
        const uint32_t cnt   = sr->foliage_cache[slot].lod_band_start[b + 1u] - start;
        if (cnt == 0u) continue;
        jce_model_set_draw_lod(b);   /* band b → LOD level b (mesh clamps to avail) */
        jce_model_draw_instanced_tinted(model, sr->renderer, view_id,
                                        &sr->foliage_cache[slot].lod_sorted[start],
                                        NULL, cnt);
    }
    jce_model_set_draw_lod(0u);      /* reset the shared per-draw LOD state */
    return true;
}

/* 千万 S5: tile world AABB (conservative, analytic — no scatter needed).
 * XZ = the tile rect grown by the model's scaled footprint; Y = terrain height
 * range sampled on a coarse grid (or the origin height without terrain) grown
 * by the model's scaled vertical extent. */
static void sr_foliage_tile_aabb(JceSceneRenderer *sr, int slot, int tx, int tz,
                                 const JceTerrain *terr,
                                 const float model_min[3], const float model_max[3],
                                 jce_vec3 *out_mn, jce_vec3 *out_mx)
{
    const float sx0 = sr->foliage_cache[slot].tile_origin.x
                    - 0.5f * sr->foliage_cache[slot].tile_params.area_x
                    + (float)tx * sr->foliage_cache[slot].tile_w;
    const float sz0 = sr->foliage_cache[slot].tile_origin.z
                    - 0.5f * sr->foliage_cache[slot].tile_params.area_z
                    + (float)tz * sr->foliage_cache[slot].tile_h;
    const float sx1 = sx0 + sr->foliage_cache[slot].tile_w;
    const float sz1 = sz0 + sr->foliage_cache[slot].tile_h;
    float ymin = sr->foliage_cache[slot].tile_origin.y;
    float ymax = ymin;
    if (terr) {
        for (int iz = 0; iz <= 3; ++iz)
            for (int ix = 0; ix <= 3; ++ix) {
                float wx = sx0 + (sx1 - sx0) * (float)ix / 3.0f;
                float wz = sz0 + (sz1 - sz0) * (float)iz / 3.0f;
                float h  = jce_terrain_sample_height((JceTerrain *)terr, wx, wz);
                if (h < ymin) ymin = h;
                if (h > ymax) ymax = h;
            }
    }
    const float smax = sr->foliage_cache[slot].tile_params.scale_max;
    float grow_xz = smax * fmaxf(fmaxf(fabsf(model_min[0]), fabsf(model_max[0])),
                                 fmaxf(fabsf(model_min[2]), fabsf(model_max[2])));
    out_mn->x = sx0 - grow_xz; out_mn->z = sz0 - grow_xz;
    out_mx->x = sx1 + grow_xz; out_mx->z = sz1 + grow_xz;
    out_mn->y = ymin + smax * fminf(model_min[1], 0.0f) - 1.0f;
    out_mx->y = ymax + smax * fmaxf(model_max[1], 0.0f) + 1.0f;
}

/* 千万 S5: (re)generate one tile's instances (deterministic: seed ^ tile hash)
 * and upload them into a fresh COMPUTE_READ roots VB.  Uses the slot's scratch
 * insts/roots arrays.  Returns true when the tile is resident afterwards. */
static bool sr_foliage_tile_make_resident(JceSceneRenderer *sr, int slot,
                                          int tx, int tz, JceTerrain *terr)
{
    struct SrFoliageTile *tile =
        &sr->foliage_cache[slot].tiles[tz * sr->foliage_cache[slot].tiles_x + tx];
    if (tile->resident) return true;

    JceFoliageScatterParams p = sr->foliage_cache[slot].tile_params;
    p.area_x = sr->foliage_cache[slot].tile_w;
    p.area_z = sr->foliage_cache[slot].tile_h;
    p.seed   = p.seed ^ (uint32_t)(tx * 73856093) ^ (uint32_t)(tz * 19349663);
    jce_vec3 origin = {
        sr->foliage_cache[slot].tile_origin.x
            - 0.5f * sr->foliage_cache[slot].tile_params.area_x
            + ((float)tx + 0.5f) * sr->foliage_cache[slot].tile_w,
        sr->foliage_cache[slot].tile_origin.y,
        sr->foliage_cache[slot].tile_origin.z
            - 0.5f * sr->foliage_cache[slot].tile_params.area_z
            + ((float)tz + 0.5f) * sr->foliage_cache[slot].tile_h,
    };

    uint32_t want = (uint32_t)((double)p.density * (double)p.area_x * (double)p.area_z);
    if (want == 0) want = 1;
    if (want > JCE_FOLIAGE_MAX_INSTANCES) want = JCE_FOLIAGE_MAX_INSTANCES;
    if (sr->foliage_cache[slot].inst_cap < want) {
        JceFoliageInstance *nb = (JceFoliageInstance *)JCE_REALLOC(
            sr->foliage_cache[slot].insts, (size_t)want * sizeof(JceFoliageInstance));
        if (!nb) return false;
        sr->foliage_cache[slot].insts    = nb;
        sr->foliage_cache[slot].inst_cap = want;
    }
    uint32_t nc = jce_foliage_scatter(&p, terr, &origin,
                                      sr->foliage_cache[slot].insts, want);
    if (nc == 0) { tile->count = 0; return false; }
    if (sr->foliage_cache[slot].roots_cap < nc) {
        jce_mat4 *nr = (jce_mat4 *)JCE_REALLOC(
            sr->foliage_cache[slot].roots, (size_t)nc * sizeof(jce_mat4));
        if (!nr) return false;
        sr->foliage_cache[slot].roots     = nr;
        sr->foliage_cache[slot].roots_cap = nc;
    }
    for (uint32_t i = 0; i < nc; ++i)
        sr->foliage_cache[slot].roots[i] =
            sr_foliage_instance_matrix(&sr->foliage_cache[slot].insts[i]);

    bgfx_vertex_layout_t il;
    bgfx_vertex_layout_begin(&il, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD7, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD6, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD5, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD4, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&il);
    tile->vb = bgfx_create_dynamic_vertex_buffer(nc, &il,
        BGFX_BUFFER_COMPUTE_READ | BGFX_BUFFER_COMPUTE_FORMAT_32X4 | BGFX_BUFFER_COMPUTE_TYPE_FLOAT);
    if (tile->vb.idx == UINT16_MAX) return false;
    bgfx_update_dynamic_vertex_buffer(tile->vb, 0,
        bgfx_copy(sr->foliage_cache[slot].roots, nc * (uint32_t)sizeof(jce_mat4)));
    tile->count    = nc;
    tile->resident = true;
    sr->foliage_cache[slot].tiles_resident++;
    return true;
}

/* 千万 S4/S5 GPU LOD-in-cull for a single-primitive MODEL scatter: one compute
 * dispatch per (visible, resident) roots VB culls + LOD-classifies every
 * instance into band PARTITIONS of one survivor buffer; one indirect draw per
 * band renders that band's reduced LOD (startInstance = partition base) with
 * the cross-fade program (千万 ②).  Non-tiled scatters are a single "tile".
 * Tiled scatters (S5) stream tile roots in/out by frustum + far distance with
 * an LRU budget — a 10M-instance AUTHORED field costs only its visible
 * neighbourhood.  Returns true if it fully handled the scatter. */
static bool sr_foliage_gpu_lod_cull_draw(JceSceneRenderer *sr, int slot,
        const JceModel *model, const JceSkinnedMesh *lsm, uint32_t bands,
        uint32_t n, uint16_t view_id, const JceCamera *camera,
        JceScene *scene, EntityList *list)
{
    if (bands == 0u || bands > JCE_FOLIAGE_LOD_BANDS) return false;

    JceShaderHandle prog_inst = jce_renderer_get_program_pbr_inst_fade(sr->renderer);
    bool fade_prog = (prog_inst.idx != UINT16_MAX);
    if (!fade_prog)
        prog_inst = jce_renderer_get_program_pbr_inst(sr->renderer);
    if (prog_inst.idx == UINT16_MAX) return false;

    const bool tiled = (sr->foliage_cache[slot].tiles != NULL);

    /* Tunables: band step (m), cross-fade width (m, 0=off → hard switch), far
     * draw distance (tiled default 400m; non-tiled unlimited = legacy). */
    static float s_step = -1.0f;
    if (s_step < 0.0f) { const char *e = getenv("JCE_FOLIAGE_LOD_STEP");
                         s_step = (e && atof(e) > 0.0) ? (float)atof(e) : 40.0f; }
    static float s_fade = -1.0f;
    if (s_fade < 0.0f) { const char *e = getenv("JCE_FOLIAGE_LOD_FADE");
                         s_fade = (e && e[0]) ? (float)atof(e) : 6.0f;
                         if (s_fade < 0.0f) s_fade = 0.0f; }
    static float s_far = -1.0f;
    if (s_far < 0.0f) { const char *e = getenv("JCE_FOLIAGE_FAR");
                        s_far = (e && atof(e) > 0.0) ? (float)atof(e) : 400.0f; }
    const float fade_w = fade_prog ? s_fade : 0.0f;
    const float far_d  = tiled ? s_far : 0.0f;

    /* (1) non-tiled: COMPUTE_READ persistent roots VB (keyed on param_hash). */
    if (!tiled &&
        (sr->foliage_cache[slot].inst_vb_hash != sr->foliage_cache[slot].param_hash ||
         sr->foliage_cache[slot].inst_vb_count != n)) {
        if (sr->foliage_cache[slot].inst_vb_count) {
            LOG_DEBUG(LOG_TAG, "[vbdbg] cull-ensure destroy slot=%d idx=%u n=%u",
                     slot, sr->foliage_cache[slot].inst_vb.idx,
                     sr->foliage_cache[slot].inst_vb_count);
            bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].inst_vb);
        }
        bgfx_vertex_layout_t il;
        bgfx_vertex_layout_begin(&il, BGFX_RENDERER_TYPE_NOOP);
        bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD7, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD6, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD5, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD4, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_end(&il);
        sr->foliage_cache[slot].inst_vb = bgfx_create_dynamic_vertex_buffer(n, &il,
            BGFX_BUFFER_COMPUTE_READ | BGFX_BUFFER_COMPUTE_FORMAT_32X4 | BGFX_BUFFER_COMPUTE_TYPE_FLOAT);
        LOG_DEBUG(LOG_TAG, "[vbdbg] cull-ensure create slot=%d idx=%u n=%u",
                 slot, sr->foliage_cache[slot].inst_vb.idx, n);
        if (sr->foliage_cache[slot].inst_vb.idx == UINT16_MAX) {
            sr->foliage_cache[slot].inst_vb_count = 0u; return false;
        }
        const bgfx_memory_t *mem = bgfx_copy(sr->foliage_cache[slot].roots,
                                             n * (uint32_t)sizeof(jce_mat4));
        bgfx_update_dynamic_vertex_buffer(sr->foliage_cache[slot].inst_vb, 0, mem);
        sr->foliage_cache[slot].inst_vb_hash  = sr->foliage_cache[slot].param_hash;
        sr->foliage_cache[slot].inst_vb_count = n;
    }

    /* (2) band-partitioned cull buffers.  Partition capacity: non-tiled = the
     * whole scatter (a band's survivors ⊆ n); tiled = a visible-set budget
     * (survivors past it are skipped — graceful overflow). */
    static int s_capb = -1;
    if (s_capb < 0) { const char *e = getenv("JCE_FOLIAGE_LOD_CAP");
                      s_capb = (e && atoi(e) > 0) ? atoi(e) : 65536; }
    const uint32_t cap_band = tiled ? (uint32_t)s_capb : n;
    if (!sr_foliage_ensure_lod_cull_buffers(sr, slot, cap_band, bands)) return false;
    const uint32_t capb_real = sr->foliage_cache[slot].cull_lod_cap;

    /* (3) per-band reduced index counts; (4) local AABB; camera. */
    uint32_t nidx[8];
    memset(nidx, 0, sizeof nidx);
    for (uint32_t b = 0; b < bands; ++b)
        nidx[b] = (b == 0u) ? jce_skinned_mesh_index_count(lsm)
                            : jce_skinned_mesh_lod_index_count(lsm, b - 1u);

    float lmin[3], lmax[3];
    if (!jce_model_get_aabb(model, lmin, lmax)) return false;
    jce_vec3 lc = { (lmin[0] + lmax[0]) * 0.5f, (lmin[1] + lmax[1]) * 0.5f, (lmin[2] + lmax[2]) * 0.5f };
    jce_vec3 le = { (lmax[0] - lmin[0]) * 0.5f, (lmax[1] - lmin[1]) * 0.5f, (lmax[2] - lmin[2]) * 0.5f };
    jce_vec3 cam = jce_camera_get_position(camera);

    /* (5) Hi-Z (same gate as the primitive foliage cull: perf default-on, env
     * override, requires the SSAO depth prepass). */
    bool fol_hiz = jce_render_pipeline_perf_enabled(JCE_RP_PERF_HIZ_OCCLUSION, true);
    { const char *v = getenv("JCE_HIZ_OCCLUSION"); if (v && v[0]) fol_hiz = (v[0] != '0'); }
    fol_hiz = fol_hiz && sr->ssao_valid;
    jce_gpu_scene_set_hiz(sr->gpu_scene,
        fol_hiz ? sr->ssao_depth_tex.idx : (uint16_t)UINT16_MAX,
        (const jce_mat4 *)sr->frame_prev_vp, sr->ssao_w, sr->ssao_h, fol_hiz);

    /* (6) reset the band counters + build the Hi-Z pyramid once. */
    bool hiz_ready = false;
    if (!jce_gpu_scene_foliage_lod_begin(sr->gpu_scene, sr->gpu_reset_view,
            sr->foliage_cache[slot].cull_lod_counter.idx, bands, &hiz_ready))
        return false;

    if (!tiled) {
        /* Single "tile": one dispatch over the whole persistent roots VB. */
        jce_gpu_scene_foliage_lod_tile(sr->gpu_scene, sr->gpu_cull_view,
            sr->foliage_cache[slot].inst_vb.idx, n,
            sr->foliage_cache[slot].cull_lod_visible.idx,
            sr->foliage_cache[slot].cull_lod_counter.idx,
            sr->gpu_frame_planes, lc, le, cam, far_d,
            s_step, bands, capb_real, fade_w, hiz_ready);
    } else {
        /* 千万 S5: stream + dispatch the visible resident tiles.
         *   - far-distance reject (tile fully beyond the draw distance)
         *   - frustum test each tile's conservative AABB (CPU, ~grid tiles)
         *   - make up to GEN_BUDGET tiles resident per frame (hitch control)
         *   - LRU-evict past the residency budget */
        static int s_resident_max = -1;
        if (s_resident_max < 0) { const char *e = getenv("JCE_FOLIAGE_TILES_RESIDENT");
                                  s_resident_max = (e && atoi(e) > 0) ? atoi(e) : 64; }
        enum { GEN_BUDGET = 2 };
        int gen_left = GEN_BUDGET;
        JceTerrain *terr = NULL;   /* terrain lookup for on-demand tile gen */
        for (int i = 0; i < list->count && !terr; ++i) {
            JceEntity te = list->entities[i];
            if (!jce_scene_has_terrain(scene, te)) continue;
            JceTerrainComponent *tc = jce_scene_get_terrain(scene, te);
            if (!tc || !tc->terrain_path[0]) continue;
            int tslot = sr_terrain_find_or_load_slot(sr, tc->terrain_path);
            if (tslot >= 0) terr = sr->terrain_cache[tslot].terrain;
        }

        const int txn = sr->foliage_cache[slot].tiles_x;
        const int tzn = sr->foliage_cache[slot].tiles_z;
        static uint32_t s_lru_stamp = 0u;
        s_lru_stamp++;
        for (int tz = 0; tz < tzn; ++tz)
        for (int tx = 0; tx < txn; ++tx) {
            struct SrFoliageTile *tile =
                &sr->foliage_cache[slot].tiles[tz * txn + tx];
            /* far reject: nearest XZ point of the tile AABB vs camera */
            float dxx = fmaxf(fmaxf(tile->mn.x - cam.x, cam.x - tile->mx.x), 0.0f);
            float dzz = fmaxf(fmaxf(tile->mn.z - cam.z, cam.z - tile->mx.z), 0.0f);
            if (far_d > 0.0f && (dxx * dxx + dzz * dzz) > far_d * far_d)
                continue;
            /* frustum test the conservative AABB */
            jce_vec3 c = { (tile->mn.x + tile->mx.x) * 0.5f,
                           (tile->mn.y + tile->mx.y) * 0.5f,
                           (tile->mn.z + tile->mx.z) * 0.5f };
            jce_vec3 h = { (tile->mx.x - tile->mn.x) * 0.5f,
                           (tile->mx.y - tile->mn.y) * 0.5f,
                           (tile->mx.z - tile->mn.z) * 0.5f };
            bool in = true;
            for (int p = 0; p < 6; ++p) {
                const jce_vec4 *pl = &sr->gpu_frame_planes[p];
                float d = pl->x * c.x + pl->y * c.y + pl->z * c.z + pl->w;
                float r = fabsf(pl->x) * h.x + fabsf(pl->y) * h.y + fabsf(pl->z) * h.z;
                if (d + r < 0.0f) { in = false; break; }
            }
            if (!in) continue;
            if (!tile->resident) {
                if (gen_left <= 0) continue;       /* stream in next frames */
                if (!sr_foliage_tile_make_resident(sr, slot, tx, tz, terr))
                    continue;
                gen_left--;
            }
            tile->last_used = s_lru_stamp;
            jce_gpu_scene_foliage_lod_tile(sr->gpu_scene, sr->gpu_cull_view,
                tile->vb.idx, tile->count,
                sr->foliage_cache[slot].cull_lod_visible.idx,
                sr->foliage_cache[slot].cull_lod_counter.idx,
                sr->gpu_frame_planes, lc, le, cam, far_d,
                s_step, bands, capb_real, fade_w, hiz_ready);
        }
        /* LRU eviction past the residency budget. */
        while ((int)sr->foliage_cache[slot].tiles_resident > s_resident_max) {
            int oldest = -1; uint32_t stamp = UINT32_MAX;
            for (int t = 0; t < txn * tzn; ++t) {
                struct SrFoliageTile *tile = &sr->foliage_cache[slot].tiles[t];
                if (tile->resident && tile->last_used < stamp &&
                    tile->last_used != s_lru_stamp) {
                    stamp = tile->last_used; oldest = t;
                }
            }
            if (oldest < 0) break;   /* everything is in use this frame */
            bgfx_destroy_dynamic_vertex_buffer(
                sr->foliage_cache[slot].tiles[oldest].vb);
            sr->foliage_cache[slot].tiles[oldest].resident = false;
            sr->foliage_cache[slot].tiles_resident--;
        }
    }

    /* (7) indirect args (one element per band)... */
    if (!jce_gpu_scene_foliage_lod_end(sr->gpu_scene, sr->gpu_cull_view,
            sr->foliage_cache[slot].cull_lod_counter.idx,
            sr->foliage_cache[slot].cull_lod_indirect.idx,
            bands, capb_real, nidx))
        return false;

    /* (8) ... then bind the global PBR + forward+ once and issue one indirect
     * draw per band at its LOD index buffer (cross-fade program when built). */
    JcePbrMaterial def = jce_pbr_material_default();
    sr_inline_bind_pbr_global(sr, &def, view_id, scene, list);
    if (sr->fp_active_frame)
        jce_forwardplus_bind(sr->forwardplus);
    jce_model_draw_foliage_lod_indirect(model, sr->renderer, view_id,
        (uint16_t)prog_inst.idx, bands,
        sr->foliage_cache[slot].cull_lod_visible.idx,
        sr->foliage_cache[slot].cull_lod_indirect.idx);

    static uint32_t s_diag_bands = 0u;
    if (s_diag_bands != bands) {
        s_diag_bands = bands;
        LOG_INFO(LOG_TAG, "foliage GPU LOD-in-cull ENGAGED: %u instances%s, %u LOD "
                 "bands (step=%.0fm, fade=%.1fm, far=%.0fm, hiz=%d, cap/band=%u)",
                 n, tiled ? " (tiled)" : "", bands, (double)s_step,
                 (double)fade_w, (double)far_d, (int)fol_hiz, capb_real);
    }
    return true;
}

void sr_draw_foliage(JceSceneRenderer *sr, JceScene *scene,
                            EntityList *list, JceEntity e, uint16_t view_id,
                            const JceCamera *camera)
{
    JceVegetationScatterComponent *vs = jce_scene_get_vegetation_scatter(scene, e);
    if (!vs || !vs->visible) return;   /* mesh_path empty → primitive (mesh_shape) path */
    if (!jce_scene_has_transform(scene, e)) return;

    int slot = sr_foliage_find_slot(sr, e);
    if (slot < 0) return;

    const uint32_t ph = sr_foliage_param_hash(vs);
    const bool rebuild = !sr->foliage_cache[slot].used ||
                         sr->foliage_cache[slot].param_hash != ph ||
                         strcmp(sr->foliage_cache[slot].mesh_path, vs->mesh_path) != 0;

    if (rebuild) {
        /* Resolve a terrain to follow: first terrain entity in the draw list
         * (loaded on demand).  None → flat placement at the entity's Y. */
        JceTerrain *terr = NULL;
        for (int i = 0; i < list->count; ++i) {
            JceEntity te = list->entities[i];
            if (!jce_scene_has_terrain(scene, te)) continue;
            JceTerrainComponent *tc = jce_scene_get_terrain(scene, te);
            if (!tc || !tc->terrain_path[0]) continue;
            int tslot = sr_terrain_find_or_load_slot(sr, tc->terrain_path);
            if (tslot >= 0) { terr = sr->terrain_cache[tslot].terrain; break; }
        }

        jce_mat4 wm = jce_scene_get_world_matrix(scene, e);
        jce_vec3 origin = { wm.col[3].x, wm.col[3].y, wm.col[3].z };

        JceFoliageScatterParams p;
        p.seed          = vs->seed;
        p.density       = vs->density;
        p.area_x        = vs->area_x;
        p.area_z        = vs->area_z;
        p.max_slope_deg = vs->max_slope_deg;
        p.scale_min     = vs->scale_min;
        p.scale_max     = vs->scale_max;
        p.density_mask  = NULL;
        p.mask_dim      = 0;
        p.mask_world_size = 0.0f;   /* legacy local-rect UV */

        /* Density mask (large-world #8a): decode the grayscale mask asset into a
         * square float grid and feed the scatter so painted/sparse regions thin
         * out. Loaded only on rebuild (param-hash gated); freed after scatter. */
        float *mask_grid = NULL;
        if (vs->density_paint_active) {
            /* In-editor painted grid (large-world #8a brush) supersedes the
             * external mask asset. */
            int dim = JCE_VEG_PAINT_DIM;
            mask_grid = (float *)JCE_MALLOC((size_t)dim * (size_t)dim * sizeof(float));
            if (mask_grid) {
                for (int i = 0; i < dim * dim; ++i)
                    mask_grid[i] = (float)vs->density_paint[i] / 255.0f;
                p.density_mask = mask_grid;
                p.mask_dim     = dim;
            }
        } else if (vs->density_mask_path[0]) {
            JceImage img;
            memset(&img, 0, sizeof img);
            if (sr_decode_image_asset(sr, vs->density_mask_path, &img) && img.pixels &&
                img.width > 0 && img.height > 0) {
                int dim = (int)img.width;
                if (dim > 256) dim = 256;
                mask_grid = (float *)JCE_MALLOC((size_t)dim * (size_t)dim * sizeof(float));
                if (mask_grid) {
                    for (int y = 0; y < dim; ++y)
                        for (int x = 0; x < dim; ++x) {
                            int sx = (int)((int64_t)x * (int64_t)img.width  / dim);
                            int sy = (int)((int64_t)y * (int64_t)img.height / dim);
                            if (sx >= (int)img.width)  sx = (int)img.width  - 1;
                            if (sy >= (int)img.height) sy = (int)img.height - 1;
                            mask_grid[(size_t)y * dim + x] =
                                img.pixels[((size_t)sy * img.width + sx) * 4u] / 255.0f;
                        }
                    p.density_mask = mask_grid;
                    p.mask_dim     = dim;
                }
            }
            jce_image_free(&img);
        }

        uint32_t want = (uint32_t)((double)p.density * (double)p.area_x * (double)p.area_z);

        /* Tear down any previous tiled state (params changed). */
        if (sr->foliage_cache[slot].tiles) {
            int nt = sr->foliage_cache[slot].tiles_x * sr->foliage_cache[slot].tiles_z;
            for (int t = 0; t < nt; ++t)
                if (sr->foliage_cache[slot].tiles[t].resident)
                    bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].tiles[t].vb);
            JCE_FREE(sr->foliage_cache[slot].tiles);
            sr->foliage_cache[slot].tiles = NULL;
            sr->foliage_cache[slot].tiles_x = sr->foliage_cache[slot].tiles_z = 0;
            sr->foliage_cache[slot].tiles_resident = 0;
        }

        /* 千万 S5: past the single-scatter cap, a MODEL scatter on the GPU-cull
         * path goes TILED — the field becomes a grid of deterministically
         * re-scatterable tiles (streamed by visibility at draw time; nothing is
         * generated here except conservative tile AABBs).  Other paths keep the
         * legacy clamp (LOW/MED tiers degrade to a 65k field). */
        const bool want_tiled = want > JCE_FOLIAGE_MAX_INSTANCES &&
                                vs->mesh_path[0] && sr->foliage_cull_enabled &&
                                !p.density_mask;   /* mask = local-rect UV; v1 skips */
        if (want_tiled) {
            /* Tile edge sized for ~16k instances per tile (streaming granularity
             * + 1MB mat4 upload per gen). */
            const double per_tile = 16384.0;
            float edge = (float)sqrt(per_tile / (double)p.density);
            int txn = (int)ceilf(p.area_x / edge);
            int tzn = (int)ceilf(p.area_z / edge);
            if (txn < 1) txn = 1;
            if (tzn < 1) tzn = 1;
            if (txn > 128) txn = 128;
            if (tzn > 128) tzn = 128;
            struct SrFoliageTile *tiles = (struct SrFoliageTile *)JCE_CALLOC(
                (size_t)txn * (size_t)tzn, sizeof(struct SrFoliageTile));
            if (tiles) {
                sr->foliage_cache[slot].tiles   = tiles;
                sr->foliage_cache[slot].tiles_x = txn;
                sr->foliage_cache[slot].tiles_z = tzn;
                sr->foliage_cache[slot].tile_w  = p.area_x / (float)txn;
                sr->foliage_cache[slot].tile_h  = p.area_z / (float)tzn;
                sr->foliage_cache[slot].tile_params = p;
                sr->foliage_cache[slot].tile_params.density_mask = NULL;
                sr->foliage_cache[slot].tile_params.mask_dim     = 0;
                sr->foliage_cache[slot].tile_origin = origin;
                /* Conservative per-tile AABBs (analytic; needs the model AABB —
                 * resolved on demand: use a generous default until the model
                 * loads, then exact values on the next rebuild).  10m default
                 * covers typical foliage; the frustum test stays conservative. */
                float mmn[3] = { -1.0f, 0.0f, -1.0f }, mmx[3] = { 1.0f, 2.0f, 1.0f };
                SrModelCache *mc0 = sr_get_model(sr, vs->mesh_path, (uint32_t)e);
                if (mc0 && mc0->model) jce_model_get_aabb(mc0->model, mmn, mmx);
                for (int tz = 0; tz < tzn; ++tz)
                    for (int tx = 0; tx < txn; ++tx)
                        sr_foliage_tile_aabb(sr, slot, tx, tz, terr, mmn, mmx,
                            &tiles[tz * txn + tx].mn, &tiles[tz * txn + tx].mx);
                /* inst_count carries the AUTHORED total (draw gating + logs). */
                sr->foliage_cache[slot].inst_count = want;
                LOG_INFO(LOG_TAG, "foliage '%s': %u instances AUTHORED — tiled "
                         "%dx%d (%.0fx%.0fm tiles, ~16k each), streamed by "
                         "visibility (千万 S5)",
                         vs->mesh_path, want, txn, tzn,
                         (double)sr->foliage_cache[slot].tile_w,
                         (double)sr->foliage_cache[slot].tile_h);
            }
        }

        if (!sr->foliage_cache[slot].tiles) {
            if (want > JCE_FOLIAGE_MAX_INSTANCES) want = JCE_FOLIAGE_MAX_INSTANCES;
            if (want == 0) want = 1;
            if (sr->foliage_cache[slot].inst_cap < want) {
                JceFoliageInstance *nb = (JceFoliageInstance *)JCE_REALLOC(
                    sr->foliage_cache[slot].insts, (size_t)want * sizeof(JceFoliageInstance));
                if (!nb) return;
                sr->foliage_cache[slot].insts    = nb;
                sr->foliage_cache[slot].inst_cap = want;
            }
            sr->foliage_cache[slot].inst_count = jce_foliage_scatter(
                &p, terr, &origin, sr->foliage_cache[slot].insts,
                sr->foliage_cache[slot].inst_cap);
        }
        if (mask_grid) JCE_FREE(mask_grid);
        sr->foliage_cache[slot].param_hash = ph;
        sr->foliage_cache[slot].lod_valid  = false;  /* 千万 S4: roots changed → re-partition */
        sr->foliage_cache[slot].entity     = e;
        sr->foliage_cache[slot].used       = true;
        snprintf(sr->foliage_cache[slot].mesh_path,
                 sizeof sr->foliage_cache[slot].mesh_path, "%s", vs->mesh_path);

        if (!sr->foliage_cache[slot].tiles) {
            /* Pre-build per-instance world matrices once (placement is static
             * after scatter) so the per-frame draw is a single GPU-instanced
             * submit instead of N CPU draws.  Rebuilt only on scatter rebuild.
             * (Tiled scatters build matrices per tile at residency time.) */
            uint32_t nc = sr->foliage_cache[slot].inst_count;
            if (sr->foliage_cache[slot].roots_cap < nc) {
                jce_mat4 *nr = (jce_mat4 *)JCE_REALLOC(
                    sr->foliage_cache[slot].roots, (size_t)nc * sizeof(jce_mat4));
                if (!nr) return;
                sr->foliage_cache[slot].roots     = nr;
                sr->foliage_cache[slot].roots_cap = nc;
            }
            for (uint32_t i = 0; i < nc; ++i)
                sr->foliage_cache[slot].roots[i] =
                    sr_foliage_instance_matrix(&sr->foliage_cache[slot].insts[i]);
            LOG_INFO(LOG_TAG, "foliage '%s': %u instances scattered "
                     "(single GPU-instanced submit; 4096 draw cap removed)",
                     vs->mesh_path, nc);
        }
    }

    uint32_t n = sr->foliage_cache[slot].inst_count;
    const bool slot_tiled = (sr->foliage_cache[slot].tiles != NULL);
    if ((!sr->foliage_cache[slot].roots && !slot_tiled) || n == 0) return;

    if (vs->mesh_path[0]) {
        /* glTF MODEL instances (existing path): one GPU-instanced submit for all
         * scattered copies (the model's real PBR materials + in-asset LOD bound
         * once, shared across every instance).  No 4096 cap on the instanced path.
         * When the instanced PBR program is unavailable (low-GPU fallback),
         * jce_model_draw_instanced_tinted degrades to solo draws → cap THAT. */
        SrModelCache *mc = sr_get_model(sr, vs->mesh_path, (uint32_t)e);
        if (!mc || !mc->model) return;
        const bool instanced =
            jce_renderer_get_program_pbr_inst(sr->renderer).idx != UINT16_MAX;
        if (!instanced && n > 4096u) n = 4096u;
        /* 千万 S4: distance-LOD bands (opt-in JCE_FOLIAGE_LOD) — distant instances
         * render reduced-LOD geometry.  Falls through to the flat LOD0 submit when
         * off, the model has no LODs, or the partition can't allocate. */
        static int s_flod = -1;
        if (s_flod < 0) { const char *e = getenv("JCE_FOLIAGE_LOD");
                          s_flod = (e && e[0] && e[0] != '0') ? 1 : 0; }
        /* 千万 S4 GPU LOD-in-cull (preferred over the CPU-band S4 below): when the
         * foliage GPU cull is active (HIGH tier + compute), the model is single-
         * primitive, and it has in-asset LODs, run one GPU frustum+Hi-Z+distance
         * cull per LOD band + one indirect draw per band — culling off-screen /
         * occluded AND LOD-ing distant survivors, CPU O(1).  Falls through to the
         * CPU-band S4 (then flat) when unavailable.
         *
         * Gate on foliage_gpu_cull_frame (== foliage_cull_enabled && gpu_first),
         * NOT foliage_cull_enabled: the cull compacts survivors into the per-band
         * buffers + builds the indirect args, and bgfx executes the color draw's
         * submit_indirect at frame end — so ONLY the first (color) render may run
         * the dispatch.  A later pass (shadow/pick) re-running it would reset the
         * counters + re-cull with that pass's camera INTO THE SAME band buffers,
         * clobbering the color survivors before the GPU reads them → empty draw.
         * Non-color passes fall through to the flat LOD0 instanced submit (correct:
         * shadow casters aren't view-frustum culled). Mirrors the primitive path. */
        if ((s_flod || slot_tiled) && instanced && sr->foliage_gpu_cull_frame &&
            sr->gpu_frame_planes_valid && n >= 2u &&
            jce_model_gpu_drawable_count(mc->model) == 1u) {
            const JceSkinnedMesh *lsm =
                jce_model_gpu_primitive_skinned_mesh(mc->model, 0);
            uint32_t lodc = lsm ? jce_skinned_mesh_lod_count(lsm) : 0u;
            if (lsm && (lodc >= 1u || slot_tiled)) {
                uint32_t bands = lodc + 1u;
                if (bands > JCE_FOLIAGE_LOD_BANDS) bands = JCE_FOLIAGE_LOD_BANDS;
                if (sr_foliage_gpu_lod_cull_draw(sr, slot, mc->model, lsm, bands,
                                                 n, view_id, camera, scene, list))
                    return;
            }
        }
        /* Tiled scatters have NO monolithic roots array — the GPU path above is
         * their ONLY renderer.  A non-color pass (shadow/pick) or a failed
         * dispatch draws nothing here (scatter shadows ride the dedicated 千万
         * ③ instanced depth path instead). */
        if (slot_tiled) return;
        if (s_flod && instanced &&
            sr_foliage_draw_model_lod(sr, slot, mc->model, view_id, camera, n))
            return;
        jce_model_draw_instanced_tinted(mc->model, sr->renderer, view_id,
                                        sr->foliage_cache[slot].roots, NULL, n);
        return;
    }

    /* PRIMITIVE (mesh_shape) instances: one explicit-instanced submit of the
     * shared primitive mesh through the render queue — the multi-cube ISM path
     * (Unity-ISM / UE-HISM equivalent for huge counts of one primitive). */
    JceMesh *pm = NULL;
    switch (vs->mesh_shape) {
        default:
        case 0: pm = sr->cube_mesh;     break;
        case 1: pm = sr->sphere_mesh;   break;
        case 2: pm = sr->plane_mesh;    break;
        case 3: pm = sr->capsule_mesh;  break;
        case 4: pm = sr->cylinder_mesh; break;
    }
    JceShaderHandle prog_inst   = jce_renderer_get_program_pbr_inst(sr->renderer);
    JceShaderHandle prog_single = jce_renderer_get_program_pbr(sr->renderer);
    if (!pm || prog_inst.idx == UINT16_MAX || !sr->render_queue) return;

    JcePbrMaterial pbr = jce_pbr_material_default();
    const uint64_t mat_state = jce_pbr_material_render_state(&pbr);
    uint32_t mat_key = sr_compute_material_key(&pbr, false, -1, NULL);
    uint32_t reg_key = sr_register_material(sr, mat_key, &pbr, false, -1,
                                            0.0f, false, NULL);
    if (!reg_key) return;

    JceDrawCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.view_id        = view_id;
    cmd.program        = (uint16_t)prog_inst.idx;
    cmd.program_single = (prog_single.idx != UINT16_MAX)
                            ? (uint16_t)prog_single.idx : (uint16_t)UINT16_MAX;
    cmd.mesh_vbh       = jce_mesh_get_vbh(pm);
    cmd.mesh_ibh       = jce_mesh_get_ibh(pm);
    cmd.index_count    = jce_mesh_index_count(pm);
    cmd.material_key   = reg_key;
    cmd.state          = mat_state;

    JceInstanceBatch batch;
    memset(&batch, 0, sizeof batch);
    batch.instance_count = n;
    batch.stride_vec4    = 4;   /* 4x4 world matrix per instance */

    /* 千万 S1: persistent GPU instance buffer (JCE_PERSIST_FOLIAGE). The static
     * `roots` are uploaded to a dynamic VB ONCE (rebuilt only when the scatter
     * param_hash changes), so the per-frame draw binds it with ZERO CPU copy —
     * per-frame cost becomes O(1) instead of O(n) memcpy. Default OFF → the
     * current per-frame transient-upload path is byte-unchanged. */
    static int s_persist_env = -1;
    if (s_persist_env < 0) {
        const char *pv = getenv("JCE_PERSIST_FOLIAGE");
        s_persist_env = (pv && pv[0] && pv[0] != '0') ? 1 : 0;
    }
    /* GPU cull (千万 S2) reads the persistent roots VB as a compute SRV → it
     * implies persist (S1) AND the COMPUTE_READ buffer flag.  The cull gate
     * (env / perf default, HIGH tier, compute support — but NOT the per-viewport
     * gpu_first guard) was resolved into sr->foliage_cull_enabled upstream; key
     * the persist VB off it so DEFAULT-ON cull creates the compute-capable VB it
     * needs (the env-only static missed the render-pipeline perf default → the
     * cull silently never fired by default). */
    const int s_foliage_cull    = sr->foliage_cull_enabled ? 1 : 0;
    const int s_persist_foliage = (s_persist_env || s_foliage_cull) ? 1 : 0;
    if (s_persist_foliage && n >= 2u) {
        if (sr->foliage_cache[slot].inst_vb_hash != sr->foliage_cache[slot].param_hash) {
            if (sr->foliage_cache[slot].inst_vb_count) {
                LOG_DEBUG(LOG_TAG, "[vbdbg] persist destroy slot=%d idx=%u n=%u",
                         slot, sr->foliage_cache[slot].inst_vb.idx,
                         sr->foliage_cache[slot].inst_vb_count);
                bgfx_destroy_dynamic_vertex_buffer(sr->foliage_cache[slot].inst_vb);
            }
            bgfx_vertex_layout_t il;
            bgfx_vertex_layout_begin(&il, BGFX_RENDERER_TYPE_NOOP);
            bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD7, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
            bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD6, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
            bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD5, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
            bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD4, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
            bgfx_vertex_layout_end(&il);
            /* COMPUTE_READ when GPU cull is on so cs_foliage_cull can read the
             * roots as a compute SRV; the buffer still binds as instance data for
             * the no-cull fallback (COMPUTE buffers bind as instance data — the
             * gpu_scene visible_buf does exactly this). */
            uint16_t vbflags = s_foliage_cull
                ? (uint16_t)(BGFX_BUFFER_COMPUTE_READ | BGFX_BUFFER_COMPUTE_FORMAT_32X4 | BGFX_BUFFER_COMPUTE_TYPE_FLOAT)
                : (uint16_t)BGFX_BUFFER_NONE;
            sr->foliage_cache[slot].inst_vb =
                bgfx_create_dynamic_vertex_buffer(n, &il, vbflags);
            LOG_DEBUG(LOG_TAG, "[vbdbg] persist create slot=%d idx=%u n=%u",
                     slot, sr->foliage_cache[slot].inst_vb.idx, n);
            if (sr->foliage_cache[slot].inst_vb.idx == UINT16_MAX) {
                /* Create failed: without this gate inst_vb_count was still
                 * set below, so a later slot_free destroyed an INVALID/stale
                 * handle — bgfx handle-table corruption, ShaderRef AV at
                 * shutdown (mirrors the check the cull-ensure path has). */
                sr->foliage_cache[slot].inst_vb_count = 0u;
                sr->foliage_cache[slot].inst_vb_hash  = 0u;
            } else {
                const bgfx_memory_t *mem =
                    bgfx_copy(sr->foliage_cache[slot].roots,
                              n * (uint32_t)sizeof(jce_mat4));
                bgfx_update_dynamic_vertex_buffer(sr->foliage_cache[slot].inst_vb,
                                                  0, mem);
                sr->foliage_cache[slot].inst_vb_hash  =
                    sr->foliage_cache[slot].param_hash;
                sr->foliage_cache[slot].inst_vb_count = n;
            }
        }
        if (sr->foliage_cache[slot].inst_vb_count == n)
            batch.persist_vb = (uint16_t)(sr->foliage_cache[slot].inst_vb.idx + 1u);
    }

    /* 千万 S2: GPU compute frustum-cull + indirect draw — the GPU rasterises only
     * the in-frustum subset (cost ∝ visible, not N).  Only on the color pass
     * (gpu_frame_planes are the color camera's) with the roots VB resident.  When
     * it fires it REPLACES the queue submit for this entity (returns). */
    if (sr->foliage_gpu_cull_frame && batch.persist_vb &&
        sr->gpu_frame_planes_valid &&
        sr_foliage_ensure_cull_buffers(sr, slot, n)) {
        float lmin[3], lmax[3];
        jce_mesh_get_aabb(pm, lmin, lmax);
        jce_vec3 lc = { (lmin[0] + lmax[0]) * 0.5f, (lmin[1] + lmax[1]) * 0.5f, (lmin[2] + lmax[2]) * 0.5f };
        jce_vec3 le = { (lmax[0] - lmin[0]) * 0.5f, (lmax[1] - lmin[1]) * 0.5f, (lmax[2] - lmin[2]) * 0.5f };
        /* Hi-Z occlusion (opt-in JCE_HIZ_OCCLUSION): feed LAST-frame's depth + VP
         * so the foliage cull can occlusion-test each instance (dense forests =
         * the canonical occlusion case). Needs the SSAO depth prepass; off →
         * frustum-only. Mirrors the model path (jce_sr_draw.c). */
        /* DEFAULT-ON for foliage (unlike the model Hi-Z path, jce_sr_draw.c, which
         * stays perf-default false / opt-in): measured 2026-07-03 the foliage
         * occlusion cull is a big win — Color raster -65% behind an opaque
         * occluder — with ~ZERO overhead in the open (single-level pyramid; frame
         * 1.91→1.93ms), and parity holds UNDER CAMERA MOTION (the 1-frame-latency
         * artifact stays within the TAA noise floor across a 0.6°/frame spin,
         * brightness preserved, no popping strip).  Same JCE_RP_PERF_HIZ_OCCLUSION
         * feature + env JCE_HIZ_OCCLUSION as the model path, but a `true` builtin
         * default so an AUTO perf state turns foliage-only on; an explicit
         * on/off (env or .rp.json) still drives both paths together.  Gated on
         * ssao_valid — the cull reads the SSAO depth prepass, so it is a graceful
         * no-op when no prepass runs. */
        static int s_fol_hiz_env = -2;
        if (s_fol_hiz_env == -2) { const char *v = getenv("JCE_HIZ_OCCLUSION");
                                   s_fol_hiz_env = (!v || !v[0]) ? -1 : (v[0] != '0'); }
        const bool fol_hiz_pref = (s_fol_hiz_env >= 0) ? (s_fol_hiz_env != 0)
            : jce_render_pipeline_perf_enabled(JCE_RP_PERF_HIZ_OCCLUSION, true);
        bool fol_hiz_en = fol_hiz_pref && sr->ssao_valid;
        if (getenv("JCE_HIZ_DIAG")) {
            static uint32_t s_fd = 0;
            if ((s_fd++ % 60u) == 5u)
                LOG_INFO(LOG_TAG, "foliage Hi-Z: env=%d ssao_valid=%d depth=%u -> en=%d",
                         s_fol_hiz_env, (int)sr->ssao_valid, (unsigned)sr->ssao_depth_tex.idx, (int)fol_hiz_en);
        }
        jce_gpu_scene_set_hiz(sr->gpu_scene,
                              fol_hiz_en ? sr->ssao_depth_tex.idx : (uint16_t)UINT16_MAX,
                              (const jce_mat4 *)sr->frame_prev_vp,
                              sr->ssao_w, sr->ssao_h, fol_hiz_en);
        if (jce_gpu_scene_foliage_dispatch(
                sr->gpu_scene, sr->gpu_reset_view, sr->gpu_cull_view,
                sr->foliage_cache[slot].inst_vb.idx,
                sr->foliage_cache[slot].cull_visible.idx,
                sr->foliage_cache[slot].cull_counter.idx,
                sr->foliage_cache[slot].cull_indirect.idx,
                n, sr->foliage_cache[slot].cull_cap,
                sr->gpu_frame_planes, lc, le, cmd.index_count)) {
            /* Indirect draw from the compacted survivors — mirror the queue
             * path's material + forward+ binding so the cubes render identically. */
            sr_inline_bind_pbr_global(sr, &pbr, view_id, scene, list);
            if (sr->fp_active_frame)
                jce_forwardplus_bind(sr->forwardplus);
            bgfx_vertex_buffer_handle_t vbh = { (uint16_t)cmd.mesh_vbh };
            bgfx_index_buffer_handle_t  ibh = { (uint16_t)cmd.mesh_ibh };
            bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
            if (cmd.mesh_ibh != (uint16_t)UINT16_MAX)
                bgfx_set_index_buffer(ibh, 0, cmd.index_count);
            bgfx_set_instance_data_from_dynamic_vertex_buffer(
                sr->foliage_cache[slot].cull_visible, 0,
                sr->foliage_cache[slot].cull_cap);
            bgfx_set_state(mat_state ? mat_state : BGFX_STATE_DEFAULT, 0);
            bgfx_program_handle_t prog = { (uint16_t)prog_inst.idx };
            bgfx_submit_indirect(view_id, prog, sr->foliage_cache[slot].cull_indirect,
                                 0, 1, 0, BGFX_DISCARD_ALL);
            return;
        }
    }

    if (!batch.persist_vb)
        batch.data = (const float *)sr->foliage_cache[slot].roots;
    jce_rq_push_instanced(sr->render_queue, &cmd, &batch);
}


/* ── Grass Field (GPU-instanced procedural blades + wind, Stage 1b.6) ──────
 *
 * One dedicated module separate from the foliage loop above:
 *  - ONE instanced submit per field (stride-80 tinted layout)
 *  - Standalone vs_grass + fs_grass (NOT fs_pbr_body)
 *  - Placement via jce_foliage_scatter (same deterministic scatter as foliage)
 *  - Shared blade mesh built once with default look; per-field look is uniforms
 * Gated to instancing-capable HIGH/ULTRA GPUs + JceRenderSettings.grass_enabled.
 */

/* Pure-CPU blade vertex fill: fills pre-allocated `v` (nverts =
 * cards*JCE_GRASS_BLADE_VERTS) and `idx` (nindices =
 * cards*JCE_GRASS_BLADE_INDICES) arrays with the blade geometry.
 * Exposed non-static so the unit test can call it without bgfx.
 * Returns the number of vertices written.
 *
 * Blade shape (stylized-reference look): each card is a SEGMENTED, TAPERED,
 * CURVED strip — wide at the root, narrowing to a near-point tip, leaning
 * over along its facing direction with a quadratic profile (the flat crossed
 * quads read as "straight needles"; real stylized grass is curved leaves).
 * uv.y still carries the 0(root)->1(tip) gradient/wind parameter vs_grass
 * bends and fs_grass shades by, so the shader contract is unchanged. */
uint32_t sr_grass_fill_blade(float blade_height, float blade_width, int cards,
                             JceMeshVertex *v, uint32_t *idx)
{
    const float h  = blade_height > 0.0f ? blade_height : 0.4f;
    const float hw = (blade_width > 0.0f ? blade_width : 0.05f) * 0.5f;
    const int   SEG = JCE_GRASS_BLADE_SEGS;
    uint32_t vi = 0, ii = 0;
    for (int c = 0; c < cards; ++c) {
        float ang = (cards > 1) ? (3.14159265f * (float)c / (float)cards) : 0.0f;
        float dx = cosf(ang), dz = sinf(ang);
        float nx = -dz, nz = dx;
        uint32_t base = vi;
        for (int s = 0; s <= SEG; ++s) {
            float t     = (float)s / (float)SEG;
            /* taper: LINEAR root->tip like the reference's single-triangle
             * blade (the quadratic taper kept blades fat to mid-height and
             * dense fields read as chunky tufts) */
            float w     = hw * (1.0f - t * 0.96f);
            /* lean: light quadratic curve along the card NORMAL.  The
             * reference blade is STRAIGHT (bending is the wind shader's,
             * per frame); a heavy 35%% static pre-lean combined with the
             * face-the-camera rotation combed patches into swirled clumps
             * instead of an upright carpet. */
            float lean  = 0.10f * h * t * t;
            float y     = h * t * (1.0f - 0.18f * t * t);
            float ox    = nx * lean, oz = nz * lean;
            v[vi].pos[0] = -dx * w + ox; v[vi].pos[1] = y; v[vi].pos[2] = -dz * w + oz;
            v[vi].normal[0] = nx; v[vi].normal[1] = 0.5f; v[vi].normal[2] = nz;
            v[vi].uv[0] = 0.0f; v[vi].uv[1] = t; vi++;
            v[vi].pos[0] =  dx * w + ox; v[vi].pos[1] = y; v[vi].pos[2] =  dz * w + oz;
            v[vi].normal[0] = nx; v[vi].normal[1] = 0.5f; v[vi].normal[2] = nz;
            v[vi].uv[0] = 1.0f; v[vi].uv[1] = t; vi++;
        }
        /* CCW-from-the-card-front (double-sided state is set at draw). */
        for (int s = 0; s < SEG; ++s) {
            uint32_t r0 = base + (uint32_t)s * 2u;
            idx[ii++] = r0 + 0; idx[ii++] = r0 + 1; idx[ii++] = r0 + 3;
            idx[ii++] = r0 + 0; idx[ii++] = r0 + 3; idx[ii++] = r0 + 2;
        }
    }
    return vi;
}

/* Build the shared procedural grass-blade mesh: `cards` crossed alpha quads,
 * world-unit `blade_height` tall x `blade_width` wide, root at local y=0.
 * a_texcoord0.y carries the 0(root)->1(tip) bend/gradient parameter; .x is
 * the 0->1 horizontal card UV.  CCW-from-above / front-facing winding (engine
 * convention).  Pure CPU mesh -- vs_grass bends it; the result is shared across
 * all fields (per-field look is uniforms, not geometry). */
JceMesh *sr_grass_build_blade(float blade_height, float blade_width, int cards)
{
    if (cards < 1) cards = 1;
    if (cards > 6) cards = 6;

    const uint32_t nverts   = (uint32_t)cards * JCE_GRASS_BLADE_VERTS;
    const uint32_t nindices = (uint32_t)cards * JCE_GRASS_BLADE_INDICES;
    JceMeshVertex *v = (JceMeshVertex *)JCE_MALLOC(nverts * sizeof(JceMeshVertex));
    if (!v) return NULL;
    uint32_t *idx = (uint32_t *)JCE_MALLOC(nindices * sizeof(uint32_t));
    if (!idx) { JCE_FREE(v); return NULL; }

    sr_grass_fill_blade(blade_height, blade_width, cards, v, idx);

    JceMesh *m = jce_mesh_create(v, nverts, idx, nindices);
    JCE_FREE(v); JCE_FREE(idx);
    return m;
}

/* Lazily create the grass program + its uniforms (once per renderer). */
static void sr_grass_lazy_init(JceSceneRenderer *sr)
{
    if (!sr->grass_prog_tried) {
        sr->grass_prog_tried = true;
        JceShaderHandle gh = shader_load_program(sr->pak, "grass");
        sr->prog_grass.idx = gh.idx;
        if (gh.idx == UINT16_MAX)
            LOG_WARN(LOG_TAG, "grass shader not found in PAK "
                              "(grass fields will not render)");
    }
    if (!BGFX_HANDLE_IS_VALID(sr->u_grass_time))
        sr->u_grass_time = bgfx_create_uniform("u_grass_time", BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->u_grass_wind))
        sr->u_grass_wind = bgfx_create_uniform("u_grass_wind", BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->u_grass_color))
        sr->u_grass_color = bgfx_create_uniform("u_grass_color", BGFX_UNIFORM_TYPE_VEC4, 2);
    if (!BGFX_HANDLE_IS_VALID(sr->u_grass_fade))
        sr->u_grass_fade = bgfx_create_uniform("u_grass_fade", BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!sr->grass_blade)
        sr->grass_blade = sr_grass_build_blade(0.4f, 0.05f, 4);
}

/* FNV-1a over the grass scatter-shaping fields (density/seed/area/slope/scale)
 * AND the look fields that affect blade geometry (blade_height/blade_width/cards).
 * Task 6 compares this hash to detect per-entity changes that require a re-scatter.
 * All nine fields must be included so that changing any one of them invalidates the
 * cached placement, matching the spec: density/seed/area_x/area_z/max_slope_deg/
 * scale_min/scale_max/blade_height/blade_width/cards. */
static uint32_t sr_grass_param_hash(const JceGrassFieldComponent *g,
                                    const jce_vec3 *origin)
{
    uint32_t h = jce_fnv1a32_append(JCE_FNV1A32_INIT, &g->seed, sizeof g->seed);
    h = jce_fnv1a32_append(h, &g->density,       sizeof g->density);
    h = jce_fnv1a32_append(h, &g->area_x,        sizeof g->area_x);
    h = jce_fnv1a32_append(h, &g->area_z,        sizeof g->area_z);
    h = jce_fnv1a32_append(h, &g->max_slope_deg, sizeof g->max_slope_deg);
    h = jce_fnv1a32_append(h, &g->scale_min,     sizeof g->scale_min);
    h = jce_fnv1a32_append(h, &g->scale_max,     sizeof g->scale_max);
    /* Look fields that affect placement geometry — include so cache is invalidated
     * when blade_height/width/cards change (Task 6 uses this hash for change detection). */
    h = jce_fnv1a32_append(h, &g->blade_height,  sizeof g->blade_height);
    h = jce_fnv1a32_append(h, &g->blade_width,   sizeof g->blade_width);
    h = jce_fnv1a32_append(h, &g->cards,         sizeof g->cards);
    /* hue_jitter feeds the per-blade tint now baked into the packed instance
     * cache, so a change must invalidate (re-pack) it. */
    h = jce_fnv1a32_append(h, &g->hue_jitter,    sizeof g->hue_jitter);
    /* World translation is baked into the instance matrices AND the cell-cull
     * grid at scatter time (pack-once), so a transform edit must re-scatter —
     * without this the grass renders frozen at its original position
     * (mirrors sr_foliage_hash, which appends origin for the same reason). */
    h = jce_fnv1a32_append(h, origin, sizeof *origin);
    /* Density-mask trio is consumed at scatter time too (world-UV sampling). */
    h = jce_fnv1a32_append(h, g->density_mask_path,
                           strlen(g->density_mask_path));
    h = jce_fnv1a32_append(h, &g->density_threshold, sizeof g->density_threshold);
    h = jce_fnv1a32_append(h, &g->mask_world_size,   sizeof g->mask_world_size);
    return h;
}

/* Free the instance buffer in a single grass cache slot and reset it.
 * Non-static: called by jce_scene_renderer.c destroy loop and Task 6 cache
 * invalidation (mirrors sr_water_slot_free declared in jce_sr_internal.h). */
void sr_grass_slot_free(JceSceneRenderer *sr, int slot)
{
    JCE_FREE(sr->grass_cache[slot].insts);
    JCE_FREE(sr->grass_cache[slot].packed);
    if (sr->grass_cache[slot].inst_vb_count)
        bgfx_destroy_dynamic_vertex_buffer(sr->grass_cache[slot].inst_vb);
    memset(&sr->grass_cache[slot], 0, sizeof sr->grass_cache[slot]);
}

/* Find (or evict into) a grass cache slot for entity `e`. */
static int sr_grass_find_slot(JceSceneRenderer *sr, JceEntity e)
{
    int n = (int)(sizeof sr->grass_cache / sizeof sr->grass_cache[0]);
    int free_slot = -1;
    for (int i = 0; i < n; ++i) {
        if (sr->grass_cache[i].used && sr->grass_cache[i].entity == e) return i;
        if (free_slot < 0 && !sr->grass_cache[i].used) free_slot = i;
    }
    if (free_slot >= 0) return free_slot;
    int victim = (int)((uint32_t)e % (uint32_t)n);
    sr_grass_slot_free(sr, victim);
    return victim;
}

/* Submit a contiguous [start, start+n) blade slice of the cell-ordered packed
 * cache with the partial-fill instance loop (mirrors the model instancer). */
static void sr_grass_submit_range(JceSceneRenderer *sr, int slot,
                                  bgfx_program_handle_t prog, uint64_t state,
                                  uint16_t view_id, uint16_t stride,
                                  bgfx_vertex_buffer_handle_t vbh,
                                  bgfx_index_buffer_handle_t ibh,
                                  uint32_t vcount, uint32_t icount,
                                  uint32_t start, uint32_t n)
{
    const uint8_t *packed = sr->grass_cache[slot].packed;
    if (!packed || n == 0u) return;

    /* Persistent instance VB (mirrors foliage 千万 S1): upload the whole
     * cell-ordered packed buffer once per param_hash, then bind [start,n)
     * slices per visible cell run with zero per-frame CPU copy.  Also
     * removes the transient-budget chunk splitting (and its silent blade
     * drop when the budget ran out).  JCE_PERSIST_GRASS=0 restores the
     * transient path for A/B. */
    static int s_persist_grass = -1;
    if (s_persist_grass < 0) {
        const char *pv = getenv("JCE_PERSIST_GRASS");
        s_persist_grass = (pv && pv[0] == '0') ? 0 : 1;
    }
    if (s_persist_grass) {
        const uint32_t total = sr->grass_cache[slot].inst_count;
        if (sr->grass_cache[slot].inst_vb_hash
                != sr->grass_cache[slot].param_hash
            || sr->grass_cache[slot].inst_vb_count != total) {
            if (sr->grass_cache[slot].inst_vb_count) {
                bgfx_destroy_dynamic_vertex_buffer(sr->grass_cache[slot].inst_vb);
                sr->grass_cache[slot].inst_vb_count = 0;
            }
            if (total >= 2u) {
                bgfx_vertex_layout_t il;
                bgfx_vertex_layout_begin(&il, BGFX_RENDERER_TYPE_NOOP);
                bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD7, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
                bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD6, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
                bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD5, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
                bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD4, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
                bgfx_vertex_layout_add(&il, BGFX_ATTRIB_TEXCOORD3, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
                bgfx_vertex_layout_end(&il);
                sr->grass_cache[slot].inst_vb =
                    bgfx_create_dynamic_vertex_buffer(total, &il,
                                                      BGFX_BUFFER_NONE);
                if (BGFX_HANDLE_IS_VALID(sr->grass_cache[slot].inst_vb)) {
                    const bgfx_memory_t *mem =
                        bgfx_copy(packed, total * (uint32_t)stride);
                    bgfx_update_dynamic_vertex_buffer(
                        sr->grass_cache[slot].inst_vb, 0, mem);
                    sr->grass_cache[slot].inst_vb_hash =
                        sr->grass_cache[slot].param_hash;
                    sr->grass_cache[slot].inst_vb_count = total;
                }
            }
        }
        if (sr->grass_cache[slot].inst_vb_count == total
            && total != 0u && start + n <= total) {
            bgfx_set_vertex_buffer(0, vbh, 0, vcount);
            bgfx_set_index_buffer(ibh, 0, icount);
            bgfx_set_instance_data_from_dynamic_vertex_buffer(
                sr->grass_cache[slot].inst_vb, start, n);
            if (sr->frame_shadow_active)
                sr_bind_frame_shadow_state(sr);
            bgfx_set_state(state, 0);
            bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
            return;
        }
    }

    uint32_t off = 0;
    while (off < n) {
        uint32_t want  = n - off;
        uint32_t avail = bgfx_get_avail_instance_data_buffer(want, stride);
        uint32_t nb    = (want < avail) ? want : avail;
        if (nb == 0) break;
        bgfx_instance_data_buffer_t idb;
        bgfx_alloc_instance_data_buffer(&idb, nb, stride);
        memcpy(idb.data, packed + (size_t)(start + off) * stride,
               (size_t)nb * stride);
        bgfx_set_vertex_buffer(0, vbh, 0, vcount);
        bgfx_set_index_buffer(ibh, 0, icount);
        bgfx_set_instance_data_buffer(&idb, 0, nb);
        /* Re-bind the directional shadow state (CSM cascade textures stages
         * 9-12) before EVERY submit.  The cull path calls this helper once per
         * visible cell-run and the loop below splits a run across several
         * bgfx_submit calls when the transient instance buffer fills — but the
         * CSM textures were bound only ONCE, before the first submit (via
         * sr_inline_bind_pbr_global in sr_draw_grass).  The D3D11/D3D12/Vulkan
         * backends clear texture-stage bindings between submits (GL leaves them
         * bound — which is why the seam was invisible on GL and, incidentally,
         * on D3D12 when the whole field fit in one submit), so every range/chunk
         * after the first sampled UNBOUND cascade textures (-> 0) and the grass
         * self-shadowed to solid black along the cell-run boundary — the
         * straight "knife-cut" band the user saw on DX11/Vulkan.  Mirrors the
         * per-primitive re-bind in sr_model_presubmit_cb and the grass no-cull
         * path; gated so non-shadow frames stay byte-identical. */
        if (sr->frame_shadow_active)
            sr_bind_frame_shadow_state(sr);
        bgfx_set_state(state, 0);
        bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
        off += nb;
    }
}

/* sr_draw_grass — ONE instanced bgfx submit per grass field.
 *
 * Resolves the terrain (for height-snapped placement), scatters blades into
 * the per-entity grass_cache slot (rebuilt only when the param hash changes),
 * then issues ONE instanced draw (partial-fill loop, mirrors
 * jce_model_draw_instanced_tinted at jce_model.c:655-712) with the shared
 * vs_grass+fs_grass program.  Instance buffer = stride-80 tinted layout:
 *   offset 0  = mat4 (per-blade TRS from sr_foliage_instance_matrix)
 *   offset 64 = vec4 tint (green↔blue-green hue jitter, index-derived)
 * No shadow submission (cast_shadow is reserved for v1; would need vs_shadow_inst). */
void sr_draw_grass(JceSceneRenderer *sr, JceScene *scene,
                   EntityList *list, JceEntity e, uint16_t view_id,
                   const JceCamera *camera, const JceSceneRenderConfig *cfg)
{
    JceGrassFieldComponent *g = jce_scene_get_grass_field(scene, e);
    if (!g || !g->visible) return;
    if (!jce_scene_has_transform(scene, e)) return;

    sr_grass_lazy_init(sr);
    if (!BGFX_HANDLE_IS_VALID(sr->prog_grass) || !sr->grass_blade) return;

    /* ── Scatter cache (rebuild only on param change) ─────────────── */
    int slot = sr_grass_find_slot(sr, e);
    if (slot < 0) return;

    jce_mat4 wm     = jce_scene_get_world_matrix(scene, e);
    jce_vec3 origin = { wm.col[3].x, wm.col[3].y, wm.col[3].z };

    const uint32_t ph    = sr_grass_param_hash(g, &origin);
    const bool     rebuild = !sr->grass_cache[slot].used ||
                              sr->grass_cache[slot].entity != e ||
                              sr->grass_cache[slot].param_hash != ph;
    if (rebuild) {
        /* Resolve the first available terrain for height-snap. */
        JceTerrain *terr = NULL;
        for (int i = 0; i < list->count; ++i) {
            JceEntity te = list->entities[i];
            if (!jce_scene_has_terrain(scene, te)) continue;
            JceTerrainComponent *tc = jce_scene_get_terrain(scene, te);
            if (!tc || !tc->terrain_path[0]) continue;
            int tslot = sr_terrain_find_or_load_slot(sr, tc->terrain_path);
            if (tslot >= 0) { terr = sr->terrain_cache[tslot].terrain; break; }
        }

        JceFoliageScatterParams p;
        p.seed          = g->seed;
        p.density       = g->density;
        /* LOW-tier grass density floor (engine guarantee: hold frame rate on
         * integrated graphics).  An authored field is ~1.5M vertices/frame of
         * vertex shading — the single biggest measured GPU item on iGPU-class
         * hardware (+10 fps with grass off on Intel UHD).  Tier LOW halves
         * the scatter density (still reads as a dense carpet); HIGH/ULTRA
         * render the authored density untouched.  JCE_GRASS_DENSITY (0..1,
         * web `?grass=N` hook) overrides for A/B. */
        {
            static float s_gd = -1.0f;
            if (s_gd < 0.0f) {
                const char *gd = getenv("JCE_GRASS_DENSITY");
                if (gd && gd[0]) {
                    s_gd = (float)atof(gd);
                    if (s_gd < 0.05f || s_gd > 1.0f) s_gd = 1.0f;
                } else {
                    s_gd = (jce_renderer_get_tier() <= JCE_GPU_TIER_LOW)
                         ? 0.5f : 1.0f;
                }
            }
            if (s_gd < 1.0f) p.density *= s_gd;
        }
        p.area_x        = g->area_x;
        p.area_z        = g->area_z;
        p.max_slope_deg = g->max_slope_deg;
        p.scale_min     = g->scale_min;
        p.scale_max     = g->scale_max;
        p.density_mask  = NULL;
        p.mask_dim      = 0;
        p.mask_world_size = g->mask_world_size;

        /* Diorama dirt-path support: gate blade placement on a density mask so
         * paths / water read through instead of being carpeted.  The reference
         * grows grass only where density >= threshold, so we HARD-threshold the
         * mask's GREEN channel (grass density) to 0/1 for crisp path edges. The
         * grid is freed right after the scatter below. */
        float *gmask = NULL;
        if (g->density_mask_path[0] && g->density_threshold > 0.0f) {
            JceImage img; memset(&img, 0, sizeof img);
            if (sr_decode_image_asset(sr, g->density_mask_path, &img) && img.pixels &&
                img.width > 0 && img.height > 0) {
                int dim = (int)img.width; if (dim > 256) dim = 256;
                gmask = (float *)JCE_MALLOC((size_t)dim * (size_t)dim * sizeof(float));
                if (gmask) {
                    for (int y = 0; y < dim; ++y)
                        for (int x = 0; x < dim; ++x) {
                            int sx = (int)((int64_t)x * (int64_t)img.width  / dim);
                            int sy = (int)((int64_t)y * (int64_t)img.height / dim);
                            if (sx >= (int)img.width)  sx = (int)img.width  - 1;
                            if (sy >= (int)img.height) sy = (int)img.height - 1;
                            /* GREEN channel = grass density (offset +1). */
                            float gd = img.pixels[((size_t)sy * img.width + sx) * 4u + 1u] / 255.0f;
                            gmask[(size_t)y * dim + x] = (gd >= g->density_threshold) ? 1.0f : 0.0f;
                        }
                    p.density_mask = gmask;
                    p.mask_dim     = dim;
                }
            }
            jce_image_free(&img);
        }

        uint32_t want = (uint32_t)((double)p.density *
                                   (double)p.area_x * (double)p.area_z);
        if (want > JCE_FOLIAGE_MAX_INSTANCES) want = JCE_FOLIAGE_MAX_INSTANCES;
        if (want == 0) want = 1;

        if (sr->grass_cache[slot].inst_cap < want) {
            JceFoliageInstance *nb = (JceFoliageInstance *)JCE_REALLOC(
                sr->grass_cache[slot].insts,
                (size_t)want * sizeof(JceFoliageInstance));
            if (!nb) return;
            sr->grass_cache[slot].insts    = nb;
            sr->grass_cache[slot].inst_cap = want;
        }

        sr->grass_cache[slot].inst_count = jce_foliage_scatter(
            &p, terr, &origin,
            sr->grass_cache[slot].insts, sr->grass_cache[slot].inst_cap);
        if (gmask) JCE_FREE(gmask);
        sr->grass_cache[slot].param_hash = ph;
        sr->grass_cache[slot].entity     = e;
        sr->grass_cache[slot].used       = true;

        /* Pre-pack the static stride-80 instance bytes (mat4 + tint) ONCE, in
         * SPATIAL-CELL order so the color pass can frustum + fade-distance cull
         * whole cells.  Blade TRS + index-derived tint do not vary per frame
         * (wind is shader-side via u_grass_time), so the color pass bulk-copies
         * these instead of rebuilding the per-blade matrix every frame.  The
         * tint keeps the blade's ORIGINAL index, and grass is opaque +
         * depth-tested, so the re-ordered draw is pixel-identical. */
        {
            const uint32_t stride80 = (uint32_t)(sizeof(jce_mat4) + sizeof(jce_vec4));
            const uint32_t n        = sr->grass_cache[slot].inst_count;
            const uint32_t need_b   = n * stride80;
            if (sr->grass_cache[slot].packed_cap < need_b) {
                uint8_t *pb = (uint8_t *)JCE_REALLOC(sr->grass_cache[slot].packed, need_b);
                if (pb) {
                    sr->grass_cache[slot].packed     = pb;
                    sr->grass_cache[slot].packed_cap = need_b;
                }
            }

            /* ── Spatial grid: XZ/Y bounds → grid → counting sort → order[] ── */
            const JceFoliageInstance *ins = sr->grass_cache[slot].insts;
            float mnx = 1e30f, mnz = 1e30f, mxx = -1e30f, mxz = -1e30f;
            float mny = 1e30f, mxy = -1e30f;
            for (uint32_t i = 0; i < n; ++i) {
                float x = ins[i].pos[0], y = ins[i].pos[1], z = ins[i].pos[2];
                if (x < mnx) mnx = x; if (x > mxx) mxx = x;
                if (z < mnz) mnz = z; if (z > mxz) mxz = z;
                if (y < mny) mny = y; if (y > mxy) mxy = y;
            }
            if (n == 0u) { mnx = mnz = mny = 0.0f; mxx = mxz = mxy = 1.0f; }
            float spanx = mxx - mnx, spanz = mxz - mnz;
            float span  = spanx > spanz ? spanx : spanz;
            float cell  = span / (float)JCE_GRASS_CELL_DIM;
            if (cell < 1.0f) cell = 1.0f;
            uint16_t gx = (uint16_t)(spanx / cell) + 1u;
            uint16_t gz = (uint16_t)(spanz / cell) + 1u;
            if (gx > (uint16_t)JCE_GRASS_CELL_DIM) gx = (uint16_t)JCE_GRASS_CELL_DIM;
            if (gz > (uint16_t)JCE_GRASS_CELL_DIM) gz = (uint16_t)JCE_GRASS_CELL_DIM;
            if (gx < 1u) gx = 1u;
            if (gz < 1u) gz = 1u;
            uint32_t ncells = (uint32_t)gx * gz;

            uint32_t *cs     = sr->grass_cache[slot].cell_start;
            uint32_t *order  = (n > 0u) ? (uint32_t *)JCE_MALLOC((size_t)n * sizeof(uint32_t)) : NULL;
            uint32_t *cursor = (uint32_t *)JCE_MALLOC((size_t)ncells * sizeof(uint32_t));
            bool binned = false;
            if (n > 0u && order && cursor) {
                memset(cs, 0, (size_t)(ncells + 1u) * sizeof(uint32_t));
                for (uint32_t i = 0; i < n; ++i) {
                    int cx = (int)((ins[i].pos[0] - mnx) / cell);
                    int cz = (int)((ins[i].pos[2] - mnz) / cell);
                    if (cx < 0) cx = 0; if (cx >= (int)gx) cx = (int)gx - 1;
                    if (cz < 0) cz = 0; if (cz >= (int)gz) cz = (int)gz - 1;
                    cs[(uint32_t)cz * gx + (uint32_t)cx + 1u]++;
                }
                for (uint32_t c = 1u; c <= ncells; ++c) cs[c] += cs[c - 1u];
                memcpy(cursor, cs, (size_t)ncells * sizeof(uint32_t));
                for (uint32_t i = 0; i < n; ++i) {
                    int cx = (int)((ins[i].pos[0] - mnx) / cell);
                    int cz = (int)((ins[i].pos[2] - mnz) / cell);
                    if (cx < 0) cx = 0; if (cx >= (int)gx) cx = (int)gx - 1;
                    if (cz < 0) cz = 0; if (cz >= (int)gz) cz = (int)gz - 1;
                    order[cursor[(uint32_t)cz * gx + (uint32_t)cx]++] = i;
                }
                binned = true;
            }
            JCE_FREE(cursor);

            sr->grass_cache[slot].grid_gx    = binned ? gx : 0u;   /* 0 ⇒ no cull */
            sr->grass_cache[slot].grid_gz    = gz;
            sr->grass_cache[slot].grid_min_x = mnx;
            sr->grass_cache[slot].grid_min_z = mnz;
            sr->grass_cache[slot].cell_size  = cell;
            sr->grass_cache[slot].field_ymin = mny;
            sr->grass_cache[slot].field_ymax = mxy +
                (g->scale_max * 2.0f > 2.0f ? g->scale_max * 2.0f : 2.0f);

            if (sr->grass_cache[slot].packed &&
                sr->grass_cache[slot].packed_cap >= need_b) {
                for (uint32_t j = 0; j < n; ++j) {
                    uint32_t oi = binned ? order[j] : j;   /* original blade idx */
                    const JceFoliageInstance *fi = &ins[oi];
                    jce_mat4 m  = sr_foliage_instance_matrix(fi);
                    float    t  = (float)(oi & 7u) / 7.0f;
                    float    hh = (t - 0.5f) * 2.0f * g->hue_jitter;
                    jce_vec4 tint = { 1.0f - hh, 1.0f, 1.0f + hh, 1.0f };
                    uint8_t *pkp = sr->grass_cache[slot].packed + (size_t)j * stride80;
                    memcpy(pkp,                    JCE_M4_PTR(m), sizeof(jce_mat4));
                    memcpy(pkp + sizeof(jce_mat4), &tint,         sizeof(jce_vec4));
                }
            } else {
                sr->grass_cache[slot].grid_gx = 0u;  /* pack failed ⇒ no cull */
            }
            JCE_FREE(order);
        }
    }

    uint32_t count = sr->grass_cache[slot].inst_count;
    if (count == 0) return;
    JceFoliageInstance *insts = sr->grass_cache[slot].insts;

    /* ── Global PBR bind (lights/camera/ambient) ──────────────────── */
    JcePbrMaterial gpbr = jce_pbr_material_default();
    sr_inline_bind_pbr_global(sr, &gpbr, view_id, scene, list);

    /* ── Grass-specific uniforms ──────────────────────────────────── */
    float utime[4]  = { sr->grass_time, 0.0f, 0.0f, 0.0f };
    float uwind[4]  = { g->wind_dir[0], g->wind_dir[1],
                        g->wind_speed, g->wind_amplitude };
    float ucolor[8] = {
        g->root_color[0], g->root_color[1], g->root_color[2], 1.0f,
        g->tip_color[0],  g->tip_color[1],  g->tip_color[2],  1.0f
    };
    float ufade[4]  = { g->fade_start, g->fade_end, g->hue_jitter, 0.0f };
    bgfx_set_uniform(sr->u_grass_time,  utime,  1);
    bgfx_set_uniform(sr->u_grass_wind,  uwind,  1);
    bgfx_set_uniform(sr->u_grass_color, ucolor, 2);
    bgfx_set_uniform(sr->u_grass_fade,  ufade,  1);

    /* ── Submit: frustum + fade-distance CELL cull, then submit only the
     *    visible contiguous blade ranges of the cell-ordered packed cache.
     *    grid_gx==0 (unbinned / pack-OOM) or no camera ⇒ cull disabled. ── */
    const bgfx_program_handle_t prog   = sr->prog_grass;
    const uint16_t              stride = (uint16_t)(sizeof(jce_mat4) + sizeof(jce_vec4));
    /* stride = 64 (mat4) + 16 (tint vec4) = 80 bytes */

    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(sr->grass_blade) };
    bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(sr->grass_blade) };
    if (!BGFX_HANDLE_IS_VALID(vbh) || !BGFX_HANDLE_IS_VALID(ibh)) return;
    const uint32_t vcount = jce_mesh_vertex_count(sr->grass_blade);
    const uint32_t icount = jce_mesh_index_count(sr->grass_blade);

    /* Opaque, depth-tested, depth-write, no back-face cull (double-sided
     * cards visible from both faces), MSAA.  No alpha-blending (v1 cards are
     * solid; an alpha-atlas path is a Phase-2 option). */
    const uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                         | BGFX_STATE_WRITE_Z   | BGFX_STATE_DEPTH_TEST_LESS
                         | BGFX_STATE_MSAA;   /* no BGFX_STATE_CULL_* = double-sided */

    bool cull = (sr->grass_cache[slot].grid_gx > 0u) && (camera != NULL) &&
                (sr->grass_cache[slot].packed != NULL);
    { static int s = -1;
      if (s < 0) { const char *v = getenv("JCE_GRASS_CULL"); s = (v && v[0] == '0') ? 0 : 1; }
      if (!s) cull = false; }

    if (!cull) {
        /* No cull: submit every blade.  Bulk-copy from the packed cache when
         * present, else rebuild per-blade from insts (the pack-OOM path). */
        if (sr->grass_cache[slot].packed) {
            sr_grass_submit_range(sr, slot, prog, state, view_id, stride,
                                  vbh, ibh, vcount, icount, 0u, count);
        } else {
            uint32_t start = 0;
            while (start < count) {
                uint32_t want_n = count - start;
                uint32_t avail  = bgfx_get_avail_instance_data_buffer(want_n, stride);
                uint32_t nb     = (want_n < avail) ? want_n : avail;
                if (nb == 0) break;
                bgfx_instance_data_buffer_t idb;
                bgfx_alloc_instance_data_buffer(&idb, nb, stride);
                uint8_t *dst = (uint8_t *)idb.data;
                for (uint32_t i = 0; i < nb; ++i) {
                    const JceFoliageInstance *fi = &insts[start + i];
                    jce_mat4 m = sr_foliage_instance_matrix(fi);
                    float t   = (float)((start + i) & 7u) / 7.0f;
                    float h   = (t - 0.5f) * 2.0f * g->hue_jitter;
                    jce_vec4 tint = { 1.0f - h, 1.0f, 1.0f + h, 1.0f };
                    uint8_t *p = dst + (size_t)i * stride;
                    memcpy(p,                   JCE_M4_PTR(m), sizeof(jce_mat4));
                    memcpy(p + sizeof(jce_mat4), &tint,        sizeof(jce_vec4));
                }
                bgfx_set_vertex_buffer(0, vbh, 0, vcount);
                bgfx_set_index_buffer(ibh, 0, icount);
                bgfx_set_instance_data_buffer(&idb, 0, nb);
                sr_bind_frame_shadow_state(sr);
                bgfx_set_state(state, 0);
                bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
                start += nb;
            }
        }
        return;
    }

    /* Frustum planes (from the pass camera + viewport aspect) + camera pos. */
    jce_vec4 planes[6];
    {
        uint32_t vw = cfg ? cfg->viewport_width  : 0u;
        uint32_t vh = cfg ? cfg->viewport_height : 0u;
        float aspect = (vw > 0u && vh > 0u) ? (float)vw / (float)vh : (16.0f / 9.0f);
        jce_mat4 vmat = jce_camera_view(camera);
        jce_mat4 pmat = jce_camera_proj(camera, aspect, sr->homogeneous_depth);
        jce_mat4 vp   = jce_m4_multiply(&pmat, &vmat);
        sr_extract_frustum_planes(&vp, planes);
    }
    const jce_vec3 cam = jce_camera_get_position(camera);

    const uint16_t  gx   = sr->grass_cache[slot].grid_gx;
    const uint16_t  gz   = sr->grass_cache[slot].grid_gz;
    const float     cell = sr->grass_cache[slot].cell_size;
    const float     gmnx = sr->grass_cache[slot].grid_min_x;
    const float     gmnz = sr->grass_cache[slot].grid_min_z;
    const float     ymin = sr->grass_cache[slot].field_ymin;
    const float     ymax = sr->grass_cache[slot].field_ymax;
    const float     fade = (g->fade_end > 0.0f) ? g->fade_end : 0.0f;
    const float     marg = cell * 0.25f + 0.5f;   /* wind-sway / blade-width slack */
    const uint32_t *cs   = sr->grass_cache[slot].cell_start;
    const uint32_t  ncells = (uint32_t)gx * gz;

    /* Cells are packed-contiguous, so merge adjacent visible cells into one
     * submit range (empty cells add 0 and keep contiguity). */
    uint32_t run_start = 0u, run_len = 0u;
    bool have_run = false;
    for (uint32_t c = 0; c < ncells; ++c) {
        uint32_t bs = cs[c], be = cs[c + 1u];
        if (be == bs) continue;                        /* empty cell */
        uint16_t cx = (uint16_t)(c % gx), cz = (uint16_t)(c / gx);
        float x0 = gmnx + (float)cx * cell - marg, x1 = gmnx + (float)(cx + 1) * cell + marg;
        float z0 = gmnz + (float)cz * cell - marg, z1 = gmnz + (float)(cz + 1) * cell + marg;
        jce_vec3 amn = { x0, ymin, z0 }, amx = { x1, ymax, z1 };
        bool vis = sr_aabb_in_frustum(planes, amn, amx);
        if (vis && fade > 0.0f) {
            float qx = cam.x < x0 ? x0 : (cam.x > x1 ? x1 : cam.x);
            float qz = cam.z < z0 ? z0 : (cam.z > z1 ? z1 : cam.z);
            float dx = cam.x - qx, dz = cam.z - qz;
            if (dx * dx + dz * dz > fade * fade) vis = false;  /* shader-faded */
        }
        if (!vis) {
            if (have_run) {
                sr_grass_submit_range(sr, slot, prog, state, view_id, stride,
                                      vbh, ibh, vcount, icount, run_start, run_len);
                have_run = false;
            }
            continue;
        }
        if (!have_run) { run_start = bs; run_len = be - bs; have_run = true; }
        else           { run_len  += be - bs; }
    }
    if (have_run)
        sr_grass_submit_range(sr, slot, prog, state, view_id, stride,
                              vbh, ibh, vcount, icount, run_start, run_len);
}


/* ── Water surface draw (Gerstner, roadmap 2.3) ───────────────────────
 *
 * Mirrors sr_draw_terrain_chunks / sr_draw_foliage: each entity with an
 * enabled, visible JceWaterComponent gets a cached flat XZ grid mesh sized
 * to size_x*size_z (SR_WATER_GRID_RES² quads, centred on the entity origin).
 * The grid is submitted with the water program in a blended/transparent pass;
 * the Gerstner displacement + analytic normal are evaluated per-vertex on the
 * GPU (vs_water.sc, the twin of jce_water.c).  Wave params, time, colors and
 * the camera reach the shaders via the u_water_* uniforms; lighting reuses the
 * PBR global bind (lights/camera/ambient/IBL).  The grid mesh is cached per
 * entity and rebuilt only when the plane size changes. */

/* Lazily create the water program + its uniforms (once per renderer). */
static void sr_water_lazy_init(JceSceneRenderer *sr)
{
    if (!sr->water_prog_tried) {
        sr->water_prog_tried = true;
        JceShaderHandle wh = shader_load_program(sr->pak, "water");
        sr->prog_water.idx = wh.idx;
        if (wh.idx == UINT16_MAX)
            LOG_WARN(LOG_TAG, "water shader not found in PAK "
                              "(water surfaces will not render)");
    }
    if (!BGFX_HANDLE_IS_VALID(sr->u_water_wave_a))
        sr->u_water_wave_a = bgfx_create_uniform("u_water_wave_a",
                                                 BGFX_UNIFORM_TYPE_VEC4, 4);
    if (!BGFX_HANDLE_IS_VALID(sr->u_water_wave_b))
        sr->u_water_wave_b = bgfx_create_uniform("u_water_wave_b",
                                                 BGFX_UNIFORM_TYPE_VEC4, 4);
    if (!BGFX_HANDLE_IS_VALID(sr->u_water_params))
        sr->u_water_params = bgfx_create_uniform("u_water_params",
                                                 BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->u_water_time))
        sr->u_water_time = bgfx_create_uniform("u_water_time",
                                               BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->u_water_color_shallow))
        sr->u_water_color_shallow = bgfx_create_uniform("u_water_color_shallow",
                                                        BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->u_water_color_deep))
        sr->u_water_color_deep = bgfx_create_uniform("u_water_color_deep",
                                                     BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->u_water_shading))
        sr->u_water_shading = bgfx_create_uniform("u_water_shading",
                                                  BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->u_water_mode))
        sr->u_water_mode = bgfx_create_uniform("u_water_mode",
                                               BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->s_water_disp))
        sr->s_water_disp = bgfx_create_uniform("s_water_disp",
                                               BGFX_UNIFORM_TYPE_SAMPLER, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->s_water_data))
        sr->s_water_data = bgfx_create_uniform("s_water_data",
                                               BGFX_UNIFORM_TYPE_SAMPLER, 1);
}

/* Build a flat XZ grid (entity-local, y=0) centred on the origin spanning
 * [-size_x/2, size_x/2] x [-size_z/2, size_z/2] with SR_WATER_GRID_RES² quads.
 * vs_water displaces each vertex; the CPU mesh is intentionally planar. */
static JceMesh *sr_water_build_grid(float size_x, float size_z)
{
    const int res = SR_WATER_GRID_RES;
    const int vside = res + 1;
    const uint32_t nverts = (uint32_t)(vside * vside);
    const uint32_t ntris  = (uint32_t)(res * res * 2);
    const uint32_t nindices = ntris * 3;

    JceMeshVertex *verts = (JceMeshVertex *)JCE_MALLOC(nverts * sizeof(JceMeshVertex));
    if (!verts) return NULL;
    uint32_t *indices = (uint32_t *)JCE_MALLOC(nindices * sizeof(uint32_t));
    if (!indices) { JCE_FREE(verts); return NULL; }

    const float hx = size_x * 0.5f;
    const float hz = size_z * 0.5f;
    const float step_x = size_x / (float)res;
    const float step_z = size_z / (float)res;

    uint32_t vi = 0;
    for (int gz = 0; gz < vside; gz++) {
        for (int gx = 0; gx < vside; gx++) {
            float x = -hx + step_x * (float)gx;
            float z = -hz + step_z * (float)gz;
            verts[vi].pos[0] = x;
            verts[vi].pos[1] = 0.0f;
            verts[vi].pos[2] = z;
            /* Flat up-normal; vs_water overwrites with the analytic normal. */
            verts[vi].normal[0] = 0.0f;
            verts[vi].normal[1] = 1.0f;
            verts[vi].normal[2] = 0.0f;
            verts[vi].uv[0] = (float)gx / (float)res;
            verts[vi].uv[1] = (float)gz / (float)res;
            vi++;
        }
    }

    uint32_t ii = 0;
    for (int gz = 0; gz < res; gz++) {
        for (int gx = 0; gx < res; gx++) {
            uint32_t i0 = (uint32_t)(gz * vside + gx);
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + (uint32_t)vside;
            uint32_t i3 = i2 + 1;
            /* CCW-from-above winding (engine convention; matches plane_ex). */
            indices[ii++] = i0; indices[ii++] = i2; indices[ii++] = i1;
            indices[ii++] = i1; indices[ii++] = i2; indices[ii++] = i3;
        }
    }

    JceMesh *m = jce_mesh_create(verts, nverts, indices, nindices);
    JCE_FREE(verts);
    JCE_FREE(indices);
    return m;
}

/* Release ALL resources a water cache slot owns (grid mesh + FFT core + the
 * dynamic FFT displacement texture) and zero it.  Used on eviction and renderer
 * destroy so neither the FFT state nor its GPU texture leak. */
void sr_water_slot_free(JceSceneRenderer *sr, int slot)
{
    if (sr->water_cache[slot].mesh)
        jce_mesh_destroy(sr->water_cache[slot].mesh);
    if (sr->water_cache[slot].fft)
        jce_water_fft_destroy(sr->water_cache[slot].fft);
    if (BGFX_HANDLE_IS_VALID(sr->water_cache[slot].fft_tex))
        bgfx_destroy_texture(sr->water_cache[slot].fft_tex);
    memset(&sr->water_cache[slot], 0, sizeof sr->water_cache[slot]);
}

/* Find (or evict into) a water cache slot for entity `e`. */
static int sr_water_find_slot(JceSceneRenderer *sr, JceEntity e)
{
    int free_slot = -1;
    for (int i = 0; i < SR_WATER_SLOT_MAX; ++i) {
        if (sr->water_cache[i].used && sr->water_cache[i].entity == e)
            return i;
        if (free_slot < 0 && !sr->water_cache[i].used) free_slot = i;
    }
    if (free_slot >= 0) return free_slot;
    /* All slots taken: evict a deterministic one (rebuilds next time). */
    int victim = (int)((uint32_t)e % (uint32_t)SR_WATER_SLOT_MAX);
    sr_water_slot_free(sr, victim);
    return victim;
}

void sr_draw_water(JceSceneRenderer *sr, JceScene *scene,
                          EntityList *list, JceEntity e, uint16_t view_id)
{
    JceWaterComponent *wc = jce_scene_get_water(scene, e);
    if (!wc || !wc->visible) return;
    if (wc->size_x <= 0.0f || wc->size_z <= 0.0f) return;
    if (!jce_scene_has_transform(scene, e)) return;

    sr_water_lazy_init(sr);
    if (!BGFX_HANDLE_IS_VALID(sr->prog_water)) return;

    int slot = sr_water_find_slot(sr, e);
    if (slot < 0) return;

    const bool rebuild = !sr->water_cache[slot].used ||
                         !sr->water_cache[slot].mesh ||
                         sr->water_cache[slot].size_x != wc->size_x ||
                         sr->water_cache[slot].size_z != wc->size_z;
    if (rebuild) {
        if (sr->water_cache[slot].mesh)
            jce_mesh_destroy(sr->water_cache[slot].mesh);
        sr->water_cache[slot].mesh = sr_water_build_grid(wc->size_x, wc->size_z);
        sr->water_cache[slot].size_x = wc->size_x;
        sr->water_cache[slot].size_z = wc->size_z;
        sr->water_cache[slot].entity = e;
        sr->water_cache[slot].used   = true;
    }
    JceMesh *mesh = sr->water_cache[slot].mesh;
    if (!mesh) return;

    jce_mat4 model = jce_scene_get_world_matrix(scene, e);

    /* Pack wave params for the vertex shader (max JCE_WATER_COMP_MAX_WAVES). */
    int wcount = wc->wave_count;
    if (wcount < 0) wcount = 0;
    if (wcount > JCE_WATER_COMP_MAX_WAVES) wcount = JCE_WATER_COMP_MAX_WAVES;
    float wave_a[4 * 4]; /* vec4[4] */
    float wave_b[4 * 4];
    memset(wave_a, 0, sizeof wave_a);
    memset(wave_b, 0, sizeof wave_b);
    for (int i = 0; i < wcount; ++i) {
        wave_a[i * 4 + 0] = wc->waves[i].amplitude;
        wave_a[i * 4 + 1] = wc->waves[i].wavelength;
        wave_a[i * 4 + 2] = wc->waves[i].speed;
        wave_a[i * 4 + 3] = wc->waves[i].steepness;
        wave_b[i * 4 + 0] = wc->waves[i].dir_x;
        wave_b[i * 4 + 1] = wc->waves[i].dir_z;
    }
    /* base_height is entity-local: add the entity's world Y so the wave plane
     * sits where the entity is placed (the grid is generated at local y=0). */
    float wparams[4] = { (float)wcount,
                         wc->base_height + model.col[3].y,
                         0.0f, 0.0f };
    float wtime[4]   = { sr->water_time, 0.0f, 0.0f, 0.0f };
    float c_shallow[4] = { wc->color_shallow[0], wc->color_shallow[1],
                           wc->color_shallow[2], 1.0f };
    float c_deep[4]    = { wc->color_deep[0], wc->color_deep[1],
                           wc->color_deep[2], 1.0f };
    /* .z/.w carry the stylized extras (shore ripple rings / ice crackle);
     * both default 0 so pre-existing content is pixel-identical. */
    float shading[4]   = { wc->transparency, wc->sun_specular,
                           wc->shore_ripple, wc->ice_ratio };

    /* ── FFT ocean path (Tessendorf, water_mode==FFT) ────────────────────
     * Lazily create the per-entity CPU FFT patch + a mutable RGBA32F
     * displacement texture (height in R, disp_x in G, disp_z in B; created once
     * and refreshed each frame).  Falls back to GERSTNER if the FFT core can't
     * be created (e.g. degenerate params).  u_water_mode.x selects the branch in
     * vs_water; .y carries the patch size for the vertex UV mapping. */
    int water_mode = JCE_WATER_MODE_GERSTNER;
    float fft_patch = 0.0f;
    if (wc->water_mode == JCE_WATER_MODE_FFT) {
        int res = wc->fft_resolution;
        if (res < 32)  res = 32;
        if (res > 256) res = 256;

        /* (Re)create the FFT core when any spectrum param changes. */
        const bool fft_rebuild =
            !sr->water_cache[slot].fft ||
            sr->water_cache[slot].fft_res        != res ||
            sr->water_cache[slot].fft_patch_size != wc->fft_patch_size ||
            sr->water_cache[slot].fft_wind_speed != wc->fft_wind_speed ||
            sr->water_cache[slot].fft_wind_dir_x != wc->fft_wind_dir_x ||
            sr->water_cache[slot].fft_wind_dir_z != wc->fft_wind_dir_z ||
            sr->water_cache[slot].fft_amplitude  != wc->fft_amplitude;
        if (fft_rebuild) {
            if (sr->water_cache[slot].fft) {
                jce_water_fft_destroy(sr->water_cache[slot].fft);
                sr->water_cache[slot].fft = NULL;
            }
            sr->water_cache[slot].fft = jce_water_fft_create(
                res, wc->fft_patch_size, wc->fft_wind_speed,
                wc->fft_wind_dir_x, wc->fft_wind_dir_z, wc->fft_amplitude,
                /* deterministic per-entity seed so distinct waters differ */
                (unsigned int)(0x9E37u ^ (uint32_t)e));
            sr->water_cache[slot].fft_patch_size = wc->fft_patch_size;
            sr->water_cache[slot].fft_wind_speed = wc->fft_wind_speed;
            sr->water_cache[slot].fft_wind_dir_x = wc->fft_wind_dir_x;
            sr->water_cache[slot].fft_wind_dir_z = wc->fft_wind_dir_z;
            sr->water_cache[slot].fft_amplitude  = wc->fft_amplitude;

            /* (Re)create the displacement texture if the resolution changed. */
            if (sr->water_cache[slot].fft &&
                (sr->water_cache[slot].fft_res != res ||
                 !BGFX_HANDLE_IS_VALID(sr->water_cache[slot].fft_tex))) {
                if (BGFX_HANDLE_IS_VALID(sr->water_cache[slot].fft_tex))
                    bgfx_destroy_texture(sr->water_cache[slot].fft_tex);
                const uint64_t tflags = BGFX_TEXTURE_NONE
                    | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
                sr->water_cache[slot].fft_tex = bgfx_create_texture_2d(
                    (uint16_t)res, (uint16_t)res, false, 1,
                    BGFX_TEXTURE_FORMAT_RGBA32F, tflags, NULL);
            }
            sr->water_cache[slot].fft_res = res;
        }

        JceWaterFft *fft = sr->water_cache[slot].fft;
        if (fft && BGFX_HANDLE_IS_VALID(sr->water_cache[slot].fft_tex)) {
            /* Advance the surface to the shared phase clock, then pack the three
             * fields into the RGBA32F texture and upload (mirrors the Forward+
             * mutable-texture refresh). */
            jce_water_fft_evolve(fft, sr->water_time);
            const float *h  = jce_water_fft_height_data(fft);
            const float *dx = jce_water_fft_disp_x_data(fft);
            const float *dz = jce_water_fft_disp_z_data(fft);
            uint32_t cells = (uint32_t)res * (uint32_t)res;
            const bgfx_memory_t *mem = bgfx_alloc(cells * 4u * (uint32_t)sizeof(float));
            float *dstf = (float *)mem->data;
            for (uint32_t i = 0; i < cells; ++i) {
                dstf[i * 4 + 0] = h  ? h[i]  : 0.0f;  /* R = height (Y)        */
                dstf[i * 4 + 1] = dx ? dx[i] : 0.0f;  /* G = horizontal X roll */
                dstf[i * 4 + 2] = dz ? dz[i] : 0.0f;  /* B = horizontal Z roll */
                dstf[i * 4 + 3] = 0.0f;
            }
            uint16_t pitch = (uint16_t)((uint32_t)res * 4u * (uint32_t)sizeof(float));
            bgfx_update_texture_2d(sr->water_cache[slot].fft_tex, 0, 0, 0, 0,
                                   (uint16_t)res, (uint16_t)res, mem, pitch);
            water_mode = JCE_WATER_MODE_FFT;
            fft_patch  = wc->fft_patch_size;
        }
    }
    /* ── STYLIZED overlay path (water_mode==STYLIZED) ─────────────────────
     * A flat, discard-everywhere-but-strokes ripple overlay: the fragment
     * shader draws only shore-hugging ripple arcs (+ splash circles + ice
     * plates) from the authored shore-distance data map; the water BODY
     * color is painted in the ground beneath.  Waves forced off (flat plane);
     * splash ratio rides u_water_mode.z, data map binds at stage 5. */
    JceTexture stylized_data = { UINT16_MAX };
    if (wc->water_mode == JCE_WATER_MODE_STYLIZED) {
        water_mode = JCE_WATER_MODE_STYLIZED;
        memset(wave_a, 0, sizeof wave_a);
        memset(wave_b, 0, sizeof wave_b);
        wparams[0] = 0.0f;   /* wave_count = 0 -> flat vs path */
        if (wc->data_tex[0])
            stylized_data = sr_resolve_texture(sr, wc->data_tex);
    }
    float wmode[4] = { (float)water_mode, fft_patch,
                       wc->splash_ratio, 0.0f };

    /* Global PBR bind (lights/camera/ambient/IBL) — same as terrain. */
    JcePbrMaterial wpbr = jce_pbr_material_default();
    sr_inline_bind_pbr_global(sr, &wpbr, view_id, scene, list);

    bgfx_set_transform(model.raw[0], 1);
    bgfx_set_uniform(sr->u_water_wave_a, wave_a, 4);
    bgfx_set_uniform(sr->u_water_wave_b, wave_b, 4);
    bgfx_set_uniform(sr->u_water_params, wparams, 1);
    bgfx_set_uniform(sr->u_water_time,   wtime,   1);
    bgfx_set_uniform(sr->u_water_color_shallow, c_shallow, 1);
    bgfx_set_uniform(sr->u_water_color_deep,    c_deep,    1);
    bgfx_set_uniform(sr->u_water_shading,       shading,   1);
    bgfx_set_uniform(sr->u_water_mode,          wmode,     1);

    /* FFT displacement texture for the vertex fetch (stage 0; Gerstner binds a
     * harmless valid texture so the sampler is always defined). */
    if (water_mode == JCE_WATER_MODE_FFT &&
        BGFX_HANDLE_IS_VALID(sr->water_cache[slot].fft_tex))
        bgfx_set_texture(0, sr->s_water_disp, sr->water_cache[slot].fft_tex,
                         UINT32_MAX);
    else
        bgfx_set_texture(0, sr->s_water_disp, sr->white_tex, UINT32_MAX);

    /* Stylized shore-distance data map (stage 5; white = harmless default:
     * depth 1 everywhere -> zero shore strokes). */
    {
        bgfx_texture_handle_t dt = { stylized_data.idx };
        if (!BGFX_HANDLE_IS_VALID(dt)) dt = sr->white_tex;
        bgfx_set_texture(5, sr->s_water_data, dt, UINT32_MAX);
    }

    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(mesh) };
    bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(mesh) };
    if (!BGFX_HANDLE_IS_VALID(vbh) || !BGFX_HANDLE_IS_VALID(ibh)) return;
    bgfx_set_vertex_buffer(0, vbh, 0, jce_mesh_vertex_count(mesh));
    bgfx_set_index_buffer(ibh, 0, jce_mesh_index_count(mesh));

    /* Translucent, depth-tested but NOT depth-writing (so the surface
     * composites over the solid scene without occluding entities behind it),
     * alpha blended, no back-face cull (the surface is viewable from both
     * sides as it rolls). */
    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_DEPTH_TEST_LESS
                   | BGFX_STATE_BLEND_ALPHA
                   | BGFX_STATE_MSAA;
    bgfx_set_state(state, 0);

    bgfx_submit(view_id, sr->prog_water, 0, BGFX_DISCARD_ALL);
}

/* ── Tilemap chunked draw (P5-tilemap) ────────────────────────────────
 *
 * Clones the terrain slot-cache pattern for .tilemap.json assets: the map +
 * its tileset load lazily on first sight (PAK-first → cbs.resolve_path →
 * loose file), the grid is baked into 32x32-cell chunks of textured quads in
 * ENTITY-LOCAL space (1 cell = 1 unit; cell (c,r) spans [c,c+1] x
 * [-(r+1),-r] — rows grow down, matching the Tile Palette), and each visible
 * chunk submits one draw through the textured mesh program with the entity
 * world matrix as the transform.  The component tint is baked into the
 * vertex color, so chunks rebuild when it changes. */

/* Same pos3f/color4u8/uv2f vertex family as the sprite batch. */
typedef struct {
    float    x, y, z;
    uint32_t abgr;
    float    u, v;
} SrTilemapVertex;

/* Lazily create the shared vertex layout, quad index buffer and sampler. */
static void sr_tilemap_lazy_init(JceSceneRenderer *sr)
{
    if (!sr->tilemap_layout_ready) {
        bgfx_vertex_layout_begin(&sr->tilemap_layout, bgfx_get_renderer_type());
        bgfx_vertex_layout_add(&sr->tilemap_layout, BGFX_ATTRIB_POSITION, 3,
                               BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&sr->tilemap_layout, BGFX_ATTRIB_COLOR0, 4,
                               BGFX_ATTRIB_TYPE_UINT8, true, false);
        bgfx_vertex_layout_add(&sr->tilemap_layout, BGFX_ATTRIB_TEXCOORD0, 2,
                               BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_end(&sr->tilemap_layout);
        sr->tilemap_layout_ready = true;
    }
    if (!BGFX_HANDLE_IS_VALID(sr->tilemap_shared_ib)) {
        /* ONE shared static IB: the 0,1,2 / 0,2,3 quad pattern x 1024 quads
         * (6144 u16).  Every chunk VB indexes a prefix of it. */
        const uint32_t n = SR_TILEMAP_CHUNK_QUADS * 6;
        const bgfx_memory_t *mem = bgfx_alloc(n * (uint32_t)sizeof(uint16_t));
        if (mem) {
            uint16_t *ib = (uint16_t *)mem->data;
            for (uint32_t q = 0; q < SR_TILEMAP_CHUNK_QUADS; q++) {
                uint16_t vi = (uint16_t)(q * 4);
                ib[q * 6 + 0] = vi;
                ib[q * 6 + 1] = (uint16_t)(vi + 1);
                ib[q * 6 + 2] = (uint16_t)(vi + 2);
                ib[q * 6 + 3] = vi;
                ib[q * 6 + 4] = (uint16_t)(vi + 2);
                ib[q * 6 + 5] = (uint16_t)(vi + 3);
            }
            sr->tilemap_shared_ib = bgfx_create_index_buffer(mem, BGFX_BUFFER_NONE);
        }
    }
    if (!BGFX_HANDLE_IS_VALID(sr->tilemap_s_tex))
        sr->tilemap_s_tex = bgfx_create_uniform("s_texColor",
                                                BGFX_UNIFORM_TYPE_SAMPLER, 1);
}

/* Free one cache slot: chunk VBs + metadata arrays + the CPU assets. */
void sr_tilemap_free_slot(JceSceneRenderer *sr, int i)
{
    if (i < 0 || i >= SR_TILEMAP_SLOT_MAX) return;
    if (!sr->tilemap_cache[i].used) {
        memset(&sr->tilemap_cache[i], 0, sizeof sr->tilemap_cache[i]);
        return;
    }
    if (sr->tilemap_cache[i].chunk_vb) {
        for (int c = 0; c < sr->tilemap_cache[i].chunk_count; c++) {
            if (BGFX_HANDLE_IS_VALID(sr->tilemap_cache[i].chunk_vb[c]))
                bgfx_destroy_vertex_buffer(sr->tilemap_cache[i].chunk_vb[c]);
        }
        JCE_FREE(sr->tilemap_cache[i].chunk_vb);
    }
    if (sr->tilemap_cache[i].chunk_quads) JCE_FREE(sr->tilemap_cache[i].chunk_quads);
    if (sr->tilemap_cache[i].chunk_min)   JCE_FREE(sr->tilemap_cache[i].chunk_min);
    if (sr->tilemap_cache[i].chunk_max)   JCE_FREE(sr->tilemap_cache[i].chunk_max);
    if (sr->tilemap_cache[i].map)         jce_tilemap_unload(sr->tilemap_cache[i].map);
    if (sr->tilemap_cache[i].tileset)     jce_tileset_unload(sr->tilemap_cache[i].tileset);
    memset(&sr->tilemap_cache[i], 0, sizeof sr->tilemap_cache[i]);
}

/* Find (or lazily load) the tilemap cache slot for the component's
 * tilemap_path, loading the map + its tileset and initialising the chunk
 * grid metadata on first load.  Returns the slot index, or -1 on failure. */
int sr_tilemap_find_or_load_slot(JceSceneRenderer *sr,
                                        const JceTilemapComponent *tmc)
{
    if (!sr || !tmc || !tmc->tilemap_path[0]) return -1;
    const char *path = tmc->tilemap_path;

    int slot = -1, free_slot = -1;
    for (int i = 0; i < SR_TILEMAP_SLOT_MAX; i++) {
        if (sr->tilemap_cache[i].used &&
            strncmp(sr->tilemap_cache[i].path, path,
                    sizeof sr->tilemap_cache[i].path) == 0) {
            slot = i; break;
        }
        if (!sr->tilemap_cache[i].used && free_slot < 0) free_slot = i;
    }
    if (slot >= 0)
        return sr->tilemap_cache[slot].failed ? -1 : slot;
    if (free_slot < 0) return -1;

    slot = free_slot;
    memset(&sr->tilemap_cache[slot], 0, sizeof sr->tilemap_cache[slot]);
    jce_strlcpy(sr->tilemap_cache[slot].path, path,
                sizeof sr->tilemap_cache[slot].path);
    sr->tilemap_cache[slot].used = true;

    /* PAK-first (deployed bundles overlay sr->pak), then the host-resolved
     * path (editor), then the raw path (loose files). */
    JceTilemapAsset *map = jce_tilemap_load_from_pak(sr->pak, path);
    char        resolved[1024];
    const char *load_path = path;
    if (!map) {
        if (sr->has_cbs && sr->cbs.resolve_path &&
            sr->cbs.resolve_path(path, resolved, (int)sizeof(resolved),
                                 sr->cbs.userdata)) {
            load_path = resolved;
        }
        map = jce_tilemap_load_file(load_path);
    }
    if (!map) {
        sr->tilemap_cache[slot].failed = true;
        LOG_WARN(LOG_TAG, "tilemap load failed: '%s' (from '%s')",
                 load_path, path);
        return -1;
    }
    sr->tilemap_cache[slot].map = map;

    /* Tileset: the map's authored "sprites" key wins; the component's
     * sprites_path is the fallback.  A missing tileset is tolerated — the
     * map loads but renders nothing (every tile_uv lookup fails). */
    const char *ts_path = jce_tilemap_sprites_path(map);
    if (!ts_path || !ts_path[0]) ts_path = tmc->sprites_path;
    if (ts_path && ts_path[0]) {
        jce_strlcpy(sr->tilemap_cache[slot].tileset_path, ts_path,
                    sizeof sr->tilemap_cache[slot].tileset_path);
        JceTilesetAsset *ts = jce_tileset_load_from_pak(sr->pak, ts_path);
        if (!ts) {
            const char *ts_load = ts_path;
            if (sr->has_cbs && sr->cbs.resolve_path &&
                sr->cbs.resolve_path(ts_path, resolved, (int)sizeof(resolved),
                                     sr->cbs.userdata)) {
                ts_load = resolved;
            }
            ts = jce_tileset_load_file(ts_load);
        }
        if (!ts)
            LOG_WARN(LOG_TAG, "tileset load failed: '%s' (tilemap '%s')",
                     ts_path, path);
        sr->tilemap_cache[slot].tileset = ts;
    } else {
        LOG_WARN(LOG_TAG, "tilemap '%s' has no tileset (sprites) path", path);
    }

    /* Chunk grid metadata (VBs are baked on first draw). */
    uint32_t w = jce_tilemap_width(map);
    uint32_t h = jce_tilemap_height(map);
    int ncx = (int)((w + SR_TILEMAP_CHUNK_DIM - 1) / SR_TILEMAP_CHUNK_DIM);
    int ncy = (int)((h + SR_TILEMAP_CHUNK_DIM - 1) / SR_TILEMAP_CHUNK_DIM);
    int n   = ncx * ncy;
    if (n > 0) {
        sr->tilemap_cache[slot].chunk_vb = (bgfx_vertex_buffer_handle_t *)
            JCE_MALLOC(sizeof(bgfx_vertex_buffer_handle_t) * (size_t)n);
        sr->tilemap_cache[slot].chunk_quads =
            (uint16_t *)JCE_CALLOC((size_t)n, sizeof(uint16_t));
        sr->tilemap_cache[slot].chunk_min =
            (jce_vec3 *)JCE_CALLOC((size_t)n, sizeof(jce_vec3));
        sr->tilemap_cache[slot].chunk_max =
            (jce_vec3 *)JCE_CALLOC((size_t)n, sizeof(jce_vec3));
        if (!sr->tilemap_cache[slot].chunk_vb ||
            !sr->tilemap_cache[slot].chunk_quads ||
            !sr->tilemap_cache[slot].chunk_min ||
            !sr->tilemap_cache[slot].chunk_max) {
            sr->tilemap_cache[slot].chunk_count = 0;
            sr->tilemap_cache[slot].failed = true;
            return -1;   /* arrays freed by sr_tilemap_free_slot at destroy */
        }
        for (int c = 0; c < n; c++)
            sr->tilemap_cache[slot].chunk_vb[c].idx = UINT16_MAX;
    }
    sr->tilemap_cache[slot].chunk_nx    = ncx;
    sr->tilemap_cache[slot].chunk_ny    = ncy;
    sr->tilemap_cache[slot].chunk_count = n;
    return slot;
}

uint32_t sr_tilemap_color_abgr(const float c[4])
{
    float r = c[0], g = c[1], b = c[2], a = c[3];
    if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; if (b > 1.0f) b = 1.0f;
    if (a < 0.0f) a = 0.0f; if (a > 1.0f) a = 1.0f;
    return ((uint32_t)(uint8_t)(a * 255.0f) << 24)
         | ((uint32_t)(uint8_t)(b * 255.0f) << 16)
         | ((uint32_t)(uint8_t)(g * 255.0f) << 8)
         |  (uint32_t)(uint8_t)(r * 255.0f);
}

/* Bake (or re-bake when the tint changed) every chunk's static VB.  Empty
 * cells are skipped; an all-empty chunk gets no VB at all. */
void sr_tilemap_build_chunks(JceSceneRenderer *sr, int slot, uint32_t abgr)
{
    if (slot < 0 || slot >= SR_TILEMAP_SLOT_MAX) return;
    if (sr->tilemap_cache[slot].chunks_built &&
        sr->tilemap_cache[slot].baked_abgr == abgr)
        return;

    sr_tilemap_lazy_init(sr);

    JceTilemapAsset *map = sr->tilemap_cache[slot].map;
    JceTilesetAsset *ts  = sr->tilemap_cache[slot].tileset;
    if (!map || sr->tilemap_cache[slot].chunk_count <= 0) {
        sr->tilemap_cache[slot].baked_abgr   = abgr;
        sr->tilemap_cache[slot].chunks_built = true;
        return;
    }

    SrTilemapVertex *verts = (SrTilemapVertex *)
        JCE_MALLOC(sizeof(SrTilemapVertex) * SR_TILEMAP_CHUNK_QUADS * 4);
    if (!verts) return;

    uint32_t w = jce_tilemap_width(map);
    uint32_t h = jce_tilemap_height(map);
    int ncx = sr->tilemap_cache[slot].chunk_nx;
    int ncy = sr->tilemap_cache[slot].chunk_ny;

    for (int cy = 0; cy < ncy; cy++)
    for (int cx = 0; cx < ncx; cx++) {
        int idx = cy * ncx + cx;

        /* Tint re-bake: drop the old VB. */
        if (BGFX_HANDLE_IS_VALID(sr->tilemap_cache[slot].chunk_vb[idx])) {
            bgfx_destroy_vertex_buffer(sr->tilemap_cache[slot].chunk_vb[idx]);
            sr->tilemap_cache[slot].chunk_vb[idx].idx = UINT16_MAX;
        }
        sr->tilemap_cache[slot].chunk_quads[idx] = 0;

        uint32_t col0 = (uint32_t)cx * SR_TILEMAP_CHUNK_DIM;
        uint32_t row0 = (uint32_t)cy * SR_TILEMAP_CHUNK_DIM;
        uint32_t col1 = col0 + SR_TILEMAP_CHUNK_DIM; if (col1 > w) col1 = w;
        uint32_t row1 = row0 + SR_TILEMAP_CHUNK_DIM; if (row1 > h) row1 = h;

        /* Chunk-extent local AABB (conservative: ignores empty cells). */
        sr->tilemap_cache[slot].chunk_min[idx] =
            jce_v3((float)col0, -(float)row1, 0.0f);
        sr->tilemap_cache[slot].chunk_max[idx] =
            jce_v3((float)col1, -(float)row0, 0.0f);

        uint32_t quads = 0;
        for (uint32_t row = row0; row < row1; row++)
        for (uint32_t col = col0; col < col1; col++) {
            uint32_t id = jce_tilemap_tile_at(map, col, row);
            if (id == 0) continue;
            float u0, v0, u1, v1;
            if (!jce_tileset_tile_uv(ts, id, &u0, &v0, &u1, &v1)) {
                /* Distinguish the two warn-once cases: out-of-range id vs
                 * an unsized atlas (sourceW/H <= 0).  Either way the cell
                 * renders as empty. */
                if (ts && id > jce_tileset_rect_count(ts)) {
                    if (!sr->tilemap_cache[slot].warned_bad_id) {
                        LOG_WARN(LOG_TAG, "tilemap '%s': tile id %u exceeds "
                                 "tileset rect count %u; treating as empty",
                                 sr->tilemap_cache[slot].path, id,
                                 jce_tileset_rect_count(ts));
                        sr->tilemap_cache[slot].warned_bad_id = true;
                    }
                } else if (ts && !sr->tilemap_cache[slot].warned_src) {
                    LOG_WARN(LOG_TAG, "tileset '%s': sourceW/H not set; "
                             "tilemap '%s' renders nothing",
                             sr->tilemap_cache[slot].tileset_path,
                             sr->tilemap_cache[slot].path);
                    sr->tilemap_cache[slot].warned_src = true;
                }
                continue;
            }

            float x0 = (float)col, x1 = (float)(col + 1);
            float yt = -(float)row, yb = -(float)(row + 1);
            SrTilemapVertex *q = &verts[quads * 4];
            /* Matches the sprite batch quad: BL, BR, TR, TL with the
             * texture's top row (v0) on the tile's top edge. */
            q[0].x = x0; q[0].y = yb; q[0].z = 0.0f; q[0].abgr = abgr; q[0].u = u0; q[0].v = v1;
            q[1].x = x1; q[1].y = yb; q[1].z = 0.0f; q[1].abgr = abgr; q[1].u = u1; q[1].v = v1;
            q[2].x = x1; q[2].y = yt; q[2].z = 0.0f; q[2].abgr = abgr; q[2].u = u1; q[2].v = v0;
            q[3].x = x0; q[3].y = yt; q[3].z = 0.0f; q[3].abgr = abgr; q[3].u = u0; q[3].v = v0;
            quads++;
        }

        if (quads > 0) {
            const bgfx_memory_t *mem = bgfx_copy(
                verts, quads * 4 * (uint32_t)sizeof(SrTilemapVertex));
            sr->tilemap_cache[slot].chunk_vb[idx] =
                bgfx_create_vertex_buffer(mem, &sr->tilemap_layout,
                                          BGFX_BUFFER_NONE);
            sr->tilemap_cache[slot].chunk_quads[idx] = (uint16_t)quads;
        }
    }

    JCE_FREE(verts);
    sr->tilemap_cache[slot].baked_abgr   = abgr;
    sr->tilemap_cache[slot].chunks_built = true;
}

/* Submit every visible chunk of one tilemap entity (per-chunk AABB-vs-frustum
 * culling like terrain; the entity-level cull is bypassed by the caller). */
void sr_draw_tilemap_chunks(JceSceneRenderer *sr, int slot,
                                   const JceCamera *camera, uint16_t view_id,
                                   const jce_mat4 *model)
{
    if (!sr || slot < 0 || slot >= SR_TILEMAP_SLOT_MAX) return;
    if (sr->tilemap_cache[slot].chunk_count <= 0) return;
    if (!BGFX_HANDLE_IS_VALID(sr->tilemap_shared_ib)) return;

    JceShaderHandle sh = jce_renderer_get_program_mesh(sr->renderer);
    bgfx_program_handle_t prog;
    prog.idx = sh.idx;
    if (!BGFX_HANDLE_IS_VALID(prog)) return;

    /* Atlas texture (async cache; white fallback while loading/missing). */
    bgfx_texture_handle_t tex = sr->white_tex;
    if (sr->tilemap_cache[slot].tileset) {
        const char *img =
            jce_tileset_image_path(sr->tilemap_cache[slot].tileset);
        if (img && img[0]) {
            JceTexture t = sr_resolve_texture(sr, img);
            if (jce_texture_valid(t)) tex.idx = t.idx;
        }
    }

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

    const uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                         | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                         | BGFX_STATE_BLEND_ALPHA;

    for (int c = 0; c < sr->tilemap_cache[slot].chunk_count; c++) {
        uint16_t quads = sr->tilemap_cache[slot].chunk_quads[c];
        if (quads == 0) continue;
        bgfx_vertex_buffer_handle_t vb = sr->tilemap_cache[slot].chunk_vb[c];
        if (!BGFX_HANDLE_IS_VALID(vb)) continue;

        if (have_planes) {
            jce_vec3 wmn, wmx;
            sr_transform_aabb(model, sr->tilemap_cache[slot].chunk_min[c],
                              sr->tilemap_cache[slot].chunk_max[c],
                              &wmn, &wmx);
            if (!sr_aabb_in_frustum(planes, wmn, wmx)) continue;
        }

        bgfx_set_transform(model->raw[0], 1);
        bgfx_set_vertex_buffer(0, vb, 0, (uint32_t)quads * 4u);
        bgfx_set_index_buffer(sr->tilemap_shared_ib, 0, (uint32_t)quads * 6u);
        bgfx_set_texture(0, sr->tilemap_s_tex, tex, UINT32_MAX);
        bgfx_set_state(state, 0);
        bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
    }
}

/* ── Sky pass ─────────────────────────────────────────────────────── */

void sr_draw_sky_gradient(JceSceneRenderer *sr, uint16_t view_id)
{
    if (!BGFX_HANDLE_IS_VALID(sr->prog_sky)) return;

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &sr->sky_layout, 4, &tib, 6, false))
        return;

    float *v = (float *)tvb.data;
    uint16_t *ix = (uint16_t *)tib.data;

    v[0] = -1.0f; v[1] = -1.0f; v[2]  = 0.0f;
    v[3] =  1.0f; v[4] = -1.0f; v[5]  = 0.0f;
    v[6] =  1.0f; v[7] =  1.0f; v[8]  = 0.0f;
    v[9] = -1.0f; v[10]=  1.0f; v[11] = 0.0f;

    ix[0] = 0; ix[1] = 1; ix[2] = 2;
    ix[3] = 0; ix[4] = 2; ix[5] = 3;

    float sky_colors[12] = {
        0.25f, 0.45f, 0.80f, 1.0f,
        0.65f, 0.78f, 0.92f, 1.0f,
        0.22f, 0.22f, 0.28f, 1.0f,
    };
    if (sr->tod_active) {
        const JceTimeOfDayState *t = &sr->tod_state;
        sky_colors[0]  = t->sky_top.x;     sky_colors[1]  = t->sky_top.y;
        sky_colors[2]  = t->sky_top.z;     sky_colors[3]  = 1.0f;
        sky_colors[4]  = t->sky_horizon.x; sky_colors[5]  = t->sky_horizon.y;
        sky_colors[6]  = t->sky_horizon.z; sky_colors[7]  = 1.0f;
        sky_colors[8]  = t->sky_ground.x;  sky_colors[9]  = t->sky_ground.y;
        sky_colors[10] = t->sky_ground.z;  sky_colors[11] = 1.0f;
    }
    /* Stylized dome: overwrite sky_colors with authored dome zenith/horizon/ground
     * so the ramp uses the scene-authored stops even when ToD is inactive.
     * sky_mode is already the (possibly downgraded) capture value; this block
     * only executes when mode 3 survived the gate (Task 6 downgrade). */
    if (sr->sky_mode == JCE_SCENE_SKY_STYLIZED) {
        sky_colors[0]  = sr->dome_zenith[0];  sky_colors[1]  = sr->dome_zenith[1];
        sky_colors[2]  = sr->dome_zenith[2];  sky_colors[3]  = 1.0f;
        sky_colors[4]  = sr->dome_horizon[0]; sky_colors[5]  = sr->dome_horizon[1];
        sky_colors[6]  = sr->dome_horizon[2]; sky_colors[7]  = 1.0f;
        sky_colors[8]  = sr->dome_ground[0];  sky_colors[9]  = sr->dome_ground[1];
        sky_colors[10] = sr->dome_ground[2];  sky_colors[11] = 1.0f;
    }
    bgfx_set_uniform(sr->u_sky_colors, sky_colors, 3);

    /* Stylized dome uniforms: always uploaded with the captured (or
     * neutral) values so the shader never samples stale data, regardless
     * of the active sky mode (mirrors the Preetham safe-default rule). */
    {
        float dmid[4]  = { sr->dome_mid[0], sr->dome_mid[1], sr->dome_mid[2], sr->dome_mid[3] };
        float dglow[4] = { sr->dome_glow[0], sr->dome_glow[1], sr->dome_glow[2], sr->dome_glow[3] };
        float dsun[4]  = { sr->dome_sun[0], sr->dome_sun[1], sr->dome_sun[2], sr->dome_sun[3] };
        /* .w = origin-anchor radius (0 = legacy view dome). */
        float dscol[4] = { sr->dome_sun_col[0], sr->dome_sun_col[1], sr->dome_sun_col[2], sr->dome_anchor };
        float dray[4]  = { sr->dome_ray[0], sr->dome_ray[1], sr->dome_ray[2], sr->dome_ray[3] };
        if (BGFX_HANDLE_IS_VALID(sr->u_sky_dome_mid))     bgfx_set_uniform(sr->u_sky_dome_mid,     dmid,  1);
        if (BGFX_HANDLE_IS_VALID(sr->u_sky_dome_glow))    bgfx_set_uniform(sr->u_sky_dome_glow,    dglow, 1);
        if (BGFX_HANDLE_IS_VALID(sr->u_sky_dome_sun))     bgfx_set_uniform(sr->u_sky_dome_sun,     dsun,  1);
        if (BGFX_HANDLE_IS_VALID(sr->u_sky_dome_sun_col)) bgfx_set_uniform(sr->u_sky_dome_sun_col, dscol, 1);
        if (BGFX_HANDLE_IS_VALID(sr->u_sky_dome_ray))     bgfx_set_uniform(sr->u_sky_dome_ray,     dray,  1);
        /* Scene fog for the anchored-dome horizon blend (fs_sky gates on
         * anchor > 0 AND fog mode > 0, so this is inert everywhere else). */
        if (BGFX_HANDLE_IS_VALID(sr->u_fog_params))
            bgfx_set_uniform(sr->u_fog_params, sr->fog_frame.params, 1);
        if (BGFX_HANDLE_IS_VALID(sr->u_fog_color))
            bgfx_set_uniform(sr->u_fog_color,  sr->fog_frame.color,  1);
    }

    /* Preetham uniforms always carry safe defaults so the shader never
     * reads stale/unset values regardless of the active mode.  The actual
     * Preetham state is filled below only when PREETHAM mode is selected. */
    float perez[16];   /* vec4[4]: Y/x/y A..D + (EY,Ex,Ey,0)             */
    float zenith[4] = { 1.0f, 0.3f, 0.3f, 0.0f };
    float sun4[4]   = { 0.0f, 1.0f, 0.0f, 0.0f };
    for (int i = 0; i < 16; ++i) perez[i] = 0.0f;

    bgfx_texture_handle_t equirect_tex = { UINT16_MAX };
    float sky_params[4] = { 0.0f, 1.0f, 0.0f, 0.0f };

    if (sr->sky_mode == JCE_SCENE_SKY_STYLIZED) {
        /* Stylized dome: an AUTHORED disk direction (dome.sunDir — e.g. a
         * near-horizon moon) wins; else mirror Preetham's selection (ToD sun
         * when active, else a default high sun) so the sun disk aligns
         * with the directional light. Zero vector = unset (legacy). */
        float sun_dir[3];
        const float adx = sr->dome_sun_dir[0], ady = sr->dome_sun_dir[1],
                    adz = sr->dome_sun_dir[2];
        const float alen2 = adx * adx + ady * ady + adz * adz;
        if (alen2 > 1e-6f) {
            /* Normalize: fs_sky compares raw dot() against the disk size
             * thresholds, so a non-unit direction would rescale the disk. */
            const float inv = 1.0f / sqrtf(alen2);
            sun_dir[0] = adx * inv; sun_dir[1] = ady * inv; sun_dir[2] = adz * inv;
        } else if (sr->tod_active) {
            sun_dir[0] = sr->tod_state.sun_direction.x;
            sun_dir[1] = sr->tod_state.sun_direction.y;
            sun_dir[2] = sr->tod_state.sun_direction.z;
        } else {
            sun_dir[0] = 0.0f; sun_dir[1] = 0.9f; sun_dir[2] = 0.4359f;
        }
        sun4[0] = sun_dir[0]; sun4[1] = sun_dir[1];
        sun4[2] = sun_dir[2]; sun4[3] = 0.0f;
        sky_params[0] = 3.0f;   /* mode = stylized */
        sky_params[1] = 1.0f;   /* exposure (dome is authored in linear)  */
    } else if (sr->sky_mode == JCE_SCENE_SKY_PREETHAM) {
        /* Analytic daylight: use the ToD sun direction when active, else a
         * default high sun.  Evaluate the SAME math as jce_sky_radiance(),
         * which fs_sky.sc mode 2 mirrors. */
        float sun_dir[3];
        if (sr->tod_active) {
            sun_dir[0] = sr->tod_state.sun_direction.x;
            sun_dir[1] = sr->tod_state.sun_direction.y;
            sun_dir[2] = sr->tod_state.sun_direction.z;
        } else {
            sun_dir[0] = 0.0f; sun_dir[1] = 0.9f; sun_dir[2] = 0.4359f;
        }

        JceSkyConfig sc = jce_sky_config_default();
        sc.turbidity = sr->sky_turbidity;
        JceSkyState ss = jce_sky_evaluate(&sc, sun_dir);

        /* Pack A..D per channel into perez[0..2]; E coeffs into perez[3]. */
        perez[0]  = ss.perezY[0]; perez[1]  = ss.perezY[1];
        perez[2]  = ss.perezY[2]; perez[3]  = ss.perezY[3];
        perez[4]  = ss.perezx[0]; perez[5]  = ss.perezx[1];
        perez[6]  = ss.perezx[2]; perez[7]  = ss.perezx[3];
        perez[8]  = ss.perezy[0]; perez[9]  = ss.perezy[1];
        perez[10] = ss.perezy[2]; perez[11] = ss.perezy[3];
        perez[12] = ss.perezY[4]; perez[13] = ss.perezx[4];
        perez[14] = ss.perezy[4]; perez[15] = 0.0f;

        zenith[0] = ss.Yz; zenith[1] = ss.xz;
        zenith[2] = ss.yz; zenith[3] = ss.normalize;

        sun4[0] = ss.sun_dir[0]; sun4[1] = ss.sun_dir[1];
        sun4[2] = ss.sun_dir[2]; sun4[3] = 0.0f;

        sky_params[0] = 2.0f;          /* mode = Preetham */
        sky_params[1] = ss.exposure;   /* exposure        */
    } else if (sr->skybox_active && sr->skybox) {
        JceTexture jet = jce_skybox_get_equirect_texture(sr->skybox);
        equirect_tex.idx = jet.idx;
        if (BGFX_HANDLE_IS_VALID(equirect_tex)) {
            sky_params[0] = 1.0f;
            sky_params[1] = sr->skybox_exposure;
            sky_params[2] = sr->skybox_rotation * 0.0174533f;
            bgfx_set_texture(0, sr->u_sky_equirect, equirect_tex, UINT32_MAX);
        }
    }
    bgfx_set_uniform(sr->u_sky_perez,   perez,  4);
    bgfx_set_uniform(sr->u_sky_zenith,  zenith, 1);
    bgfx_set_uniform(sr->u_sky_sun_dir, sun4,   1);
    bgfx_set_uniform(sr->u_sky_params,  sky_params, 1);

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 6);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA, 0);

    jce_mat4 identity = jce_m4_identity();
    bgfx_set_transform(identity.raw[0], 1);
    bgfx_submit(view_id, sr->prog_sky, 0, BGFX_DISCARD_ALL);
}

/* ── Cloth pass (P3-C.4: render the soft-body grid in-game) ───────────
 *
 * Builds a transient pos+normal+uv mesh from the live solver node positions
 * each frame and submits it with the simple mesh program.  Runs in the color
 * view AFTER the entity pass, so it inherits the lighting uniforms.  Cloth
 * node positions are world-space, so the model transform is identity. */

typedef struct { JceSceneRenderer *sr; uint16_t view_id; } SrClothDrawCtx;

static void sr_draw_one_cloth(JceScene *scene, JceEntity e, void *ud)
{
    SrClothDrawCtx *ctx = (SrClothDrawCtx *)ud;
    JceSceneRenderer *sr = ctx->sr;

    JceClothComponent *cl = jce_scene_get_cloth(scene, e);
    if (!cl || cl->handle == 0 || cl->res_u < 2 || cl->res_v < 2) return;
    if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_CLOTH)) return;

    const uint32_t ru = cl->res_u, rv = cl->res_v;
    const uint32_t nodes = ru * rv;
    /* uint16 transient indices cap the grid; skip oversized patches. */
    if (nodes > 65535u) return;
    if (jce_cloth_node_count((JceClothHandle)cl->handle) != nodes) return;

    float *pos = (float *)JCE_MALLOC((size_t)nodes * 3u * sizeof(float));
    if (!pos) return;
    if (!jce_cloth_get_positions((JceClothHandle)cl->handle, pos, nodes * 3u)) {
        JCE_FREE(pos);
        return;
    }

    const uint32_t quads       = (ru - 1u) * (rv - 1u);
    const uint32_t num_indices = quads * 6u;

    bgfx_vertex_layout_t layout;
    bgfx_vertex_layout_begin(&layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_POSITION,  3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_NORMAL,    3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&layout);

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &layout, nodes, &tib, num_indices, false)) {
        JCE_FREE(pos);
        return;
    }

    float *vtx = (float *)tvb.data;   /* 8 floats/vertex: pos(3) normal(3) uv(2) */
    for (uint32_t v = 0; v < rv; ++v) {
        for (uint32_t u = 0; u < ru; ++u) {
            const uint32_t i = v * ru + u;
            /* Central-difference grid normal (clamped at edges). */
            const uint32_t iu0 = (u > 0) ? i - 1u : i;
            const uint32_t iu1 = (u + 1u < ru) ? i + 1u : i;
            const uint32_t iv0 = (v > 0) ? i - ru : i;
            const uint32_t iv1 = (v + 1u < rv) ? i + ru : i;
            const float dux = pos[iu1*3+0] - pos[iu0*3+0];
            const float duy = pos[iu1*3+1] - pos[iu0*3+1];
            const float duz = pos[iu1*3+2] - pos[iu0*3+2];
            const float dvx = pos[iv1*3+0] - pos[iv0*3+0];
            const float dvy = pos[iv1*3+1] - pos[iv0*3+1];
            const float dvz = pos[iv1*3+2] - pos[iv0*3+2];
            float nx = duy*dvz - duz*dvy;
            float ny = duz*dvx - dux*dvz;
            float nz = dux*dvy - duy*dvx;
            const float len = sqrtf(nx*nx + ny*ny + nz*nz);
            if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
            else { nx = 0.0f; ny = 1.0f; nz = 0.0f; }

            float *o = vtx + (size_t)i * 8u;
            o[0] = pos[i*3+0]; o[1] = pos[i*3+1]; o[2] = pos[i*3+2];
            o[3] = nx; o[4] = ny; o[5] = nz;
            o[6] = (float)u / (float)(ru - 1u);
            o[7] = (float)v / (float)(rv - 1u);
        }
    }

    uint16_t *idx = (uint16_t *)tib.data;
    uint32_t k = 0;
    for (uint32_t v = 0; v + 1u < rv; ++v) {
        for (uint32_t u = 0; u + 1u < ru; ++u) {
            const uint16_t i00 = (uint16_t)(v * ru + u);
            const uint16_t i10 = (uint16_t)(v * ru + u + 1u);
            const uint16_t i01 = (uint16_t)((v + 1u) * ru + u);
            const uint16_t i11 = (uint16_t)((v + 1u) * ru + u + 1u);
            idx[k++] = i00; idx[k++] = i01; idx[k++] = i10;
            idx[k++] = i10; idx[k++] = i01; idx[k++] = i11;
        }
    }

    JCE_FREE(pos);

    JceUniformHandle uh = jce_renderer_get_tex_uniform(sr->renderer);
    bgfx_uniform_handle_t su = { uh.idx };
    if (BGFX_HANDLE_IS_VALID(su) && BGFX_HANDLE_IS_VALID(sr->white_tex))
        bgfx_set_texture(0, su, sr->white_tex, UINT32_MAX);

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, nodes);
    bgfx_set_transient_index_buffer(&tib, 0, num_indices);
    /* Double-sided (no cull) — a cloth sheet is visible from both faces. */
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z
                 | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA, 0);

    jce_mat4 identity = jce_m4_identity();
    bgfx_set_transform(identity.raw[0], 1);

    JceShaderHandle sh = jce_renderer_get_program_mesh(sr->renderer);
    bgfx_program_handle_t prog = { sh.idx };
    if (BGFX_HANDLE_IS_VALID(prog))
        bgfx_submit(ctx->view_id, prog, 0, BGFX_DISCARD_ALL);
}

void sr_draw_cloth(JceSceneRenderer *sr, JceScene *scene, uint16_t view_id)
{
    if (!sr || !scene) return;
    SrClothDrawCtx ctx = { sr, view_id };
    /* Component-filtered walk (O(#cloth)); O(1) all-clear gate — this ran
     * a full-entity probe per viewport per frame on cloth-free worlds. */
    if (jce_scene_count_cloth(scene) > 0)
        jce_scene_each_cloth(scene, sr_draw_one_cloth, &ctx);
}

/* ── Async IBL bake ────────────────────────────────────────────────── */

/* struct SrIblJob is defined in jce_sr_internal.h so jce_scene_renderer_destroy
 * (core) can tear down an in-flight bake while the worker/poller live here. */

static void sr_ibl_worker(void *arg)
{
    struct SrIblJob *j = (struct SrIblJob *)arg;
    j->result = jce_ibl_bake_cpu(j->pixels, j->w, j->h, j->irr, j->pf);
    JCE_FREE(j->pixels);
    j->pixels = NULL;
    j->done = 1;
}

/* MAIN thread: if the async bake finished, join it, upload to GPU and swap it
 * in — unless it is now stale (the skybox changed since the bake started). */
static void sr_ibl_poll(JceSceneRenderer *sr)
{
    if (!sr->ibl_job || !sr->ibl_job->done)
        return;
    if (sr->ibl_thread) {        /* join provides the memory barrier */
        jce_thread_join(sr->ibl_thread);
        sr->ibl_thread = NULL;
    }
    JceIblCpuData *res = sr->ibl_job->result;
    bool stale = (strcmp(sr->ibl_job_hdr, sr->skybox_hdr_path) != 0);
    JCE_FREE(sr->ibl_job);
    sr->ibl_job = NULL;

    if (!res) return;
    if (stale) { jce_ibl_cpu_free(res); return; }

    if (sr->ibl_data) { jce_ibl_destroy(sr->ibl_data); sr->ibl_data = NULL; }
    sr->ibl_data = jce_ibl_upload_cpu(res);   /* MAIN-thread GPU upload */
    if (sr->ibl_data)
        LOG_INFO(LOG_TAG, "IBL ready (async): %s", sr->skybox_hdr_path);
}

/* Start an async IBL bake from the skybox's decoded equirect pixels. The pixels
 * are copied so the worker is independent of the skybox lifetime. Falls back to
 * a synchronous bake if the worker thread cannot be created. */
static void sr_ibl_start_async(JceSceneRenderer *sr, const char *hdr_path)
{
    uint32_t eqw = 0, eqh = 0;
    const float *eqpx = jce_skybox_get_equirect_pixels(sr->skybox, &eqw, &eqh);
    if (!eqpx || eqw == 0 || eqh == 0)
        return;

    /* Keep to one in-flight job: if a previous bake is still running (rapid
     * skybox swap), wait it out and discard its result. */
    if (sr->ibl_thread) {
        jce_thread_join(sr->ibl_thread);
        sr->ibl_thread = NULL;
    }
    if (sr->ibl_job) {
        if (sr->ibl_job->result) jce_ibl_cpu_free(sr->ibl_job->result);
        JCE_FREE(sr->ibl_job);
        sr->ibl_job = NULL;
    }

    size_t bytes = (size_t)eqw * (size_t)eqh * 4u * sizeof(float);
    struct SrIblJob *job = (struct SrIblJob *)JCE_CALLOC(1, sizeof(*job));
    float *copy = (float *)JCE_MALLOC(bytes);
    if (!job || !copy) {
        JCE_FREE(job);
        JCE_FREE(copy);
        sr->ibl_data = jce_ibl_generate_from_pixels(eqpx, eqw, eqh, 32, 128);
        return;
    }
    memcpy(copy, eqpx, bytes);
    job->pixels = copy;
    job->w = eqw; job->h = eqh; job->irr = 32; job->pf = 128;

    snprintf(sr->ibl_job_hdr, sizeof(sr->ibl_job_hdr), "%s", hdr_path);
    sr->ibl_job = job;
    sr->ibl_thread = jce_thread_create(sr_ibl_worker, job, "ibl_bake");
    if (!sr->ibl_thread) {
        /* No thread: run on this (main) thread, then finalize immediately. */
        sr_ibl_worker(job);
        sr_ibl_poll(sr);
    }
}

typedef struct SrSkyboxScanCtx {
    JceScene   *scene;
    const char *hdr_path;
    float       rotation;
    float       exposure;
} SrSkyboxScanCtx;

static void sr_skybox_scan_cb(JceScene *s, JceEntity e, void *ud)
{
    SrSkyboxScanCtx *c = (SrSkyboxScanCtx *)ud;
    if (c->hdr_path) return;                    /* first hit wins */
    if (!entity_enabled(s, e)) return;
    JceSkyboxComponent *sb = jce_scene_get_skybox(s, e);
    if (!sb || sb->hdr_path[0] == '\0') return;
    c->hdr_path = sb->hdr_path;
    c->rotation = sb->rotation;
    c->exposure = sb->exposure > 0.0f ? sb->exposure : 1.0f;
}

void sr_scan_skybox(JceSceneRenderer *sr, JceScene *scene, EntityList *list)
{
    const char *hdr_path = NULL;
    float rotation = 0.0f;
    float exposure = 1.0f;

    /* Pick up a finished async IBL bake (if any) before anything else. */
    sr_ibl_poll(sr);

    /* Component-filtered pick (O(#skyboxes), typically 0-1) — this walked
     * the FULL collect list probing has_skybox per viewport per frame
     * (150k probes/frame on a skybox-free stress world).  With one skybox
     * the picked entity is identical; with several the "first" was already
     * list-order-unspecified across streaming rebuilds. */
    SrSkyboxScanCtx sctx = { scene, NULL, 0.0f, 1.0f };
    jce_scene_each_skybox(scene, sr_skybox_scan_cb, &sctx);
    hdr_path = sctx.hdr_path;
    rotation = sctx.rotation;
    exposure = sctx.exposure;
    (void)list;

    if (hdr_path && strcmp(hdr_path, sr->skybox_hdr_path) != 0) {
        if (sr->ibl_data) { jce_ibl_destroy(sr->ibl_data); sr->ibl_data = NULL; }
        if (sr->skybox)   { jce_skybox_destroy(sr->skybox); sr->skybox = NULL; }
        /* Try PAK chain first (engine + bundle overlays) so a bundled
         * HDR works without a sidecar file on disk.  Fall back to the
         * host filesystem for user-authored / loose HDRs. */
        const JcePakAsset *hdr_asset = jce_pak_find(sr->pak, hdr_path);
        if (hdr_asset && hdr_asset->original_size > 0) {
            void *hdr_buf = JCE_MALLOC((size_t)hdr_asset->original_size);
            if (hdr_buf) {
                size_t got = jce_pak_decompress_ex(sr->pak, hdr_asset,
                                                  hdr_buf,
                                                  (size_t)hdr_asset->original_size);
                if (got == (size_t)hdr_asset->original_size)
                    sr->skybox = jce_skybox_create_from_hdr_memory(
                                     hdr_buf, (uint32_t)got, 512);
                JCE_FREE(hdr_buf);
            }
        }
        if (!sr->skybox) {
            /* Not in the PAK: resolve to a host-openable path (editor / loose
             * files) the same way meshes and terrain do, then load from disk.
             * Without this the raw scene-relative path is opened relative to the
             * editor CWD and fails ("failed to open HDR file"). */
            char        resolved[1024];
            const char *load_path = hdr_path;
            if (sr->has_cbs && sr->cbs.resolve_path &&
                sr->cbs.resolve_path(hdr_path, resolved, (int)sizeof(resolved),
                                     sr->cbs.userdata)) {
                load_path = resolved;
            }
            sr->skybox = jce_skybox_create_from_hdr_file(load_path, 512);
        }
        if (sr->skybox) {
            snprintf(sr->skybox_hdr_path, sizeof(sr->skybox_hdr_path),
                     "%s", hdr_path);
            sr->skybox_active = true;
            /* The sky is already visible; kick the IBL convolution onto a
             * worker thread and swap the cubemaps in via sr_ibl_poll() when
             * ready (fallback ambient until then). A cache hit finishes in ~1
             * frame; a cold bake no longer freezes the main thread. */
            sr_ibl_start_async(sr, hdr_path);
            LOG_INFO(LOG_TAG, "skybox loaded (IBL baking async): %s", hdr_path);
        } else {
            /* Remember the FAILED path (do NOT clear it) so the load is not
             * re-attempted — and re-logged — every frame. It is retried only
             * if the scene's hdr_path actually changes. */
            snprintf(sr->skybox_hdr_path, sizeof(sr->skybox_hdr_path),
                     "%s", hdr_path);
            sr->skybox_active = false;
            LOG_WARN(LOG_TAG, "skybox HDR load failed: %s (will not retry)",
                     hdr_path);
        }
    } else if (!hdr_path && sr->skybox_active) {
        if (sr->ibl_data) { jce_ibl_destroy(sr->ibl_data); sr->ibl_data = NULL; }
        if (sr->skybox)   { jce_skybox_destroy(sr->skybox); sr->skybox = NULL; }
        sr->skybox_hdr_path[0] = '\0';
        sr->skybox_active = false;
    }

    sr->skybox_exposure = exposure;
    sr->skybox_rotation = rotation;
}

/* ── Time-of-day / weather / decals (P2-weather-decals-tod) ───────────
 *
 * These three authored environment systems were built (jce_time_of_day.c,
 * jce_weather.c, jce_decals.c) but never created or driven outside smoke
 * tests.  They are wired here, off the scene's serialized rendering
 * settings + JceDecalComponent, so a scene that authors them sees them
 * update and render every frame in both the editor preview and runtime.
 */

/* Advance the internal day clock and push the evaluated lighting snapshot
 * into the renderer's ToD override (consumed by the sky + lighting passes
 * already wired to sr->tod_state). */
void sr_drive_time_of_day(JceSceneRenderer *sr,
                                 const JceSceneRenderingSettings *rs,
                                 float dt_sec)
{
    if (!sr || !rs) return;

    if (!rs->tod_enabled) {
        /* Disabled this frame: release any time-of-day override, including
         * legacy editor preview state left behind before ToD became
         * scene-owned. */
        if (sr->tod_active) {
            jce_scene_renderer_set_time_of_day(sr, NULL);
            sr->tod_driven    = false;
            sr->tod_clock_valid = false;
        }
        return;
    }

    float authored_hour = rs->tod_hour;
    while (authored_hour >= 24.0f) authored_hour -= 24.0f;
    while (authored_hour < 0.0f)   authored_hour += 24.0f;

    /* (Re)seed from authored hour whenever the scene data changes.  The
     * editor advances tod_hour directly for visible previews; runtime keeps
     * the authored hour stable, so this branch prevents double-advance in
     * editor while preserving renderer-owned runtime progression. */
    bool authored_changed =
        !sr->tod_clock_valid ||
        fabsf(authored_hour - sr->tod_authored_hour) > 0.0001f ||
        rs->tod_speed <= 0.0f;
    if (authored_changed) {
        sr->tod_clock_hour    = authored_hour;
        sr->tod_authored_hour = authored_hour;
        sr->tod_clock_valid   = true;
    }
    if (!authored_changed && rs->tod_speed > 0.0f && dt_sec > 0.0f) {
        sr->tod_clock_hour += rs->tod_speed * dt_sec;
        while (sr->tod_clock_hour >= 24.0f) sr->tod_clock_hour -= 24.0f;
        while (sr->tod_clock_hour < 0.0f)   sr->tod_clock_hour += 24.0f;
    }

    JceTimeOfDayConfig cfg = jce_time_of_day_default_config();
    cfg.latitude_degrees = rs->tod_latitude;
    cfg.dawn_hour        = rs->tod_dawn_hour;
    cfg.dusk_hour        = rs->tod_dusk_hour;

    JceTimeOfDayState state;
    jce_time_of_day_evaluate(&cfg, sr->tod_clock_hour, &state);
    jce_scene_renderer_set_time_of_day(sr, &state);
    sr->tod_driven = true;
}

/* Lazily create the screen-space weather overlay, sync its state from the
 * authored settings, advance its animation clock and render it on top of the
 * scene color view. */
void sr_drive_weather(JceSceneRenderer *sr,
                             const JceSceneRenderingSettings *rs,
                             uint16_t view_id, float dt_sec)
{
    if (!sr || !rs || !sr->pak) return;

    JceWeatherType type = (JceWeatherType)rs->weather_type;
    if (type == JCE_WEATHER_CLEAR || rs->weather_intensity <= 0.0f) {
        /* Nothing to draw; leave any existing system idle (cheap). */
        if (sr->weather) {
            JceWeatherState clear = jce_weather_default(JCE_WEATHER_CLEAR, 0.0f);
            jce_weather_set_state(sr->weather, &clear);
        }
        return;
    }

    if (!sr->weather) {
        JceWeatherDesc d = { sr->pak };
        sr->weather = jce_weather_create(&d);
        if (!sr->weather) return;   /* shader missing — fail soft */
    }

    JceWeatherState st = jce_weather_default(type, rs->weather_intensity);
    jce_weather_set_state(sr->weather, &st);
    jce_weather_update(sr->weather, dt_sec);
    jce_weather_render(sr->weather, view_id);
}

/* Rebuild the authored-decal pool each frame from JceDecalComponent
 * projectors so transform / colour edits update live, then render both the
 * authored and the runtime-stamped pools into the color view. */
typedef struct { JceSceneRenderer *sr; JceScene *scene; uint32_t spawned; } SrDecalEachCtx;

static void sr_decal_each_entity(JceScene *scene, JceEntity e, void *ud)
{
    SrDecalEachCtx *ctx = (SrDecalEachCtx *)ud;
    JceSceneRenderer *sr = ctx->sr;
    JceDecalComponent *dc = jce_scene_get_decal(scene, e);
    if (!dc) return;
    if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_DECAL)) return;
    if (dc->opacity <= 0.0f) return;

    /* World transform of the projector entity. */
    jce_mat4 w = jce_scene_get_world_matrix(scene, e);
    jce_vec3 pos = jce_v3(w.raw[3][0], w.raw[3][1], w.raw[3][2]);

    /* Projector points DOWN its local -Y by convention (Unity/HDRP decal);
     * the surface normal we stamp is that projection axis.  Columns of the
     * world matrix are the rotated local axes. */
    jce_vec3 down = jce_v3(-w.raw[1][0], -w.raw[1][1], -w.raw[1][2]);
    jce_vec3 right = jce_v3(w.raw[0][0], w.raw[0][1], w.raw[0][2]);

    /* Offset the stamp to the projector's far face so it sits on the surface
     * below the pivot rather than floating at the box centre. */
    float depth = dc->size[2] > 0.0f ? dc->size[2] : 1.0f;
    jce_vec3 hit = jce_v3(pos.x + down.x * depth * 0.5f + dc->pivot[0],
                          pos.y + down.y * depth * 0.5f + dc->pivot[1],
                          pos.z + down.z * depth * 0.5f + dc->pivot[2]);

    JceDecalSpawn s;
    memset(&s, 0, sizeof s);
    s.position     = hit;
    s.normal       = jce_v3(-down.x, -down.y, -down.z);  /* face toward projector */
    s.tangent_hint = right;
    float sx = dc->size[0] > 0.0f ? dc->size[0] : 1.0f;
    float sy = dc->size[1] > 0.0f ? dc->size[1] : sx;
    s.size      = (sx > sy ? sx : sy);
    s.thickness = 0.01f;
    s.texture   = sr_resolve_texture(sr, dc->material_path);
    s.tint      = jce_v4(dc->color[0], dc->color[1], dc->color[2],
                         dc->color[3] * dc->opacity);
    s.lifetime_seconds = 0.0f;   /* authored projectors are persistent */

    if (jce_decals_spawn(sr->decals_authored, &s))
        ctx->spawned++;
}

void sr_drive_decals(JceSceneRenderer *sr, JceScene *scene,
                            uint16_t view_id, float dt_sec)
{
    if (!sr || !scene || !sr->pak) return;

    /* Rebuild the authored projector pool from scratch this frame. */
    if (!sr->decals_authored) {
        JceDecalPoolDesc d = { 256u, sr->pak };
        sr->decals_authored = jce_decals_create(&d);
        if (!sr->decals_authored) return;   /* shader missing — fail soft */
    }
    jce_decals_clear(sr->decals_authored);

    SrDecalEachCtx ctx = { sr, scene, 0 };
    /* Component-filtered walk (O(#decals)); O(1) all-clear gate. */
    if (jce_scene_count_decals(scene) > 0)
        jce_scene_each_decal(scene, sr_decal_each_entity, &ctx);
    if (ctx.spawned > 0)
        jce_decals_render(sr->decals_authored, view_id);

    /* Runtime-stamped decals (created on demand by the public spawn API). */
    if (sr->decals) {
        jce_decals_update(sr->decals, dt_sec);
        jce_decals_render(sr->decals, view_id);
    }
}

/* ── Foliage Cluster (stylized billboard canopy / bush) ───────────────────
 *
 * The hand-painted-diorama foliage model (reference: Elemental-Serenity's
 * BushManager): N camera-facing billboard cards sampled on a squashed
 * sphere shell around the entity, each carrying its OUTWARD shell normal.
 * vs_foliage billboards + wind-sways the cards; fs_foliage alpha-masks the
 * leaf shape and shades a 3-tone toon ramp (shadow/mid/highlight x
 * multiplier) keyed on that normal vs the primary light — which reads as a
 * shaded leafy BALL instead of flat cards.  One instanced submit per
 * cluster; instance data cached per entity (rebuilt on param change). */

static uint32_t sr_foliage_hash(const JceFoliageClusterComponent *c,
                                const jce_vec3 *origin)
{
    uint32_t h = jce_fnv1a32_append(JCE_FNV1A32_INIT, &c->leaf_count,
                                    sizeof c->leaf_count);
    h = jce_fnv1a32_append(h, &c->radius,     sizeof c->radius);
    h = jce_fnv1a32_append(h, &c->squash_y,   sizeof c->squash_y);
    h = jce_fnv1a32_append(h, &c->leaf_scale, sizeof c->leaf_scale);
    h = jce_fnv1a32_append(h, &c->seed,       sizeof c->seed);
    h = jce_fnv1a32_append(h, origin,         sizeof *origin);
    return h;
}

/* mulberry32-style deterministic RNG (matches the reference generator). */
static float sr_foliage_rand(uint32_t *state)
{
    uint32_t t = (*state += 0x6d2b79f5u);
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return (float)((t ^ (t >> 14)) & 0xFFFFFFu) / 16777216.0f;
}

static void sr_foliage_lazy_init(JceSceneRenderer *sr)
{
    if (!sr->fcluster_prog_tried) {
        sr->fcluster_prog_tried = true;
        JceShaderHandle fh = shader_load_program(sr->pak, "foliage");
        sr->prog_fcluster.idx = fh.idx;
        if (fh.idx == UINT16_MAX)
            LOG_WARN(LOG_TAG, "foliage shader not found in PAK "
                              "(foliage clusters will not render)");
        /* Depth-only pairing (vs_foliage_shadow + fs_foliage_shadow): REAL
         * leaf shadows - alpha-tested cards, wind-animated like the color
         * pass (the reference's discard-below-0.8 depth material).  The
         * DEDICATED vs orients cards tangent to the shell instead of
         * billboarding: in a cascade view vs_foliage's u_invView is the
         * LIGHT's inverse view, so every card faced the light coherently
         * and the union cast one solid straight band across the grass at
         * grazing angles.  Missing program -> sphere-proxy fallback. */
        JceShaderHandle sh = shader_load_program_named(sr->pak,
                                                       "foliage_shadow",
                                                       "foliage_shadow");
        sr->prog_fcluster_shadow.idx = sh.idx;
        if (sh.idx == UINT16_MAX)
            LOG_WARN(LOG_TAG, "foliage_shadow shader not found in PAK "
                              "(leaf shadows fall back to sphere proxies)");
    }
    if (!BGFX_HANDLE_IS_VALID(sr->u_foliage_time))
        sr->u_foliage_time = bgfx_create_uniform("u_foliage_time",
                                                 BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->u_foliage_colors))
        sr->u_foliage_colors = bgfx_create_uniform("u_foliage_colors",
                                                   BGFX_UNIFORM_TYPE_VEC4, 4);
    if (!BGFX_HANDLE_IS_VALID(sr->u_foliage_light))
        sr->u_foliage_light = bgfx_create_uniform("u_foliage_light",
                                                  BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->s_foliage_alpha))
        sr->s_foliage_alpha = bgfx_create_uniform("s_foliageAlpha",
                                                  BGFX_UNIFORM_TYPE_SAMPLER, 1);
    if (!sr->fcluster_quad) {
        /* 1x1 card centered at origin in the XY plane; uv 0..1. */
        JceMeshVertex v[4];
        memset(v, 0, sizeof v);
        static const float c[4][2] = { {-0.5f,-0.5f}, {0.5f,-0.5f},
                                       {0.5f,0.5f},  {-0.5f,0.5f} };
        static const float uv[4][2] = { {0,1}, {1,1}, {1,0}, {0,0} };
        for (int i = 0; i < 4; i++) {
            v[i].pos[0] = c[i][0]; v[i].pos[1] = c[i][1]; v[i].pos[2] = 0.0f;
            v[i].normal[2] = 1.0f;
            v[i].uv[0] = uv[i][0]; v[i].uv[1] = uv[i][1];
        }
        uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
        sr->fcluster_quad = jce_mesh_create(v, 4, idx, 6);
    }
    if (!sr->fcluster_shadow_sphere)
        sr->fcluster_shadow_sphere = jce_mesh_create_sphere(0.5f);
}

/* Submit a solid sphere-proxy depth for a FoliageCluster into a shadow view
 * (soft round tree/bush shadow on the ground).  Returns true if the entity is
 * a foliage cluster (handled).  Pushes into the shared render queue so it
 * batches with the mesh shadows. */
static int sr_fcluster_ensure_cache(JceSceneRenderer *sr, JceScene *scene,
                                    JceEntity e,
                                    const JceFoliageClusterComponent *fc);

bool sr_try_submit_foliage_shadow(JceSceneRenderer *sr, JceScene *scene,
                                  JceEntity e, uint16_t view_id,
                                  uint16_t shadow_inst_idx, uint16_t shadow_idx)
{
    if (!jce_scene_has_foliage_cluster(scene, e)) return false;
    JceFoliageClusterComponent *fc = jce_scene_get_foliage_cluster(scene, e);
    if (!fc || !fc->visible) return true;
    sr_foliage_lazy_init(sr);

    /* REAL leaf shadows: the cluster's own alpha-tested cards, wind and
     * all, submitted into this cascade view (mirrors the reference's
     * custom depth material).  vs_foliage billboards against the CASCADE
     * view's u_invView, so cards face the light - full silhouettes. */
    if (BGFX_HANDLE_IS_VALID(sr->prog_fcluster_shadow) && sr->fcluster_quad &&
        fc->leaf_count > 0 && jce_scene_has_transform(scene, e)) {
        int slot = sr_fcluster_ensure_cache(sr, scene, e, fc);
        uint32_t n = (slot >= 0) ? sr->fcluster_cache[slot].count : 0u;
        const uint16_t stride = 32;
        if (n && bgfx_get_avail_instance_data_buffer(n, stride) >= n) {
            bgfx_instance_data_buffer_t idb;
            bgfx_alloc_instance_data_buffer(&idb, n, stride);
            memcpy(idb.data, sr->fcluster_cache[slot].inst,
                   (size_t)n * stride);

            float tm[4] = { sr->water_time, 0.0f, 0.0f, 0.0f };
            bgfx_set_uniform(sr->u_foliage_time, tm, 1);

            JceTexture at = { UINT16_MAX };
            if (fc->alpha_tex[0]) at = sr_resolve_texture(sr, fc->alpha_tex);
            bgfx_texture_handle_t ath = { at.idx };
            if (!BGFX_HANDLE_IS_VALID(ath)) ath = sr->white_tex;
            bgfx_set_texture(0, sr->s_foliage_alpha, ath, UINT32_MAX);

            bgfx_vertex_buffer_handle_t vbh =
                { (uint16_t)jce_mesh_get_vbh(sr->fcluster_quad) };
            bgfx_index_buffer_handle_t  ibh =
                { (uint16_t)jce_mesh_get_ibh(sr->fcluster_quad) };
            if (BGFX_HANDLE_IS_VALID(vbh) && BGFX_HANDLE_IS_VALID(ibh)) {
                float ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
                bgfx_set_transform(ident, 1);
                bgfx_set_vertex_buffer(0, vbh, 0, 4);
                bgfx_set_index_buffer(ibh, 0, 6);
                bgfx_set_instance_data_buffer(&idb, 0, n);
                /* No face cull: wind sway can flip a billboard edge-on. */
                bgfx_set_state(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                              | BGFX_STATE_MSAA, 0);
                bgfx_submit(view_id, sr->prog_fcluster_shadow, 0,
                            BGFX_DISCARD_ALL);
                return true;
            }
        }
    }

    /* Fallback: soft sphere-proxy blob (shadow program unavailable). */
    if (!sr->fcluster_shadow_sphere) return true;

    jce_mat4 wm = jce_scene_get_world_matrix(scene, e);
    jce_vec3 o  = jce_v3(wm.col[3].x, wm.col[3].y, wm.col[3].z);
    float r  = fc->radius > 0.0f ? fc->radius : 1.0f;
    float sy = fc->squash_y > 0.0f ? fc->squash_y : 1.0f;
    /* sphere mesh is radius 0.5 -> scale by 2*radius; 0.85 keeps the shadow a
     * touch inside the visual canopy so it reads dappled, not a hard disc. */
    float k = 1.7f;
    float dx = r * k, dz = r * k, dyv = r * sy * k;

    JceMesh *m = sr->fcluster_shadow_sphere;
    JceDrawCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    jce_mat4 mm = jce_m4_identity();
    mm.col[0] = jce_v4(dx, 0.0f, 0.0f, 0.0f);
    mm.col[1] = jce_v4(0.0f, dyv, 0.0f, 0.0f);
    mm.col[2] = jce_v4(0.0f, 0.0f, dz, 0.0f);
    mm.col[3] = jce_v4(o.x, o.y, o.z, 1.0f);
    cmd.view_id        = view_id;
    cmd.program        = shadow_inst_idx;
    cmd.program_single = shadow_idx;
    cmd.mesh_vbh       = jce_mesh_get_vbh(m);
    cmd.mesh_ibh       = jce_mesh_get_ibh(m);
    cmd.index_count    = jce_mesh_index_count(m);
    cmd.transform      = mm;
    cmd.depth          = 0.0f;
    cmd.material_key   = 1u;
    cmd.state          = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                       | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA;
    jce_rq_push(sr->render_queue, &cmd);
    return true;
}

/* 千万 ③ scatter shadows: submit every shadow-casting VegetationScatter as ONE
 * depth-only instanced draw per cascade view, at a REDUCED LOD (default: the
 * model's coarsest level; env JCE_FOLIAGE_SHADOW_LOD=<n> picks level n, =-1
 * forces base/LOD0) — a 64k-instance field casts believable shadows for a few
 * percent of its full-detail geometry cost.  Instance matrices ride the
 * scatter's persistent COMPUTE_READ roots VB (doubles as plain instance data =
 * zero per-frame CPU copies), so this requires the persist path (HIGH tier /
 * cull enabled); scatters without a resident inst_vb are skipped (LOW/MED
 * degrade to no scatter shadows — the color pass is unaffected).  Called from
 * the CSM cascade loop OUTSIDE the per-entity walk: scatter entities carry no
 * ecull caster AABB, so the whole field submits to every rendered cascade
 * (cascade-level caster culling for scatter = follow-up). */
void sr_submit_scatter_shadows(JceSceneRenderer *sr, JceScene *scene,
                               uint16_t cv, uint16_t shadow_inst_idx)
{
    if (!scene || shadow_inst_idx == UINT16_MAX) return;
    static int s_slod = -2;   /* -2 unparsed; -1 LOD0; >=0 explicit level; INT_MAX coarsest */
    if (s_slod == -2) {
        const char *v = getenv("JCE_FOLIAGE_SHADOW_LOD");
        s_slod = (v && v[0]) ? atoi(v) : 0x7fffffff;   /* default: coarsest */
    }
    const uint32_t level = (s_slod < 0) ? 0u : (uint32_t)s_slod;

    for (int slot = 0; slot < (int)(sizeof sr->foliage_cache /
                                    sizeof sr->foliage_cache[0]); ++slot) {
        if (!sr->foliage_cache[slot].used) continue;
        JceEntity e = sr->foliage_cache[slot].entity;
        if (!jce_scene_has_vegetation_scatter(scene, e)) continue;
        JceVegetationScatterComponent *vs = jce_scene_get_vegetation_scatter(scene, e);
        if (!vs || !vs->cast_shadow || !vs->visible) continue;

        /* 千万 S5 tiled scatter: one instanced depth draw per RESIDENT tile
         * (the streamed visible neighbourhood is exactly the set that can
         * plausibly shadow the view). */
        if (sr->foliage_cache[slot].tiles && vs->mesh_path[0]) {
            SrModelCache *mc = sr_get_model(sr, vs->mesh_path, (uint32_t)e);
            if (!mc || !mc->model) continue;
            int nt = sr->foliage_cache[slot].tiles_x * sr->foliage_cache[slot].tiles_z;
            for (int t = 0; t < nt; ++t) {
                struct SrFoliageTile *tile = &sr->foliage_cache[slot].tiles[t];
                if (!tile->resident || tile->count == 0) continue;
                jce_model_draw_shadow_instanced_lod(mc->model, sr->renderer, cv,
                    shadow_inst_idx, tile->vb.idx, tile->count, level);
            }
            continue;
        }

        uint32_t n = sr->foliage_cache[slot].inst_count;
        if (n == 0 || sr->foliage_cache[slot].inst_vb_count != n) continue;

        if (vs->mesh_path[0]) {
            SrModelCache *mc = sr_get_model(sr, vs->mesh_path, (uint32_t)e);
            if (!mc || !mc->model) continue;
            jce_model_draw_shadow_instanced_lod(mc->model, sr->renderer, cv,
                shadow_inst_idx, sr->foliage_cache[slot].inst_vb.idx, n, level);
        } else {
            /* Primitive scatter (cube/sphere/...): already low-poly — depth
             * submit of the shared primitive mesh, same zero-copy instances. */
            JceMesh *pm = NULL;
            switch (vs->mesh_shape) {
                default:
                case 0: pm = sr->cube_mesh;     break;
                case 1: pm = sr->sphere_mesh;   break;
                case 2: pm = sr->plane_mesh;    break;
                case 3: pm = sr->capsule_mesh;  break;
                case 4: pm = sr->cylinder_mesh; break;
            }
            if (!pm) continue;
            bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(pm) };
            bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(pm) };
            bgfx_program_handle_t       prg = { shadow_inst_idx };
            bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
            if (ibh.idx != UINT16_MAX)
                bgfx_set_index_buffer(ibh, 0, jce_mesh_index_count(pm));
            bgfx_set_instance_data_from_dynamic_vertex_buffer(
                sr->foliage_cache[slot].inst_vb, 0, n);
            bgfx_set_state(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                         | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA, 0);
            bgfx_submit(cv, prg, 0, BGFX_DISCARD_ALL);
        }
    }
}

/* (renamed from sr_foliage_find_slot — that name already belongs to the
 * VegetationScatter cache at the top of this file; this variant probes the
 * separate fcluster_cache.) */
static int sr_fcluster_find_slot(JceSceneRenderer *sr, JceEntity e)
{
    int n = (int)(sizeof sr->fcluster_cache / sizeof sr->fcluster_cache[0]);
    int free_slot = -1;
    for (int i = 0; i < n; i++) {
        if (sr->fcluster_cache[i].used && sr->fcluster_cache[i].entity == e)
            return i;
        if (!sr->fcluster_cache[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) return free_slot;
    /* All slots taken: evict a deterministic one (rebuilds next time).
     * Without this, a table full of stale entries — e.g. entity ids
     * renumbered by an editor undo (scene world recreate) — made every
     * new cluster return -1 and silently skip drawing forever. */
    static bool warned = false;
    if (!warned) {
        warned = true;
        LOG_WARN(LOG_TAG,
                 "fcluster cache full (%d slots) — evicting; stale entries "
                 "suggest a scene swap without jce_scene_renderer_reset_entity_caches()",
                 n);
    }
    int victim = (int)((uint32_t)e % (uint32_t)n);
    JCE_FREE(sr->fcluster_cache[victim].inst);
    memset(&sr->fcluster_cache[victim], 0, sizeof sr->fcluster_cache[victim]);
    return victim;
}

/* Drop every entity-keyed environment cache (VegetationScatter foliage,
 * grass fields, water surfaces, foliage-cluster canopies).  Call after any
 * operation that renumbers entity ids while keeping this renderer alive —
 * editor undo/redo and scene switches destroy + recreate the ECS world, so
 * recycled ids come back with bumped generation bits and every cached slot
 * goes permanently stale.  Stale slots squat their fixed-size tables (a
 * full fcluster table used to silently skip drawing new clusters) and pin
 * dead CPU/GPU buffers.  Content is rebuilt lazily on the next draw. */
void jce_scene_renderer_reset_entity_caches(JceSceneRenderer *sr)
{
    if (!sr) return;
    int dropped = 0;
    for (int i = 0; i < (int)(sizeof sr->foliage_cache /
                              sizeof sr->foliage_cache[0]); ++i)
        if (sr->foliage_cache[i].used) { sr_foliage_slot_free(sr, i); ++dropped; }
    for (int i = 0; i < (int)(sizeof sr->grass_cache /
                              sizeof sr->grass_cache[0]); ++i)
        if (sr->grass_cache[i].used) { sr_grass_slot_free(sr, i); ++dropped; }
    for (int i = 0; i < SR_WATER_SLOT_MAX; ++i)
        if (sr->water_cache[i].used) { sr_water_slot_free(sr, i); ++dropped; }
    for (int i = 0; i < (int)(sizeof sr->fcluster_cache /
                              sizeof sr->fcluster_cache[0]); ++i)
        if (sr->fcluster_cache[i].used) {
            JCE_FREE(sr->fcluster_cache[i].inst);
            memset(&sr->fcluster_cache[i], 0, sizeof sr->fcluster_cache[i]);
            ++dropped;
        }
    /* Nanite-lite meshlet indirects are ENTITY-keyed too: a recreated world
     * replays entity ids, so a stale slot would alias a fresh entity and
     * draw last world's cluster args.  Same seam, same rule: drop them all. */
    for (int i = 0; i < (int)(sizeof sr->meshlet_cache /
                              sizeof sr->meshlet_cache[0]); ++i)
        if (sr->meshlet_cache[i].used) {
            bgfx_destroy_indirect_buffer(sr->meshlet_cache[i].indirect);
            for (int mc = 0; mc < JCE_CSM_MAX_CASCADES; ++mc)  /* V4 shadow */
                if (sr->meshlet_cache[i].shadow_indirect[mc].idx != UINT16_MAX)
                    bgfx_destroy_indirect_buffer(sr->meshlet_cache[i].shadow_indirect[mc]);
            sr->meshlet_cache[i].used = false;
            ++dropped;
        }

    /* Frame-invariance caches.  A recreated scene replays its counter
     * sequences from scratch (structural_epoch / xform_counter / per-entity
     * xform gens restart with the new world), so equality-keyed snapshots
     * taken before the swap can FALSE-HIT after it once the deterministic
     * reload drives the counters back to a previously seen value: the frozen
     * ecull then replays draw commands built with pre-swap world matrices
     * (a mesh stays at its old position after redo) and the CSM cascade
     * cache skips re-rendering (a shadow stays at the old position after
     * undo).  Every snapshot below rebuilds in one frame — drop them all. */
    sr->ecull_frz_valid = false;          /* frozen-frame ecull replay */
    sr->eidx_list_gen   = 0;              /* entity→ecull-slot index */
    if (sr->wcache && sr->wcache_cap) {   /* per-entity world/AABB/kind cache
                                           * (embeds lever-③ draw cmds) */
        memset(sr->wcache, 0,
               (size_t)sr->wcache_cap * sizeof sr->wcache[0]);
        sr->wcache_count = 0;
    }
    for (int c = 0; c < JCE_CSM_MAX_CASCADES; ++c)
        sr->shadow_cache_valid[c] = false;   /* CSM cascade skip-cache */
    sr->shadow_cache_cascades = 0;           /* poison the global-clean key */
    sr->shadow_space_valid    = false;       /* CSM caster-space fit cache */

    if (dropped)
        LOG_INFO(LOG_TAG, "entity caches reset (scene swap): %d slots dropped",
                 dropped);
}

/* Fill (or reuse) the per-entity leaf-card instance cache; returns the
 * slot or -1.  Shared by the color pass AND the CSM shadow pass - the
 * shadow views are submitted earlier in the frame, so the shadow path
 * must be able to build the cache before the color pass ever ran. */
static int sr_fcluster_ensure_cache(JceSceneRenderer *sr, JceScene *scene,
                                    JceEntity e,
                                    const JceFoliageClusterComponent *fc)
{
    int slot = sr_fcluster_find_slot(sr, e);
    if (slot < 0) return -1;

    jce_mat4 wm     = jce_scene_get_world_matrix(scene, e);
    jce_vec3 origin = jce_v3(wm.col[3].x, wm.col[3].y, wm.col[3].z);

    int count = fc->leaf_count;
    if (count > 256) count = 256;
    /* LOW-tier floor: canopy cards are the scene's biggest ALPHA-TEST
     * overdraw source (hundreds of screen-covering discard billboards, worst
     * case several fullscreen relayers on an iGPU).  Tier LOW keeps ~2/3 of
     * the cards — the shell stays visually closed because the scatter is
     * shell-uniform — HIGH/ULTRA draw the authored count. */
    {
        static int s_low = -1;
        if (s_low < 0)
            s_low = (jce_renderer_get_tier() <= JCE_GPU_TIER_LOW) ? 1 : 0;
        if (s_low) { count = (count * 2) / 3; if (count < 4) count = 4; }
    }

    const uint32_t ph = sr_foliage_hash(fc, &origin);
    if (!sr->fcluster_cache[slot].used ||
        sr->fcluster_cache[slot].entity != e ||
        sr->fcluster_cache[slot].param_hash != ph) {
        const uint32_t need = (uint32_t)count * 8u;
        if (sr->fcluster_cache[slot].inst_cap < need) {
            float *nb = (float *)JCE_REALLOC(sr->fcluster_cache[slot].inst,
                                             need * sizeof(float));
            if (!nb) return -1;
            sr->fcluster_cache[slot].inst     = nb;
            sr->fcluster_cache[slot].inst_cap = need;
        }
        uint32_t rng = fc->seed ? fc->seed : 1u;
        float *out = sr->fcluster_cache[slot].inst;
        for (int i = 0; i < count; i++) {
            /* Uniform direction on the sphere. */
            float u  = sr_foliage_rand(&rng) * 2.0f - 1.0f;   /* cos(theta) */
            float az = sr_foliage_rand(&rng) * 6.2831853f;
            float sq = sqrtf(1.0f - u * u);
            float nx = sq * cosf(az), ny = u, nz = sq * sinf(az);
            /* Shell radius jitter (85..100%) + vertical squash. */
            float rr = fc->radius * (0.85f + 0.15f * sr_foliage_rand(&rng));
            float px = origin.x + nx * rr;
            float py = origin.y + ny * rr * fc->squash_y;
            float pz = origin.z + nz * rr;
            /* Card size: base + up to +50% jitter (reference: rand*0.5+scale). */
            float s   = fc->leaf_scale + sr_foliage_rand(&rng) * 0.5f;
            float wph = sr_foliage_rand(&rng) * 6.2831853f;   /* wind phase */
            out[i * 8 + 0] = px;  out[i * 8 + 1] = py;
            out[i * 8 + 2] = pz;  out[i * 8 + 3] = s;
            /* Squash the shading normal too, then renormalize. */
            float sy = ny / (fc->squash_y > 0.05f ? fc->squash_y : 1.0f);
            float nl = sqrtf(nx * nx + sy * sy + nz * nz);
            out[i * 8 + 4] = nx / nl; out[i * 8 + 5] = sy / nl;
            out[i * 8 + 6] = nz / nl; out[i * 8 + 7] = wph;
        }
        sr->fcluster_cache[slot].count      = (uint32_t)count;
        sr->fcluster_cache[slot].param_hash = ph;
        sr->fcluster_cache[slot].entity     = e;
        sr->fcluster_cache[slot].used       = true;
    }
    return slot;
}

void sr_draw_foliage_cluster(JceSceneRenderer *sr, JceScene *scene,
                             EntityList *list, JceEntity e, uint16_t view_id)
{
    JceFoliageClusterComponent *fc = jce_scene_get_foliage_cluster(scene, e);
    if (!fc || !fc->visible || fc->leaf_count <= 0) return;
    if (!jce_scene_has_transform(scene, e)) return;

    sr_foliage_lazy_init(sr);
    if (!BGFX_HANDLE_IS_VALID(sr->prog_fcluster) || !sr->fcluster_quad) return;

    int slot = sr_fcluster_ensure_cache(sr, scene, e, fc);
    if (slot < 0) return;

    const uint32_t n = sr->fcluster_cache[slot].count;
    if (!n) return;

    /* Transient instance buffer (2 vec4 stride). */
    const uint16_t stride = 32;
    bgfx_instance_data_buffer_t idb;
    if (bgfx_get_avail_instance_data_buffer(n, stride) < n) return;
    bgfx_alloc_instance_data_buffer(&idb, n, stride);
    memcpy(idb.data, sr->fcluster_cache[slot].inst, (size_t)n * stride);

    /* Primary light (TO-light) for the toon ramp; falls back to overhead. */
    jce_vec3 to_light = jce_v3(0.3f, 0.8f, 0.2f);
    sr_resolve_primary_dir_light(sr, scene, list, false, &to_light, NULL, NULL);
    float lt[4] = { to_light.x, to_light.y, to_light.z, 0.0f };

    float colors[16] = {
        fc->shadow_color[0], fc->shadow_color[1], fc->shadow_color[2], 0.0f,
        fc->mid_color[0], fc->mid_color[1], fc->mid_color[2], 0.0f,
        fc->highlight_color[0], fc->highlight_color[1], fc->highlight_color[2], 0.0f,
        fc->color_multiplier[0], fc->color_multiplier[1], fc->color_multiplier[2], 1.0f,
    };
    float tm[4] = { sr->water_time, 0.0f, 0.0f, 0.0f };

    bgfx_set_uniform(sr->u_foliage_time,   tm,     1);
    bgfx_set_uniform(sr->u_foliage_colors, colors, 4);
    bgfx_set_uniform(sr->u_foliage_light,  lt,     1);

    JceTexture at = { UINT16_MAX };
    if (fc->alpha_tex[0]) at = sr_resolve_texture(sr, fc->alpha_tex);
    bgfx_texture_handle_t ath = { at.idx };
    if (!BGFX_HANDLE_IS_VALID(ath)) ath = sr->white_tex;
    bgfx_set_texture(0, sr->s_foliage_alpha, ath, UINT32_MAX);

    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(sr->fcluster_quad) };
    bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(sr->fcluster_quad) };
    if (!BGFX_HANDLE_IS_VALID(vbh) || !BGFX_HANDLE_IS_VALID(ibh)) return;

    float ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    bgfx_set_transform(ident, 1);
    bgfx_set_vertex_buffer(0, vbh, 0, 4);
    bgfx_set_index_buffer(ibh, 0, 6);
    bgfx_set_instance_data_buffer(&idb, 0, n);

    /* Opaque alpha-test (discard in fs) — draws in the opaque pass; no face
     * cull (billboards + wind sway can flip an edge). */
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_Z
                  | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA, 0);
    bgfx_submit(view_id, sr->prog_fcluster, 0, BGFX_DISCARD_ALL);
}
