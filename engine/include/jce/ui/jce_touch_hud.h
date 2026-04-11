/*
 * jce_touch_hud.h  On-screen touch controls (MCBE-style).
 *
 * Provides a virtual joystick (movement), look-drag area (camera),
 * and action buttons (pause, jump, crouch).  Renders semi-transparent
 * overlays using the existing 2D primitive API.
 *
 * Auto-created on touch platforms (Android, iOS, Emscripten).
 * On desktop, F9 creates this HUD for touch-control debugging
 * (mouse left-click emulates a finger).
 * All functions safely accept NULL (no-ops).
 */

#ifndef JCE_TOUCH_HUD_H
#define JCE_TOUCH_HUD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRenderer JceRenderer;
typedef struct JceWindow   JceWindow;
typedef struct JceInput    JceInput;
typedef struct JceFont     JceFont;
typedef struct JceTouchHud JceTouchHud;

/* Virtual button IDs. */
typedef enum {
    JCE_TOUCH_BTN_PAUSE  = 0,
    JCE_TOUCH_BTN_JUMP   = 1,
    JCE_TOUCH_BTN_CROUCH = 2,
    JCE_TOUCH_BTN_MENU_0 = 3,   /* Continue (pause menu) */
    JCE_TOUCH_BTN_MENU_1 = 4,   /* Quit     (pause menu) */
    JCE_TOUCH_BTN_COUNT
} JceTouchButton;

/* Create / destroy.  label_font may be NULL (labels will be skipped). */
JceTouchHud *jce_touch_hud_create(JceRenderer *renderer, JceWindow *window,
                                   JceFont *label_font);
void         jce_touch_hud_destroy(JceTouchHud *hud);

/* Process touch input and update virtual controls.  Call once per frame. */
void         jce_touch_hud_update(JceTouchHud *hud,
                                   const JceInput *input, float dt_ms);

/* Draw semi-transparent overlays.  Call after game rendering. */
void         jce_touch_hud_draw(JceTouchHud *hud);

/* Query virtual joystick output (each axis in [-1, 1]). */
void         jce_touch_hud_get_move(const JceTouchHud *hud,
                                     float *dx, float *dz);

/* Query accumulated look delta (in degrees) since last update. */
void         jce_touch_hud_get_look(const JceTouchHud *hud,
                                     float *yaw, float *pitch);

/* Query whether a virtual button was "pressed" this frame (edge). */
bool         jce_touch_hud_button(const JceTouchHud *hud, JceTouchButton btn);

/* Query whether a virtual button is currently held down. */
bool         jce_touch_hud_button_down(const JceTouchHud *hud, JceTouchButton btn);

/* Switch to menu mode (pause screen): draws Continue/Quit buttons
   instead of the normal HUD.  Pass false to return to normal HUD. */
void         jce_touch_hud_set_menu_mode(JceTouchHud *hud, bool menu_mode);

/* Show or hide the HUD overlay.  Input processing continues even when hidden,
   so mobile controls stay responsive.  Default: visible = true. */
void         jce_touch_hud_set_visible(JceTouchHud *hud, bool visible);
bool         jce_touch_hud_is_visible(const JceTouchHud *hud);

#ifdef __cplusplus
}
#endif

#endif /* JCE_TOUCH_HUD_H */
