/*
 * jce_sprite.c  Sprite sheet loading and frame animation.
 *
 * Supports grid-based sheets and Aseprite JSON atlases (via cJSON).
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_sprite.h>

#include "os/core/jce_memory.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "jce_sprite"

/* ================================================================== */
/* Sprite sheet struct                                                 */
/* ================================================================== */

#define MAX_FRAMES 1024
#define MAX_ANIMS  64

struct JceSpriteSheet {
    char           image_path[256];
    JceSpriteFrame frames[MAX_FRAMES];
    uint32_t       frame_count;
    JceSpriteAnim  anims[MAX_ANIMS];
    uint32_t       anim_count;
};

/* ================================================================== */
/* Grid sheet creation                                                 */
/* ================================================================== */

JceSpriteSheet *jce_sprite_sheet_create_grid(const char *image_path,
                                              uint32_t image_w,
                                              uint32_t image_h,
                                              uint32_t frame_w,
                                              uint32_t frame_h,
                                              float frame_duration_ms)
{
    if (!image_path || frame_w == 0 || frame_h == 0) return NULL;
    if (image_w == 0 || image_h == 0) return NULL;
    if (frame_duration_ms <= 0.0f) frame_duration_ms = 100.0f;

    JceSpriteSheet *sheet = (JceSpriteSheet *)JCE_CALLOC(1, sizeof(*sheet));
    if (!sheet) return NULL;

    snprintf(sheet->image_path, sizeof(sheet->image_path), "%s", image_path);

    uint32_t cols = image_w / frame_w;
    uint32_t rows = image_h / frame_h;
    uint32_t total = cols * rows;
    if (total > MAX_FRAMES) total = MAX_FRAMES;

    for (uint32_t i = 0; i < total; i++) {
        uint32_t col = i % cols;
        uint32_t row = i / cols;
        sheet->frames[i].x = (uint16_t)(col * frame_w);
        sheet->frames[i].y = (uint16_t)(row * frame_h);
        sheet->frames[i].w = (uint16_t)frame_w;
        sheet->frames[i].h = (uint16_t)frame_h;
        sheet->frames[i].duration_ms = frame_duration_ms;
        sheet->frames[i].pivot_x = 0;
        sheet->frames[i].pivot_y = 0;
    }
    sheet->frame_count = total;

    /* Create a default animation covering all frames. */
    snprintf(sheet->anims[0].name, 64, "default");
    sheet->anims[0].first_frame = 0;
    sheet->anims[0].frame_count = total;
    sheet->anims[0].loop = true;
    sheet->anim_count = 1;

    LOG_INFO(LOG_TAG, "grid sheet: %s (%ux%u frames, %ux%u each)",
             image_path, cols, rows, frame_w, frame_h);

    return sheet;
}

/* ================================================================== */
/* Aseprite JSON loading                                               */
/* ================================================================== */

static char *read_file_text(const char *path, uint64_t *out_size)
{
    return (char *)jce_fs_host_read_all(path, out_size);
}

