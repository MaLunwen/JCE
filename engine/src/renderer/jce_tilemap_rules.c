/*
 * jce_tilemap_rules.c  Rule tile resolver.
 *
 * Resolution order per cell:
 *   1. If cell empty → JCE_TILEMAP_EMPTY.
 *   2. Look up rule by cell.sprite_index (treated as rule_id).
 *   3. Build 4-neighbour mask + check variant_sprite_id[mask].
 *      0xFF or 0 in the slot falls through.
 *   4. If rule has anim_frame_count > 0, override with
 *      anim_frame_id[(int)(time*fps) % frame_count].
 *   5. Otherwise return base_sprite_id.
 */

#include <jce/renderer/jce_tilemap_rules.h>

#include <math.h>
#include <string.h>

void jce_tile_rules_init(JceTileRuleSet *set)
{
    if (!set) return;
    memset(set, 0, sizeof(*set));
}

uint16_t jce_tile_rules_add(JceTileRuleSet *set, const JceTileRule *r)
{
    if (!set || !r) return 0xFFFFu;
    if (set->rule_count >= JCE_TILE_RULE_MAX) return 0xFFFFu;
    set->rules[set->rule_count] = *r;
    set->rules[set->rule_count].active = true;
    return set->rule_count++;
}

const JceTileRule *jce_tile_rules_find(const JceTileRuleSet *set,
                                        uint16_t rule_id)
{
    if (!set) return NULL;
    for (uint16_t i = 0; i < set->rule_count; ++i) {
        if (set->rules[i].active && set->rules[i].rule_id == rule_id)
            return &set->rules[i];
    }
    return NULL;
}

static uint8_t neighbour_mask(const JceTilemap *map, const JceTileRuleSet *set,
                                int32_t tx, int32_t ty, uint16_t rule_id)
{
    uint8_t m = 0;
    static const int dx[4] = {  0, +1,  0, -1 };
    static const int dy[4] = { -1,  0, +1,  0 };
    for (int i = 0; i < 4; ++i) {
        JceTileCell n = jce_tilemap_get(map, tx + dx[i], ty + dy[i]);
        if (n.sprite_index == JCE_TILEMAP_EMPTY) continue;
        if (set->match_any_neighbour || n.sprite_index == rule_id)
            m |= (uint8_t)(1u << i);
    }
    return m;
}

uint16_t jce_tilemap_resolve_visual_sprite(const JceTilemap *map,
                                             const JceTileRuleSet *set,
                                             int32_t tx, int32_t ty,
                                             float time_seconds)
{
    if (!map || !set) return JCE_TILEMAP_EMPTY;
    JceTileCell c = jce_tilemap_get(map, tx, ty);
    if (c.sprite_index == JCE_TILEMAP_EMPTY) return JCE_TILEMAP_EMPTY;
    const JceTileRule *r = jce_tile_rules_find(set, c.sprite_index);
    if (!r) return c.sprite_index;

    /* Animation overrides variant. */
    if (r->anim_frame_count > 0 && r->anim_fps > 0.0f) {
        int frame = (int)(time_seconds * r->anim_fps) % r->anim_frame_count;
        if (frame < 0) frame += r->anim_frame_count;
        return r->anim_frame_id[frame];
    }

    uint8_t mask = neighbour_mask(map, set, tx, ty, r->rule_id);
    uint16_t var = r->variant_sprite_id[mask & 0xF];
    if (var != 0 && var != 0xFFFFu) return var;
    return r->base_sprite_id;
}
