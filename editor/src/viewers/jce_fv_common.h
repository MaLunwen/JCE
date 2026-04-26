/*
 * jce_fv_common.h  Shared types and declarations for file viewer sub-viewers.
 *
 * Each sub-viewer (code, image, model, hex, scene) includes this header
 * and implements its render function.  The tab manager in
 * jce_panel_file_viewer.cpp dispatches to the appropriate renderer.
 */

#ifndef JCE_FV_COMMON_H
#define JCE_FV_COMMON_H

#include "jce_editor_alloc.h"
#include "jce_editor_colors.h"
#include "jce_editor_defaults.h"
#include "jce_editor_i18n.h"
#include "jce_editor_panels.h"
#include "jce_file_viewer.h"

#include <ctype.h>
#include <imgui.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/platform/jce_host_shell.h>
#include <jce/renderer/jce_texture.h>
}

/* ══════════════════════════════════════════════════════════════════════
 *  CONSTANTS
 * ══════════════════════════════════════════════════════════════════════ */

#define FV_MAX_TABS       16
#define FV_MAX_ASSET_BYTES (1024 * 1024 * 1024) /* 1 GB per asset */
#define FV_MAX_CONTENT    (1024 * 256)   /* 256 KB per file */
#define FV_EDIT_BUF_CAP   (1024 * 64)   /* 64 KB edit buffer */

/* ══════════════════════════════════════════════════════════════════════
 *  TAB DATA
 * ══════════════════════════════════════════════════════════════════════ */

struct FvTab {
    char  path[512];
    char  display_name[64];
    char  ext[16];
    char *content;          /* heap-allocated file bytes */
    int   content_len;
    long  file_size;
    JceFileViewerType type;
    bool  open;

    /* Image viewer */
    JceTexture gpu_tex;
    int   img_w;
    int   img_h;
    float zoom;
    float pan_x;            /* middle-click drag offset */
    float pan_y;

    /* Code viewer */
    bool  edit_mode;
    char *edit_buf;         /* allocated on edit-mode activation */
    int   edit_buf_cap;
    bool  modified;
    char  find_buf[256];
    char  replace_buf[256];
    bool  show_find_replace;
    int   find_index;       /* current match index (-1 = none) */
};

/* ══════════════════════════════════════════════════════════════════════
 *  ZOOMABLE IMAGE / FRAME HELPER
 *
 *  Shared by image and video viewers. Renders a texture inside the
 *  current ImGui window with:
 *    - Mouse wheel zoom (centered on cursor)
 *    - Left or middle drag pan
 *    - Double-left-click toggles between Fit and 1:1
 *    - Checker/dark matte background
 *
 *  Caller owns zoom/pan_x/pan_y storage. Pass content_w/h = source pixel
 *  dimensions. If first_view is true, the helper auto-fits on the first
 *  call (zoom == 0).
 * ══════════════════════════════════════════════════════════════════════ */
struct FvZoomable {
    JceTexture tex;
    int        content_w;
    int        content_h;
    float     *zoom;
    float     *pan_x;
    float     *pan_y;
    bool       allow_double_click_toggle;
    /* Background color (BG behind the image inside the canvas). */
    uint32_t   matte_color;
};

void fv_render_zoomable(const FvZoomable *params);
void fv_zoomable_fit(const FvZoomable *params, ImVec2 avail);
void fv_zoomable_one_to_one(const FvZoomable *params);


void fv_render_code(FvTab *tab);
void fv_render_image(FvTab *tab);
void fv_render_model(FvTab *tab);
void fv_render_scene(FvTab *tab);
void fv_render_material(FvTab *tab);
void fv_render_audio(FvTab *tab);
void fv_render_video(FvTab *tab);
void fv_render_hex(FvTab *tab);

/* ── Per-viewer cleanup (called when closing a tab) ──────────────── */

void fv_code_close_tab(FvTab *tab);
void fv_audio_close_tab(FvTab *tab);
void fv_audio_update_focus(const char *active_tab_path, bool allow_playback);
void fv_audio_request_play(const char *path);
void fv_video_close_tab(FvTab *tab);
void fv_video_update_focus(const char *active_tab_path, bool allow_playback);
void fv_video_request_play(const char *path);
void fv_model_close_tab(const char *path);
void fv_model_shutdown(void);

#endif /* JCE_FV_COMMON_H */
