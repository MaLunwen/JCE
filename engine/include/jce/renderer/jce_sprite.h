/*
 * jce_sprite.h  Sprite sheet and frame animation.
 *
 * Supports:
 * - Grid-based sprite sheets (uniform frame size)
 * - Aseprite JSON atlas (frame regions + named animations)
 * - Frame-by-frame animation playback
 */

#ifndef JCE_SPRITE_H
#define JCE_SPRITE_H


#include <jce/os/core/jce_defs.h>
#include <stdint.h>
#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Frame and animation types                                           */
/* ================================================================== */

typedef struct {
    uint16_t x, y, w, h;      /* pixel region in the atlas */
    float    duration_ms;      /* frame duration in milliseconds */
    int16_t  pivot_x, pivot_y; /* pivot offset (pixels) */
} JceSpriteFrame;

typedef struct {
    char     name[64];
    uint32_t first_frame;      /* index into the sheet's frame array */
    uint32_t frame_count;
    bool     loop;
} JceSpriteAnim;

/* ================================================================== */
/* Sprite sheet                                                        */
/* ================================================================== */

typedef struct JceSpriteSheet JceSpriteSheet;

/*
 * Load a sprite sheet from an Aseprite JSON atlas.
 * The JSON must contain a "frames" array/object and optional "meta.frameTags".
 *
 * @param json_path  Path to the Aseprite .json atlas.
 * @param image_path Override image path (NULL = use meta.image from JSON).
 */
JceSpriteSheet *jce_sprite_sheet_load_json(const char *json_path,
                                            const char *image_path);

/*
 * Create a sprite sheet from a grid layout (uniform frame size).
 *
 * @param image_path  Path to the sprite sheet image.
 * @param image_w     Total image width (pixels).
 * @param image_h     Total image height (pixels).
 * @param frame_w     Frame width (pixels).
 * @param frame_h     Frame height (pixels).
 * @param frame_duration_ms  Duration per frame in milliseconds (default 100).
 */
JceSpriteSheet *jce_sprite_sheet_create_grid(const char *image_path,
                                              uint32_t image_w,
                                              uint32_t image_h,
                                              uint32_t frame_w,
                                              uint32_t frame_h,
                                              float frame_duration_ms);

void jce_sprite_sheet_destroy(JceSpriteSheet *sheet);

/* Accessors. */
const char       *jce_sprite_sheet_image_path(const JceSpriteSheet *sheet);
uint32_t          jce_sprite_sheet_frame_count(const JceSpriteSheet *sheet);
const JceSpriteFrame *jce_sprite_sheet_get_frame(const JceSpriteSheet *sheet,
                                                  uint32_t index);

uint32_t          jce_sprite_sheet_anim_count(const JceSpriteSheet *sheet);
const JceSpriteAnim *jce_sprite_sheet_get_anim(const JceSpriteSheet *sheet,
                                                uint32_t index);
const JceSpriteAnim *jce_sprite_sheet_find_anim(const JceSpriteSheet *sheet,
                                                 const char *name);

/* ================================================================== */
/* Animation player                                                    */
/* ================================================================== */

typedef struct JceSpritePlayer JceSpritePlayer;

JceSpritePlayer *jce_sprite_player_create(const JceSpriteSheet *sheet);
void             jce_sprite_player_destroy(JceSpritePlayer *p);

/* Set current animation by name. Returns false if not found. */
bool jce_sprite_player_set_anim(JceSpritePlayer *p, const char *name);

/* Advance animation by dt seconds. */
void jce_sprite_player_update(JceSpritePlayer *p, float dt, float speed);

/* Get current frame's UV region (for rendering). */
const JceSpriteFrame *jce_sprite_player_current_frame(const JceSpritePlayer *p);
uint32_t              jce_sprite_player_current_index(const JceSpritePlayer *p);

bool jce_sprite_player_is_finished(const JceSpritePlayer *p);
void jce_sprite_player_reset(JceSpritePlayer *p);

JCE_EXTERN_C_END

#endif /* JCE_SPRITE_H */
