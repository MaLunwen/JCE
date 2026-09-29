/*
 * jce_game_hud.h -- in-game player HUD (action-game style).
 *
 * Renders the standard "running game" HUD elements:
 *   - Health bar / Armor bar
 *   - Ammo counter (current / reserve)
 *   - Hit markers (transient X overlays on damage dealt)
 *   - Damage indicators (radial flashes on damage received)
 *   - Objective banner (transient text)
 *   - Pickup notifications
 *   - Crosshair
 *   - Wanted-level dots
 *
 * Render path uses jce_primitives (2D rects) + jce_text (font output);
 * no custom shaders needed.  Designed to be called once per frame after
 * the main scene render but before postfx.
 *
 * NOT PART OF <jce/api.h>.  This module draws real pixels
 * (jce_game_hud_draw -> jce_primitives -> bgfx_submit on JCE_VIEW_UI) and
 * stays available to a user who includes this header directly.  It is not in
 * api_ui.h because engine/src/middleware/ui/AGENTS.md gives SCENE UI --
 * including the HUD -- to the ECS Canvas (middleware/scene/jce_ui_canvas.h);
 * a second HUD in the aggregate header would read as the engine offering two.
 * It reached api_ui.h on 2026-08-31 as a side effect of completing the api.h
 * closure, which asks only whether a header is REACHABLE and not whether it
 * belongs in that promise.  See tools/lint/api_closure_exempt.txt.
 *
 * Two lines were removed from the list above on the same day because the
 * features were advertised and absent: a "Weapon slot indicator" (the
 * weapon_slot / weapon_count fields below are written by the caller and read
 * by nothing) and a "Compass / mini-map (optional, set via callback)" (there
 * is no callback anywhere in this API).
 *
 * Layer: middleware/ui (Layer 4).
 */
#ifndef JCE_GAME_HUD_H
#define JCE_GAME_HUD_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRenderer JceRenderer;
typedef struct JceFont     JceFont;
typedef struct JceGameHud  JceGameHud;

typedef struct {
    JceRenderer *renderer;
    JceFont     *font;        /* may be NULL — text drawing skipped */
    int          screen_w;
    int          screen_h;
} JceGameHudDesc;

typedef struct {
    /* Player state — caller updates each frame. */
    float    health;          /* 0..1 */
    float    armor;           /* 0..1 */
    int32_t  ammo_clip;
    int32_t  ammo_reserve;
    int32_t  weapon_slot;     /* 0..n */
    int32_t  weapon_count;
    uint32_t score;
    int32_t  wanted_level;    /* 0..6 */

    /* Display strings — owned by caller, must outlive next render. */
    const char *weapon_name;
    const char *objective;    /* NULL = no objective banner */
} JceGameHudState;

JCE_API JceGameHud *jce_game_hud_create(const JceGameHudDesc *desc);
JCE_API void        jce_game_hud_destroy(JceGameHud *hud);

JCE_API void        jce_game_hud_resize(JceGameHud *hud, int w, int h);
JCE_API void        jce_game_hud_set_state(JceGameHud *hud, const JceGameHudState *st);

/* Trigger transient effects. */
JCE_API void        jce_game_hud_trigger_hitmarker(JceGameHud *hud);
/* Damage indicator: angle in radians (0 = front, +clockwise viewed top-down). */
JCE_API void        jce_game_hud_trigger_damage(JceGameHud *hud, float angle, float intensity);
JCE_API void        jce_game_hud_show_pickup(JceGameHud *hud, const char *label, float seconds);

/* Visibility toggles. */
JCE_API void        jce_game_hud_set_visible(JceGameHud *hud, bool visible);
JCE_API void        jce_game_hud_set_crosshair(JceGameHud *hud, bool visible);

/* Per-frame: advance transient timers, then draw. */
JCE_API void        jce_game_hud_update(JceGameHud *hud, float dt);
JCE_API void        jce_game_hud_draw(JceGameHud *hud);

#ifdef __cplusplus
}
#endif

#endif /* JCE_GAME_HUD_H */
