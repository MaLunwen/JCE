/*
 * jce_screenshot.h  Frame capture and screenshot utility.
 *
 * Captures the current backbuffer or a specific render target to
 * a PNG/BMP file or an in-memory pixel buffer.
 *
 * Layer: Application (Layer 6).
 *
 * STATUS: Architecture stub — API surface defined, implementation pending.
 */

#ifndef JCE_SCREENSHOT_H
#define JCE_SCREENSHOT_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Output format                                                       */
/* ================================================================== */

typedef enum {
    JCE_SCREENSHOT_PNG,
    JCE_SCREENSHOT_BMP,
    JCE_SCREENSHOT_TGA,
} JceScreenshotFormat;

/* ================================================================== */
/* Capture to file                                                     */
/* ================================================================== */

/* Request a screenshot of the current frame.
 * The capture is deferred: it happens at the end of bgfx_frame(),
 * and is written asynchronously.  Returns false if path is invalid. */
bool jce_screenshot_save(const char *path, JceScreenshotFormat format);

/* ================================================================== */
/* Capture to memory                                                   */
/* ================================================================== */

/* Request a frame capture to an in-memory RGBA8 buffer.
 * When complete, callback is invoked with the pixel data.
 * The pixel buffer is only valid for the duration of the callback.
 * The callback is called from the render thread. */
typedef void (*JceScreenshotCallback)(const uint8_t *rgba_pixels,
                                       uint32_t width, uint32_t height,
                                       void *userdata);

void jce_screenshot_capture(JceScreenshotCallback cb, void *userdata);

/* ================================================================== */
/* Query                                                               */
/* ================================================================== */

/* Returns true if a screenshot is currently pending. */
bool jce_screenshot_pending(void);

JCE_EXTERN_C_END

#endif /* JCE_SCREENSHOT_H */
