/*
 * jce_scene_tags_layers.c — Unity-style Tags & Layers registry (P4-A.4).
 *
 * Scene-level Tag (interned string id) and Layer (uint8 0..31) per
 * entity, kept distinct from the physics-only collision mask
 * (<jce/api_physics.h>).  Other systems (camera culling masks, ray
 * filters, render queue groupings) consume the same layer index.
 *
 * Persistence: "<project>/Settings/TagsAndLayers.json".
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>

#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "scene_tags"

/* ── Process-wide registries ───────────────────────────────────────── */

typedef struct {
    char name[JCE_TAG_NAME_MAX];
    bool in_use;
} JceTagSlot;

static JceTagSlot s_tags[JCE_TAG_REGISTRY_MAX];
static int        s_tags_initialized = 0;

static char       s_layers[JCE_LAYER_COUNT][JCE_LAYER_NAME_MAX];
static int        s_layers_initialized = 0;

static const char *const k_unity_default_layers[8] = {
    "Default",
    "TransparentFX",
    "Ignore Raycast",
    "Water",
    "UI",
    "",
    "",
    ""
};

static void ensure_tag_init(void)
{
    if (s_tags_initialized) return;
    memset(s_tags, 0, sizeof(s_tags));
    /* Slot 0 reserved = "Untagged". */
    snprintf(s_tags[0].name, sizeof(s_tags[0].name), "%s", "Untagged");
    s_tags[0].in_use = true;
    s_tags_initialized = 1;
}

static void ensure_layer_init(void)
{
    if (s_layers_initialized) return;
    jce_layer_reset_defaults();
}

/* ── Tag registry ──────────────────────────────────────────────────── */

uint16_t jce_tag_intern(const char *name)
{
    ensure_tag_init();
    if (!name || !*name) return 0;
    for (int i = 0; i < JCE_TAG_REGISTRY_MAX; i++) {
        if (s_tags[i].in_use && strcmp(s_tags[i].name, name) == 0)
            return (uint16_t)i;
    }
    for (int i = 1; i < JCE_TAG_REGISTRY_MAX; i++) {
        if (!s_tags[i].in_use) {
            snprintf(s_tags[i].name, sizeof(s_tags[i].name), "%s", name);
            s_tags[i].in_use = true;
            return (uint16_t)i;
        }
    }
    LOG_WARN(LOG_TAG, "tag registry full (cap=%d)", JCE_TAG_REGISTRY_MAX);
    return 0;
}

const char *jce_tag_name(uint16_t tag_id)
{
    ensure_tag_init();
    if (tag_id >= JCE_TAG_REGISTRY_MAX) return "";
    if (!s_tags[tag_id].in_use) return "";
    return s_tags[tag_id].name;
}

int jce_tag_count(void)
{
    ensure_tag_init();
    int count = 0;
    for (int i = 0; i < JCE_TAG_REGISTRY_MAX; i++)
        if (s_tags[i].in_use) count++;
    return count;
}

const char *jce_tag_at(int idx)
{
    ensure_tag_init();
    if (idx < 0) return "";
    int seen = 0;
    for (int i = 0; i < JCE_TAG_REGISTRY_MAX; i++) {
        if (!s_tags[i].in_use) continue;
        if (seen == idx) return s_tags[i].name;
        seen++;
    }
    return "";
}

bool jce_tag_remove(const char *name)
{
    ensure_tag_init();
    if (!name || !*name) return false;
    if (strcmp(name, "Untagged") == 0) return false;
    for (int i = 1; i < JCE_TAG_REGISTRY_MAX; i++) {
        if (s_tags[i].in_use && strcmp(s_tags[i].name, name) == 0) {
            s_tags[i].in_use = false;
            s_tags[i].name[0] = '\0';
            return true;
        }
    }
    return false;
}

/* ── Layer registry ────────────────────────────────────────────────── */

