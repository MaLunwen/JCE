/*
 * jce_scene_components_terrain.c  Scene component (de)serialize module
 * for the terrain domain (split from jce_scene_components_json.c).
 *
 * Pure move from the monolith: the shared JSON/Euler helpers live as
 * static inline in jce_scene_components_internal.h; the registry-referenced
 * parse_<x>/serw_<x> are external (declared in that header's shared section)
 * so the REG table can take their address; ser_<x> writers stay file-static.
 */

#include "jce_scene_components_internal.h"

void parse_terrain(JceScene *s, JceEntity e, const cJSON *c)
{
    JceTerrainComponent tc;
    memset(&tc, 0, sizeof(tc));
    copy_str(tc.terrain_path, sizeof(tc.terrain_path),
             j_str(c, "terrainPath", ""));
    const char *keys[4] = { "layerAlbedoPath0", "layerAlbedoPath1",
                            "layerAlbedoPath2", "layerAlbedoPath3" };
    for (int i = 0; i < 4; ++i)
        copy_str(tc.layer_albedo_path[i], sizeof(tc.layer_albedo_path[i]),
                 j_str(c, keys[i], ""));
    tc.tile_scale     = (float)j_num(c, "tileScale", 10.0);
    tc.tint[0]        = (float)j_num(c, "tintR", 1.0);
    tc.tint[1]        = (float)j_num(c, "tintG", 1.0);
    tc.tint[2]        = (float)j_num(c, "tintB", 1.0);
    tc.visible        = j_bool(c, "visible", true);
    tc.splat_enabled  = j_bool(c, "splatEnabled", true);
    jce_scene_set_terrain(s, e, &tc);
}

