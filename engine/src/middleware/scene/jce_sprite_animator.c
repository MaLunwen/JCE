/*
 * jce_sprite_animator.c  Sprite library asset + per-entity animator.
 *
 * Library is POD — no allocation.  Animator is a per-frame state
 * machine that derives current frame from elapsed*fps, then queries
 * the library's clip table for the matching sprite_id.
 */

#include <jce/middleware/scene/jce_sprite_animator.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

void jce_sprite_lib_init(JceSpriteLibraryAsset *lib, const char *atlas_path)
{
    if (!lib) return;
    memset(lib, 0, sizeof(*lib));
    if (atlas_path) {
        strncpy(lib->atlas_path, atlas_path, sizeof(lib->atlas_path) - 1);
        lib->atlas_path[sizeof(lib->atlas_path) - 1] = '\0';
    }
}

uint16_t jce_sprite_lib_add_clip(JceSpriteLibraryAsset *lib,
                                   const char *name,
                                   const uint16_t *sprite_ids,
                                   uint16_t frame_count)
{
    if (!lib || !name) return 0xFFFFu;
    if (lib->clip_count >= JCE_SPRITE_LIB_CLIPS_MAX) return 0xFFFFu;
    if (frame_count > JCE_SPRITE_LIB_FRAMES_MAX) frame_count = JCE_SPRITE_LIB_FRAMES_MAX;

    JceSpriteLibClip *c = &lib->clips[lib->clip_count];
    memset(c, 0, sizeof(*c));
    strncpy(c->name, name, JCE_SPRITE_LIB_NAME_LEN - 1);
    c->name[JCE_SPRITE_LIB_NAME_LEN - 1] = '\0';
    if (sprite_ids && frame_count > 0)
        memcpy(c->frame_sprite_ids, sprite_ids,
                frame_count * sizeof(uint16_t));
    c->frame_count = frame_count;
    c->active = true;
    return lib->clip_count++;
}

const JceSpriteLibClip *jce_sprite_lib_find_clip(
    const JceSpriteLibraryAsset *lib, const char *name)
{
    if (!lib || !name) return NULL;
    for (uint16_t i = 0; i < lib->clip_count; ++i) {
        if (lib->clips[i].active &&
            strncmp(lib->clips[i].name, name,
                     JCE_SPRITE_LIB_NAME_LEN) == 0)
            return &lib->clips[i];
    }
    return NULL;
}

bool jce_sprite_lib_save_json(const JceSpriteLibraryAsset *lib,
                                const char *path)
{
    if (!lib || !path) return false;
    JceJson *root = jce_json_object();
    if (!root) return false;
    jce_json_set_string(root, "atlas", lib->atlas_path);
    jce_json_set_number(root, "clip_count", lib->clip_count);
    for (uint16_t i = 0; i < lib->clip_count; ++i) {
        const JceSpriteLibClip *c = &lib->clips[i];
        if (!c->active) continue;
        char key[40];
        snprintf(key, sizeof(key), "c%u_name", (unsigned)i);
        jce_json_set_string(root, key, c->name);
        snprintf(key, sizeof(key), "c%u_n",    (unsigned)i);
        jce_json_set_number(root, key, c->frame_count);
        for (uint16_t f = 0; f < c->frame_count; ++f) {
            snprintf(key, sizeof(key), "c%u_f%u",
                      (unsigned)i, (unsigned)f);
            jce_json_set_number(root, key, c->frame_sprite_ids[f]);
        }
    }
    return jce_json_write_file(path, root, true, true);
}

bool jce_sprite_lib_load_json(JceSpriteLibraryAsset *lib, const char *path)
{
    if (!lib || !path) return false;
    JceJson *root = jce_json_parse_file(path);
    if (!root) return false;
    jce_sprite_lib_init(lib, jce_json_get_string(root, "atlas", ""));
    uint16_t nc = (uint16_t)jce_json_get_number(root, "clip_count", 0);
    if (nc > JCE_SPRITE_LIB_CLIPS_MAX) nc = JCE_SPRITE_LIB_CLIPS_MAX;
    for (uint16_t i = 0; i < nc; ++i) {
        char key[40];
        snprintf(key, sizeof(key), "c%u_name", (unsigned)i);
        const char *cn = jce_json_get_string(root, key, "");
        if (!cn[0]) continue;
        snprintf(key, sizeof(key), "c%u_n", (unsigned)i);
        uint16_t fn = (uint16_t)jce_json_get_number(root, key, 0);
        if (fn > JCE_SPRITE_LIB_FRAMES_MAX) fn = JCE_SPRITE_LIB_FRAMES_MAX;
        uint16_t ids[JCE_SPRITE_LIB_FRAMES_MAX];
        for (uint16_t f = 0; f < fn; ++f) {
            snprintf(key, sizeof(key), "c%u_f%u",
                      (unsigned)i, (unsigned)f);
            ids[f] = (uint16_t)jce_json_get_number(root, key, 0);
        }
        jce_sprite_lib_add_clip(lib, cn, ids, fn);
    }
    jce_json_free(root);
    return true;
}

/* ── Animator runtime ───────────────────────────────────────── */

void jce_sprite_animator_play(JceSpriteAnimatorRuntime *a, const char *clip)
{
    if (!a || !clip) return;
    strncpy(a->current_clip, clip, JCE_SPRITE_LIB_NAME_LEN - 1);
    a->current_clip[JCE_SPRITE_LIB_NAME_LEN - 1] = '\0';
    a->elapsed_seconds     = 0.0f;
    a->current_frame_index = 0;
    a->playing             = true;
}

void jce_sprite_animator_advance(JceSpriteAnimatorRuntime *a,
                                   const JceSpriteLibraryAsset *lib,
                                   float dt)
{
    if (!a || !lib || !a->playing) return;
    const JceSpriteLibClip *c = jce_sprite_lib_find_clip(lib, a->current_clip);
    if (!c || c->frame_count == 0) return;
    if (a->fps <= 0.0f) a->fps = 12.0f;

    a->elapsed_seconds += dt;
    float frame_period = 1.0f / a->fps;
    int   idx          = (int)(a->elapsed_seconds / frame_period);

    if (idx >= c->frame_count) {
        if (a->loop) {
            idx %= c->frame_count;
            a->elapsed_seconds = (float)idx * frame_period +
                                 (a->elapsed_seconds - (int)(a->elapsed_seconds / (c->frame_count * frame_period)) * c->frame_count * frame_period);
        } else {
            idx = c->frame_count - 1;
            a->playing = false;
        }
    }
    a->current_frame_index = idx;
    a->current_sprite_id   = c->frame_sprite_ids[idx];
}
