#ifndef TD_VIEW_H
#define TD_VIEW_H
#include "rules.h"
#include <jce/api_script.h>
#define TD_WIDGETS 96
/* Only TD-authored HUD presentation. Runtime hover/pressed tint is not
 * sampled into this cache; unchanged authored paint is never republished. */
typedef struct {
    float rect[4], color[4], feedback[13];
    char sprite[96];
    int action;
    bool paint, feedback_set, raycast;
} TdUiPresentation;
typedef struct {
    JceScriptHost host;
    JceScriptEntity camera, selection, towers[TD_CELLS], enemies[TD_ENEMIES];
    JceScriptEntity bullets[TD_BULLETS], hud[TD_WIDGETS], canvas;
    JceScriptEntity blood[TD_BLOOD]; bool blood_visible[TD_BLOOD];
    float ui_rects[TD_WIDGETS][4]; int ui_actions[TD_WIDGETS];
    TdUiPresentation ui_desired[TD_WIDGETS], ui_published[TD_WIDGETS];
    float ui_text_rect[TD_WIDGETS][4];
    unsigned short ui_valid[TD_WIDGETS];
    bool ui_button_interactable[TD_WIDGETS], ui_button_enabled[TD_WIDGETS];
    bool tower_visible[TD_CELLS], enemy_visible[TD_ENEMIES], bullet_visible[TD_BULLETS];
    int tower_kind[TD_CELLS]; float distance, yaw, pitch, pan_x, pan_y, pan_z, width, height;
    int enemy_kind[TD_ENEMIES], bullet_kind[TD_BULLETS];
    float cards_time, ui_opacity; int cards_cell; bool cards_open, cards_present;
} TdView;
bool td_view_init(TdView *v, const JceScriptHost *host);
void td_view_camera(TdView *v);
void td_view_render(TdView *v, const TdGame *g, int menu, int selected, const char *notice);
/* True when all requested bool host writes were accepted. False keeps
 * failed components pending; the controller must retry on its next update. */
bool td_view_ui(TdView *v, const TdGame *g, int menu, int selected, const char *notice);
int td_view_pick(TdView *v, float x, float y);
int td_view_ui_action(TdView *v, float x, float y);
bool td_view_ui_contains(TdView *v, float x, float y);
void td_view_pan(TdView *v, float dx, float dy);
#endif