void parse_vegetation_scatter(JceScene *s, JceEntity e, const cJSON *c)
{
    JceVegetationScatterComponent vs;
    memset(&vs, 0, sizeof(vs));
    copy_str(vs.mesh_path,   sizeof(vs.mesh_path),   j_str(c, "meshPath", ""));
    vs.mesh_shape    = (int)j_num(c, "meshShape", 0); /* primitive when meshPath empty */
    copy_str(vs.albedo_path, sizeof(vs.albedo_path), j_str(c, "albedoPath", ""));
    copy_str(vs.density_mask_path, sizeof(vs.density_mask_path),
             j_str(c, "densityMaskPath", ""));
    vs.density         = (float)j_num(c, "density", 1.0);
    vs.seed            = (uint32_t)j_num(c, "seed", 12345.0);
    vs.area_x          = (float)j_num(c, "areaX", 10.0);
    vs.area_z          = (float)j_num(c, "areaZ", 10.0);
    vs.max_slope_deg   = (float)j_num(c, "maxSlopeDeg", 45.0);
    vs.scale_min       = (float)j_num(c, "scaleMin", 0.8);
    vs.scale_max       = (float)j_num(c, "scaleMax", 1.2);
    vs.tint[0]         = (float)j_num(c, "tintR", 1.0);
    vs.tint[1]         = (float)j_num(c, "tintG", 1.0);
    vs.tint[2]         = (float)j_num(c, "tintB", 1.0);
    vs.align_to_normal = j_bool(c, "alignToNormal", false);
    vs.cast_shadow     = j_bool(c, "castShadow", true);
    vs.visible         = j_bool(c, "visible", true);
    /* In-editor painted density grid (large-world #8a). */
    vs.density_paint_active = j_bool(c, "densityPaintActive", false);
    if (vs.density_paint_active) {
        memset(vs.density_paint, 255, sizeof vs.density_paint);  /* default full */
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(c, "densityPaintData");
        if (arr && cJSON_IsArray(arr)) {
            int n = cJSON_GetArraySize(arr);
            if (n > (int)sizeof vs.density_paint) n = (int)sizeof vs.density_paint;
            for (int i = 0; i < n; ++i) {
                int iv = (int)cJSON_GetNumberValue(cJSON_GetArrayItem(arr, i));
                vs.density_paint[i] = (uint8_t)(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
            }
        }
    }
    jce_scene_set_vegetation_scatter(s, e, &vs);
}

void parse_grass_field(JceScene *s, JceEntity e, const cJSON *c)
{
    JceGrassFieldComponent g; memset(&g, 0, sizeof g);
    g.density       = (float)j_num(c, "density", 8.0);
    g.seed          = (uint32_t)j_num(c, "seed", 1337.0);
    g.area_x        = (float)j_num(c, "areaX", 40.0);
    g.area_z        = (float)j_num(c, "areaZ", 40.0);
    g.max_slope_deg = (float)j_num(c, "maxSlopeDeg", 35.0);
    g.scale_min     = (float)j_num(c, "scaleMin", 0.8);
    g.scale_max     = (float)j_num(c, "scaleMax", 1.3);
    g.blade_height  = (float)j_num(c, "bladeHeight", 0.4);
    g.blade_width   = (float)j_num(c, "bladeWidth", 0.05);
    g.cards         = (int)j_num(c, "cards", 4);
    g.root_color[0] = (float)j_num(c, "rootR", 0.10);
    g.root_color[1] = (float)j_num(c, "rootG", 0.22);
    g.root_color[2] = (float)j_num(c, "rootB", 0.05);
    g.tip_color[0]  = (float)j_num(c, "tipR", 0.55);
    g.tip_color[1]  = (float)j_num(c, "tipG", 0.78);
    g.tip_color[2]  = (float)j_num(c, "tipB", 0.25);
    g.wind_dir[0]   = (float)j_num(c, "windDirX", 1.0);
    g.wind_dir[1]   = (float)j_num(c, "windDirZ", 0.0);
    g.wind_speed    = (float)j_num(c, "windSpeed", 1.5);
    g.wind_amplitude= (float)j_num(c, "windAmplitude", 0.10);
    g.fade_start    = (float)j_num(c, "fadeStart", 50.0);
    g.fade_end      = (float)j_num(c, "fadeEnd", 110.0);
    g.hue_jitter    = (float)j_num(c, "hueJitter", 0.2);
    g.cast_shadow   = j_bool(c, "castShadow", false);
    g.visible       = j_bool(c, "visible", true);
    copy_str(g.density_mask_path, sizeof(g.density_mask_path),
             j_str(c, "densityMaskPath", ""));
    g.density_threshold = (float)j_num(c, "densityThreshold", 0.0);
    g.mask_world_size   = (float)j_num(c, "maskWorldSize", 33.0);
    jce_scene_set_grass_field(s, e, &g);
}

void parse_foliage_cluster(JceScene *s, JceEntity e, const cJSON *c)
{
    JceFoliageClusterComponent fc; memset(&fc, 0, sizeof fc);
    fc.leaf_count = (int)j_num(c, "leafCount", 45.0);
    fc.radius     = (float)j_num(c, "radius", 1.2);
    fc.squash_y   = (float)j_num(c, "squashY", 0.8);
    fc.leaf_scale = (float)j_num(c, "leafScale", 1.0);
    fc.seed       = (uint32_t)j_num(c, "seed", 12345.0);
    fc.shadow_color[0] = (float)j_num(c, "shadowR", 0.003);
    fc.shadow_color[1] = (float)j_num(c, "shadowG", 0.074);
    fc.shadow_color[2] = (float)j_num(c, "shadowB", 0.003);
    fc.mid_color[0] = (float)j_num(c, "midR", 0.06);
    fc.mid_color[1] = (float)j_num(c, "midG", 0.23);
    fc.mid_color[2] = (float)j_num(c, "midB", 0.0);
    fc.highlight_color[0] = (float)j_num(c, "highR", 0.44);
    fc.highlight_color[1] = (float)j_num(c, "highG", 0.5);
    fc.highlight_color[2] = (float)j_num(c, "highB", 0.0);
    fc.color_multiplier[0] = (float)j_num(c, "multR", 0.46);
    fc.color_multiplier[1] = (float)j_num(c, "multG", 0.65);
    fc.color_multiplier[2] = (float)j_num(c, "multB", 0.3);
    copy_str(fc.alpha_tex, sizeof fc.alpha_tex, j_str(c, "alphaTex", ""));
    fc.visible = j_bool(c, "visible", true);
    jce_scene_set_foliage_cluster(s, e, &fc);
}

static void ser_foliage_cluster(const JceFoliageClusterComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddStringToObject(o, "type", "FoliageCluster");
    cJSON_AddNumberToObject(o, "leafCount", c->leaf_count);
    cJSON_AddNumberToObject(o, "radius", c->radius);
    cJSON_AddNumberToObject(o, "squashY", c->squash_y);
    cJSON_AddNumberToObject(o, "leafScale", c->leaf_scale);
    cJSON_AddNumberToObject(o, "seed", (double)c->seed);
    cJSON_AddNumberToObject(o, "shadowR", c->shadow_color[0]);
    cJSON_AddNumberToObject(o, "shadowG", c->shadow_color[1]);
    cJSON_AddNumberToObject(o, "shadowB", c->shadow_color[2]);
    cJSON_AddNumberToObject(o, "midR", c->mid_color[0]);
    cJSON_AddNumberToObject(o, "midG", c->mid_color[1]);
    cJSON_AddNumberToObject(o, "midB", c->mid_color[2]);
    cJSON_AddNumberToObject(o, "highR", c->highlight_color[0]);
    cJSON_AddNumberToObject(o, "highG", c->highlight_color[1]);
    cJSON_AddNumberToObject(o, "highB", c->highlight_color[2]);
    cJSON_AddNumberToObject(o, "multR", c->color_multiplier[0]);
    cJSON_AddNumberToObject(o, "multG", c->color_multiplier[1]);
    cJSON_AddNumberToObject(o, "multB", c->color_multiplier[2]);
    if (c->alpha_tex[0])
        cJSON_AddStringToObject(o, "alphaTex", c->alpha_tex);
    cJSON_AddBoolToObject(o, "visible", c->visible);
    cJSON_AddItemToArray(arr, o);
}

void serw_foliage_cluster(JceScene *s, JceEntity e, cJSON *arr)
{
    JceFoliageClusterComponent *c = jce_scene_get_foliage_cluster(s, e);
    if (c) ser_foliage_cluster(c, arr);
}

void parse_water(JceScene *s, JceEntity e, const cJSON *c)
{
    JceWaterComponent w;
    memset(&w, 0, sizeof(w));
    w.size_x       = (float)j_num(c, "sizeX", 100.0);
    w.size_z       = (float)j_num(c, "sizeZ", 100.0);
    w.wave_count   = (int)j_num(c, "waveCount", 0);
    if (w.wave_count < 0) w.wave_count = 0;
    if (w.wave_count > JCE_WATER_COMP_MAX_WAVES) w.wave_count = JCE_WATER_COMP_MAX_WAVES;
    w.base_height  = (float)j_num(c, "baseHeight", 0.0);
    char key[32];
    for (int i = 0; i < JCE_WATER_COMP_MAX_WAVES; ++i) {
        snprintf(key, sizeof(key), "wAmp%d", i);
        w.waves[i].amplitude  = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "wLen%d", i);
        w.waves[i].wavelength = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "wSpd%d", i);
        w.waves[i].speed      = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "wDirX%d", i);
        w.waves[i].dir_x      = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "wDirZ%d", i);
        w.waves[i].dir_z      = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "wSteep%d", i);
        w.waves[i].steepness  = (float)j_num(c, key, 0.0);
    }
    w.color_shallow[0] = (float)j_num(c, "shallowR", 0.1);
    w.color_shallow[1] = (float)j_num(c, "shallowG", 0.4);
    w.color_shallow[2] = (float)j_num(c, "shallowB", 0.5);
    w.color_deep[0]    = (float)j_num(c, "deepR", 0.0);
    w.color_deep[1]    = (float)j_num(c, "deepG", 0.1);
    w.color_deep[2]    = (float)j_num(c, "deepB", 0.2);
    w.transparency     = (float)j_num(c, "transparency", 0.5);
    /* Default 0 = absorption off: an authored scene must keep rendering as
     * authored until someone opts in. */
    w.clarity          = (float)j_num(c, "clarity", 0.0);
    w.caustics         = (float)j_num(c, "caustics", 0.0);
    w.shore_foam_m     = (float)j_num(c, "shoreFoam", 0.0);
    w.shore_surge_s    = (float)j_num(c, "shoreSurge", 0.0);
    w.sun_specular     = (float)j_num(c, "sunSpecular", 1.0);
    /* Stylized extras default to 0 (off) so pre-existing scenes render and
     * round-trip byte-identically. */
    w.shore_ripple     = (float)j_num(c, "shoreRipple", 0.0);
    w.ice_ratio        = (float)j_num(c, "iceRatio", 0.0);
    w.splash_ratio     = (float)j_num(c, "splashRatio", 0.0);
    /* Absent => false => byte-identical to every scene authored before this
     * field existed, which is the whole reason it defaults off. */
    w.depth_write      = j_bool(c, "depthWrite", false);
    /* Absent => 0 => Phillips, so every scene authored before this existed
     * round-trips byte-identically. */
    w.ocean            = j_bool(c, "ocean", false);
    w.fft_fetch        = (float)j_num(c, "fftFetch", 0.0);
    w.fft_swell        = (float)j_num(c, "fftSwell", 0.0);
    copy_str(w.data_tex, sizeof(w.data_tex), j_str(c, "dataTex", ""));
    w.visible          = j_bool(c, "visible", true);

    /* ── FFT ocean (additive) ───────────────────────────────────────────
     * Absent keys default to GERSTNER + sane FFT params, so scenes authored
     * before this feature round-trip byte-identically. */
    w.water_mode = (int)j_num(c, "waterMode", (double)JCE_WATER_MODE_GERSTNER);
    if (w.water_mode != JCE_WATER_MODE_FFT &&
        w.water_mode != JCE_WATER_MODE_STYLIZED)
        w.water_mode = JCE_WATER_MODE_GERSTNER;
    w.fft_patch_size = (float)j_num(c, "fftPatchSize", 100.0);
    w.fft_wind_speed = (float)j_num(c, "fftWindSpeed", 8.0);
    w.fft_wind_dir_x = (float)j_num(c, "fftWindDirX", 1.0);
    w.fft_wind_dir_z = (float)j_num(c, "fftWindDirZ", 0.0);
    w.fft_amplitude  = (float)j_num(c, "fftAmplitude", 0.0008);
    w.fft_resolution = (int)j_num(c, "fftResolution", 64);
    w.fft_resolution = clamp_pow2_i(w.fft_resolution, 32, 256);

    jce_scene_set_water(s, e, &w);
}

