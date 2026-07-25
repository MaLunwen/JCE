/*
 * jce_scene_components_render.c  Scene component (de)serialize module
 * for the render domain (split from jce_scene_components_json.c).
 *
 * Pure move from the monolith: the shared JSON/Euler helpers live as
 * static inline in jce_scene_components_internal.h; the registry-referenced
 * parse_<x>/serw_<x> are external (declared in that header's shared section)
 * so the REG table can take their address; ser_<x> writers stay file-static.
 */

#include "jce_scene_components_internal.h"

void parse_dir_light(JceScene *s, JceEntity e, const cJSON *c)
{
    JceDirectionalLight dl;
    memset(&dl, 0, sizeof(dl));
    dl.cookie_texture = JCE_TEXTURE_INVALID;
    dl.direction.x = (float)j_num(c, "dirX", 0.0);
    dl.direction.y = (float)j_num(c, "dirY", -1.0);
    dl.direction.z = (float)j_num(c, "dirZ", 0.0);
    dl.color.x = (float)j_num(c, "colorR", 1.0);
    dl.color.y = (float)j_num(c, "colorG", 1.0);
    dl.color.z = (float)j_num(c, "colorB", 1.0);
    dl.intensity = (float)j_num(c, "intensity", 1.0);
    dl.casts_shadow = j_bool(c, "castsShadow", false);
    /* P3-E.5 — optional cookie projection. */
    copy_str(dl.cookie_path, sizeof(dl.cookie_path), j_str(c, "cookiePath", ""));
    dl.cookie_strength = (float)j_num(c, "cookieStrength", 0.0);
    jce_scene_set_dir_light(s, e, &dl);
}

void parse_point_light(JceScene *s, JceEntity e, const cJSON *c)
{
    JcePointLight pl;
    memset(&pl, 0, sizeof(pl));
    pl.position.x = (float)j_num(c, "posX", 0.0);
    pl.position.y = (float)j_num(c, "posY", 0.0);
    pl.position.z = (float)j_num(c, "posZ", 0.0);
    pl.color.x = (float)j_num(c, "colorR", 1.0);
    pl.color.y = (float)j_num(c, "colorG", 1.0);
    pl.color.z = (float)j_num(c, "colorB", 1.0);
    pl.intensity = (float)j_num(c, "intensity", 1.0);
    pl.radius    = (float)j_num(c, "radius", 10.0);
    jce_scene_set_point_light(s, e, &pl);
}

void parse_spot_light(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSpotLight sl;
    memset(&sl, 0, sizeof(sl));
    sl.cookie_texture = JCE_TEXTURE_INVALID;
    sl.ies_lut_texture = JCE_TEXTURE_INVALID;
    sl.position.x = (float)j_num(c, "posX", 0.0);
    sl.position.y = (float)j_num(c, "posY", 0.0);
    sl.position.z = (float)j_num(c, "posZ", 0.0);
    sl.direction.x = (float)j_num(c, "dirX", 0.0);
    sl.direction.y = (float)j_num(c, "dirY", -1.0);
    sl.direction.z = (float)j_num(c, "dirZ", 0.0);
    sl.color.x = (float)j_num(c, "colorR", 1.0);
    sl.color.y = (float)j_num(c, "colorG", 1.0);
    sl.color.z = (float)j_num(c, "colorB", 1.0);
    sl.intensity = (float)j_num(c, "intensity", 1.0);
    sl.radius    = (float)j_num(c, "radius", 10.0);
    float inner_deg = (float)j_num(c, "innerConeDeg", 30.0);
    float outer_deg = (float)j_num(c, "outerConeDeg", 45.0);
    sl.inner_cone_cos = cosf(inner_deg * JCE_DEG2RAD);
    sl.outer_cone_cos = cosf(outer_deg * JCE_DEG2RAD);
    /* P3-E.5 — cookie + IES profile (optional, additive). */
    copy_str(sl.cookie_path, sizeof(sl.cookie_path), j_str(c, "cookiePath", ""));
    copy_str(sl.ies_path,    sizeof(sl.ies_path),    j_str(c, "iesPath", ""));
    sl.cookie_strength = (float)j_num(c, "cookieStrength", 0.0);
    jce_scene_set_spot_light(s, e, &sl);
}

void parse_skybox(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSkyboxComponent sky;
    memset(&sky, 0, sizeof(sky));
    copy_str(sky.hdr_path, sizeof(sky.hdr_path), j_str(c, "hdrPath", ""));
    sky.rotation   = (float)j_num(c, "rotation", 0.0);
    sky.exposure   = (float)j_num(c, "exposure", 1.0);
    sky.use_as_ibl = j_bool(c, "useAsIbl", true);
    jce_scene_set_skybox(s, e, &sky);
}

void parse_sprite_renderer(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSpriteRendererComponent sp;
    memset(&sp, 0, sizeof(sp));
    copy_str(sp.sprite_path, sizeof(sp.sprite_path), j_str(c, "spritePath", ""));
    sp.color[0] = (float)j_num(c, "colorR", 1.0);
    sp.color[1] = (float)j_num(c, "colorG", 1.0);
    sp.color[2] = (float)j_num(c, "colorB", 1.0);
    sp.color[3] = (float)j_num(c, "colorA", 1.0);
    sp.flip_x   = j_bool(c, "flipX", false);
    sp.flip_y   = j_bool(c, "flipY", false);
    sp.sorting_order = (int)j_num(c, "sortingOrder", 0);
    jce_scene_set_sprite_renderer(s, e, &sp);
}

