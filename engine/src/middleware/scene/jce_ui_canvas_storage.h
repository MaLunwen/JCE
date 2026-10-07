/* Canvas-owned raycast records and widget interaction state. */
#ifndef JCE_UI_CANVAS_STORAGE_H
#define JCE_UI_CANVAS_STORAGE_H
#include "jce_ui_canvas_widgets.h"

typedef struct {
    JceEntity entity;
    UCRect rect;
    float alpha;
    bool interactable;
} UCHit;

typedef struct {
    uint64_t entity;
    float fade;
    int state;
    bool press_inside;
    bool drag_active;
    int dd_press;
} UCButtonState;

typedef struct {
    UCHit *hits;
    uint32_t hit_capacity;
    UCButtonState *buttons;
    uint32_t button_capacity;
    uint32_t button_count;
    const JceScene *scene;
    bool allocation_failed;
} UCInteractionStore;

bool uc_interaction_reserve_hits(UCInteractionStore *store, uint32_t needed);
UCButtonState *uc_interaction_button(UCInteractionStore *store, uint64_t entity);
void uc_interaction_begin(UCInteractionStore *store, JceScene *scene);
void uc_interaction_cancel_presses(UCInteractionStore *store);
void uc_interaction_destroy(UCInteractionStore *store);
#endif