void parse_virtual_camera(JceScene *s, JceEntity e, const cJSON *c)
{
    JceVirtualCameraComponent v;
    memset(&v, 0, sizeof(v));
    copy_str(v.vcam_name, sizeof(v.vcam_name), j_str(c, "name", "VCam"));
    v.priority   = (int32_t)j_num(c, "priority", 0);
    v.active     = j_bool(c, "active", true);
    v.track_mode = (int)j_num(c, "trackMode", 0);
    v.position[0] = (float)j_num(c, "posX", 0.0);
    v.position[1] = (float)j_num(c, "posY", 0.0);
    v.position[2] = (float)j_num(c, "posZ", 0.0);
    v.look_at[0]  = (float)j_num(c, "lookX", 0.0);
    v.look_at[1]  = (float)j_num(c, "lookY", 0.0);
    v.look_at[2]  = (float)j_num(c, "lookZ", 0.0);
    v.fov_deg     = (float)j_num(c, "fov", 60.0);
    v.follow_offset[0] = (float)j_num(c, "offX", 0.0);
    v.follow_offset[1] = (float)j_num(c, "offY", 0.0);
    v.follow_offset[2] = (float)j_num(c, "offZ", 0.0);
    v.damping     = (float)j_num(c, "damping", 0.5);
    v.follow_target  = (uint64_t)j_num(c, "followTarget", 0.0);
    v.look_at_target = (uint64_t)j_num(c, "lookAtTarget", 0.0);
    jce_scene_set_virtual_camera(s, e, &v);
}