void jce_layer_reset_defaults(void)
{
    memset(s_layers, 0, sizeof(s_layers));
    for (int i = 0; i < 8; i++) {
        snprintf(s_layers[i], sizeof(s_layers[i]), "%s",
                 k_unity_default_layers[i]);
    }
    s_layers_initialized = 1;
}

void jce_layer_set_name(uint8_t layer, const char *name)
{
    ensure_layer_init();
    if (layer >= JCE_LAYER_COUNT) return;
    if (!name) name = "";
    snprintf(s_layers[layer], sizeof(s_layers[layer]), "%s", name);
}

const char *jce_layer_name(uint8_t layer)
{
    ensure_layer_init();
    if (layer >= JCE_LAYER_COUNT) return "";
    static char fallback[JCE_LAYER_NAME_MAX];
    if (s_layers[layer][0] != '\0') return s_layers[layer];
    snprintf(fallback, sizeof(fallback), "Layer %u", (unsigned)layer);
    return fallback;
}

/* ── Persistence (TagsAndLayers.json) ──────────────────────────────── */

static void build_path(char *out, size_t out_sz, const char *project_root)
{
    const char *root = (project_root && *project_root) ? project_root : ".";
    snprintf(out, out_sz, "%s/Settings/TagsAndLayers.json", root);
}

static void ensure_settings_dir(const char *project_root)
{
    const char *root = (project_root && *project_root) ? project_root : ".";
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/Settings", root);
    if (!jce_fs_host_exists_dir(dir))
        jce_fs_host_create_directory(dir);
}

bool jce_scene_tags_layers_load(const char *project_root)
{
    ensure_tag_init();
    ensure_layer_init();

    char path[512];
    build_path(path, sizeof(path), project_root);
    if (!jce_fs_host_exists_file(path)) return false;

    uint64_t sz = 0;
    void *buf = jce_fs_host_read_all(path, &sz);
    if (!buf) return false;

    cJSON *root = cJSON_ParseWithLength((const char *)buf, (size_t)sz);
    jce_fs_buffer_free(buf);
    if (!root) {
        LOG_WARN(LOG_TAG, "TagsAndLayers.json parse failed: %s", path);
        return false;
    }

    /* Tags. */
    cJSON *tags = cJSON_GetObjectItemCaseSensitive(root, "tags");
    if (cJSON_IsArray(tags)) {
        /* Reset (keep slot 0). */
        for (int i = 1; i < JCE_TAG_REGISTRY_MAX; i++) {
            s_tags[i].in_use = false;
            s_tags[i].name[0] = '\0';
        }
        cJSON *t = NULL;
        cJSON_ArrayForEach(t, tags) {
            if (cJSON_IsString(t) && t->valuestring && t->valuestring[0])
                jce_tag_intern(t->valuestring);
        }
    }

    /* Layers. */
    cJSON *layers = cJSON_GetObjectItemCaseSensitive(root, "layers");
    if (cJSON_IsArray(layers)) {
        int idx = 0;
        cJSON *l = NULL;
        cJSON_ArrayForEach(l, layers) {
            if (idx >= JCE_LAYER_COUNT) break;
            if (cJSON_IsString(l) && l->valuestring)
                snprintf(s_layers[idx], sizeof(s_layers[idx]), "%s",
                         l->valuestring);
            else
                s_layers[idx][0] = '\0';
            idx++;
        }
    }

    cJSON_Delete(root);
    return true;
}

bool jce_scene_tags_layers_save(const char *project_root)
{
    ensure_tag_init();
    ensure_layer_init();

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    cJSON *tags = cJSON_AddArrayToObject(root, "tags");
    for (int i = 0; i < JCE_TAG_REGISTRY_MAX; i++) {
        if (!s_tags[i].in_use) continue;
        cJSON_AddItemToArray(tags, cJSON_CreateString(s_tags[i].name));
    }

    cJSON *layers = cJSON_AddArrayToObject(root, "layers");
    for (int i = 0; i < JCE_LAYER_COUNT; i++)
        cJSON_AddItemToArray(layers, cJSON_CreateString(s_layers[i]));

    char *txt = cJSON_Print(root);
    cJSON_Delete(root);
    if (!txt) return false;

    ensure_settings_dir(project_root);
    char path[512];
    build_path(path, sizeof(path), project_root);
    bool ok = jce_fs_host_write_all(path, txt, (uint64_t)strlen(txt));
    /* cJSON allocates txt via its own hooks — its free is the matching one. */
    cJSON_free(txt);
    return ok;
}

