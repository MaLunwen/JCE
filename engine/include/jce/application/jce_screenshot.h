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
#include <stddef.h>
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
JCE_API bool jce_screenshot_save(const char *path, JceScreenshotFormat format);

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

JCE_API void jce_screenshot_capture(JceScreenshotCallback cb, void *userdata);

/* ================================================================== */
/* Query                                                               */
/* ================================================================== */

/* Returns true if a screenshot is currently pending. */
JCE_API bool jce_screenshot_pending(void);

/* ================================================================== */
/* Deterministic capture schedules                                     */
/* ================================================================== */

#define JCE_SCREENSHOT_SCHEDULE_MAX 8
#define JCE_SCREENSHOT_SCHEDULE_PATH_MAX 1024

typedef enum JceScreenshotClockState {
    JCE_SCREENSHOT_CLOCK_STOPPED = 0,
    JCE_SCREENSHOT_CLOCK_PLAYING = 1,
    JCE_SCREENSHOT_CLOCK_PAUSED = 2,
} JceScreenshotClockState;

typedef struct JceScreenshotScheduleEntry {
    double seconds;
    char path[JCE_SCREENSHOT_SCHEDULE_PATH_MAX];
} JceScreenshotScheduleEntry;

typedef struct JceScreenshotSchedule {
    JceScreenshotScheduleEntry entries[JCE_SCREENSHOT_SCHEDULE_MAX];
    int count;
    int next_index;
    double elapsed_seconds;
    JceScreenshotClockState previous_clock_state;
    bool in_flight;
    bool failed;
} JceScreenshotSchedule;

/* Parse `seconds|absolute-path[;...]`. Times must be finite, positive,
 * strictly increasing, and the list is bounded to eight entries. */
JCE_API bool JCE_CALL jce_screenshot_schedule_parse(
    const char *spec, JceScreenshotSchedule *out,
    char *error, size_t error_size);

/* Advance only while PLAYING. A STOPPED -> PLAYING edge starts a fresh run;
 * PAUSED preserves both elapsed time and the in-flight capture. */
JCE_API void JCE_CALL jce_screenshot_schedule_tick(
    JceScreenshotSchedule *schedule, JceScreenshotClockState clock_state,
    double dt_seconds);

JCE_API const JceScreenshotScheduleEntry *JCE_CALL
jce_screenshot_schedule_due(const JceScreenshotSchedule *schedule);
JCE_API bool JCE_CALL jce_screenshot_schedule_mark_submitted(
    JceScreenshotSchedule *schedule);
JCE_API void JCE_CALL jce_screenshot_schedule_mark_complete(
    JceScreenshotSchedule *schedule, bool success);
JCE_API bool JCE_CALL jce_screenshot_schedule_finished(
    const JceScreenshotSchedule *schedule);

JCE_EXTERN_C_END

#endif /* JCE_SCREENSHOT_H */