JceSpriteSheet *jce_sprite_sheet_load_json(const char *json_path,
                                            const char *image_path)
{
    if (!json_path) return NULL;

    uint64_t json_size = 0;
    char *json_text = read_file_text(json_path, &json_size);
    if (!json_text) {
        LOG_WARN(LOG_TAG, "cannot read atlas: %s", json_path);
        return NULL;
    }

    JceJson *root = jce_json_parse(json_text, 0);
    JCE_FREE(json_text);
    if (!root) {
        LOG_WARN(LOG_TAG, "JSON parse failed: %s", json_path);
        return NULL;
    }

    JceSpriteSheet *sheet = (JceSpriteSheet *)JCE_CALLOC(1, sizeof(*sheet));
    if (!sheet) { jce_json_free(root); return NULL; }

    /* Image path: use override or meta.image. */
    if (image_path) {
        snprintf(sheet->image_path, sizeof(sheet->image_path), "%s", image_path);
    } else {
        const JceJson *meta = jce_json_get(root, "meta");
        const char *img = jce_json_get_string(meta, "image", NULL);
        if (img)
            snprintf(sheet->image_path, sizeof(sheet->image_path), "%s", img);
    }

    /* Parse frames (array or object format).  Both shapes are iterated
     * uniformly via the linked-list walker. */
    JceJson *frames_json = jce_json_get(root, "frames");
    if (jce_json_is_array(frames_json) || jce_json_is_object(frames_json)) {
        for (JceJson *item = jce_json_first_child(frames_json); item;
             item = jce_json_next_sibling(item)) {
            if (sheet->frame_count >= MAX_FRAMES) break;
            const JceJson *frame_obj = jce_json_get(item, "frame");
            if (!frame_obj) continue;

            JceSpriteFrame *f = &sheet->frames[sheet->frame_count];
            f->x = (uint16_t)jce_json_get_int(frame_obj, "x", 0);
            f->y = (uint16_t)jce_json_get_int(frame_obj, "y", 0);
            f->w = (uint16_t)jce_json_get_int(frame_obj, "w", 0);
            f->h = (uint16_t)jce_json_get_int(frame_obj, "h", 0);
            f->duration_ms = (float)jce_json_get_number(item, "duration", 100.0);

            sheet->frame_count++;
        }
    }

    /* Parse frame tags (named animations). */
    const JceJson *meta = jce_json_get(root, "meta");
    if (meta) {
        JceJson *tags = jce_json_get(meta, "frameTags");
        if (jce_json_is_array(tags)) {
            for (JceJson *tag = jce_json_first_child(tags); tag;
                 tag = jce_json_next_sibling(tag)) {
                if (sheet->anim_count >= MAX_ANIMS) break;
                JceSpriteAnim *a = &sheet->anims[sheet->anim_count];

                const char *nm = jce_json_get_string(tag, "name", NULL);
                if (nm) snprintf(a->name, 64, "%s", nm);

                a->first_frame = (uint32_t)jce_json_get_int(tag, "from", 0);
                uint32_t last  = (uint32_t)jce_json_get_int(tag, "to",
                                                           (int)a->first_frame);
                a->frame_count = last - a->first_frame + 1;
                /* TODO: handle "direction" (pingpong, reverse). */
                a->loop = true;

                sheet->anim_count++;
            }
        }
    }

    /* If no tags, create a default animation. */
    if (sheet->anim_count == 0 && sheet->frame_count > 0) {
        snprintf(sheet->anims[0].name, 64, "default");
        sheet->anims[0].first_frame = 0;
        sheet->anims[0].frame_count = sheet->frame_count;
        sheet->anims[0].loop = true;
        sheet->anim_count = 1;
    }

    jce_json_free(root);

    LOG_INFO(LOG_TAG, "atlas loaded: %s (%u frames, %u anims)",
             json_path, sheet->frame_count, sheet->anim_count);

    return sheet;
}

/* ================================================================== */
/* Destroy                                                             */
/* ================================================================== */

void jce_sprite_sheet_destroy(JceSpriteSheet *sheet)
{
    if (!sheet) return;
    JCE_FREE(sheet);
}

/* ================================================================== */
/* Accessors                                                           */
/* ================================================================== */

const char *jce_sprite_sheet_image_path(const JceSpriteSheet *sheet)
{
    return sheet ? sheet->image_path : "";
}

uint32_t jce_sprite_sheet_frame_count(const JceSpriteSheet *sheet)
{
    return sheet ? sheet->frame_count : 0;
}

const JceSpriteFrame *jce_sprite_sheet_get_frame(const JceSpriteSheet *sheet,
                                                  uint32_t index)
{
    if (!sheet || index >= sheet->frame_count) return NULL;
    return &sheet->frames[index];
}

