/*
 * jce_imgui_renderer.h  ImGui renderer backend (bgfx-backed).
 *
 * Renders ImGui draw data via the JCE renderer on the JCE_VIEW_IMGUI view.
 * Must be initialized after the renderer + ImGui::CreateContext().
 *
 * Loads the imgui shader from the supplied PAK archive.
 */

#ifndef JCE_IMGUI_RENDERER_H
#define JCE_IMGUI_RENDERER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jce/os/core/jce_defs.h>
#include <jce/resource/jce_pak_loader.h>

#ifdef __cplusplus
extern "C" {
#endif

JCE_API bool jce_imgui_renderer_init(const JcePakArchive *pak,
                                     uint8_t              view_id);

JCE_API void jce_imgui_renderer_shutdown(void);

/* Set the actual drawable pixel extent before drawing this frame. Current
 * hosts use pixel coordinates with ImGui FramebufferScale=1. Drawing rejects
 * clips outside this extent; an empty extent admits no draw commands. */
JCE_API void jce_imgui_renderer_setup_view(uint16_t width, uint16_t height);

/** Draw the current final ImGui draw data on the initialized UI thread. */
JCE_API void jce_imgui_renderer_draw(void);

/** The final draw-data queue was inspected, including an observed empty queue. */
#define JCE_IMGUI_DRAW_QUEUE_OBSERVED UINT32_C(1)
/** The CPU submit path was observed, including zero accepted matching submits. */
#define JCE_IMGUI_DRAW_SUBMISSIONS_OBSERVED UINT32_C(2)

/** Optional CPU-only observations for one texture in one draw invocation.
 * Counts include only noncallback commands with nonzero element counts.
 * Main and recording counts identify actual bgfx submission calls, never GPU
 * execution, visibility, presentation or resource lifetime completion. Flags
 * distinguish a measured zero from unavailable initialization/draw data. */
typedef struct JceImGuiDrawObservation {
    uint32_t struct_size;       /**< Bytes written by this implementation. */
    uint32_t flags;             /**< JCE_IMGUI_DRAW_*_OBSERVED availability. */
    uint32_t queued_commands;   /**< Matching commands before draw admission. */
    uint32_t main_submits;      /**< Matching main-view CPU submission calls. */
    uint32_t recording_submits; /**< Matching recording-view submission calls. */
} JceImGuiDrawObservation;

/** Draw once and optionally observe one live renderer texture handle index.
 * Call on the same UI thread and after the same setup as the ordinary draw.
 * No pointer is retained. The caller supplies writable storage of out_size
 * bytes; this function writes only sizeof(JceImGuiDrawObservation), sets its
 * struct_size, and does not read the prior buffer contents. UINT16_MAX is an
 * invalid watched handle; handle zero is valid (ImGui encodes it as ID one).
 * Returns true when a complete observation was written. NULL/short storage,
 * an invalid handle, or counter overflow returns false and leaves all caller
 * bytes untouched. Rejected observation requests still perform the ordinary
 * draw once. Unavailable initialization/draw data writes a valid flags-zero
 * observation. Queue inspection precedes transient/clip/empty-draw rejection.
 * The ordinary draw has no observation scan, allocation or GPU query. */
JCE_API bool jce_imgui_renderer_draw_observed(uint16_t watched_texture,
    JceImGuiDrawObservation *out, size_t out_size);

JCE_API void jce_imgui_renderer_rebuild_fonts(void);

/* Request a whole-window screenshot to `path` (.png).  The next frame renders
 * the ImGui view into an offscreen FBO and reads it back (the backbuffer cannot
 * be screen-shot reliably on D3D flip-model swap chains — it comes back black).
 * Asynchronous: the PNG is written a couple of frames later by the renderer's
 * read-back poll.  One capture in flight at a time. */
JCE_API void jce_imgui_renderer_request_capture(const char *path);

/* Enable/disable whole-window video recording.  While on, the UI is rendered a
 * second time into an offscreen FBO and read back into the renderer's capture
 * sink each frame (the backbuffer cannot be captured on D3D flip-model swap
 * chains).  Pair with jce_renderer_set_capture_imgui_mode(true). */
JCE_API void jce_imgui_renderer_set_recording(bool on);

#ifdef __cplusplus
}
#endif

#endif /* JCE_IMGUI_RENDERER_H */
