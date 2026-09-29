/*
 * jce_renderer_bgfx_callback.h -- the boundary between the renderer and the
 * bgfx callback interface that was split out of it.
 *
 * WHY THIS EXISTS.  jce_renderer.c was 3,247 lines: 247 past the 3,000-line cap
 * and frozen at check_file_size's baseline, so every addition had to be paid
 * for by a removal -- and the graphics-API tier work needed to add to it.
 *
 * The callback block was 1,043 CONTIGUOUS lines (fatal, trace, profiler, the
 * shader-cache pair, screen_shot, capture_begin/frame/end, the vtbl and the
 * interface instance) with the narrowest interface in the file: one symbol in,
 * four functions out.  Nothing else in either file became visible.
 *
 * The capture state stayed with the callback rather than being published as
 * six extern variables.  The callback RECEIVES frames and the renderer only
 * starts and stops recording, so the three functions below say what the
 * renderer means; six raw flags would have said only what it touches.
 */

#ifndef JCE_RENDERER_BGFX_CALLBACK_H
#define JCE_RENDERER_BGFX_CALLBACK_H

#include <bgfx/c99/bgfx.h>

#include "renderer/jce_renderer_host_hook.h"  /* jce_rcb_host_took_capture */

#include <stdbool.h>
#include <stdint.h>

/* The pseudo-path bgfx_request_screen_shot() is handed so the screen_shot
 * callback can tell a recording frame from a user screenshot.  Both sides
 * name it: the renderer requests with it, the callback matches on it. */
#define JCE_CAPTURE_SENTINEL "\x01__jce_capture__"

/* -- Owned by jce_renderer.c, used by the callback ------------------ */
/* The frame counter every trace line is stamped with. */
uint32_t jce_rcb_host_frame_index(void);

/* -- Owned by jce_renderer_bgfx_callback.c, used by the renderer ---- */
/* The interface handed to bgfx_init().  Never NULL. */
bgfx_callback_interface_t *jce_rcb_callback_interface(void);

/* True when recording is on, is not the imgui-composited mode, and no shot is
 * already in flight -- i.e. this frame should request a backbuffer capture. */
bool jce_rcb_capture_wants_shot(void);

/* Mark the shot requested for this frame, so the next frame does not stack a
 * second one behind it. */
void jce_rcb_capture_mark_shot_pending(void);

/* Start or stop backbuffer recording.  Stopping frees the swizzle scratch: an
 * idle editor should not hold capture-sized buffers. */
void jce_rcb_capture_set_active(bool enable);

/* jce_rcb_host_took_capture() is declared in jce_renderer_host_hook.h, which
 * this header includes above -- ONE declaration, in a file with no
 * dependencies, so a unit test can reach it without dragging bgfx in.  See
 * that header for why a second forward declaration was the wrong answer. */

/* jce_renderer_take_device_lost() is defined in this TU but declared in the
 * PUBLIC <jce/renderer/jce_renderer.h>, beside the other jce_renderer_*
 * entry points this file implements: the engine polls it from the
 * application layer, which must not pull in <bgfx/c99/bgfx.h> to do so. */

#endif /* JCE_RENDERER_BGFX_CALLBACK_H */