/* Unified "Light" component: dispatches to dir/point/spot based on lightType. */
void parse_unified_light(JceScene *s, JceEntity e, const cJSON *c)
{
    float colorR = (float)j_num2(c, "colorR", "color_r", 1.0);
    float colorG = (float)j_num2(c, "colorG", "color_g", 1.0);
    float colorB = (float)j_num2(c, "colorB", "color_b", 1.0);
    float intensity = (float)j_num(c, "intensity", 1.0);
    bool  casts_shadow = j_bool(c, "castsShadow", false);
    float dirX = (float)j_num2(c, "dirX", "dir_x", 0.0);
    float dirY = (float)j_num2(c, "dirY", "dir_y", -1.0);
    float dirZ = (float)j_num2(c, "dirZ", "dir_z", 0.0);

    int ltype = 0;
    const cJSON *lt = cJSON_GetObjectItemCaseSensitive(c, "lightType");
    if (!lt) lt = cJSON_GetObjectItemCaseSensitive(c, "type");
    if (cJSON_IsNumber(lt)) ltype = lt->valueint;
    else if (cJSON_IsString(lt) && lt->valuestring) {
        if (streq_ci(lt->valuestring, "point")) ltype = 1;
        else if (streq_ci(lt->valuestring, "spot")) ltype = 2;
    }

    if (ltype == 1) {
        JcePointLight pl;
        memset(&pl, 0, sizeof(pl));
        pl.color.x = colorR; pl.color.y = colorG; pl.color.z = colorB;
        pl.intensity = intensity;
        pl.radius    = (float)j_num(c, "radius", 10.0);
        pl.casts_shadow = casts_shadow;
        pl.shadow_bias  = (float)j_num(c, "shadowBias", 0.0);
        jce_scene_set_point_light(s, e, &pl);
    } else if (ltype == 2) {
        JceSpotLight sl;
        memset(&sl, 0, sizeof(sl));
        sl.cookie_texture = JCE_TEXTURE_INVALID;
        sl.ies_lut_texture = JCE_TEXTURE_INVALID;
        sl.direction.x = dirX;
        sl.direction.y = dirY;
        sl.direction.z = dirZ;
        sl.color.x = colorR; sl.color.y = colorG; sl.color.z = colorB;
        sl.intensity = intensity;
        sl.radius    = (float)j_num(c, "radius", 10.0);
        float inner_deg = (float)j_num2(c, "innerConeDeg", "inner_cone_deg", 25.0);
        float outer_deg = (float)j_num2(c, "outerConeDeg", "outer_cone_deg", 35.0);
        sl.inner_cone_cos = cosf(inner_deg * JCE_DEG2RAD);
        sl.outer_cone_cos = cosf(outer_deg * JCE_DEG2RAD);
        /* P3-E.5 — cookie + IES profile (additive). */
        copy_str(sl.cookie_path, sizeof(sl.cookie_path), j_str(c, "cookiePath", ""));
        copy_str(sl.ies_path,    sizeof(sl.ies_path),    j_str(c, "iesPath", ""));
        sl.cookie_strength = (float)j_num(c, "cookieStrength", 0.0);
        sl.casts_shadow = casts_shadow;
        sl.shadow_bias  = (float)j_num(c, "shadowBias", 0.0);
        jce_scene_set_spot_light(s, e, &sl);
    } else {
        JceDirectionalLight dl;
        memset(&dl, 0, sizeof(dl));
        dl.cookie_texture = JCE_TEXTURE_INVALID;
        dl.direction.x = dirX;
        dl.direction.y = dirY;
        dl.direction.z = dirZ;
        dl.color.x = colorR; dl.color.y = colorG; dl.color.z = colorB;
        dl.intensity    = intensity;
        dl.casts_shadow = casts_shadow;
        /* P3-E.5 — directional cookie (data-side scaffold). */
        copy_str(dl.cookie_path, sizeof(dl.cookie_path), j_str(c, "cookiePath", ""));
        dl.cookie_strength = (float)j_num(c, "cookieStrength", 0.0);
        jce_scene_set_dir_light(s, e, &dl);
    }
}

void parse_lod_group(JceScene *s, JceEntity e, const cJSON *c)
{
    JceLodGroupComponent lg;
    memset(&lg, 0, sizeof(lg));
    lg.level_count = (int)j_num(c, "levelCount", 0);
    if (lg.level_count < 0) lg.level_count = 0;
    if (lg.level_count > JCE_LOD_COMP_MAX_LEVELS) lg.level_count = JCE_LOD_COMP_MAX_LEVELS;
    lg.hysteresis        = (float)j_num(c, "hysteresis", 0.05);
    lg.cull_when_too_far = j_bool(c, "cullWhenTooFar", false);
    lg.fade_width        = (float)j_num(c, "fadeWidth", 0.0);  /* P1 #6 */
    /* Octahedral impostor terminal LOD (P2 #10). */
    copy_str(lg.impostor_meta_path, sizeof(lg.impostor_meta_path),
             j_str(c, "impostorMetaPath", ""));
    lg.impostor_distance = (float)j_num(c, "impostorDistance", 0.0);
    char key[32];
    for (int i = 0; i < JCE_LOD_COMP_MAX_LEVELS; ++i) {
        snprintf(key, sizeof(key), "distance%d", i);
        lg.distances[i] = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "meshPath%d", i);
        copy_str(lg.level_mesh_paths[i], sizeof(lg.level_mesh_paths[i]), j_str(c, key, ""));
    }
    jce_scene_set_lod_group(s, e, &lg);
}

