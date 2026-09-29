/*
 * jce_ui_canvas_widgets.h — the boundary between the UI canvas and the two
 * widgets that were split out of it.
 *
 * WHY THIS EXISTS.  jce_ui_canvas.c was 3,102 lines, past the 3,000-line cap
 * and frozen by tools/lint/check_file_size.py, so every addition to it had
 * to be paid for by a removal -- which is exactly what happened earlier today
 * when a seven-line comment correction had to be compressed back to five.
 *
 * The dropdown and the input field were 242 CONTIGUOUS lines with the
 * smallest interface in the file: three helpers in, seven functions out, and
 * one struct.  Nothing else in either file became visible.  The widget
 * interaction state machines (button, slider, toggle) are a bigger and more
 * tempting block at 555 lines, and were NOT chosen: they need UCFrame, which
 * is the canvas's whole per-frame state, so the boundary would have been the
 * file's interior rather than a seam.
 *
 * A resolved screen rectangle and the three drawing primitives live here
 * because both sides need them; the canvas still owns their definitions.
 */

#ifndef JCE_UI_CANVAS_WIDGETS_H
#define JCE_UI_CANVAS_WIDGETS_H

#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_text.h>
#include <jce/renderer/jce_renderer.h>

#include <stdbool.h>
#include <stdint.h>

/* A resolved screen rectangle (top-left origin, logical pixels). */
typedef struct { float x, y, w, h; } UCRect;

/* ── Owned by jce_ui_canvas.c, used by the widgets ──────────────────── */
/* struct JceUICanvas stays PRIVATE to jce_ui_canvas.c.  The widgets need
 * exactly two things off it, so they get two accessors rather than the
 * struct: moving the definition here would have published forty fields to
 * buy two, and the split is supposed to narrow the boundary, not widen it. */
JceRenderer *uc_renderer(const JceUICanvas *uc);
int          uc_caret(const JceUICanvas *uc);

/* The px a font is ACTUALLY rasterised at, given the atlas ceiling.  Public
 * to this module because a caller that does not know the ceiling draws a
 * 400px label with a 256px atlas at scale 1.0 -- silently 36% smaller than
 * authored.  Ask this, then draw at requested/actual. */
int uc_font_px_clamp(int px);

/* `sdf` picks a distance-field atlas over a coverage one.  It is part of
 * the CACHE KEY, not a draw-time choice: the atlas is rasterised when the
 * font opens, so two labels asking for the same font and size with
 * different answers here are asking for two different fonts. */
JceFont *uc_get_font(JceUICanvas *uc, const char *path, int px, bool sdf);

/* Three more the ContentSizeFitter needs (jce_ui_canvas_fitter.c).  Widening
 * this boundary by three declarations is what a 130-line removal from a file
 * that had just crossed the 3000-line cap is worth; the alternative was
 * raising the cap for the newest code in the file. */
bool                    uc_is_ui_element(JceScene *s, JceEntity e);
const JceRectTransform *uc_entity_rect(JceScene *s, JceEntity e);
/* The same lookup, WRITABLE -- what an animator needs to move a UI
 * element.  uc_entity_rect is a forward to this one, so the component
 * order exists once.  Used by the scene sequencer's uirect.* props. */
JceRectTransform       *uc_entity_rect_mut(JceScene *s, JceEntity e);
void                    uc_text_block_extent(JceFont *font,
                                             const char *const *lines,
                                             int nlines, float lsp,
                                             float *out_w, float *out_h);
uint32_t uc_color(const float rgba[4], float alpha_mul);
void     uc_draw_quad(JceUICanvas *uc, uint16_t view_id, float x, float y,
                      float w, float h, const char *sprite,
                      const float rgba[4], float alpha_mul);

/* ── Owned by jce_ui_canvas_widgets.c, used by the canvas ───────────── */
int    uc_dd_option_count(const JceUIDropdownComponent *d);
UCRect uc_dd_row_rect(const UCRect *main, int i, int count, float screen_h);
void   uc_draw_dropdown(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                        const JceUIDropdownComponent *d, float alpha_mul,
                        float ui_scale);
void   uc_draw_dropdown_popup(JceUICanvas *uc, uint16_t view_id,
                              const UCRect *r,
                              const JceUIDropdownComponent *d,
                              float alpha_mul, float ui_scale,
                              float ptr_x, float ptr_y, bool ptr_valid,
                              float screen_h);
void   uc_draw_input_field(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                           const JceUIInputFieldComponent *f, float alpha_mul,
                           float ui_scale, bool focused, float blink);
bool   uc_if_accepts(int content_type, char ch, const char *text, int caret);
int    uc_if_cap(const JceUIInputFieldComponent *f);

#endif /* JCE_UI_CANVAS_WIDGETS_H */
