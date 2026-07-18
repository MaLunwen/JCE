/*
 * jce_editor_project_state.cpp  Per-project, machine-local editor state.
 *
 * See the header for the scope architecture.  Storage model: one flat
 * in-memory entry table (project-level entries have scene[0]=='\0'),
 * serialized as
 *
 *   { "_schema": 1,
 *     "kv":     { "<key>": <number|string>, ... },
 *     "scenes": { "<scene>": { "_touch": N, "<key>": ..., ... }, ... } }
 *
 * Type is carried by the JSON value type (number vs string); int getters
 * cast.  Debounced atomic flush mirrors jce_editor_config.
 */

#include "jce_editor_project_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
}

#include "jce_editor_alloc.h"
#include "io/jce_editor_file_util.h"

#define LOG_TAG "editor_pstate"

#define PSTATE_KEY_MAX    64
#define PSTATE_SCENE_MAX  256
#define PSTATE_STR_MAX    512
#define PSTATE_SCENE_LRU  24

/* Open-project root — owned by dialog_project.cpp (same explicit-root
 * pattern as jce_project_settings / jce_pak_key).  Declared at global
 * scope: inside the anonymous namespace the extern would acquire internal
 * linkage and never bind to the definition. */
extern char s_current_project_root[512];

namespace {

struct Entry {
    char   scene[PSTATE_SCENE_MAX];  /* "" = project-level */
    char   key[PSTATE_KEY_MAX];
    bool   is_str;
    double num;
    char   str[PSTATE_STR_MAX];
};

Entry *s_entries = nullptr;
int s_count = 0, s_cap = 0;

/* Per-scene LRU touch stamps. */
struct SceneTouch { char scene[PSTATE_SCENE_MAX]; unsigned stamp; };
SceneTouch *s_touch = nullptr;
int s_touch_count = 0, s_touch_cap = 0;
unsigned s_touch_next = 1;

char  s_root[512]  = {0};   /* project root this table was loaded for */
bool  s_loaded     = false;
bool  s_dirty      = false;
float s_quiet      = 0.0f;

const char *current_root(void)
{
    return s_current_project_root;
}

void state_path(char *out, size_t cap, const char *root)
{
    snprintf(out, cap, "%s/.jce/editor-state.json", root);
}

void normalize_scene(const char *in, char *out, size_t cap)
{
    if (!in) { out[0] = '\0'; return; }
    size_t n = strlen(in);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = (in[i] == '\\') ? '/' : in[i];
    out[n] = '\0';
}

bool key_valid(const char *key)
{
    return key && key[0] && strlen(key) < PSTATE_KEY_MAX;
}

int entry_find(const char *scene, const char *key)
{
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_entries[i].key, key) == 0 &&
            strcmp(s_entries[i].scene, scene) == 0)
            return i;
    return -1;
}

Entry *entry_upsert(const char *scene, const char *key)
{
    int idx = entry_find(scene, key);
    if (idx >= 0) return &s_entries[idx];
    if (s_count >= s_cap) {
        int ncap = s_cap ? s_cap * 2 : 64;
        Entry *nt = (Entry *)ED_REALLOC(s_entries,
                                         (size_t)ncap * sizeof *nt);
        if (!nt) return nullptr;
        s_entries = nt; s_cap = ncap;
    }
    Entry *e = &s_entries[s_count++];
    memset(e, 0, sizeof *e);
    strncpy(e->scene, scene, sizeof(e->scene) - 1);
    strncpy(e->key, key, sizeof(e->key) - 1);
    return e;
}

void touch_scene(const char *scene)
{
    if (!scene[0]) return;
    for (int i = 0; i < s_touch_count; i++) {
        if (strcmp(s_touch[i].scene, scene) == 0) {
            s_touch[i].stamp = s_touch_next++;
            return;
        }
    }
    if (s_touch_count >= s_touch_cap) {
        int ncap = s_touch_cap ? s_touch_cap * 2 : 16;
        SceneTouch *nt = (SceneTouch *)ED_REALLOC(
            s_touch, (size_t)ncap * sizeof *nt);
        if (!nt) return;
        s_touch = nt; s_touch_cap = ncap;
    }
    SceneTouch *t = &s_touch[s_touch_count++];
    strncpy(t->scene, scene, sizeof(t->scene) - 1);
    t->scene[sizeof(t->scene) - 1] = '\0';
    t->stamp = s_touch_next++;
}

unsigned scene_stamp(const char *scene)
{
    for (int i = 0; i < s_touch_count; i++)
        if (strcmp(s_touch[i].scene, scene) == 0) return s_touch[i].stamp;
    return 0;
}

void table_clear(void)
{
    s_count = 0;
    s_touch_count = 0;
    s_touch_next = 1;
}

