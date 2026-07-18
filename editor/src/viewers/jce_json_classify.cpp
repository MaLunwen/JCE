/*
 * jce_json_classify.cpp  Content-based JSON asset-type classification.
 *
 * See header for the format catalog.  Every discriminator below mirrors the
 * corresponding engine/editor PARSER (not just observed files), so the badge
 * agrees with what the engine would actually do with the file.
 */

#include "jce_json_classify.h"

#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>

#include <ctype.h>
#include <string.h>

extern "C" {
#include <jce/os/core/jce_json.h>
}

/* ── path helpers ───────────────────────────────────────────────────── */

static bool path_ends_with(const char *path, const char *suffix)
{
    if (!path || !suffix) return false;
    size_t pl = strlen(path), sl = strlen(suffix);
    if (pl < sl) return false;
    for (size_t i = 0; i < sl; i++) {
        char a = (char)tolower((unsigned char)path[pl - sl + i]);
        char b = (char)tolower((unsigned char)suffix[i]);
        if (a != b) return false;
    }
    return true;
}

static bool path_has_dir(const char *path, const char *dir /* e.g. "i18n" */)
{
    if (!path || !dir) return false;
    size_t dl = strlen(dir);
    for (const char *p = path; *p; p++) {
        if ((*p == '/' || *p == '\\') &&
            strncmp(p + 1, dir, dl) == 0 &&
            (p[1 + dl] == '/' || p[1 + dl] == '\\'))
            return true;
    }
    return false;
}

/* ── parsed-root classification ─────────────────────────────────────── */

static bool has_key(const JceJson *obj, const char *key)
{
    return jce_json_get(obj, key) != NULL;
}

static bool schema_prefix(const JceJson *root, const char *prefix)
{
    /* render_settings uses "$schema", editor-preferences uses "_schema"
     * (numeric) — only the string "$schema" family is dispatched here. */
    const char *s = jce_json_get_string(root, "$schema", NULL);
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}

static JceJsonKind classify_root(const JceJson *root, const char *path)
{
    if (!jce_json_is_object(root)) {
        /* A bare top-level array has no known schema in JCE. */
        return JCE_JSONK_GENERIC;
    }

    /* Prefab BEFORE scene: both carry the {contract} envelope. */
    {
        const JceJson *prefab = jce_json_get(root, "prefab");
        if ((prefab && jce_json_is_object(prefab) && has_key(prefab, "root"))
            || has_key(root, "$variantOf"))
            return JCE_JSONK_PREFAB;
    }

    /* Scene, all three accepted shapes (resolve_entities order):
     * wrapped {contract,scene:{entities}}, bare {scene:{entities}},
     * flat {entities:[...]}. */
    {
        const JceJson *scene = jce_json_get(root, "scene");
        if (scene && jce_json_is_object(scene) && has_key(scene, "entities"))
            return JCE_JSONK_SCENE;
        const JceJson *ents = jce_json_get(root, "entities");
        if (ents && jce_json_is_array(ents))
            return JCE_JSONK_SCENE;
    }

    /* $schema string dispatch (exact families the loaders check). */
    if (schema_prefix(root, "jce.rendersettings")) return JCE_JSONK_RENDER_SETTINGS;
    if (schema_prefix(root, "jce.rp."))            return JCE_JSONK_RENDER_PIPELINE;
    if (schema_prefix(root, "jce.physmat"))        return JCE_JSONK_PHYSMAT;
    if (schema_prefix(root, "jce.physlayers"))     return JCE_JSONK_PHYS_LAYERS;

    /* Anim state machine: states + transitions arrays (jce_anim_sm.c). */
    {
        const JceJson *st = jce_json_get(root, "states");
        const JceJson *tr = jce_json_get(root, "transitions");
        if (st && jce_json_is_array(st) && tr && jce_json_is_array(tr))
            return JCE_JSONK_ANIM_SM;
    }

    /* Avatar mask: weights[] items carrying "weight" (jce_avatar_mask.c). */
    {
        const JceJson *w = jce_json_get(root, "weights");
        if (w && jce_json_is_array(w)) {
            const JceJson *w0 = jce_json_array_at(w, 0);
            if (!w0 || has_key(w0, "weight"))
                return JCE_JSONK_AVATAR_MASK;
        }
    }

    /* Particles: maxParticles OR emitRate+lifetimeMin (jce_particles.c). */
    if (has_key(root, "maxParticles")
        || (has_key(root, "emitRate") && has_key(root, "lifetimeMin")))
        return JCE_JSONK_PARTICLES;

    /* Render pipeline without $schema (v1 legacy): the enable_* cluster. */
    if (has_key(root, "enable_csm") || has_key(root, "enable_ssao"))
        return JCE_JSONK_RENDER_PIPELINE;

    /* Render settings without $schema. */
    if (has_key(root, "shadowQuality") && has_key(root, "lodBias"))
        return JCE_JSONK_RENDER_SETTINGS;

    /* Physics layer matrix: names[] + matrix[] (jce_physics_layers.c). */
    {
        const JceJson *n = jce_json_get(root, "names");
        const JceJson *m = jce_json_get(root, "matrix");
        if (n && jce_json_is_array(n) && m && jce_json_is_array(m))
            return JCE_JSONK_PHYS_LAYERS;
    }

    /* Material: properties{}+shader, or PBR map keys at root
     * (jce_pbr_material.c accepts both shapes). */
    if ((has_key(root, "properties") && has_key(root, "shader"))
        || has_key(root, "albedoMap") || has_key(root, "baseColorFactor"))
        return JCE_JSONK_MATERIAL;

    /* Impostor meta (jce_impostor.c). */
    {
        const char *t = jce_json_get_string(root, "type", NULL);
        if ((t && strcmp(t, "impostor") == 0)
            || (has_key(root, "atlasPath") && has_key(root, "gridN")))
            return JCE_JSONK_IMPOSTOR;
    }

    /* Project manifest (jce_project.c). */
    if (has_key(root, "startup_scene")
        || (has_key(root, "source_assets") && has_key(root, "cooked_assets")))
        return JCE_JSONK_PROJECT;

    /* Per-project settings sections (jce_project_settings.cpp). */
    if (has_key(root, "quality") && has_key(root, "tags_layers"))
        return JCE_JSONK_PROJECT_SETTINGS;

    /* Settings/TagsAndLayers.json (jce_scene_tags_layers.c). */
    {
        const JceJson *t = jce_json_get(root, "tags");
        const JceJson *l = jce_json_get(root, "layers");
        if (t && jce_json_is_array(t) && l && jce_json_is_array(l))
            return JCE_JSONK_TAGS_LAYERS;
    }

    /* Node graphs: shadergraph "nextId" / VFX "next_id" / matgraph — all
     * carry nodes[] + links[]. */
    if (has_key(root, "nodes") && has_key(root, "links"))
        return JCE_JSONK_GRAPH;

    /* Editor preferences (~/.jce/editor-preferences.json). */
    if (has_key(root, "_schema") && has_key(root, "language"))
        return JCE_JSONK_EDITOR_PREFS;

    /* Sprite sheets: no self-identifying keys — path hint only. */
    if (path_ends_with(path, ".sprites.json"))
        return JCE_JSONK_SPRITES;

    /* i18n string tables: flat string->string maps under an i18n/ dir. */
    if (path_has_dir(path, "i18n"))
        return JCE_JSONK_I18N;

    return JCE_JSONK_GENERIC;
}