uint32_t jce_sprite_sheet_anim_count(const JceSpriteSheet *sheet)
{
    return sheet ? sheet->anim_count : 0;
}

const JceSpriteAnim *jce_sprite_sheet_get_anim(const JceSpriteSheet *sheet,
                                                uint32_t index)
{
    if (!sheet || index >= sheet->anim_count) return NULL;
    return &sheet->anims[index];
}

const JceSpriteAnim *jce_sprite_sheet_find_anim(const JceSpriteSheet *sheet,
                                                 const char *name)
{
    if (!sheet || !name) return NULL;
    for (uint32_t i = 0; i < sheet->anim_count; i++) {
        if (strcmp(sheet->anims[i].name, name) == 0)
            return &sheet->anims[i];
    }
    return NULL;
}

/* ================================================================== */
/* Animation player                                                    */
/* ================================================================== */

struct JceSpritePlayer {
    const JceSpriteSheet *sheet;
    const JceSpriteAnim  *current_anim;
    uint32_t              local_frame;   /* index within current anim */
    float                 elapsed_ms;
    bool                  finished;
};

JceSpritePlayer *jce_sprite_player_create(const JceSpriteSheet *sheet)
{
    if (!sheet) return NULL;
    JceSpritePlayer *p = (JceSpritePlayer *)JCE_CALLOC(1, sizeof(*p));
    if (!p) return NULL;
    p->sheet = sheet;
    /* Auto-select first animation. */
    if (sheet->anim_count > 0)
        p->current_anim = &sheet->anims[0];
    return p;
}

void jce_sprite_player_destroy(JceSpritePlayer *p)
{
    if (p) JCE_FREE(p);
}

bool jce_sprite_player_set_anim(JceSpritePlayer *p, const char *name)
{
    if (!p || !p->sheet) return false;
    const JceSpriteAnim *a = jce_sprite_sheet_find_anim(p->sheet, name);
    if (!a) return false;
    p->current_anim = a;
    p->local_frame = 0;
    p->elapsed_ms = 0.0f;
    p->finished = false;
    return true;
}

void jce_sprite_player_update(JceSpritePlayer *p, float dt, float speed)
{
    if (!p || !p->current_anim || p->finished) return;
    if (p->current_anim->frame_count == 0) return;

    uint32_t abs_idx = p->current_anim->first_frame + p->local_frame;
    const JceSpriteFrame *frame = jce_sprite_sheet_get_frame(p->sheet, abs_idx);
    if (!frame) return;

    p->elapsed_ms += dt * 1000.0f * speed;

    while (p->elapsed_ms >= frame->duration_ms) {
        p->elapsed_ms -= frame->duration_ms;
        p->local_frame++;

        if (p->local_frame >= p->current_anim->frame_count) {
            if (p->current_anim->loop) {
                p->local_frame = 0;
            } else {
                p->local_frame = p->current_anim->frame_count - 1;
                p->finished = true;
                return;
            }
        }

        abs_idx = p->current_anim->first_frame + p->local_frame;
        frame = jce_sprite_sheet_get_frame(p->sheet, abs_idx);
        if (!frame) return;
    }
}

const JceSpriteFrame *jce_sprite_player_current_frame(const JceSpritePlayer *p)
{
    if (!p || !p->current_anim || !p->sheet) return NULL;
    uint32_t abs_idx = p->current_anim->first_frame + p->local_frame;
    return jce_sprite_sheet_get_frame(p->sheet, abs_idx);
}

uint32_t jce_sprite_player_current_index(const JceSpritePlayer *p)
{
    if (!p || !p->current_anim) return 0;
    return p->current_anim->first_frame + p->local_frame;
}

bool jce_sprite_player_is_finished(const JceSpritePlayer *p)
{
    return p ? p->finished : true;
}

void jce_sprite_player_reset(JceSpritePlayer *p)
{
    if (!p) return;
    p->local_frame = 0;
    p->elapsed_ms = 0.0f;
    p->finished = false;
}
