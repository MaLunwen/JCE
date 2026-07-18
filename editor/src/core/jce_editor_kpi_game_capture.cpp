#include "core/jce_editor_kpi_game_capture.h"

extern "C" {
#include <jce/os/core/jce_str.h>
}

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {

struct GlobalCaptureState {
    JceEditorKpiGameCaptureSchedule schedule;
    uint16_t width = 1280;
    uint16_t height = 720;
    uint32_t rendered_frames = 0;
    bool active = false;
};

GlobalCaptureState g_capture;

void set_error(char *error, size_t error_size, const char *message)
{
    if (!error || error_size == 0)
        return;
    jce_strlcpy(error, message ? message : "invalid capture schedule",
                error_size);
}

} // namespace

bool jce_editor_kpi_game_capture_parse(
    const char *spec, JceEditorKpiGameCaptureSchedule *out,
    char *error, size_t error_size)
{
    return jce_screenshot_schedule_parse(
        spec, out, error, error_size);
}

bool jce_editor_kpi_game_capture_parse_size(
    const char *spec, uint16_t *out_width, uint16_t *out_height)
{
    if (!spec || !spec[0] || !out_width || !out_height)
        return false;
    const char *split = std::strchr(spec, 'x');
    if (!split || split == spec || split[1] == '\0' ||
        std::strchr(split + 1, 'x')) {
        return false;
    }

    errno = 0;
    char *width_end = nullptr;
    const unsigned long width = std::strtoul(spec, &width_end, 10);
    if (errno == ERANGE || width_end != split)
        return false;
    char *height_end = nullptr;
    const unsigned long height = std::strtoul(split + 1, &height_end, 10);
    if (errno == ERANGE || !height_end || *height_end != '\0' ||
        width == 0 || height == 0 || width > UINT16_MAX || height > UINT16_MAX) {
        return false;
    }

    *out_width = static_cast<uint16_t>(width);
    *out_height = static_cast<uint16_t>(height);
    return true;
}

void jce_editor_kpi_game_capture_tick(
    JceEditorKpiGameCaptureSchedule *schedule,
    JceEditorKpiPlayState play_state, double dt_seconds)
{
    jce_screenshot_schedule_tick(
        schedule, static_cast<JceScreenshotClockState>(play_state),
        dt_seconds);
}

const JceEditorKpiGameCaptureEntry *jce_editor_kpi_game_capture_due(
    const JceEditorKpiGameCaptureSchedule *schedule)
{
    return jce_screenshot_schedule_due(schedule);
}

bool jce_editor_kpi_game_capture_mark_submitted(
    JceEditorKpiGameCaptureSchedule *schedule)
{
    return jce_screenshot_schedule_mark_submitted(schedule);
}

void jce_editor_kpi_game_capture_mark_complete(
    JceEditorKpiGameCaptureSchedule *schedule, bool success)
{
    jce_screenshot_schedule_mark_complete(schedule, success);
}

bool jce_editor_kpi_game_capture_finished(
    const JceEditorKpiGameCaptureSchedule *schedule)
{
    return jce_screenshot_schedule_finished(schedule);
}

bool jce_editor_kpi_game_capture_global_configure(
    const char *shots_spec, const char *size_spec,
    char *error, size_t error_size)
{
    g_capture = {};
    g_capture.width = 1280;
    g_capture.height = 720;
    if (!jce_editor_kpi_game_capture_parse(
            shots_spec, &g_capture.schedule, error, error_size)) {
        return false;
    }
    if (size_spec && size_spec[0] &&
        !jce_editor_kpi_game_capture_parse_size(
            size_spec, &g_capture.width, &g_capture.height)) {
        set_error(error, error_size,
                  "game capture size must be WIDTHxHEIGHT");
        g_capture = {};
        return false;
    }
    g_capture.active = true;
    return true;
}

void jce_editor_kpi_game_capture_global_disable(void)
{
    g_capture = {};
}

bool jce_editor_kpi_game_capture_global_active(void)
{
    return g_capture.active;
}

bool jce_editor_kpi_game_capture_global_in_flight(void)
{
    return g_capture.active && g_capture.schedule.in_flight;
}

bool jce_editor_kpi_game_capture_global_needs_focus(void)
{
    return g_capture.active && !g_capture.schedule.failed &&
           !jce_editor_kpi_game_capture_finished(&g_capture.schedule) &&
           g_capture.rendered_frames < 10u;
}

void jce_editor_kpi_game_capture_global_tick(
    JceEditorKpiPlayState play_state, double dt_seconds)
{
    if (!g_capture.active)
        return;
    jce_editor_kpi_game_capture_tick(
        &g_capture.schedule, play_state, dt_seconds);
}

bool jce_editor_kpi_game_capture_global_render_size(
    uint16_t *out_width, uint16_t *out_height)
{
    if (!g_capture.active || !out_width || !out_height)
        return false;
    *out_width = g_capture.width;
    *out_height = g_capture.height;
    return true;
}

void jce_editor_kpi_game_capture_global_note_rendered(void)
{
    if (g_capture.active && g_capture.rendered_frames < UINT32_MAX)
        ++g_capture.rendered_frames;
}

const char *jce_editor_kpi_game_capture_global_due_path(void)
{
    const JceEditorKpiGameCaptureEntry *entry =
        g_capture.active
            ? jce_editor_kpi_game_capture_due(&g_capture.schedule)
            : nullptr;
    return entry ? entry->path : nullptr;
}

bool jce_editor_kpi_game_capture_global_mark_submitted(void)
{
    return g_capture.active &&
           jce_editor_kpi_game_capture_mark_submitted(&g_capture.schedule);
}

void jce_editor_kpi_game_capture_global_mark_complete(bool success)
{
    if (g_capture.active)
        jce_editor_kpi_game_capture_mark_complete(&g_capture.schedule, success);
}
