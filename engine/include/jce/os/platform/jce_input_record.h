/*
 * jce_input_record.h  Deterministic input record / replay.
 *
 * Wraps a JceInput instance to capture per-frame snapshots to a file
 * (record) or to inject snapshots from a file overriding live input
 * (replay).  Designed for bug repros, regression tests, and demos.
 *
 * File format (little-endian, padded for portability):
 *   magic      : "JIRC"           4 B
 *   version    : uint32           4 B   (== JCE_INPUT_FRAME_VERSION)
 *   frame_size : uint32           4 B   (== sizeof(JceInputFrame))
 *   _pad       : uint32           4 B   (reserved, must be 0; refused if not)
 *   frames     : JceInputFrame[N]       (N derived from EOF)
 *
 * frame_size is CHECKED on open and a mismatch is REFUSED.  A version number
 * says the layout changed without saying by how much, and striding frames at
 * the wrong pitch does not fail -- it produces garbage that looks like input.
 * It occupies four of the eight bytes schema 1 reserved and always wrote as
 * zero, so the header is still 16 bytes and a v1 file is still readable.
 *
 * A schema-1 file is UPGRADED on read, not rejected: its four pads widen into
 * device frames owned by player 0, and touch and identity fill empty, which is
 * exactly the information v1 carried.  The v1 frame stride is 696 bytes (692
 * of members, rounded up by the 8-byte alignment of keys_bits) and is pinned
 * at compile time in jce_input_record.c against
 * tests/os/platform/fixtures/jce_input_v1.jirc.
 *
 * Consumers: the engine app loop (jce_engine.c) drives this via the
 * JCE_INPUT_RECORD=<file.jirc> / JCE_INPUT_REPLAY=<file.jirc> env vars
 * (DEBUG TOGGLE family, alongside JCE_BACKEND / JCE_CAPTURE_FRAME) —
 * works in the editor and in shipped games alike.  Record/replay ticks
 * right before the action-map update each frame; the recorder is
 * finalized on engine destroy.
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
 * replay mode, on a bad magic, an unreadable schema version, a non-zero
 * reserved word, or a frame_size this build does not stride at.  Every
 * refusal is logged with the path and the reason. */
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
