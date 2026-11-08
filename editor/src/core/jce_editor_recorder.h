/*
 * jce_editor_recorder.h  F9 screen recorder (editor).
 *
 * Captures the editor window through its renderer sink and encodes VP9 with
 * optional engine master-mix Opus on a worker. Single global instance.
 */

#ifndef JCE_EDITOR_RECORDER_H
#define JCE_EDITOR_RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRenderer JceRenderer;

/* Editor-main-thread lifecycle only; calls from a capture callback refuse.
 * Start recording to `out_webm_path` (VP9 with optional Opus). Returns
 * false if already recording or on failure. */
bool jce_editor_recorder_start(JceRenderer *r, const char *out_webm_path);

/* Close callback admission, wait actual Recorder borrowers, drain/join worker.
 * Waits have no timeout-and-free fallback. Sink clearing alone is not a source
 * callback join. A late old screenshot after a new Start has unknown generation.
 * Recursive/non-main Stop refuses and retains the current session owner. */
void jce_editor_recorder_stop(void);

/* Ordinary GUI main update consumes an invalid/changed capture-format request.
 * The first valid admitted format stays fixed until Stop; callbacks never Stop.
 * Restart F9 after retirement to record at the new dimensions. */
void jce_editor_recorder_update(void);

bool     jce_editor_recorder_is_active(void);
uint32_t jce_editor_recorder_frame_count(void);  /* frames encoded */
uint32_t jce_editor_recorder_dropped(void);      /* frames dropped (12 total video buffers busy) */

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_RECORDER_H */