/* ── raw fallback for truncated reads ───────────────────────────────────
 * A capped read of a very large file will not parse; sniff the head for the
 * same discriminating keys instead so the badge still shows.  Only keys that
 * appear near the TOP of their format are used. */

static JceJsonKind classify_raw(const char *text, int len, const char *path)
{
    (void)len;
    if (strstr(text, "\"prefab\""))                          return JCE_JSONK_PREFAB;
    if (strstr(text, "\"entities\"") || strstr(text, "\"jce.scene\""))
        return JCE_JSONK_SCENE;
    if (strstr(text, "\"states\"") && strstr(text, "\"transitions\""))
        return JCE_JSONK_ANIM_SM;
    if (strstr(text, "\"maxParticles\""))                    return JCE_JSONK_PARTICLES;
    if (path_ends_with(path, ".sprites.json"))               return JCE_JSONK_SPRITES;
    if (path_has_dir(path, "i18n"))                          return JCE_JSONK_I18N;
    return JCE_JSONK_INVALID;
}

/* ── public API ─────────────────────────────────────────────────────── */

JceJsonKind jce_json_classify(const char *text, int len, const char *path)
{
    if (!text || len <= 0) return JCE_JSONK_NOT_JSON;

    /* Skip UTF-8 BOM + leading whitespace; JSON must open with { or [. */
    const char *p = text;
    int n = len;
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB
        && (unsigned char)p[2] == 0xBF) { p += 3; n -= 3; }
    while (n > 0 && isspace((unsigned char)*p)) { p++; n--; }
    if (n <= 0 || (*p != '{' && *p != '['))
        return JCE_JSONK_NOT_JSON;

    JceJson *root = jce_json_parse(p, (size_t)n);
    if (!root)
        return classify_raw(text, len, path);

    JceJsonKind k = classify_root(root, path);
    jce_json_free(root);
    return k;
}