void parse_tilemap(JceScene *s, JceEntity e, const cJSON *props)
{
    JceTilemapComponent tc; memset(&tc, 0, sizeof tc);
    copy_str(tc.tilemap_path, sizeof tc.tilemap_path, j_str(props, "tilemapPath", ""));
    copy_str(tc.sprites_path, sizeof tc.sprites_path, j_str(props, "spritesPath", ""));
    tc.cell_size_px = (uint16_t)j_num(props, "cellSizePx", 16);
    tc.sort_order   = (uint16_t)j_num(props, "sortOrder", 0);
    tc.orientation  = (uint8_t) j_num(props, "orientation", 0);
    tc.visible      = j_bool(props, "visible", true);
    tc.color[0]     = (float)j_num(props, "colorR", 1.0);
    tc.color[1]     = (float)j_num(props, "colorG", 1.0);
    tc.color[2]     = (float)j_num(props, "colorB", 1.0);
    tc.color[3]     = (float)j_num(props, "colorA", 1.0);
    jce_scene_set_tilemap(s, e, &tc);
}

void parse_tilemap_collider2d(JceScene *s, JceEntity e, const cJSON *props)
{
    JceTilemapCollider2DComponent cc; memset(&cc, 0, sizeof cc);
    cc.used_by_composite = j_bool(props, "usedByComposite", false);
    cc.trigger           = j_bool(props, "trigger", false);
    cc.offset[0]         = (float)j_num(props, "offsetX", 0);
    cc.offset[1]         = (float)j_num(props, "offsetY", 0);
    cc.friction_x100     = (uint16_t)j_num(props, "frictionX100", 40);
    cc.bounciness_x100   = (uint16_t)j_num(props, "bouncinessX100", 0);
    jce_scene_set_tilemap_collider2d(s, e, &cc);
}

