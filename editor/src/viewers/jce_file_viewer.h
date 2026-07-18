/*
 * jce_file_viewer.h  Multi-tab file viewer with specialized sub-viewers.
 *
 * Matches Java reference: FileViewerWindow, ImageViewerWindow,
 * CodeViewerWindow, ModelViewerWindow, TexturePreviewWindow.
 */

#ifndef JCE_FILE_VIEWER_H
#define JCE_FILE_VIEWER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* File type categories (determines which sub-viewer is used). */
typedef enum {
    JCE_FV_TEXT = 0,    /* code, config, data, markdown */
    JCE_FV_IMAGE,       /* png, jpg, bmp, tga, hdr, gif, dds, ktx */
    JCE_FV_MODEL,       /* gltf, glb, obj, fbx */
    JCE_FV_SCENE,       /* .scene, .scene.json */
    JCE_FV_MATERIAL,    /* .mat.json PBR material */
    JCE_FV_PHYSMAT,     /* .physmat.json Physics material */
    JCE_FV_RENDER_PIPELINE, /* .rp.json Render Pipeline asset */
    JCE_FV_AUDIO,       /* wav, ogg, mp3, flac */
    JCE_FV_VIDEO,       /* mp4, webm, mov, mkv, avi, flv, m4v */
    JCE_FV_BINARY,      /* unknown binary */
} JceFileViewerType;

/* Open a file in a new tab (or focus existing tab). */
void  jce_file_viewer_open(const char *path);

/* Open a file as PLAIN TEXT in the code viewer and jump to a 1-based line
 * (highlighted + scrolled into view).  Unlike jce_file_viewer_open this
 * never redirects to a specialized viewer and never side-loads a scene —
 * it is the "view the real JSON source" path (hierarchy right-click,
 * search results).  line <= 0 opens at the top.  An already-open tab is
 * refreshed from disk so the text reflects the latest save. */
void  jce_file_viewer_open_text_at(const char *path, int line);

/* Draw the tabbed file viewer content (for panel embedding). */
void  jce_file_viewer_draw_content(void);

/* Draw standalone file viewer window (Begin/End). */
void  jce_file_viewer_draw_window(bool *p_visible);

/* Close all open tabs/free resources. */
void  jce_file_viewer_close_all(void);

/* Shutdown (called from editor shutdown). */
void  jce_file_viewer_shutdown(void);

/* Request the file viewer window to be focused on next frame. */
void  jce_file_viewer_request_focus(void);

/* Detect file type by extension. */
JceFileViewerType jce_file_viewer_detect_type(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* JCE_FILE_VIEWER_H */
