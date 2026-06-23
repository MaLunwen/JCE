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
    return h;
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
    JCE_FREE(sr->foliage_cache[victim].insts);
    memset(&sr->foliage_cache[victim], 0, sizeof sr->foliage_cache[victim]);
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

void sr_draw_foliage(JceSceneRenderer *sr, JceScene *scene,
                            EntityList *list, JceEntity e, uint16_t view_id)
{
    JceVegetationScatterComponent *vs = jce_scene_get_vegetation_scatter(scene, e);
    if (!vs || !vs->visible || !vs->mesh_path[0]) return;
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

        uint32_t want = (uint32_t)((double)p.density * (double)p.area_x * (double)p.area_z);
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
        sr->foliage_cache[slot].param_hash = ph;
        sr->foliage_cache[slot].entity     = e;
        sr->foliage_cache[slot].used       = true;
        snprintf(sr->foliage_cache[slot].mesh_path,
                 sizeof sr->foliage_cache[slot].mesh_path, "%s", vs->mesh_path);
    }

    SrModelCache *mc = sr_get_model(sr, vs->mesh_path, (uint32_t)e);
    if (!mc || !mc->model) return;

    /* Per-instance draw (capped for per-frame cost; instancing is follow-up). */
    uint32_t n = sr->foliage_cache[slot].inst_count;
    const uint32_t kDrawCap = 4096u;
    if (n > kDrawCap) n = kDrawCap;
    for (uint32_t i = 0; i < n; ++i) {
        jce_mat4 m = sr_foliage_instance_matrix(&sr->foliage_cache[slot].insts[i]);
        jce_model_draw(mc->model, sr->renderer, view_id, &m, NULL, 0);
    }
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
    float shading[4]   = { wc->transparency, wc->sun_specular, 0.0f, 0.0f };

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
    float wmode[4] = { (float)water_mode, fft_patch, 0.0f, 0.0f };

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
    bgfx_set_uniform(sr->u_sky_colors, sky_colors, 3);

    /* Preetham uniforms always carry safe defaults so the shader never
     * reads stale/unset values regardless of the active mode.  The actual
     * Preetham state is filled below only when PREETHAM mode is selected. */
    float perez[16];   /* vec4[4]: Y/x/y A..D + (EY,Ex,Ey,0)             */
    float zenith[4] = { 1.0f, 0.3f, 0.3f, 0.0f };
    float sun4[4]   = { 0.0f, 1.0f, 0.0f, 0.0f };
    for (int i = 0; i < 16; ++i) perez[i] = 0.0f;

    bgfx_texture_handle_t equirect_tex = { UINT16_MAX };
    float sky_params[4] = { 0.0f, 1.0f, 0.0f, 0.0f };

    if (sr->sky_mode == JCE_SCENE_SKY_PREETHAM) {
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
    jce_scene_each_entity(scene, sr_draw_one_cloth, &ctx);
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

void sr_scan_skybox(JceSceneRenderer *sr, JceScene *scene, EntityList *list)
{
    const char *hdr_path = NULL;
    float rotation = 0.0f;
    float exposure = 1.0f;

    /* Pick up a finished async IBL bake (if any) before anything else. */
    sr_ibl_poll(sr);

    for (int i = 0; i < list->count && !hdr_path; i++) {
        JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_skybox(scene, e)) continue;
        JceSkyboxComponent *c = jce_scene_get_skybox(scene, e);
        if (!c || c->hdr_path[0] == '\0') continue;
        hdr_path = c->hdr_path;
        rotation = c->rotation;
        exposure = c->exposure > 0.0f ? c->exposure : 1.0f;
    }

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
    jce_scene_each_entity(scene, sr_decal_each_entity, &ctx);
    if (ctx.spawned > 0)
        jce_decals_render(sr->decals_authored, view_id);

    /* Runtime-stamped decals (created on demand by the public spawn API). */
    if (sr->decals) {
        jce_decals_update(sr->decals, dt_sec);
        jce_decals_render(sr->decals, view_id);
    }
}