static void ser_terrain(const JceTerrainComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Terrain");
    cJSON_AddStringToObject(o, "terrainPath", c->terrain_path);
    cJSON_AddStringToObject(o, "layerAlbedoPath0", c->layer_albedo_path[0]);
    cJSON_AddStringToObject(o, "layerAlbedoPath1", c->layer_albedo_path[1]);
    cJSON_AddStringToObject(o, "layerAlbedoPath2", c->layer_albedo_path[2]);
    cJSON_AddStringToObject(o, "layerAlbedoPath3", c->layer_albedo_path[3]);
    cJSON_AddNumberToObject(o, "tileScale", c->tile_scale);
    cJSON_AddNumberToObject(o, "tintR", c->tint[0]);
    cJSON_AddNumberToObject(o, "tintG", c->tint[1]);
    cJSON_AddNumberToObject(o, "tintB", c->tint[2]);
    cJSON_AddBoolToObject(o, "visible", c->visible);
    cJSON_AddBoolToObject(o, "splatEnabled", c->splat_enabled);
    cJSON_AddItemToArray(arr, o);
}

static void ser_vegetation_scatter(const JceVegetationScatterComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "VegetationScatter");
    cJSON_AddStringToObject(o, "meshPath", c->mesh_path);
    cJSON_AddNumberToObject(o, "meshShape", c->mesh_shape);
    cJSON_AddStringToObject(o, "albedoPath", c->albedo_path);
    cJSON_AddStringToObject(o, "densityMaskPath", c->density_mask_path);
    cJSON_AddNumberToObject(o, "density", c->density);
    cJSON_AddNumberToObject(o, "seed", (double)c->seed);
    cJSON_AddNumberToObject(o, "areaX", c->area_x);
    cJSON_AddNumberToObject(o, "areaZ", c->area_z);
    cJSON_AddNumberToObject(o, "maxSlopeDeg", c->max_slope_deg);
    cJSON_AddNumberToObject(o, "scaleMin", c->scale_min);
    cJSON_AddNumberToObject(o, "scaleMax", c->scale_max);
    cJSON_AddNumberToObject(o, "tintR", c->tint[0]);
    cJSON_AddNumberToObject(o, "tintG", c->tint[1]);
    cJSON_AddNumberToObject(o, "tintB", c->tint[2]);
    cJSON_AddBoolToObject  (o, "alignToNormal", c->align_to_normal);
    cJSON_AddBoolToObject  (o, "castShadow", c->cast_shadow);
    cJSON_AddBoolToObject  (o, "visible", c->visible);
    cJSON_AddBoolToObject  (o, "densityPaintActive", c->density_paint_active);
    if (c->density_paint_active) {
        int tmp[JCE_VEG_PAINT_DIM * JCE_VEG_PAINT_DIM];
        for (int i = 0; i < (int)sizeof c->density_paint; ++i)
            tmp[i] = (int)c->density_paint[i];
        cJSON_AddItemToObject(o, "densityPaintData",
                              cJSON_CreateIntArray(tmp, (int)sizeof c->density_paint));
    }
    cJSON_AddItemToArray(arr, o);
}

