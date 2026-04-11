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

#include <stdint.h>
#include <stdbool.h>
#include <jce/core/jce_allocator.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRmlBackend JceRmlBackend;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

JceRmlBackend *jce_rml_create(uint32_t width, uint32_t height);
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

/* ── Events ───────────────────────────────────────────────────────────── */

typedef void (*jce_rml_event_fn)(uint32_t elem_idx, const char *event_type,
                                 void *ud);

void           jce_rml_elem_on(JceRmlBackend *b, uint32_t elem_idx,
                               const char *evt, jce_rml_event_fn fn, void *ud);

/* ── Per-frame ────────────────────────────────────────────────────────── */

void           jce_rml_process_input(JceRmlBackend *b, const void *input);
void           jce_rml_update(JceRmlBackend *b, float dt);
void           jce_rml_render(JceRmlBackend *b);
void           jce_rml_resize(JceRmlBackend *b, uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif

#endif /* JCE_UI_BACKEND_H */
