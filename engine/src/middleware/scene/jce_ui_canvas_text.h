/* Shared Canvas text measurement and draw owner; no public ABI. */
#ifndef JCE_UI_CANVAS_TEXT_H
#define JCE_UI_CANVAS_TEXT_H
#include "jce_ui_canvas_widgets.h"

void uc_draw_text(JceUICanvas *uc, uint16_t view_id, const UCRect *rect,
                   const JceUITextComponent *text, float alpha,
                   float ui_scale, const JceRectXform *transform);
#endif