static void ser_grass_field(const JceGrassFieldComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "GrassField");
    cJSON_AddNumberToObject(o, "density", c->density);
    cJSON_AddNumberToObject(o, "seed", (double)c->seed);
    cJSON_AddNumberToObject(o, "areaX", c->area_x);
    cJSON_AddNumberToObject(o, "areaZ", c->area_z);
    cJSON_AddNumberToObject(o, "maxSlopeDeg", c->max_slope_deg);
    cJSON_AddNumberToObject(o, "scaleMin", c->scale_min);
    cJSON_AddNumberToObject(o, "scaleMax", c->scale_max);
    cJSON_AddNumberToObject(o, "bladeHeight", c->blade_height);
    cJSON_AddNumberToObject(o, "bladeWidth", c->blade_width);
    cJSON_AddNumberToObject(o, "cards", c->cards);
    cJSON_AddNumberToObject(o, "rootR", c->root_color[0]);
    cJSON_AddNumberToObject(o, "rootG", c->root_color[1]);
    cJSON_AddNumberToObject(o, "rootB", c->root_color[2]);
    cJSON_AddNumberToObject(o, "tipR", c->tip_color[0]);
    cJSON_AddNumberToObject(o, "tipG", c->tip_color[1]);
    cJSON_AddNumberToObject(o, "tipB", c->tip_color[2]);
    cJSON_AddNumberToObject(o, "windDirX", c->wind_dir[0]);
    cJSON_AddNumberToObject(o, "windDirZ", c->wind_dir[1]);
    cJSON_AddNumberToObject(o, "windSpeed", c->wind_speed);
    cJSON_AddNumberToObject(o, "windAmplitude", c->wind_amplitude);
    cJSON_AddNumberToObject(o, "fadeStart", c->fade_start);
    cJSON_AddNumberToObject(o, "fadeEnd", c->fade_end);
    cJSON_AddNumberToObject(o, "hueJitter", c->hue_jitter);
    cJSON_AddBoolToObject  (o, "castShadow", c->cast_shadow);
    cJSON_AddBoolToObject  (o, "visible", c->visible);
    cJSON_AddStringToObject(o, "densityMaskPath", c->density_mask_path);
    cJSON_AddNumberToObject(o, "densityThreshold", c->density_threshold);
    cJSON_AddNumberToObject(o, "maskWorldSize", c->mask_world_size);
    cJSON_AddItemToArray(arr, o);
}

