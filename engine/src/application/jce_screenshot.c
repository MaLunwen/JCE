/*
 * jce_screenshot.c  Frame capture.
 *
 * Captures the current backbuffer to an image file.  The capture is deferred
 * to the next frame and written asynchronously via the renderer's bgfx
 * screen_shot callback (see engine/src/renderer/jce_renderer.c).
 */

#include <jce/application/jce_screenshot.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_str.h>
#include <jce/renderer/jce_renderer.h>

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static void screenshot_schedule_error(char *error, size_t error_size,
                                      const char *message)
{
    if (!error || error_size == 0)
        return;
    jce_strlcpy(error, message ? message : "invalid capture schedule",
                error_size);
}

static bool screenshot_schedule_fail(JceScreenshotSchedule *out,
                                     char *error, size_t error_size,
                                     const char *message)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        out->previous_clock_state = JCE_SCREENSHOT_CLOCK_STOPPED;
    }
    screenshot_schedule_error(error, error_size, message);
    return false;
}

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

bool JCE_CALL jce_screenshot_schedule_parse(
    const char *spec, JceScreenshotSchedule *out,
    char *error, size_t error_size)
{
    const char *begin;
    double previous_seconds = 0.0;

    if (error && error_size > 0)
        error[0] = '\0';
    if (!out) {
        screenshot_schedule_error(error, error_size,
                                  "capture schedule output is null");
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->previous_clock_state = JCE_SCREENSHOT_CLOCK_STOPPED;
    if (!spec || !spec[0])
        return screenshot_schedule_fail(out, error, error_size,
                                        "capture schedule is empty");

    begin = spec;
    for (;;) {
        const char *item_end = strchr(begin, ';');
        const size_t item_size = item_end
            ? (size_t)(item_end - begin) : strlen(begin);
        const char *split;
        size_t seconds_size;
        size_t path_size;
        char seconds_text[64];
        char *seconds_end = NULL;
        double seconds;
        JceScreenshotScheduleEntry *entry;

        if (item_size == 0)
            return screenshot_schedule_fail(
                out, error, error_size,
                "capture schedule contains an empty item");
        if (out->count >= JCE_SCREENSHOT_SCHEDULE_MAX)
            return screenshot_schedule_fail(
                out, error, error_size,
                "capture schedule exceeds eight items");

        split = (const char *)memchr(begin, '|', item_size);
        if (!split || split == begin || split + 1 == begin + item_size ||
            memchr(split + 1, '|', (size_t)((begin + item_size) - (split + 1)))) {
            return screenshot_schedule_fail(
                out, error, error_size,
                "capture item must be seconds|absolute-path");
        }

        seconds_size = (size_t)(split - begin);
        if (seconds_size >= sizeof(seconds_text))
            return screenshot_schedule_fail(
                out, error, error_size,
                "capture seconds must be finite and positive");
        memcpy(seconds_text, begin, seconds_size);
        seconds_text[seconds_size] = '\0';
        errno = 0;
        seconds = strtod(seconds_text, &seconds_end);
        if (errno == ERANGE || seconds_end == seconds_text ||
            !seconds_end || *seconds_end != '\0' || !isfinite(seconds) ||
            seconds <= 0.0) {
            return screenshot_schedule_fail(
                out, error, error_size,
                "capture seconds must be finite and positive");
        }
        if (seconds <= previous_seconds)
            return screenshot_schedule_fail(
                out, error, error_size,
                "capture seconds must be strictly increasing");

        path_size = (size_t)((begin + item_size) - (split + 1));
        if (path_size >= JCE_SCREENSHOT_SCHEDULE_PATH_MAX)
            return screenshot_schedule_fail(
                out, error, error_size,
                "capture path must be absolute");
        entry = &out->entries[out->count];
        memcpy(entry->path, split + 1, path_size);
        entry->path[path_size] = '\0';
        if (!jce_path_is_absolute(entry->path))
            return screenshot_schedule_fail(
                out, error, error_size,
                "capture path must be absolute");

        entry->seconds = seconds;
        ++out->count;
        previous_seconds = seconds;
        if (!item_end)
            break;
        begin = item_end + 1;
    }
    return out->count > 0;
}

void JCE_CALL jce_screenshot_schedule_tick(
    JceScreenshotSchedule *schedule, JceScreenshotClockState clock_state,
    double dt_seconds)
{
    JceScreenshotClockState previous;

    if (!schedule)
        return;
    previous = schedule->previous_clock_state;
    if (clock_state == JCE_SCREENSHOT_CLOCK_PLAYING &&
        previous == JCE_SCREENSHOT_CLOCK_STOPPED) {
        schedule->next_index = 0;
        schedule->elapsed_seconds = 0.0;
        schedule->in_flight = false;
        schedule->failed = false;
    }
    if (clock_state == JCE_SCREENSHOT_CLOCK_PLAYING &&
        isfinite(dt_seconds) && dt_seconds > 0.0) {
        /* Clamp each step to 100 ms — the same stall clamp deterministic
         * mission clocks use (a load/compile hitch must not advance the
         * schedule faster than the simulation it is photographing). */
        schedule->elapsed_seconds +=
            dt_seconds > 0.1 ? 0.1 : dt_seconds;
    }
    if (clock_state == JCE_SCREENSHOT_CLOCK_STOPPED &&
        previous != JCE_SCREENSHOT_CLOCK_STOPPED) {
        schedule->elapsed_seconds = 0.0;
        schedule->next_index = 0;
        schedule->in_flight = false;
        schedule->failed = false;
    }
    schedule->previous_clock_state = clock_state;
}

const JceScreenshotScheduleEntry *JCE_CALL
jce_screenshot_schedule_due(const JceScreenshotSchedule *schedule)
{
    const JceScreenshotScheduleEntry *entry;

    if (!schedule || schedule->failed || schedule->in_flight ||
        schedule->next_index < 0 || schedule->next_index >= schedule->count) {
        return NULL;
    }
    entry = &schedule->entries[schedule->next_index];
    return schedule->elapsed_seconds >= entry->seconds ? entry : NULL;
}

bool JCE_CALL jce_screenshot_schedule_mark_submitted(
    JceScreenshotSchedule *schedule)
{
    if (!jce_screenshot_schedule_due(schedule))
        return false;
    schedule->in_flight = true;
    return true;
}

void JCE_CALL jce_screenshot_schedule_mark_complete(
    JceScreenshotSchedule *schedule, bool success)
{
    if (!schedule || !schedule->in_flight)
        return;
    schedule->in_flight = false;
    if (!success) {
        schedule->failed = true;
        return;
    }
    ++schedule->next_index;
}

bool JCE_CALL jce_screenshot_schedule_finished(
    const JceScreenshotSchedule *schedule)
{
    return schedule && schedule->count > 0 && !schedule->failed &&
           !schedule->in_flight && schedule->next_index >= schedule->count;
}
