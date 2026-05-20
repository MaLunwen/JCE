/*
 * jce_tilemap_rules.h  Animated / variant tile rules.
 *
 * Unity Rule Tile equivalent (on top of the B12 JceTilemap).  Each
 * rule binds:
 *   - A base sprite (default appearance)
 *   - Optional N animation frames + fps (animated tile)
 *   - A neighbour-match table (variants for corners / edges /
 *     T-junctions in a grid of like tiles)
 *
 * Runtime queries `jce_tilemap_resolve_visual_sprite(map, rules,
 * tx, ty, time)` to get the sprite_id to actually render — instead
 * of the raw cell.sprite_index, which is treated as a "rule id"
 * when this layer is wired.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_TILEMAP_RULES_H
#define JCE_TILEMAP_RULES_H

#include <jce/renderer/jce_tilemap.h>

JCE_EXTERN_C_BEGIN

#define JCE_TILE_RULE_MAX               256
#define JCE_TILE_RULE_FRAMES_MAX        16
#define JCE_TILE_RULE_VARIANTS          16

/* 4-neighbour mask:
 *   bit 0 = north (ty-1)
 *   bit 1 = east  (tx+1)
 *   bit 2 = south (ty+1)
 *   bit 3 = west  (tx-1)
 * 16 combinations → 16 variant slots.  A 0xFF variant slot means
 * "fall through to base sprite". */
typedef struct {
    uint16_t rule_id;                  /* matches JceTileCell.sprite_index */
    uint16_t base_sprite_id;
    /* Variant table indexed by 4-neighbour mask. */
    uint16_t variant_sprite_id[JCE_TILE_RULE_VARIANTS];
    /* Animation. */
    uint16_t anim_frame_id[JCE_TILE_RULE_FRAMES_MAX];
    uint16_t anim_frame_count;         /* 0 = static */
    float    anim_fps;
    bool     active;
} JceTileRule;

typedef struct {
    JceTileRule rules[JCE_TILE_RULE_MAX];
    uint16_t    rule_count;
    /* If true, when evaluating a variant the neighbour comparison
     * considers any non-empty cell as a match; else only same
     * rule_id counts as a match. */
    bool        match_any_neighbour;
} JceTileRuleSet;

JCE_API void     jce_tile_rules_init(JceTileRuleSet *set);
JCE_API uint16_t jce_tile_rules_add (JceTileRuleSet *set,
                                       const JceTileRule *r);
JCE_API const JceTileRule *jce_tile_rules_find(const JceTileRuleSet *set,
                                                 uint16_t rule_id);

/* Resolve the visible sprite for cell (tx, ty) of `map` against the
 * supplied rule set + current time.  Returns the rule's
 * base_sprite_id when no variant matches, or
 * JCE_TILEMAP_EMPTY when the cell is empty / rule unknown. */
JCE_API uint16_t jce_tilemap_resolve_visual_sprite(
    const JceTilemap     *map,
    const JceTileRuleSet *set,
    int32_t               tx,
    int32_t               ty,
    float                 time_seconds);

JCE_EXTERN_C_END

#endif /* JCE_TILEMAP_RULES_H */
