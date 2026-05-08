/*
 * jce_time.h  Global frame-time accessor (Unity-style Time singleton).
 *
 * The engine's iterate loop calls jce_time_advance() once per frame
 * with the wall-clock delta.  Game code reads back via jce_time_*().
 *
 * Equivalent Unity APIs:
 *   jce_time_delta()                  ↔ Time.deltaTime
 *   jce_time_unscaled_delta()         ↔ Time.unscaledDeltaTime
 *   jce_time_realtime_since_startup() ↔ Time.realtimeSinceStartup
 *   jce_time_frame_count()            ↔ Time.frameCount
 *   jce_time_get_scale() / set        ↔ Time.timeScale
 *
 * Thread safety: written from the main thread (engine iterate); reads
 * are best-effort lock-free.  Don't read from background threads while
 * the iterate loop is running.
 *
 * Layer: application.
 */

#ifndef JCE_TIME_H
#define JCE_TIME_H

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* Scaled delta-time (= unscaled_delta * timescale).  Game logic that
 * should pause when timescale = 0 reads this. */
JCE_API float JCE_CALL jce_time_delta(void);

/* Unscaled wall-clock delta in seconds.  UI animations / pause menus
 * read this so they continue when timescale = 0. */
JCE_API float JCE_CALL jce_time_unscaled_delta(void);

/* Total elapsed seconds since engine create. */
JCE_API float JCE_CALL jce_time_realtime_since_startup(void);

/* Total scaled elapsed seconds (sum of jce_time_delta over all frames). */
JCE_API float JCE_CALL jce_time_total(void);

/* Number of frames elapsed since startup.  Increments inside advance(). */
JCE_API uint64_t JCE_CALL jce_time_frame_count(void);

/* Time scale.  Default = 1.0.  0.0 freezes scaled time (UI/pause menus
 * that need to keep ticking should use jce_time_unscaled_delta()). */
JCE_API float JCE_CALL jce_time_get_scale(void);
JCE_API void  JCE_CALL jce_time_set_scale(float scale);

/* Engine-internal: called once per frame by jce_engine_iterate before
 * dispatching to game update.  Game code should not call this. */
JCE_API void JCE_CALL jce_time_advance(float unscaled_dt);

/* Engine-internal: called once at engine create to reset state. */
JCE_API void JCE_CALL jce_time_reset(void);

JCE_EXTERN_C_END

#endif /* JCE_TIME_H */
