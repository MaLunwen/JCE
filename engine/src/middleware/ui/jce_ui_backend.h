/*
 * jce_ui_backend.h  Internal bridge between the C UI API and the RmlUi C++ backend.
 *
 * This header is consumed by jce_ui.c (as plain C) and by
 * jce_ui_rmlui.cpp (as C++).  All symbols use C linkage.
 *
 * NOT part of the public engine API — lives in engine/src/ui/.
 */

#ifndef JCE_UI_BACKEND_H
#define JCE_UI_BACKEND_H

#include <jce/os/core/jce_allocator.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRmlBackend  JceRmlBackend;
typedef struct JceRenderer    JceRenderer;
typedef struct JcePakArchive     JcePakArchive;
typedef struct JceInput       JceInput;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

JceRmlBackend *jce_rml_create(uint32_t width, uint32_t height,
                              JceRenderer *renderer, JcePakArchive *pak);
void           jce_rml_destroy(JceRmlBackend *b);

/* ── Documents ────────────────────────────────────────────────────────── */

uint32_t       jce_rml_doc_load(JceRmlBackend *b, const char *name,
                                const char *markup, uint32_t len);
uint32_t       jce_rml_doc_load_file(JceRmlBackend *b, const char *path);
void           jce_rml_doc_show(JceRmlBackend *b, uint32_t doc_idx);
void           jce_rml_doc_hide(JceRmlBackend *b, uint32_t doc_idx);
void           jce_rml_doc_close(JceRmlBackend *b, uint32_t doc_idx);

/* ── Elements ─────────────────────────────────────────────────────────── */

uint32_t       jce_rml_find_element(JceRmlBackend *b, uint32_t doc_idx,
                                    const char *id);
void           jce_rml_elem_set_text(JceRmlBackend *b, uint32_t elem_idx,
                                     const char *text);
const char    *jce_rml_elem_get_text(JceRmlBackend *b, uint32_t elem_idx);
void           jce_rml_elem_set_property(JceRmlBackend *b, uint32_t elem_idx,
                                         const char *prop, const char *val);
const char    *jce_rml_elem_get_value(JceRmlBackend *b, uint32_t elem_idx);
void           jce_rml_elem_set_value(JceRmlBackend *b, uint32_t elem_idx,
                                      const char *value);
const char    *jce_rml_elem_get_attribute(JceRmlBackend *b, uint32_t elem_idx,
                                          const char *attr);
void           jce_rml_elem_set_attribute(JceRmlBackend *b, uint32_t elem_idx,
                                          const char *attr, const char *val);
void           jce_rml_elem_remove_attribute(JceRmlBackend *b, uint32_t elem_idx,
                                             const char *attr);
void           jce_rml_elem_set_inner_rml(JceRmlBackend *b, uint32_t elem_idx,
                                          const char *rml);
bool           jce_rml_elem_get_bounds(JceRmlBackend *b, uint32_t elem_idx,
                                       float *x, float *y,
                                       float *w, float *h);
uint32_t       jce_rml_doc_get_body(JceRmlBackend *b, uint32_t doc_idx);

/* ── Events ───────────────────────────────────────────────────────────── */

typedef void (*jce_rml_event_fn)(uint32_t elem_idx, const char *event_type,
                                 void *ud);

/* Register a listener.  See jce_ui.h::jce_ui_elem_on for the LIFETIME
 * CONTRACT — `fn` and `ud` MUST outlive the JceRmlBackend.  The backend
 * stores listener adapters in a vector that is torn down strictly AFTER
 * Rml::Shutdown() (see jce_rml_destroy), so adapters remain alive while
 * RmlUi tears down its event dispatchers. */
void           jce_rml_elem_on(JceRmlBackend *b, uint32_t elem_idx,
                               const char *evt, jce_rml_event_fn fn, void *ud);

/* ── Font loading ─────────────────────────────────────────────────────── */

bool           jce_rml_load_font(JceRmlBackend *b, const char *pak_path);

/* ── Per-frame ────────────────────────────────────────────────────────── */

void           jce_rml_process_pointer_input(JceRmlBackend *b,
                                             const JceInput *input);
void           jce_rml_process_input(JceRmlBackend *b, const JceInput *input);
void           jce_rml_update(JceRmlBackend *b, float dt);
void           jce_rml_render(JceRmlBackend *b);
void           jce_rml_resize(JceRmlBackend *b, uint32_t w, uint32_t h);

/* ── DPI scaling ─────────────────────────────────────────────────────── */

/* Set the device-pixel ratio for RmlUi rendering.
   This affects how CSS px units map to physical pixels.
   1.0 = standard, 2.0 = Retina / HiDPI. */
void           jce_rml_set_dp_ratio(JceRmlBackend *b, float dp_ratio);

#ifdef __cplusplus
}
#endif

#endif /* JCE_UI_BACKEND_H */