void load_kv_object(const JceJson *obj, const char *scene)
{
    for (JceJson *it = jce_json_first_child(obj); it;
         it = jce_json_next_sibling(it)) {
        const char *key = jce_json_member_key(it);
        if (!key_valid(key) || key[0] == '_') continue;
        Entry *e = entry_upsert(scene, key);
        if (!e) return;
        if (jce_json_is_string(it)) {
            e->is_str = true;
            strncpy(e->str, jce_json_string_value(it, ""), sizeof(e->str) - 1);
            e->str[sizeof(e->str) - 1] = '\0';
        } else if (jce_json_is_number(it)) {
            e->is_str = false;
            e->num = jce_json_number_value(it, 0.0);
        }
    }
}

void load_from_disk(const char *root)
{
    table_clear();
    char path[1024];
    state_path(path, sizeof(path), root);
    size_t len = 0;
    char *buf = (char *)ed_read_file(path, &len);
    if (!buf) return;
    JceJson *j = jce_json_parse(buf, len);
    ED_FREE(buf);
    if (!j) return;

    const JceJson *kv = jce_json_get(j, "kv");
    if (jce_json_is_object(kv)) load_kv_object(kv, "");

    const JceJson *scenes = jce_json_get(j, "scenes");
    if (jce_json_is_object(scenes)) {
        for (JceJson *sc = jce_json_first_child(scenes); sc;
             sc = jce_json_next_sibling(sc)) {
            const char *sname = jce_json_member_key(sc);
            if (!sname || !sname[0] || !jce_json_is_object(sc)) continue;
            char scene[PSTATE_SCENE_MAX];
            normalize_scene(sname, scene, sizeof(scene));
            load_kv_object(sc, scene);
            /* Restore LRU ordering from the persisted touch stamps. */
            unsigned st = (unsigned)jce_json_get_int(sc, "_touch", 0);
            touch_scene(scene);
            if (st > 0 && s_touch_count > 0)
                s_touch[s_touch_count - 1].stamp = st;
            if (st >= s_touch_next) s_touch_next = st + 1;
        }
    }
    jce_json_free(j);
}

bool flush_to_disk(const char *root)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.jce", root);
    jce_fs_host_create_directory(dir);

    JceJson *j = jce_json_object();
    if (!j) return false;
    jce_json_set_int(j, "_schema", 1);

    JceJson *kv = jce_json_object();
    for (int i = 0; i < s_count && kv; i++) {
        if (s_entries[i].scene[0]) continue;
        if (s_entries[i].is_str)
            jce_json_set_string(kv, s_entries[i].key, s_entries[i].str);
        else
            jce_json_set_number(kv, s_entries[i].key, s_entries[i].num);
    }
    if (kv) jce_json_set_child(j, "kv", kv);

    /* Scene sections, LRU-capped: keep the PSTATE_SCENE_LRU most recently
     * touched scenes, silently dropping the tail (logged — no silent caps). */
    unsigned cutoff = 0;
    if (s_touch_count > PSTATE_SCENE_LRU) {
        /* Find the LRU-th largest stamp as the cutoff. */
        unsigned stamps[512];
        int n = s_touch_count < 512 ? s_touch_count : 512;
        for (int i = 0; i < n; i++) stamps[i] = s_touch[i].stamp;
        /* insertion sort descending (n is small) */
        for (int i = 1; i < n; i++) {
            unsigned v = stamps[i]; int k = i;
            while (k > 0 && stamps[k - 1] < v) { stamps[k] = stamps[k - 1]; --k; }
            stamps[k] = v;
        }
        cutoff = stamps[PSTATE_SCENE_LRU - 1];
        LOG_INFO(LOG_TAG, "scene-state LRU: keeping %d of %d scenes",
                 PSTATE_SCENE_LRU, s_touch_count);
    }

    JceJson *scenes = jce_json_object();
    if (scenes) {
        for (int t = 0; t < s_touch_count; t++) {
            if (cutoff && s_touch[t].stamp < cutoff) continue;
            JceJson *sc = jce_json_object();
            if (!sc) continue;
            jce_json_set_int(sc, "_touch", (int)s_touch[t].stamp);
            for (int i = 0; i < s_count; i++) {
                if (strcmp(s_entries[i].scene, s_touch[t].scene) != 0) continue;
                if (s_entries[i].is_str)
                    jce_json_set_string(sc, s_entries[i].key, s_entries[i].str);
                else
                    jce_json_set_number(sc, s_entries[i].key, s_entries[i].num);
            }
            jce_json_set_child(scenes, s_touch[t].scene, sc);
        }
        jce_json_set_child(j, "scenes", scenes);
    }

    char path[1024];
    state_path(path, sizeof(path), root);
    bool ok = ed_write_json_to_file(path, j);   /* atomic via engine writer */
    if (!ok) LOG_ERROR(LOG_TAG, "failed to write %s", path);
    return ok;
}

/* Follow the open project: flush the outgoing root, load the incoming.
 * Called lazily from every accessor and from flush_tick. */
void ensure_root(void)
{
    const char *root = current_root();
    if (strcmp(root, s_root) == 0 && s_loaded) return;
    if (s_loaded && s_dirty && s_root[0]) {
        flush_to_disk(s_root);
        s_dirty = false;
    }
    strncpy(s_root, root, sizeof(s_root) - 1);
    s_root[sizeof(s_root) - 1] = '\0';
    s_loaded = true;
    s_quiet = 0.0f;
    s_dirty = false;
    if (s_root[0]) load_from_disk(s_root);
    else table_clear();
}

