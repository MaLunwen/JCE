/*
 * jce_sprite_animator.h  Unity Sprite Library + Sprite Animator.
 *
 * Two-tier 2D animation:
 *   1. JceSpriteLibraryAsset  — category-keyed table mapping a
 *      named category to a sequence of sprite ids in a chosen
 *      atlas (jce_sprite_atlas).  E.g. "Walk_Right" → 8 frames.
 *   2. JceSpriteAnimatorRuntime  — runtime state attached to an
 *      entity: current clip name, fps, time elapsed, loop flag,
 *      computed frame index + sprite_id ready for the renderer.
 *
 * Decoupled from JceSpriteRenderer — caller reads frame_sprite_id
 * each tick and writes it into the matching component.
 *
 * Layer: scene (Layer 4) — public.
 */

#ifndef JCE_SPRITE_ANIMATOR_H
#define JCE_SPRITE_ANIMATOR_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SPRITE_LIB_NAME_LEN       32
#define JCE_SPRITE_LIB_FRAMES_MAX     32
#define JCE_SPRITE_LIB_CLIPS_MAX      32

typedef struct {
    char     name[JCE_SPRITE_LIB_NAME_LEN];
    uint16_t frame_sprite_ids[JCE_SPRITE_LIB_FRAMES_MAX];
    uint16_t frame_count;
    bool     active;
} JceSpriteLibClip;

typedef struct {
    char            atlas_path[160];     /* reference to JceSpriteAtlas */
    JceSpriteLibClip clips[JCE_SPRITE_LIB_CLIPS_MAX];
    uint16_t         clip_count;
} JceSpriteLibraryAsset;

JCE_API void     jce_sprite_lib_init(JceSpriteLibraryAsset *lib,
                                       const char *atlas_path);
JCE_API uint16_t jce_sprite_lib_add_clip(JceSpriteLibraryAsset *lib,
                                          const char *name,
                                          const uint16_t *sprite_ids,
                                          uint16_t        frame_count);
JCE_API const JceSpriteLibClip *jce_sprite_lib_find_clip(
    const JceSpriteLibraryAsset *lib, const char *name);

JCE_API bool jce_sprite_lib_save_json(const JceSpriteLibraryAsset *lib,
                                        const char *path);
JCE_API bool jce_sprite_lib_load_json(JceSpriteLibraryAsset *lib,
                                        const char *path);

/* ── Runtime animator (per-entity) ──────────────────────────── */

typedef struct {
    /* Reference to a library asset (resolved by path).  Caller
     * supplies the actual pointer at tick time. */
    char     library_path[160];
    char     current_clip[JCE_SPRITE_LIB_NAME_LEN];
    float    fps;
    float    elapsed_seconds;
    int      current_frame_index;
    uint16_t current_sprite_id;        /* output - read by renderer */
    bool     loop;
    bool     playing;
} JceSpriteAnimatorRuntime;

/* Advance the animator by `dt` against the supplied library.  Sets
 * `current_frame_index` and `current_sprite_id` based on the clip
 * named in `current_clip`.  When `loop` is false, stops at the
 * last frame and clears `playing`. */
JCE_API void jce_sprite_animator_advance(JceSpriteAnimatorRuntime *a,
                                           const JceSpriteLibraryAsset *lib,
                                           float dt);

/* Switch to a different clip (resets elapsed/frame index). */
JCE_API void jce_sprite_animator_play(JceSpriteAnimatorRuntime *a,
                                        const char *clip_name);

JCE_EXTERN_C_END

#endif /* JCE_SPRITE_ANIMATOR_H */
