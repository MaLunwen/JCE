/*
 * jce_editor_kpi_game_capture.h  Deterministic Game View capture schedule.
 */

#ifndef JCE_EDITOR_KPI_GAME_CAPTURE_H
#define JCE_EDITOR_KPI_GAME_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jce/application/jce_screenshot.h>

#define JCE_EDITOR_KPI_GAME_CAPTURE_MAX JCE_SCREENSHOT_SCHEDULE_MAX
#define JCE_EDITOR_KPI_GAME_CAPTURE_PATH_MAX \
    JCE_SCREENSHOT_SCHEDULE_PATH_MAX

enum JceEditorKpiPlayState {
    JCE_EDITOR_KPI_PLAY_STOPPED = 0,
    JCE_EDITOR_KPI_PLAY_PLAYING = 1,
    JCE_EDITOR_KPI_PLAY_PAUSED = 2,
};

typedef JceScreenshotScheduleEntry JceEditorKpiGameCaptureEntry;
typedef JceScreenshotSchedule JceEditorKpiGameCaptureSchedule;

bool jce_editor_kpi_game_capture_parse(
    const char *spec, JceEditorKpiGameCaptureSchedule *out,
    char *error, size_t error_size);

bool jce_editor_kpi_game_capture_parse_size(
    const char *spec, uint16_t *out_width, uint16_t *out_height);

void jce_editor_kpi_game_capture_tick(
    JceEditorKpiGameCaptureSchedule *schedule,
    JceEditorKpiPlayState play_state, double dt_seconds);

const JceEditorKpiGameCaptureEntry *jce_editor_kpi_game_capture_due(
    const JceEditorKpiGameCaptureSchedule *schedule);

bool jce_editor_kpi_game_capture_mark_submitted(
    JceEditorKpiGameCaptureSchedule *schedule);

void jce_editor_kpi_game_capture_mark_complete(
    JceEditorKpiGameCaptureSchedule *schedule, bool success);

bool jce_editor_kpi_game_capture_finished(
    const JceEditorKpiGameCaptureSchedule *schedule);

/* Process-global QA controller used by the editor host and Game View panel.
 * It owns only schedule state; rendering/readback stays in scene/. */
bool jce_editor_kpi_game_capture_global_configure(
    const char *shots_spec, const char *size_spec,
    char *error, size_t error_size);
void jce_editor_kpi_game_capture_global_disable(void);
bool jce_editor_kpi_game_capture_global_active(void);
bool jce_editor_kpi_game_capture_global_in_flight(void);
bool jce_editor_kpi_game_capture_global_needs_focus(void);
void jce_editor_kpi_game_capture_global_tick(
    JceEditorKpiPlayState play_state, double dt_seconds);
bool jce_editor_kpi_game_capture_global_render_size(
    uint16_t *out_width, uint16_t *out_height);
void jce_editor_kpi_game_capture_global_note_rendered(void);
const char *jce_editor_kpi_game_capture_global_due_path(void);
bool jce_editor_kpi_game_capture_global_mark_submitted(void);
void jce_editor_kpi_game_capture_global_mark_complete(bool success);

#endif /* JCE_EDITOR_KPI_GAME_CAPTURE_H */