static void ser_water(const JceWaterComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Water");
    cJSON_AddNumberToObject(o, "sizeX", c->size_x);
    cJSON_AddNumberToObject(o, "sizeZ", c->size_z);
    cJSON_AddNumberToObject(o, "waveCount", c->wave_count);
    cJSON_AddNumberToObject(o, "baseHeight", c->base_height);
    char key[32];
    for (int i = 0; i < JCE_WATER_COMP_MAX_WAVES; ++i) {
        snprintf(key, sizeof(key), "wAmp%d", i);
        cJSON_AddNumberToObject(o, key, c->waves[i].amplitude);
        snprintf(key, sizeof(key), "wLen%d", i);
        cJSON_AddNumberToObject(o, key, c->waves[i].wavelength);
        snprintf(key, sizeof(key), "wSpd%d", i);
        cJSON_AddNumberToObject(o, key, c->waves[i].speed);
        snprintf(key, sizeof(key), "wDirX%d", i);
        cJSON_AddNumberToObject(o, key, c->waves[i].dir_x);
        snprintf(key, sizeof(key), "wDirZ%d", i);
        cJSON_AddNumberToObject(o, key, c->waves[i].dir_z);
        snprintf(key, sizeof(key), "wSteep%d", i);
        cJSON_AddNumberToObject(o, key, c->waves[i].steepness);
    }
    cJSON_AddNumberToObject(o, "shallowR", c->color_shallow[0]);
    cJSON_AddNumberToObject(o, "shallowG", c->color_shallow[1]);
    cJSON_AddNumberToObject(o, "shallowB", c->color_shallow[2]);
    cJSON_AddNumberToObject(o, "deepR", c->color_deep[0]);
    cJSON_AddNumberToObject(o, "deepG", c->color_deep[1]);
    cJSON_AddNumberToObject(o, "deepB", c->color_deep[2]);
    cJSON_AddNumberToObject(o, "transparency", c->transparency);
    cJSON_AddNumberToObject(o, "clarity", c->clarity);
    cJSON_AddNumberToObject(o, "caustics", c->caustics);
    cJSON_AddNumberToObject(o, "shoreFoam", c->shore_foam_m);
    cJSON_AddNumberToObject(o, "shoreSurge", c->shore_surge_s);
    cJSON_AddNumberToObject(o, "sunSpecular", c->sun_specular);
    cJSON_AddNumberToObject(o, "shoreRipple", c->shore_ripple);
    cJSON_AddNumberToObject(o, "iceRatio", c->ice_ratio);
    cJSON_AddNumberToObject(o, "splashRatio", c->splash_ratio);
    cJSON_AddBoolToObject(o, "depthWrite", c->depth_write);
    cJSON_AddBoolToObject(o, "ocean", c->ocean);
    cJSON_AddNumberToObject(o, "fftFetch", c->fft_fetch);
    cJSON_AddNumberToObject(o, "fftSwell", c->fft_swell);
    if (c->data_tex[0])
        cJSON_AddStringToObject(o, "dataTex", c->data_tex);
    cJSON_AddBoolToObject  (o, "visible", c->visible);
    /* FFT ocean (additive) — see parse_water for the matching keys. */
    cJSON_AddNumberToObject(o, "waterMode", c->water_mode);
    cJSON_AddNumberToObject(o, "fftPatchSize", c->fft_patch_size);
    cJSON_AddNumberToObject(o, "fftWindSpeed", c->fft_wind_speed);
    cJSON_AddNumberToObject(o, "fftWindDirX", c->fft_wind_dir_x);
    cJSON_AddNumberToObject(o, "fftWindDirZ", c->fft_wind_dir_z);
    cJSON_AddNumberToObject(o, "fftAmplitude", c->fft_amplitude);
    cJSON_AddNumberToObject(o, "fftResolution", c->fft_resolution);
    cJSON_AddItemToArray(arr, o);
}

