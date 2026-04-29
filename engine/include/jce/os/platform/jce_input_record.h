/*
 * jce_input_record.h  Deterministic input record / replay.
 *
 * Wraps a JceInput instance to capture per-frame snapshots to a file
 * (record) or to inject snapshots from a file overriding live input
 * (replay).  Designed for bug repros, regression tests, and demos.
 *
 * File format (little-endian, padded for portability):
 *   magic   : "JIRC"            4 B
 *   version : uint32            4 B   (== JCE_INPUT_FRAME_VERSION)
 *   _pad    : uint64            8 B   (reserved, must be 0)
 *   frames  : JceInputFrame[N]        (N derived from EOF)
 *
 * Threading: not thread-safe.  Drive from the main loop.
 */

#ifndef JCE_INPUT_RECORD_H
#define JCE_INPUT_RECORD_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/platform/jce_input.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceInputRecorder JceInputRecorder;

/* Open `path` for record OR replay.  Returns NULL on I/O failure or, in
 * replay mode, on header / version mismatch. */
JCE_API JceInputRecorder *jce_input_record_open(const char *path);
JCE_API JceInputRecorder *jce_input_replay_open(const char *path);

JCE_API void              jce_input_record_close(JceInputRecorder *r);

/* Record mode: capture the current input snapshot and append it to the
 * file.  Call once per frame, AFTER your normal input update. */
JCE_API bool jce_input_record_tick(JceInputRecorder *r, const JceInput *input);

/* Replay mode: read the next frame snapshot and apply it to `input`,
 * overriding live state.  Returns false at EOF — caller should then
 * close the recorder.  Call once per frame BEFORE the game reads input. */
JCE_API bool jce_input_replay_tick(JceInputRecorder *r, JceInput *input);

/* Total number of frames written so far (record) or read so far (replay). */
JCE_API uint64_t jce_input_record_frame_count(const JceInputRecorder *r);

JCE_EXTERN_C_END

#endif /* JCE_INPUT_RECORD_H */
