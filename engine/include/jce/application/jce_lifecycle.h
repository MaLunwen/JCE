/*
 * jce_lifecycle.h  Application lifecycle event registry (P3-B.3).
 *
 * Unity parity:
 *   FOCUS_GAINED / FOCUS_LOST   ≈ OnApplicationFocus
 *   PAUSE        / RESUME       ≈ OnApplicationPause
 *   LOW_MEMORY                  ≈ OnApplicationLowMemory
 *   WILL_QUIT                   ≈ OnApplicationQuit (last-chance to save)
 *   DEVICE_LOST  / DEVICE_RESET ≈ GPU device-lost notifications (desktop)
 *
 * Events are emitted by the platform layer (SDL3 event bridge) on the
 * MAIN thread.  Callbacks must therefore not assume any other thread
 * affinity, and must be cheap (no blocking I/O — save snapshots should
 * be queued, not synchronous).
 *
 * Layer: L6 (application).  Consumed via <jce/api_app.h>.
 */

#ifndef JCE_LIFECYCLE_H
#define JCE_LIFECYCLE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum JceLifecycleEvent {
    JCE_LIFECYCLE_FOCUS_GAINED = 0,
    JCE_LIFECYCLE_FOCUS_LOST,
    JCE_LIFECYCLE_PAUSE,         /* app backgrounded (mobile / minimized desktop) */
    JCE_LIFECYCLE_RESUME,        /* app foregrounded again                        */
    JCE_LIFECYCLE_LOW_MEMORY,    /* OS signals memory pressure                    */
    JCE_LIFECYCLE_WILL_QUIT,     /* about to exit — last chance to save           */
    JCE_LIFECYCLE_DEVICE_LOST,   /* GPU device lost (desktop only typically)      */
    JCE_LIFECYCLE_DEVICE_RESET,  /* GPU device reset complete                     */
    JCE_LIFECYCLE_EVENT_COUNT
} JceLifecycleEvent;

typedef void (JCE_CALL *JceLifecycleCallback)(JceLifecycleEvent event, void *user);

/* Opaque handle returned by registration.  id == 0 means invalid. */
typedef struct JceLifecycleHandle {
    uint32_t id;
} JceLifecycleHandle;

/* Register a callback.  Lower priority runs earlier.  Returns a handle
 * whose `id` is non-zero on success.  Main-thread only. */
JCE_API JceLifecycleHandle JCE_CALL
jce_lifecycle_register(JceLifecycleCallback cb, int32_t priority, void *user);

JCE_API void JCE_CALL jce_lifecycle_unregister(JceLifecycleHandle h);

/* Emit an event to all listeners (in priority order).  This is the
 * platform-internal hook called by the SDL event pump; game code
 * should not call it directly. */
JCE_API void JCE_CALL jce_lifecycle_emit(JceLifecycleEvent event);

/* Convenience state getters reflecting the last paired event seen. */
JCE_API bool JCE_CALL jce_lifecycle_is_focused(void);
JCE_API bool JCE_CALL jce_lifecycle_is_paused(void);

/* String helper for logs.  Returns a static literal; never NULL. */
JCE_API const char *JCE_CALL jce_lifecycle_event_to_string(JceLifecycleEvent event);

/* Release the listener table.  Called from jce_engine_destroy after
 * every subsystem has had a chance to unregister.  Safe to call when
 * empty. */
JCE_API void JCE_CALL jce_lifecycle_shutdown(void);

JCE_EXTERN_C_END

#endif /* JCE_LIFECYCLE_H */
