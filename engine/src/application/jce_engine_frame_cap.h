/*
 * jce_engine_frame_cap.h — the frame-rate cap's policy, split from its sleep.
 *
 * jce_engine_set_target_fps holds the loop to an authored frame rate (Unity's
 * Application.targetFrameRate).  Two things happen at the end of each frame:
 * a DECISION -- how long to wait, and where the next frame's deadline goes --
 * and a WAIT.  Only the decision can be wrong in an interesting way, and it is
 * the half that a test cannot reach through jce_engine_iterate: that needs a
 * window, a renderer and a real clock, so a test of it would be measuring this
 * machine's scheduler rather than the policy.
 *
 * So the decision is a pure function of (now, budget, deadline) and lives
 * here.  jce_engine.c wraps it in the sleep.  Internal to the application
 * layer; not part of the public API.
 */
#ifndef JCE_ENGINE_FRAME_CAP_H
#define JCE_ENGINE_FRAME_CAP_H

#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

/*
 * Advance the frame-cap window and say how long to wait.
 *
 *   now       the monotonic counter, in jce_time_perf_counter() ticks
 *   budget    ticks per frame at the target rate (freq / fps); 0 disables
 *   deadline  in/out: the tick this frame was due to end.  0 means "unset" --
 *             the first capped frame, or the frame after a cap change.
 *
 * Returns the tick to wait until, or 0 for "do not wait".
 *
 * THREE CASES, and the two that are not "wait" are the ones worth stating:
 *
 *   unset      (*deadline == 0)  anchor a fresh window and do not wait.  A cap
 *              that stalled its very first frame would make a cap change look
 *              like a hitch.
 *   overrun    the frame blew its budget by a whole budget or more: re-anchor
 *              instead of waiting.  The alternative -- advancing the deadline
 *              by one budget and letting the loop catch up -- repays a 200 ms
 *              hitch with a burst of zero-length frames, which is visible as a
 *              speed-up and is exactly what a frame cap should not produce.
 *   wait       return the deadline and advance it by one budget.  The next
 *              deadline comes from the PREVIOUS deadline, not from `now`, so
 *              the cap does not drift late by the sleep's own overshoot every
 *              frame.
 */
/* NOT JCE_API: this is internal to the application layer.  JCE_API is this
 * tree's marker for public API surface -- it would count toward
 * check_editor_consumption, sit outside contracts/abi-snapshot.txt, and
 * promise SDK holders a symbol they cannot call.  The test reaches it by
 * adding engine/src to its include path, the same way the runtime-internal
 * tests do. */
uint64_t jce_engine_frame_cap_advance(uint64_t now, uint64_t budget,
                                      uint64_t *deadline);

JCE_EXTERN_C_END

#endif /* JCE_ENGINE_FRAME_CAP_H */