void parse_trail_renderer(JceScene *s, JceEntity e, const cJSON *c)
{
    JceTrailRendererComponent t;
    memset(&t, 0, sizeof(t));
    const char *mp = j_str(c, "materialPath", "");
    snprintf(t.material_path, sizeof(t.material_path), "%s", mp ? mp : "");
    t.time                = (float)j_num(c, "time", 1.0);
    t.min_vertex_distance = (float)j_num(c, "minVertexDistance", 0.1);
    t.width_start         = (float)j_num(c, "widthStart", 0.1);
    t.width_end           = (float)j_num(c, "widthEnd", 0.0);
    t.color_start[0]      = (float)j_num(c, "colorStartR", 1.0);
    t.color_start[1]      = (float)j_num(c, "colorStartG", 1.0);
    t.color_start[2]      = (float)j_num(c, "colorStartB", 1.0);
    t.color_start[3]      = (float)j_num(c, "colorStartA", 1.0);
    t.color_end[0]        = (float)j_num(c, "colorEndR", 1.0);
    t.color_end[1]        = (float)j_num(c, "colorEndG", 1.0);
    t.color_end[2]        = (float)j_num(c, "colorEndB", 1.0);
    t.color_end[3]        = (float)j_num(c, "colorEndA", 0.0);
    t.emitting            = j_bool(c, "emitting", true);
    t.autodestruct        = j_bool(c, "autodestruct", false);
    int n = (int)j_num(c, "pointCount", 0);
    if (n < 0) n = 0;
    if (n > JCE_TRAIL_MAX_POINTS) n = JCE_TRAIL_MAX_POINTS;
    t.point_count = n;
    char key[24];
    for (int i = 0; i < n; ++i) {
        snprintf(key, sizeof(key), "px%d", i); t.points[i][0] = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "py%d", i); t.points[i][1] = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "pz%d", i); t.points[i][2] = (float)j_num(c, key, 0.0);
    }
    jce_scene_set_trail_renderer(s, e, &t);
}

void parse_line_renderer(JceScene *s, JceEntity e, const cJSON *c)
{
    JceLineRendererComponent l;
    memset(&l, 0, sizeof(l));
    const char *mp = j_str(c, "materialPath", "");
    snprintf(l.material_path, sizeof(l.material_path), "%s", mp ? mp : "");
    l.width_start    = (float)j_num(c, "widthStart", 0.1);
    l.width_end      = (float)j_num(c, "widthEnd",   0.1);
    l.color_start[0] = (float)j_num(c, "colorStartR", 1.0);
    l.color_start[1] = (float)j_num(c, "colorStartG", 1.0);
    l.color_start[2] = (float)j_num(c, "colorStartB", 1.0);
    l.color_start[3] = (float)j_num(c, "colorStartA", 1.0);
    l.color_end[0]   = (float)j_num(c, "colorEndR", 1.0);
    l.color_end[1]   = (float)j_num(c, "colorEndG", 1.0);
    l.color_end[2]   = (float)j_num(c, "colorEndB", 1.0);
    l.color_end[3]   = (float)j_num(c, "colorEndA", 1.0);
    l.use_world_space = j_bool(c, "useWorldSpace", true);
    l.loop            = j_bool(c, "loop", false);
    int n = (int)j_num(c, "positionCount", 0);
    if (n < 0) n = 0;
    if (n > JCE_LINE_MAX_POINTS) n = JCE_LINE_MAX_POINTS;
    l.position_count = n;
    char key[24];
    for (int i = 0; i < n; ++i) {
        snprintf(key, sizeof(key), "px%d", i); l.positions[i][0] = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "py%d", i); l.positions[i][1] = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "pz%d", i); l.positions[i][2] = (float)j_num(c, key, 0.0);
    }
    jce_scene_set_line_renderer(s, e, &l);
}

void parse_reflection_probe(JceScene *s, JceEntity e, const cJSON *c)
{
    JceReflectionProbeComponent r;
    memset(&r, 0, sizeof(r));
    r.mode           = (int)j_num(c, "mode", 0);
    r.resolution     = (int)j_num(c, "resolution", 128);
    r.intensity      = (float)j_num(c, "intensity", 1.0);
    r.blend_distance = (float)j_num(c, "blendDistance", 0.0);
    r.box_size[0]    = (float)j_num(c, "boxSizeX", 10.0);
    r.box_size[1]    = (float)j_num(c, "boxSizeY", 10.0);
    r.box_size[2]    = (float)j_num(c, "boxSizeZ", 10.0);
    r.box_offset[0]  = (float)j_num(c, "boxOffX", 0.0);
    r.box_offset[1]  = (float)j_num(c, "boxOffY", 0.0);
    r.box_offset[2]  = (float)j_num(c, "boxOffZ", 0.0);
    r.near_clip      = (float)j_num(c, "nearClip", 0.3);
    r.far_clip       = (float)j_num(c, "farClip", 1000.0);
    const char *hp = j_str(c, "hdrPath", "");
    snprintf(r.custom_hdr_path, sizeof(r.custom_hdr_path), "%s", hp ? hp : "");
    const char *bp = j_str(c, "bakedCubemapPath", "");
    snprintf(r.baked_cubemap_path, sizeof(r.baked_cubemap_path), "%s", bp ? bp : "");
    r.box_projection = j_bool(c, "boxProjection", true);
    r.hdr            = j_bool(c, "hdr", true);
    jce_scene_set_reflection_probe(s, e, &r);
}

void parse_decal(JceScene *s, JceEntity e, const cJSON *c)
{
    JceDecalComponent d;
    memset(&d, 0, sizeof(d));
    const char *mp = j_str(c, "materialPath", "");
    snprintf(d.material_path, sizeof(d.material_path), "%s", mp ? mp : "");
    d.size[0]  = (float)j_num(c, "sizeX", 1.0);
    d.size[1]  = (float)j_num(c, "sizeY", 1.0);
    d.size[2]  = (float)j_num(c, "sizeZ", 1.0);
    d.pivot[0] = (float)j_num(c, "pivotX", 0.0);
    d.pivot[1] = (float)j_num(c, "pivotY", 0.0);
    d.pivot[2] = (float)j_num(c, "pivotZ", 0.0);
    d.color[0] = (float)j_num(c, "colorR", 1.0);
    d.color[1] = (float)j_num(c, "colorG", 1.0);
    d.color[2] = (float)j_num(c, "colorB", 1.0);
    d.color[3] = (float)j_num(c, "colorA", 1.0);
    d.opacity       = (float)j_num(c, "opacity", 1.0);
    d.draw_distance = (float)j_num(c, "drawDistance", 1000.0);
    d.fade_factor   = (float)j_num(c, "fadeFactor", 1.0);
    d.layer_mask    = (int)j_num(c, "layerMask", -1);
    jce_scene_set_decal(s, e, &d);
}

