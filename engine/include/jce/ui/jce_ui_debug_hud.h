/*
 * jce_ui_debug_hud.h  Built-in debug HUD overlay (MangoHud-style).
 *
 * Engine-level debug information panel displayed as a dark corner overlay
 * showing GPU, CPU, RAM, FPS, frametime, resolution, and VSync status.
 * The markup is loaded from an RML file in the PAK archive.
 *
 * Thread safety: NOT thread-safe.  Call from the main thread only.
 */

#ifndef JCE_UI_DEBUG_HUD_H
#define JCE_UI_DEBUG_HUD_H

#include <jce/ui/jce_ui.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRenderer JceRenderer;
typedef struct JceWindow   JceWindow;

typedef struct JceDebugHud JceDebugHud;

/* Configuration passed at creation time. */
typedef struct JceDebugHudDesc {
    JceUIContext *ui;              /* required */
    JceRenderer  *renderer;       /* required: GPU name, VSync queries */
    JceWindow    *window;         /* required: resolution queries */
    const char   *font_family;    /* NULL = uses built-in default */
} JceDebugHudDesc;

/* Per-frame data the caller provides. */
typedef struct JceDebugHudData {
    float       fps;
    float       frametime_ms;
    int         cpu_cores;
    float       cpu_usage;
    int         ram_used_mb;
    int         ram_total_mb;
    bool        wireframe;
    const float *frametime_history;
    int         frametime_history_count;
    int         frametime_history_head;
    int         frametime_graph_columns;
    const char *extra_status;     /* optional extra line, NULL to hide */
    const char *shortcut_hints;   /* optional bottom-left hint line */
} JceDebugHudData;

JceDebugHud   *jce_debug_hud_create(const JceDebugHudDesc *desc);
void           jce_debug_hud_destroy(JceDebugHud *hud);

void           jce_debug_hud_show(JceDebugHud *hud);
void           jce_debug_hud_hide(JceDebugHud *hud);
bool           jce_debug_hud_is_visible(const JceDebugHud *hud);

/* Push current data into HUD elements.  Call before jce_ui_update(). */
void           jce_debug_hud_update(JceDebugHud *hud,
                                    const JceDebugHudData *data);

/* Draw supplemental HUD graphics after jce_ui_render(). */
void           jce_debug_hud_draw(JceDebugHud *hud);

/* Change the body font-family at runtime (e.g. on language switch). */
void           jce_debug_hud_set_font_family(JceDebugHud *hud,
                                             const char *family);

/* Get the underlying document handle for advanced use. */
JceUIDocHandle jce_debug_hud_get_doc(const JceDebugHud *hud);

#ifdef __cplusplus
}
#endif

#endif /* JCE_UI_DEBUG_HUD_H */
