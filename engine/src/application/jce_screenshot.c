/*
 * jce_screenshot.c  Frame capture.
 *
 * Captures the current backbuffer to an image file.  The capture is deferred
 * to the next frame and written asynchronously via the renderer's bgfx
 * screen_shot callback (see engine/src/renderer/jce_renderer.c).
 */

#include <jce/application/jce_screenshot.h>
#include <jce/renderer/jce_renderer.h>

#include <stddef.h>

bool jce_screenshot_save(const char *path, JceScreenshotFormat format)
{
    /* Output format is selected by the file extension (.png default, .bmp);
     * the enum is advisory for now. */
    (void)format;
    if (!path || !path[0])
        return false;
    return jce_renderer_request_screenshot(path);
}

void jce_screenshot_capture(JceScreenshotCallback cb, void *userdata)
{
    /* Capture-to-memory is not wired yet; only file capture (F12) is used.
     * Reserved API surface — see jce_screenshot.h. */
    (void)cb;
    (void)userdata;
}

bool jce_screenshot_pending(void)
{
    return jce_renderer_screenshot_pending();
}
