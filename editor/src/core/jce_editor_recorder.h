/*
 * jce_editor_recorder.h  F9 screen recorder (editor).
 *
 * Captures the editor backbuffer (via the renderer capture sink, screenshot
 * path) and encodes it to a VP9 .webm on a worker thread. Single global
 * instance. Audio (Opus) is a planned follow-up.
 */

#ifndef JCE_EDITOR_RECORDER_H
#define JCE_EDITOR_RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRenderer JceRenderer;

/* Start recording the backbuffer to `out_webm_path` (.webm, VP9). Returns
 * false if already recording or on failure. */
bool jce_editor_recorder_start(JceRenderer *r, const char *out_webm_path);

/* Stop recording: disable capture, drain + join the worker, finalize the file. */
void jce_editor_recorder_stop(void);

bool     jce_editor_recorder_is_active(void);
uint32_t jce_editor_recorder_frame_count(void);  /* frames encoded */
uint32_t jce_editor_recorder_dropped(void);      /* frames dropped (queue full) */

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_RECORDER_H */