void mark_dirty(void) { s_dirty = true; s_quiet = 0.0f; }

} /* namespace */

/* ── Public API ─────────────────────────────────────────────────────── */

extern "C" {

bool jce_editor_pstate_active(void) { ensure_root(); return s_root[0] != '\0'; }

static bool pstate_get(const char *scene_in, const char *key,
                       const Entry **out)
{
    ensure_root();
    if (!s_root[0] || !key_valid(key)) return false;
    char scene[PSTATE_SCENE_MAX];
    normalize_scene(scene_in, scene, sizeof(scene));
    int idx = entry_find(scene, key);
    if (idx < 0) return false;
    *out = &s_entries[idx];
    return true;
}

static void pstate_set_num(const char *scene_in, const char *key, double v)
{
    ensure_root();
    if (!s_root[0] || !key_valid(key)) return;   /* inert without a project */
    char scene[PSTATE_SCENE_MAX];
    normalize_scene(scene_in, scene, sizeof(scene));
    Entry *e = entry_upsert(scene, key);
    if (!e) return;
    if (!e->is_str && e->num == v && e->key[0]) { touch_scene(scene); return; }
    e->is_str = false;
    e->num = v;
    touch_scene(scene);
    mark_dirty();
}

static void pstate_set_string(const char *scene_in, const char *key,
                              const char *v)
{
    ensure_root();
    if (!s_root[0] || !key_valid(key)) return;
    if (!v) v = "";
    char scene[PSTATE_SCENE_MAX];
    normalize_scene(scene_in, scene, sizeof(scene));
    Entry *e = entry_upsert(scene, key);
    if (!e) return;
    if (e->is_str && strncmp(e->str, v, sizeof(e->str)) == 0) {
        touch_scene(scene); return;
    }
    e->is_str = true;
    strncpy(e->str, v, sizeof(e->str) - 1);
    e->str[sizeof(e->str) - 1] = '\0';
    touch_scene(scene);
    mark_dirty();
}

int jce_editor_pstate_get_int(const char *key, int fallback)
{ const Entry *e; return (pstate_get("", key, &e) && !e->is_str) ? (int)e->num : fallback; }
void jce_editor_pstate_set_int(const char *key, int value)
{ pstate_set_num("", key, (double)value); }
float jce_editor_pstate_get_float(const char *key, float fallback)
{ const Entry *e; return (pstate_get("", key, &e) && !e->is_str) ? (float)e->num : fallback; }
void jce_editor_pstate_set_float(const char *key, float value)
{ pstate_set_num("", key, (double)value); }
bool jce_editor_pstate_get_str(const char *key, char *out, size_t cap)
{
    const Entry *e;
    if (!(pstate_get("", key, &e) && e->is_str) || !out || cap == 0) return false;
    strncpy(out, e->str, cap - 1); out[cap - 1] = '\0';
    return true;
}
void jce_editor_pstate_set_str(const char *key, const char *value)
{ pstate_set_string("", key, value); }

int jce_editor_pstate_scene_get_int(const char *scene, const char *key, int fallback)
{ const Entry *e; return (scene && pstate_get(scene, key, &e) && !e->is_str) ? (int)e->num : fallback; }
void jce_editor_pstate_scene_set_int(const char *scene, const char *key, int value)
{ if (scene && scene[0]) pstate_set_num(scene, key, (double)value); }
float jce_editor_pstate_scene_get_float(const char *scene, const char *key, float fallback)
{ const Entry *e; return (scene && pstate_get(scene, key, &e) && !e->is_str) ? (float)e->num : fallback; }
void jce_editor_pstate_scene_set_float(const char *scene, const char *key, float value)
{ if (scene && scene[0]) pstate_set_num(scene, key, (double)value); }
bool jce_editor_pstate_scene_get_str(const char *scene, const char *key,
                                     char *out, size_t cap)
{
    const Entry *e;
    if (!scene || !(pstate_get(scene, key, &e) && e->is_str) || !out || cap == 0)
        return false;
    strncpy(out, e->str, cap - 1); out[cap - 1] = '\0';
    return true;
}
void jce_editor_pstate_scene_set_str(const char *scene, const char *key,
                                     const char *value)
{ if (scene && scene[0]) pstate_set_string(scene, key, value); }

void jce_editor_pstate_flush_tick(float dt_sec)
{
    ensure_root();   /* also handles project switches */
    if (!s_dirty || !s_root[0]) return;
    s_quiet += (dt_sec > 0.0f ? dt_sec : 0.0f);
    if (s_quiet < 0.5f) return;
    s_dirty = false;
    s_quiet = 0.0f;
    flush_to_disk(s_root);
}

void jce_editor_pstate_flush_now(void)
{
    if (!s_dirty || !s_root[0]) return;
    s_dirty = false;
    s_quiet = 0.0f;
    flush_to_disk(s_root);
}

} /* extern "C" */
