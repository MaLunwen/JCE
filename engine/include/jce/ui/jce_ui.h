/*
 * jce_ui.h  Game UI system (retained-mode document/element API).
 *
 * Provides an in-game UI layer separate from ImGui (which is editor-only).
 * Supports styled documents loaded from markup (HTML/CSS-like via RmlUi
 * concepts), input routing, and data binding.
 *
 * NOT a direct wrapper around RmlUi — it's an engine-level API that
 * can switch backends without changing game code.
 *
 * Thread safety: NOT thread-safe.  Call from the main thread only.
 *
 * Layer: UI (Layer 3 — optional subsystem, priority 170).
 */

#ifndef JCE_UI_H
#define JCE_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <jce/core/jce_allocator.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations. */
typedef struct JceRenderer JceRenderer;
typedef struct JceInput    JceInput;

/* ================================================================== */
/* Context lifecycle                                                   */
/* ================================================================== */

typedef struct JceUIContext JceUIContext;

typedef struct {
    uint32_t width;             /* viewport width */
    uint32_t height;            /* viewport height */
} JceUIContextDesc;

JceUIContext *jce_ui_create(const JceUIContextDesc *desc, jce_allocator_t alloc);
void          jce_ui_destroy(JceUIContext *ctx);

/* ================================================================== */
/* Documents                                                           */
/* ================================================================== */

typedef struct { uint32_t idx; } JceUIDocHandle;
#define JCE_UI_DOC_INVALID ((JceUIDocHandle){ UINT32_MAX })

static inline bool jce_ui_doc_valid(JceUIDocHandle h) { return h.idx != UINT32_MAX; }

/* Load a UI document from a markup string.
   The document is not shown until jce_ui_doc_show() is called. */
JceUIDocHandle jce_ui_doc_load(JceUIContext *ctx, const char *name,
                               const char *markup, uint32_t markup_len);

/* Load a UI document from a file path. */
JceUIDocHandle jce_ui_doc_load_file(JceUIContext *ctx, const char *path);

/* Show / hide / close a document. */
void jce_ui_doc_show(JceUIContext *ctx, JceUIDocHandle doc);
void jce_ui_doc_hide(JceUIContext *ctx, JceUIDocHandle doc);
void jce_ui_doc_close(JceUIContext *ctx, JceUIDocHandle doc);

/* ================================================================== */
/* Elements                                                            */
/* ================================================================== */

typedef struct { uint32_t idx; } JceUIElementHandle;
#define JCE_UI_ELEM_INVALID ((JceUIElementHandle){ UINT32_MAX })

static inline bool jce_ui_elem_valid(JceUIElementHandle h) { return h.idx != UINT32_MAX; }

/* Find an element by ID within a document. */
JceUIElementHandle jce_ui_find_element(JceUIContext *ctx, JceUIDocHandle doc,
                                      const char *element_id);

/* Set / get the inner text of an element. */
void        jce_ui_elem_set_text(JceUIContext *ctx, JceUIElementHandle elem,
                                 const char *text);
const char *jce_ui_elem_get_text(JceUIContext *ctx, JceUIElementHandle elem);

/* Set a CSS property on an element. */
void jce_ui_elem_set_property(JceUIContext *ctx, JceUIElementHandle elem,
                              const char *property, const char *value);

/* ================================================================== */
/* Event callbacks                                                     */
/* ================================================================== */

typedef void (*jce_ui_event_fn)(JceUIElementHandle elem, const char *event_type,
                                void *userdata);

void jce_ui_elem_on(JceUIContext *ctx, JceUIElementHandle elem,
                    const char *event_type, jce_ui_event_fn fn, void *userdata);

/* ================================================================== */
/* Per-frame update & render                                           */
/* ================================================================== */

/* Route input events to the UI.  Call before jce_ui_update(). */
void jce_ui_process_input(JceUIContext *ctx, const JceInput *input);

/* Update document layout and animations. */
void jce_ui_update(JceUIContext *ctx, float dt);

/* Render all visible documents.  Call during the render phase. */
void jce_ui_render(JceUIContext *ctx);

/* Handle viewport resize. */
void jce_ui_resize(JceUIContext *ctx, uint32_t width, uint32_t height);

/* Set the DPI scale factor for the UI system.
   All document sizes and coordinates will be multiplied by this value.
   Typically obtained from jce_window_get_dpi_scale().
   Default: 1.0.  Call before jce_ui_update() when the scale changes. */
void jce_ui_set_dpi_scale(JceUIContext *ctx, float dpi_scale);

/* Get the current DPI scale factor. */
float jce_ui_get_dpi_scale(const JceUIContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* JCE_UI_H */