void parse_light_probe_group(JceScene *s, JceEntity e, const cJSON *c)
{
    JceLightProbeGroupComponent g;
    memset(&g, 0, sizeof(g));
    int n = (int)j_num(c, "probeCount", 0);
    if (n < 0) n = 0;
    if (n > JCE_LIGHT_PROBE_MAX) n = JCE_LIGHT_PROBE_MAX;
    g.probe_count = n;
    g.dering = j_bool(c, "dering", false);
    char key[24];
    for (int i = 0; i < n; ++i) {
        snprintf(key, sizeof(key), "px%d", i); g.positions[i][0] = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "py%d", i); g.positions[i][1] = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "pz%d", i); g.positions[i][2] = (float)j_num(c, key, 0.0);
    }
    /* Baked SH9 irradiance (P1-baked-gi-consume round 2: persistence). Absent
     * sh9Baked => false => legacy/unbaked scenes load byte-identically. */
    g.sh9_baked = j_bool(c, "sh9Baked", false);
    if (g.sh9_baked) {
        char shk[24];
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < 9; ++j) {
                snprintf(shk, sizeof(shk), "sh9_%d_%d_r", i, j); g.sh9[i][j][0] = (float)j_num(c, shk, 0.0);
                snprintf(shk, sizeof(shk), "sh9_%d_%d_g", i, j); g.sh9[i][j][1] = (float)j_num(c, shk, 0.0);
                snprintf(shk, sizeof(shk), "sh9_%d_%d_b", i, j); g.sh9[i][j][2] = (float)j_num(c, shk, 0.0);
            }
        }
    }
    jce_scene_set_light_probe_group(s, e, &g);
}

void parse_billboard_renderer(JceScene *s, JceEntity e, const cJSON *c)
{
    JceBillboardRendererComponent b; memset(&b, 0, sizeof b);
    copy_str(b.texture_path, sizeof b.texture_path, j_str(c, "texturePath", ""));
    b.mode = (int)j_num(c, "mode", 0);
    b.size[0] = (float)j_num(c, "sizeX", 1.0);
    b.size[1] = (float)j_num(c, "sizeY", 1.0);
    b.color[0] = (float)j_num(c, "colorR", 1.0);
    b.color[1] = (float)j_num(c, "colorG", 1.0);
    b.color[2] = (float)j_num(c, "colorB", 1.0);
    b.color[3] = (float)j_num(c, "colorA", 1.0);
    b.visible = j_bool(c, "visible", true);
    jce_scene_set_billboard_renderer(s, e, &b);
}

void parse_volume(JceScene *s, JceEntity e, const cJSON *props)
{
    JceVolumeComponent vc; memset(&vc, 0, sizeof vc);
    vc.shape          = (JceVolumeShape)(int)j_num(props, "shape",         0);
    vc.extents.x      = (float)j_num(props, "extentsX",     1.0);
    vc.extents.y      = (float)j_num(props, "extentsY",     1.0);
    vc.extents.z      = (float)j_num(props, "extentsZ",     1.0);
    vc.blend_distance = (float)j_num(props, "blendDistance", 1.0);
    vc.weight         = (float)j_num(props, "weight",        1.0);
    vc.is_global      = j_bool(props, "isGlobal", false);
    vc.profile.enabled_mask = (uint16_t)(int)j_num(props, "profileMask", 0);
    /* An absent profile key falls back to the neutral post-FX value.  Derive
     * those from the single authority (jce_postfx_default_params) instead of
     * restating them here — the editor's Add-Component initialiser reads the
     * same function, so all three sites stay in step. */
    const JcePostFXParams pfx_def = jce_postfx_default_params();
    vc.profile.values.exposure            = (float)j_num(props, "exposure",           pfx_def.exposure);
    vc.profile.values.gamma               = (float)j_num(props, "gamma",              pfx_def.gamma);
    vc.profile.values.bloom_threshold     = (float)j_num(props, "bloomThreshold",     pfx_def.bloom_threshold);
    vc.profile.values.bloom_intensity     = (float)j_num(props, "bloomIntensity",     pfx_def.bloom_intensity);
    vc.profile.values.fxaa_span_max       = (float)j_num(props, "fxaaSpanMax",        pfx_def.fxaa_span_max);
    vc.profile.values.fxaa_reduce_min     = (float)j_num(props, "fxaaReduceMin",      pfx_def.fxaa_reduce_min);
    vc.profile.values.fxaa_reduce_mul     = (float)j_num(props, "fxaaReduceMul",      pfx_def.fxaa_reduce_mul);
    vc.profile.values.vignette_intensity  = (float)j_num(props, "vignetteIntensity",  pfx_def.vignette_intensity);
    vc.profile.values.vignette_smoothness = (float)j_num(props, "vignetteSmoothness", pfx_def.vignette_smoothness);
    vc.profile.values.chromatic_strength  = (float)j_num(props, "chromaticStrength",  pfx_def.chromatic_strength);
    jce_scene_set_volume(s, e, &vc);
}

void parse_occlusion_portal(JceScene *s, JceEntity e, const cJSON *props)
{
    JceOcclusionPortalComponent op; memset(&op, 0, sizeof op);
    op.size.x   = (float)j_num(props, "sizeX",    1.0);
    op.size.y   = (float)j_num(props, "sizeY",    2.0);
    op.size.z   = (float)j_num(props, "sizeZ",    0.1);
    op.open     = j_bool(props, "open", true);
    op.portal_id = (int32_t)j_num(props, "portalId", 0);
    jce_scene_set_occlusion_portal(s, e, &op);
}

