/*
 * jce_sprite_atlas.h  Sprite atlas + 9-slice metadata.
 *
 * Data layer for a "many small sprites packed into one big texture"
 * artifact — matches Unity SpriteAtlas at the asset level.  A sprite
 * is a named UV rect on the atlas texture, optionally annotated with
 * a 9-slice border for stretch-tolerant UI panels.
 *
 * Authoring helpers (uniform grid → entries) keep tilemap workflows
 * one-line.  Runtime rendering binds the atlas texture once + uses
 * jce_sprite_atlas_find to translate a sprite name → UV rect.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_SPRITE_ATLAS_H
#define JCE_SPRITE_ATLAS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SPRITE_ATLAS_NAME_LEN   48
#define JCE_SPRITE_ATLAS_MAX_SPRITES 1024

typedef struct {
    char  name[JCE_SPRITE_ATLAS_NAME_LEN];
    /* Pixel-space rect in the atlas. */
    int   x, y, w, h;
    /* Normalised UV rect (computed on add or recompute). */
    float u0, v0, u1, v1;
    /* 9-slice border, pixels (l, t, r, b).  All zero ⇒ not sliced. */
    int   border[4];
    /* Pivot in 0..1 sprite-local coordinates (Unity default 0.5,0.5). */
    float pivot[2];
    bool  active;
} JceSpriteEntry;

typedef struct {
    int  atlas_width;
    int  atlas_height;
    char texture_path[160];
    JceSpriteEntry sprites[JCE_SPRITE_ATLAS_MAX_SPRITES];
    uint32_t        sprite_count;
} JceSpriteAtlas;

/* ── Lifecycle / metadata ────────────────────────────────────── */

JCE_API void jce_sprite_atlas_init(JceSpriteAtlas *atlas,
                                    int width, int height,
                                    const char *texture_path);

/* Append a single sprite; recomputes its UV rect.  Returns the new
 * index or UINT32_MAX on full. */
JCE_API uint32_t jce_sprite_atlas_add(JceSpriteAtlas *atlas,
                                       const char *name,
                                       int x, int y, int w, int h);

/* Configure 9-slice border (in pixels) and pivot for `sprite_idx`.
 * Either field is optional — pass NULL to skip. */
JCE_API bool jce_sprite_atlas_set_slice(JceSpriteAtlas *atlas,
                                         uint32_t sprite_idx,
                                         const int border[4],
                                         const float pivot[2]);

/* Build a uniform `cols × rows` grid of identically-sized cells.
 * Cells are named `prefix_NNN` (zero-padded to 3 digits) — typical
 * use is tilemap building.  Clears any existing sprites first. */
JCE_API uint32_t jce_sprite_atlas_grid(JceSpriteAtlas *atlas,
                                        int cols, int rows,
                                        int cell_w, int cell_h,
                                        int origin_x, int origin_y,
                                        const char *name_prefix);

/* Name-based lookup. */
JCE_API const JceSpriteEntry *jce_sprite_atlas_find(const JceSpriteAtlas *a,
                                                     const char *name);
JCE_API const JceSpriteEntry *jce_sprite_atlas_at  (const JceSpriteAtlas *a,
                                                     uint32_t idx);

/* ── 9-slice geometry helper ─────────────────────────────────── */

/* Given a sprite entry with non-zero border + a target rect
 * (dst_x, dst_y, dst_w, dst_h), emit 9 quads (positions + uvs) so
 * the centre stretches but the corners stay 1:1.  `out_positions`
 * and `out_uvs` need 9 * 4 elements (one quad = 4 corners; xy / uv
 * pairs).  Returns number of quads emitted (always 9 if input is
 * sliced, else 1 for a single quad). */
JCE_API uint32_t jce_sprite_atlas_emit_9slice(const JceSpriteEntry *e,
                                                int atlas_w, int atlas_h,
                                                float dst_x, float dst_y,
                                                float dst_w, float dst_h,
                                                float *out_positions,
                                                float *out_uvs);

/* ── JSON I/O ────────────────────────────────────────────────── */
JCE_API bool jce_sprite_atlas_save_json(const JceSpriteAtlas *a,
                                         const char *path);
JCE_API bool jce_sprite_atlas_load_json(JceSpriteAtlas *a,
                                         const char *path);

JCE_EXTERN_C_END

#endif /* JCE_SPRITE_ATLAS_H */