const char *jce_json_kind_label(JceJsonKind k)
{
    switch (k) {
    case JCE_JSONK_INVALID:          return jce_editor_i18n_or("jsonKind.invalid",         "Invalid JSON");
    case JCE_JSONK_GENERIC:          return jce_editor_i18n_or("jsonKind.generic",         "JSON");
    case JCE_JSONK_SCENE:            return jce_editor_i18n_or("jsonKind.scene",           "Scene");
    case JCE_JSONK_PREFAB:           return jce_editor_i18n_or("jsonKind.prefab",          "Prefab");
    case JCE_JSONK_ANIM_SM:          return jce_editor_i18n_or("jsonKind.animSm",          "Anim State Machine");
    case JCE_JSONK_AVATAR_MASK:      return jce_editor_i18n_or("jsonKind.avatarMask",      "Avatar Mask");
    case JCE_JSONK_PARTICLES:        return jce_editor_i18n_or("jsonKind.particles",       "Particle System");
    case JCE_JSONK_MATERIAL:         return jce_editor_i18n_or("jsonKind.material",        "Material");
    case JCE_JSONK_PHYSMAT:          return jce_editor_i18n_or("jsonKind.physmat",         "Physics Material");
    case JCE_JSONK_RENDER_SETTINGS:  return jce_editor_i18n_or("jsonKind.renderSettings",  "Render Settings");
    case JCE_JSONK_RENDER_PIPELINE:  return jce_editor_i18n_or("jsonKind.renderPipeline",  "Render Pipeline");
    case JCE_JSONK_PHYS_LAYERS:      return jce_editor_i18n_or("jsonKind.physLayers",      "Physics Layers");
    case JCE_JSONK_PROJECT:          return jce_editor_i18n_or("jsonKind.project",         "Project");
    case JCE_JSONK_PROJECT_SETTINGS: return jce_editor_i18n_or("jsonKind.projectSettings", "Project Settings");
    case JCE_JSONK_TAGS_LAYERS:      return jce_editor_i18n_or("jsonKind.tagsLayers",      "Tags & Layers");
    case JCE_JSONK_IMPOSTOR:         return jce_editor_i18n_or("jsonKind.impostor",        "Impostor Meta");
    case JCE_JSONK_SPRITES:          return jce_editor_i18n_or("jsonKind.sprites",         "Sprite Sheet");
    case JCE_JSONK_GRAPH:            return jce_editor_i18n_or("jsonKind.graph",           "Node Graph");
    case JCE_JSONK_I18N:             return jce_editor_i18n_or("jsonKind.i18n",            "String Table");
    case JCE_JSONK_EDITOR_PREFS:     return jce_editor_i18n_or("jsonKind.editorPrefs",     "Editor Preferences");
    default:                         return "";
    }
}

unsigned int jce_json_kind_color(JceJsonKind k)
{
    switch (k) {
    case JCE_JSONK_INVALID:          return IM_COL32(150,  55,  55, 255);
    case JCE_JSONK_SCENE:            return IM_COL32( 46, 110,  74, 255);
    case JCE_JSONK_PREFAB:           return IM_COL32( 44, 108, 128, 255);
    case JCE_JSONK_ANIM_SM:          return IM_COL32(140,  90,  40, 255);
    case JCE_JSONK_AVATAR_MASK:      return IM_COL32(140,  90,  40, 255);
    case JCE_JSONK_PARTICLES:        return IM_COL32(128,  70, 130, 255);
    case JCE_JSONK_MATERIAL:         return IM_COL32( 60,  90, 140, 255);
    case JCE_JSONK_PHYSMAT:          return IM_COL32( 96,  84,  40, 255);
    case JCE_JSONK_RENDER_SETTINGS:  return IM_COL32( 90,  70, 130, 255);
    case JCE_JSONK_RENDER_PIPELINE:  return IM_COL32( 90,  70, 130, 255);
    case JCE_JSONK_PHYS_LAYERS:      return IM_COL32( 96,  84,  40, 255);
    case JCE_JSONK_PROJECT:          return IM_COL32( 50,  95,  95, 255);
    case JCE_JSONK_PROJECT_SETTINGS: return IM_COL32( 50,  95,  95, 255);
    case JCE_JSONK_TAGS_LAYERS:      return IM_COL32( 50,  95,  95, 255);
    case JCE_JSONK_IMPOSTOR:         return IM_COL32( 70,  70,  70, 255);
    case JCE_JSONK_SPRITES:          return IM_COL32(128,  70, 130, 255);
    case JCE_JSONK_GRAPH:            return IM_COL32( 44, 108, 128, 255);
    case JCE_JSONK_I18N:             return IM_COL32( 70,  70,  70, 255);
    case JCE_JSONK_EDITOR_PREFS:     return IM_COL32( 70,  70,  70, 255);
    default:                         return IM_COL32( 70,  70,  70, 255);
    }
}