static void ser_mesh_renderer(const JceMeshRenderer *mr, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "MeshRenderer");
    cJSON_AddStringToObject(o, "meshPath", mr->mesh_path);
    cJSON_AddStringToObject(o, "materialPath", mr->material_path);
    cJSON_AddNumberToObject(o, "meshShape", mr->mesh_shape);
    cJSON_AddNumberToObject(o, "baseColorR", mr->base_color[0]);
    cJSON_AddNumberToObject(o, "baseColorG", mr->base_color[1]);
    cJSON_AddNumberToObject(o, "baseColorB", mr->base_color[2]);
    cJSON_AddNumberToObject(o, "baseColorA", mr->base_color[3]);
    cJSON_AddNumberToObject(o, "metallic",  mr->metallic);
    cJSON_AddNumberToObject(o, "roughness", mr->roughness);
    cJSON_AddNumberToObject(o, "emissiveR", mr->emissive[0]);
    cJSON_AddNumberToObject(o, "emissiveG", mr->emissive[1]);
    cJSON_AddNumberToObject(o, "emissiveB", mr->emissive[2]);
    cJSON_AddNumberToObject(o, "normalScale", mr->normal_scale);
    cJSON_AddNumberToObject(o, "aoStrength",  mr->ao_strength);
    cJSON_AddNumberToObject(o, "alphaMode",   mr->alpha_mode);
    cJSON_AddNumberToObject(o, "alphaCutoff", mr->alpha_cutoff);
    cJSON_AddBoolToObject(o, "doubleSided", mr->double_sided);
    cJSON_AddBoolToObject(o, "castsShadow",    !mr->shadow_cast_off);
    cJSON_AddBoolToObject(o, "receivesShadow", !mr->shadow_receive_off);
    if (mr->albedo_tex[0])   cJSON_AddStringToObject(o, "albedoTex",   mr->albedo_tex);
    if (mr->mr_tex[0])       cJSON_AddStringToObject(o, "mrTex",       mr->mr_tex);
    if (mr->normal_tex[0])   cJSON_AddStringToObject(o, "normalTex",   mr->normal_tex);
    if (mr->ao_tex[0])       cJSON_AddStringToObject(o, "aoTex",       mr->ao_tex);
    if (mr->emissive_tex[0]) cJSON_AddStringToObject(o, "emissiveTex", mr->emissive_tex);
    /* Toon keys are absent when toon=false; parser defaults all knobs to 0/off. */
    if (mr->toon) {
        cJSON_AddBoolToObject  (o, "toon",             mr->toon);
        cJSON_AddNumberToObject(o, "toonBands",        mr->toon_bands);
        cJSON_AddNumberToObject(o, "toonRimPower",     mr->rim_power);
        cJSON_AddNumberToObject(o, "toonRimIntensity", mr->rim_intensity);
        cJSON_AddNumberToObject(o, "toonRimColorR",    mr->rim_color[0]);
        cJSON_AddNumberToObject(o, "toonRimColorG",    mr->rim_color[1]);
        cJSON_AddNumberToObject(o, "toonRimColorB",    mr->rim_color[2]);
        cJSON_AddNumberToObject(o, "toonOutlineWidth", mr->outline_width);
        cJSON_AddNumberToObject(o, "toonOutlineColorR", mr->outline_color[0]);
        cJSON_AddNumberToObject(o, "toonOutlineColorG", mr->outline_color[1]);
        cJSON_AddNumberToObject(o, "toonOutlineColorB", mr->outline_color[2]);
    }
    cJSON_AddItemToArray(arr, o);
}

static void ser_volume(const JceVolumeComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Volume");
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "shape",              (double)c->shape);
    cJSON_AddNumberToObject(p, "extentsX",           c->extents.x);
    cJSON_AddNumberToObject(p, "extentsY",           c->extents.y);
    cJSON_AddNumberToObject(p, "extentsZ",           c->extents.z);
    cJSON_AddNumberToObject(p, "blendDistance",      c->blend_distance);
    cJSON_AddNumberToObject(p, "weight",             c->weight);
    cJSON_AddBoolToObject  (p, "isGlobal",           c->is_global);
    cJSON_AddNumberToObject(p, "profileMask",        (double)c->profile.enabled_mask);
    cJSON_AddNumberToObject(p, "exposure",           c->profile.values.exposure);
    cJSON_AddNumberToObject(p, "gamma",              c->profile.values.gamma);
    cJSON_AddNumberToObject(p, "bloomThreshold",     c->profile.values.bloom_threshold);
    cJSON_AddNumberToObject(p, "bloomIntensity",     c->profile.values.bloom_intensity);
    cJSON_AddNumberToObject(p, "fxaaSpanMax",        c->profile.values.fxaa_span_max);
    cJSON_AddNumberToObject(p, "fxaaReduceMin",      c->profile.values.fxaa_reduce_min);
    cJSON_AddNumberToObject(p, "fxaaReduceMul",      c->profile.values.fxaa_reduce_mul);
    cJSON_AddNumberToObject(p, "vignetteIntensity",  c->profile.values.vignette_intensity);
    cJSON_AddNumberToObject(p, "vignetteSmoothness", c->profile.values.vignette_smoothness);
    cJSON_AddNumberToObject(p, "chromaticStrength",  c->profile.values.chromatic_strength);
    cJSON_AddItemToObject(o, "properties", p);
    cJSON_AddItemToArray(arr, o);
}

static void ser_occlusion_portal(const JceOcclusionPortalComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "OcclusionPortal");
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "sizeX",    c->size.x);
    cJSON_AddNumberToObject(p, "sizeY",    c->size.y);
    cJSON_AddNumberToObject(p, "sizeZ",    c->size.z);
    cJSON_AddBoolToObject  (p, "open",     c->open);
    cJSON_AddNumberToObject(p, "portalId", (double)c->portal_id);
    cJSON_AddItemToObject(o, "properties", p);
    cJSON_AddItemToArray(arr, o);
}