static void ser_virtual_camera(const JceVirtualCameraComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "VirtualCamera");
    cJSON_AddStringToObject(o, "name", c->vcam_name);
    cJSON_AddNumberToObject(o, "priority", c->priority);
    cJSON_AddBoolToObject  (o, "active", c->active);
    cJSON_AddNumberToObject(o, "trackMode", c->track_mode);
    cJSON_AddNumberToObject(o, "posX", c->position[0]);
    cJSON_AddNumberToObject(o, "posY", c->position[1]);
    cJSON_AddNumberToObject(o, "posZ", c->position[2]);
    cJSON_AddNumberToObject(o, "lookX", c->look_at[0]);
    cJSON_AddNumberToObject(o, "lookY", c->look_at[1]);
    cJSON_AddNumberToObject(o, "lookZ", c->look_at[2]);
    cJSON_AddNumberToObject(o, "fov", c->fov_deg);
    cJSON_AddNumberToObject(o, "offX", c->follow_offset[0]);
    cJSON_AddNumberToObject(o, "offY", c->follow_offset[1]);
    cJSON_AddNumberToObject(o, "offZ", c->follow_offset[2]);
    cJSON_AddNumberToObject(o, "damping", c->damping);
    cJSON_AddNumberToObject(o, "followTarget",  (double)c->follow_target);
    cJSON_AddNumberToObject(o, "lookAtTarget", (double)c->look_at_target);
    cJSON_AddItemToArray(arr, o);
}

void serw_terrain(JceScene *s, JceEntity e, cJSON *arr)
{
    JceTerrainComponent *c = jce_scene_get_terrain(s, e);
    if (c) ser_terrain(c, arr);
}

void serw_vegetation_scatter(JceScene *s, JceEntity e, cJSON *arr)
{
    JceVegetationScatterComponent *c = jce_scene_get_vegetation_scatter(s, e);
    if (c) ser_vegetation_scatter(c, arr);
}

void serw_grass_field(JceScene *s, JceEntity e, cJSON *arr)
{
    const JceGrassFieldComponent *c = jce_scene_get_grass_field(s, e);
    if (c) ser_grass_field(c, arr);
}

void serw_water(JceScene *s, JceEntity e, cJSON *arr)
{
    JceWaterComponent *c = jce_scene_get_water(s, e);
    if (c) ser_water(c, arr);
}

void serw_virtual_camera(JceScene *s, JceEntity e, cJSON *arr)
{
    JceVirtualCameraComponent *c = jce_scene_get_virtual_camera(s, e);
    if (c) ser_virtual_camera(c, arr);
}

void serw_tilemap(JceScene *s, JceEntity e, cJSON *arr)
{
    JceTilemapComponent *c = jce_scene_get_tilemap(s, e);
    if (c) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "Tilemap");
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "tilemapPath", c->tilemap_path);
        cJSON_AddStringToObject(p, "spritesPath", c->sprites_path);
        cJSON_AddNumberToObject(p, "cellSizePx",  c->cell_size_px);
        cJSON_AddNumberToObject(p, "sortOrder",   c->sort_order);
        cJSON_AddNumberToObject(p, "orientation", c->orientation);
        cJSON_AddBoolToObject  (p, "visible",     c->visible);
        cJSON_AddNumberToObject(p, "colorR",      c->color[0]);
        cJSON_AddNumberToObject(p, "colorG",      c->color[1]);
        cJSON_AddNumberToObject(p, "colorB",      c->color[2]);
        cJSON_AddNumberToObject(p, "colorA",      c->color[3]);
        cJSON_AddItemToObject(o, "properties", p);
        cJSON_AddItemToArray(arr, o);
    }
}

void serw_tilemap_collider2d(JceScene *s, JceEntity e, cJSON *arr)
{
    JceTilemapCollider2DComponent *c = jce_scene_get_tilemap_collider2d(s, e);
    if (c) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "TilemapCollider2D");
        cJSON *p = cJSON_CreateObject();
        cJSON_AddBoolToObject  (p, "usedByComposite", c->used_by_composite);
        cJSON_AddBoolToObject  (p, "trigger",         c->trigger);
        cJSON_AddNumberToObject(p, "offsetX",         c->offset[0]);
        cJSON_AddNumberToObject(p, "offsetY",         c->offset[1]);
        cJSON_AddNumberToObject(p, "frictionX100",    c->friction_x100);
        cJSON_AddNumberToObject(p, "bouncinessX100",  c->bounciness_x100);
        cJSON_AddItemToObject(o, "properties", p);
        cJSON_AddItemToArray(arr, o);
    }
}