/* ── Convenience: tag/layer per-entity by name ─────────────────────── */

void jce_scene_set_entity_tag_name(JceScene *s, JceEntity e, const char *tag)
{
    if (!s || e == JCE_ENTITY_INVALID) return;
    JceTagComponent c;
    c.tag_id = jce_tag_intern(tag);
    jce_scene_set_tag_component(s, e, &c);
}

const char *jce_scene_get_entity_tag_name(JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return "";
    JceTagComponent *c = jce_scene_get_tag_component(s, e);
    if (!c) return "";
    return jce_tag_name(c->tag_id);
}

void jce_scene_set_entity_layer(JceScene *s, JceEntity e, uint8_t layer)
{
    if (!s || e == JCE_ENTITY_INVALID) return;
    if (layer >= JCE_LAYER_COUNT) layer = 0;
    JceLayerComponent c;
    c.layer = layer;
    jce_scene_set_layer_component(s, e, &c);
}

uint8_t jce_scene_get_entity_layer(JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return 0;
    JceLayerComponent *c = jce_scene_get_layer_component(s, e);
    return c ? c->layer : 0;
}

/* ── Find helpers ──────────────────────────────────────────────────── */

typedef struct {
    uint16_t   want_tag;
    uint8_t    want_layer;
    bool       use_layer;
    JceEntity *out;
    int        max;
    int        count;
    JceEntity  first;
} FindCtx;

static void find_cb(JceScene *s, JceEntity e, void *ud)
{
    FindCtx *ctx = (FindCtx *)ud;
    if (ctx->use_layer) {
        JceLayerComponent *lc = jce_scene_get_layer_component(s, e);
        uint8_t l = lc ? lc->layer : 0;
        if (l != ctx->want_layer) return;
    } else {
        JceTagComponent *tc = jce_scene_get_tag_component(s, e);
        uint16_t t = tc ? tc->tag_id : 0;
        if (t != ctx->want_tag) return;
    }
    if (ctx->first == JCE_ENTITY_INVALID) ctx->first = e;
    if (ctx->out && ctx->count < ctx->max)
        ctx->out[ctx->count] = e;
    ctx->count++;
}

JceEntity jce_scene_find_with_tag(JceScene *s, const char *tag)
{
    if (!s || !tag || !*tag) return JCE_ENTITY_INVALID;
    FindCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.want_tag = jce_tag_intern(tag);
    if (ctx.want_tag == 0 && strcmp(tag, "Untagged") != 0)
        return JCE_ENTITY_INVALID;
    ctx.max = 1;
    jce_scene_each_entity(s, find_cb, &ctx);
    return ctx.first;
}

int jce_scene_find_all_with_tag(JceScene *s, const char *tag,
                                JceEntity *out, int max)
{
    if (!s || !tag || !*tag) return 0;
    FindCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.want_tag = jce_tag_intern(tag);
    ctx.out  = out;
    ctx.max  = max;
    jce_scene_each_entity(s, find_cb, &ctx);
    return ctx.count;
}

int jce_scene_find_all_in_layer(JceScene *s, uint8_t layer,
                                JceEntity *out, int max)
{
    if (!s) return 0;
    FindCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.use_layer  = true;
    ctx.want_layer = layer;
    ctx.out  = out;
    ctx.max  = max;
    jce_scene_each_entity(s, find_cb, &ctx);
    return ctx.count;
}