/* Unified light serialization: always emits "Light" with lightType sub-field. */
static void ser_light_unified(JceScene *s, JceEntity e, cJSON *arr)
{
    JceDirectionalLight *dl = jce_scene_get_dir_light(s, e);
    if (dl) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "Light");
        cJSON_AddNumberToObject(o, "colorR", dl->color.x);
        cJSON_AddNumberToObject(o, "colorG", dl->color.y);
        cJSON_AddNumberToObject(o, "colorB", dl->color.z);
        cJSON_AddNumberToObject(o, "intensity", dl->intensity);
        cJSON_AddNumberToObject(o, "lightType", 0);
        cJSON_AddNumberToObject(o, "dirX", dl->direction.x);
        cJSON_AddNumberToObject(o, "dirY", dl->direction.y);
        cJSON_AddNumberToObject(o, "dirZ", dl->direction.z);
        cJSON_AddBoolToObject(o, "castsShadow", dl->casts_shadow);
        /* P3-E.5 — emit cookie fields only when set (omit-on-default). */
        if (dl->cookie_path[0] != '\0')
            cJSON_AddStringToObject(o, "cookiePath", dl->cookie_path);
        if (dl->cookie_strength > 0.0f)
            cJSON_AddNumberToObject(o, "cookieStrength", dl->cookie_strength);
        cJSON_AddItemToArray(arr, o);
        return;
    }
    JcePointLight *pl = jce_scene_get_point_light(s, e);
    if (pl) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "Light");
        cJSON_AddNumberToObject(o, "colorR", pl->color.x);
        cJSON_AddNumberToObject(o, "colorG", pl->color.y);
        cJSON_AddNumberToObject(o, "colorB", pl->color.z);
        cJSON_AddNumberToObject(o, "intensity", pl->intensity);
        cJSON_AddNumberToObject(o, "lightType", 1);
        cJSON_AddNumberToObject(o, "radius", pl->radius);
        cJSON_AddBoolToObject(o, "castsShadow", pl->casts_shadow);
        if (pl->shadow_bias != 0.0f)
            cJSON_AddNumberToObject(o, "shadowBias", pl->shadow_bias);
        cJSON_AddItemToArray(arr, o);
        return;
    }
    JceSpotLight *sl = jce_scene_get_spot_light(s, e);
    if (sl) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "Light");
        cJSON_AddNumberToObject(o, "colorR", sl->color.x);
        cJSON_AddNumberToObject(o, "colorG", sl->color.y);
        cJSON_AddNumberToObject(o, "colorB", sl->color.z);
        cJSON_AddNumberToObject(o, "intensity", sl->intensity);
        cJSON_AddNumberToObject(o, "lightType", 2);
        cJSON_AddNumberToObject(o, "radius", sl->radius);
        cJSON_AddNumberToObject(o, "dirX", sl->direction.x);
        cJSON_AddNumberToObject(o, "dirY", sl->direction.y);
        cJSON_AddNumberToObject(o, "dirZ", sl->direction.z);
        float inner_deg = acosf(sl->inner_cone_cos) * JCE_RAD2DEG;
        float outer_deg = acosf(sl->outer_cone_cos) * JCE_RAD2DEG;
        cJSON_AddNumberToObject(o, "innerConeDeg", inner_deg);
        cJSON_AddNumberToObject(o, "outerConeDeg", outer_deg);
        cJSON_AddBoolToObject(o, "castsShadow", sl->casts_shadow);
        if (sl->shadow_bias != 0.0f)
            cJSON_AddNumberToObject(o, "shadowBias", sl->shadow_bias);
        /* P3-E.5 — emit cookie + IES paths only when set. */
        if (sl->cookie_path[0] != '\0')
            cJSON_AddStringToObject(o, "cookiePath", sl->cookie_path);
        if (sl->ies_path[0] != '\0')
            cJSON_AddStringToObject(o, "iesPath", sl->ies_path);
        if (sl->cookie_strength > 0.0f)
            cJSON_AddNumberToObject(o, "cookieStrength", sl->cookie_strength);
        cJSON_AddItemToArray(arr, o);
    }
}

static void ser_lod_group(const JceLodGroupComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "LODGroup");
    cJSON_AddNumberToObject(o, "levelCount", c->level_count);
    cJSON_AddNumberToObject(o, "hysteresis", c->hysteresis);
    cJSON_AddBoolToObject  (o, "cullWhenTooFar", c->cull_when_too_far);
    cJSON_AddNumberToObject(o, "fadeWidth", c->fade_width);  /* P1 #6 */
    /* Octahedral impostor terminal LOD (P2 #10). */
    cJSON_AddStringToObject(o, "impostorMetaPath", c->impostor_meta_path);
    cJSON_AddNumberToObject(o, "impostorDistance", c->impostor_distance);
    char key[32];
    for (int i = 0; i < JCE_LOD_COMP_MAX_LEVELS; ++i) {
        snprintf(key, sizeof(key), "distance%d", i);
        cJSON_AddNumberToObject(o, key, c->distances[i]);
        snprintf(key, sizeof(key), "meshPath%d", i);
        cJSON_AddStringToObject(o, key, c->level_mesh_paths[i]);
    }
    cJSON_AddItemToArray(arr, o);
}

static void ser_trail_renderer(const JceTrailRendererComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "TrailRenderer");
    cJSON_AddStringToObject(o, "materialPath", c->material_path);
    cJSON_AddNumberToObject(o, "time", c->time);
    cJSON_AddNumberToObject(o, "minVertexDistance", c->min_vertex_distance);
    cJSON_AddNumberToObject(o, "widthStart", c->width_start);
    cJSON_AddNumberToObject(o, "widthEnd",   c->width_end);
    cJSON_AddNumberToObject(o, "colorStartR", c->color_start[0]);
    cJSON_AddNumberToObject(o, "colorStartG", c->color_start[1]);
    cJSON_AddNumberToObject(o, "colorStartB", c->color_start[2]);
    cJSON_AddNumberToObject(o, "colorStartA", c->color_start[3]);
    cJSON_AddNumberToObject(o, "colorEndR", c->color_end[0]);
    cJSON_AddNumberToObject(o, "colorEndG", c->color_end[1]);
    cJSON_AddNumberToObject(o, "colorEndB", c->color_end[2]);
    cJSON_AddNumberToObject(o, "colorEndA", c->color_end[3]);
    cJSON_AddBoolToObject  (o, "emitting", c->emitting);
    cJSON_AddBoolToObject  (o, "autodestruct", c->autodestruct);
    int n = c->point_count;
    if (n < 0) n = 0; if (n > JCE_TRAIL_MAX_POINTS) n = JCE_TRAIL_MAX_POINTS;
    cJSON_AddNumberToObject(o, "pointCount", n);
    char key[24];
    for (int i = 0; i < n; ++i) {
        snprintf(key, sizeof(key), "px%d", i); cJSON_AddNumberToObject(o, key, c->points[i][0]);
        snprintf(key, sizeof(key), "py%d", i); cJSON_AddNumberToObject(o, key, c->points[i][1]);
        snprintf(key, sizeof(key), "pz%d", i); cJSON_AddNumberToObject(o, key, c->points[i][2]);
    }
    cJSON_AddItemToArray(arr, o);
}

static void ser_line_renderer(const JceLineRendererComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "LineRenderer");
    cJSON_AddStringToObject(o, "materialPath", c->material_path);
    cJSON_AddNumberToObject(o, "widthStart", c->width_start);
    cJSON_AddNumberToObject(o, "widthEnd",   c->width_end);
    cJSON_AddNumberToObject(o, "colorStartR", c->color_start[0]);
    cJSON_AddNumberToObject(o, "colorStartG", c->color_start[1]);
    cJSON_AddNumberToObject(o, "colorStartB", c->color_start[2]);
    cJSON_AddNumberToObject(o, "colorStartA", c->color_start[3]);
    cJSON_AddNumberToObject(o, "colorEndR", c->color_end[0]);
    cJSON_AddNumberToObject(o, "colorEndG", c->color_end[1]);
    cJSON_AddNumberToObject(o, "colorEndB", c->color_end[2]);
    cJSON_AddNumberToObject(o, "colorEndA", c->color_end[3]);
    cJSON_AddBoolToObject  (o, "useWorldSpace", c->use_world_space);
    cJSON_AddBoolToObject  (o, "loop", c->loop);
    int n = c->position_count;
    if (n < 0) n = 0; if (n > JCE_LINE_MAX_POINTS) n = JCE_LINE_MAX_POINTS;
    cJSON_AddNumberToObject(o, "positionCount", n);
    char key[24];
    for (int i = 0; i < n; ++i) {
        snprintf(key, sizeof(key), "px%d", i); cJSON_AddNumberToObject(o, key, c->positions[i][0]);
        snprintf(key, sizeof(key), "py%d", i); cJSON_AddNumberToObject(o, key, c->positions[i][1]);
        snprintf(key, sizeof(key), "pz%d", i); cJSON_AddNumberToObject(o, key, c->positions[i][2]);
    }
    cJSON_AddItemToArray(arr, o);
}

static void ser_reflection_probe(const JceReflectionProbeComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "ReflectionProbe");
    cJSON_AddNumberToObject(o, "mode", c->mode);
    cJSON_AddNumberToObject(o, "resolution", c->resolution);
    cJSON_AddNumberToObject(o, "intensity", c->intensity);
    cJSON_AddNumberToObject(o, "blendDistance", c->blend_distance);
    cJSON_AddNumberToObject(o, "boxSizeX", c->box_size[0]);
    cJSON_AddNumberToObject(o, "boxSizeY", c->box_size[1]);
    cJSON_AddNumberToObject(o, "boxSizeZ", c->box_size[2]);
    cJSON_AddNumberToObject(o, "boxOffX",  c->box_offset[0]);
    cJSON_AddNumberToObject(o, "boxOffY",  c->box_offset[1]);
    cJSON_AddNumberToObject(o, "boxOffZ",  c->box_offset[2]);
    cJSON_AddNumberToObject(o, "nearClip", c->near_clip);
    cJSON_AddNumberToObject(o, "farClip",  c->far_clip);
    cJSON_AddStringToObject(o, "hdrPath",  c->custom_hdr_path);
    cJSON_AddStringToObject(o, "bakedCubemapPath", c->baked_cubemap_path);
    cJSON_AddBoolToObject  (o, "boxProjection", c->box_projection);
    cJSON_AddBoolToObject  (o, "hdr", c->hdr);
    cJSON_AddItemToArray(arr, o);
}

static void ser_decal(const JceDecalComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Decal");
    cJSON_AddStringToObject(o, "materialPath", c->material_path);
    cJSON_AddNumberToObject(o, "sizeX", c->size[0]);
    cJSON_AddNumberToObject(o, "sizeY", c->size[1]);
    cJSON_AddNumberToObject(o, "sizeZ", c->size[2]);
    cJSON_AddNumberToObject(o, "pivotX", c->pivot[0]);
    cJSON_AddNumberToObject(o, "pivotY", c->pivot[1]);
    cJSON_AddNumberToObject(o, "pivotZ", c->pivot[2]);
    cJSON_AddNumberToObject(o, "colorR", c->color[0]);
    cJSON_AddNumberToObject(o, "colorG", c->color[1]);
    cJSON_AddNumberToObject(o, "colorB", c->color[2]);
    cJSON_AddNumberToObject(o, "colorA", c->color[3]);
    cJSON_AddNumberToObject(o, "opacity", c->opacity);
    cJSON_AddNumberToObject(o, "drawDistance", c->draw_distance);
    cJSON_AddNumberToObject(o, "fadeFactor",   c->fade_factor);
    cJSON_AddNumberToObject(o, "layerMask",    c->layer_mask);
    cJSON_AddItemToArray(arr, o);
}

static void ser_light_probe_group(const JceLightProbeGroupComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "LightProbeGroup");
    int n = c->probe_count;
    if (n < 0) n = 0; if (n > JCE_LIGHT_PROBE_MAX) n = JCE_LIGHT_PROBE_MAX;
    cJSON_AddNumberToObject(o, "probeCount", n);
    cJSON_AddBoolToObject  (o, "dering", c->dering);
    char key[24];
    for (int i = 0; i < n; ++i) {
        snprintf(key, sizeof(key), "px%d", i); cJSON_AddNumberToObject(o, key, c->positions[i][0]);
        snprintf(key, sizeof(key), "py%d", i); cJSON_AddNumberToObject(o, key, c->positions[i][1]);
        snprintf(key, sizeof(key), "pz%d", i); cJSON_AddNumberToObject(o, key, c->positions[i][2]);
    }
    /* Baked SH9 irradiance (P1-baked-gi-consume round 2: persistence). Only
     * emitted when baked, so unbaked groups serialize byte-identically. */
    cJSON_AddBoolToObject(o, "sh9Baked", c->sh9_baked);
    if (c->sh9_baked) {
        char shk[24];
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < 9; ++j) {
                snprintf(shk, sizeof(shk), "sh9_%d_%d_r", i, j); cJSON_AddNumberToObject(o, shk, c->sh9[i][j][0]);
                snprintf(shk, sizeof(shk), "sh9_%d_%d_g", i, j); cJSON_AddNumberToObject(o, shk, c->sh9[i][j][1]);
                snprintf(shk, sizeof(shk), "sh9_%d_%d_b", i, j); cJSON_AddNumberToObject(o, shk, c->sh9[i][j][2]);
            }
        }
    }
    cJSON_AddItemToArray(arr, o);
}

static void ser_billboard_renderer(const JceBillboardRendererComponent *b, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "BillboardRenderer");
    cJSON_AddStringToObject(o, "texturePath", b->texture_path);
    cJSON_AddNumberToObject(o, "mode", b->mode);
    cJSON_AddNumberToObject(o, "sizeX", b->size[0]);
    cJSON_AddNumberToObject(o, "sizeY", b->size[1]);
    cJSON_AddNumberToObject(o, "colorR", b->color[0]);
    cJSON_AddNumberToObject(o, "colorG", b->color[1]);
    cJSON_AddNumberToObject(o, "colorB", b->color[2]);
    cJSON_AddNumberToObject(o, "colorA", b->color[3]);
    cJSON_AddBoolToObject  (o, "visible", b->visible);
    cJSON_AddItemToArray(arr, o);
}

static void ser_skybox(const JceSkyboxComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Skybox");
    cJSON_AddStringToObject(o, "hdrPath", c->hdr_path);
    cJSON_AddNumberToObject(o, "rotation", c->rotation);
    cJSON_AddNumberToObject(o, "exposure", c->exposure);
    cJSON_AddBoolToObject(o, "useAsIbl", c->use_as_ibl);
    cJSON_AddItemToArray(arr, o);
}

static void ser_sprite_renderer(const JceSpriteRendererComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SpriteRenderer");
    cJSON_AddStringToObject(o, "spritePath", c->sprite_path);
    cJSON_AddNumberToObject(o, "colorR", c->color[0]);
    cJSON_AddNumberToObject(o, "colorG", c->color[1]);
    cJSON_AddNumberToObject(o, "colorB", c->color[2]);
    cJSON_AddNumberToObject(o, "colorA", c->color[3]);
    cJSON_AddBoolToObject(o, "flipX", c->flip_x);
    cJSON_AddBoolToObject(o, "flipY", c->flip_y);
    cJSON_AddNumberToObject(o, "sortingOrder", c->sorting_order);
    cJSON_AddItemToArray(arr, o);
}

void serw_mesh_renderer(JceScene *s, JceEntity e, cJSON *arr)
{
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(s, e);
    if (mr) ser_mesh_renderer(mr, arr);
}

/* Unified Light format for all light types (dir > point > spot). */
void serw_light_unified(JceScene *s, JceEntity e, cJSON *arr)
{
    ser_light_unified(s, e, arr);
}

void serw_skybox(JceScene *s, JceEntity e, cJSON *arr)
{
    JceSkyboxComponent *c = jce_scene_get_skybox(s, e);
    if (c) ser_skybox(c, arr);
}

void serw_sprite_renderer(JceScene *s, JceEntity e, cJSON *arr)
{
    JceSpriteRendererComponent *c = jce_scene_get_sprite_renderer(s, e);
    if (c) ser_sprite_renderer(c, arr);
}

void serw_lod_group(JceScene *s, JceEntity e, cJSON *arr)
{
    JceLodGroupComponent *c = jce_scene_get_lod_group(s, e);
    if (c) ser_lod_group(c, arr);
}

void serw_trail_renderer(JceScene *s, JceEntity e, cJSON *arr)
{
    JceTrailRendererComponent *c = jce_scene_get_trail_renderer(s, e);
    if (c) ser_trail_renderer(c, arr);
}

void serw_line_renderer(JceScene *s, JceEntity e, cJSON *arr)
{
    JceLineRendererComponent *c = jce_scene_get_line_renderer(s, e);
    if (c) ser_line_renderer(c, arr);
}

void serw_reflection_probe(JceScene *s, JceEntity e, cJSON *arr)
{
    JceReflectionProbeComponent *c = jce_scene_get_reflection_probe(s, e);
    if (c) ser_reflection_probe(c, arr);
}

void serw_decal(JceScene *s, JceEntity e, cJSON *arr)
{
    JceDecalComponent *c = jce_scene_get_decal(s, e);
    if (c) ser_decal(c, arr);
}

void serw_light_probe_group(JceScene *s, JceEntity e, cJSON *arr)
{
    JceLightProbeGroupComponent *c = jce_scene_get_light_probe_group(s, e);
    if (c) ser_light_probe_group(c, arr);
}

void serw_billboard_renderer(JceScene *s, JceEntity e, cJSON *arr)
{
    JceBillboardRendererComponent *c = jce_scene_get_billboard_renderer(s, e);
    if (c) ser_billboard_renderer(c, arr);
}

void serw_volume(JceScene *s, JceEntity e, cJSON *arr)
{
    JceVolumeComponent *c = jce_scene_get_volume(s, e);
    if (c) ser_volume(c, arr);
}

void serw_occlusion_portal(JceScene *s, JceEntity e, cJSON *arr)
{
    JceOcclusionPortalComponent *c = jce_scene_get_occlusion_portal(s, e);
    if (c) ser_occlusion_portal(c, arr);
}

