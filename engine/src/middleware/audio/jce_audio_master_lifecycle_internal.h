/* Private Engine-session ownership of the existing process audio master. */
#ifndef JCE_AUDIO_MASTER_LIFECYCLE_INTERNAL_H
#define JCE_AUDIO_MASTER_LIFECYCLE_INTERNAL_H
#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
JCE_EXTERN_C_BEGIN
/* Main owner only. Close is latched through deferred Engine destruction.
 * Standalone audio before the first Engine retains its existing admission.
 * Close denies new device-driven graphs/taps, not offline graphs or release. */
/* Lock-free, allocation-free current-callback query, including Web ID zero. */
bool jce_audio_master_is_callback_internal(void);
/* Serialized owner-thread observation only; no callback/device control.
 * True means the actual process master device is initialized. False also
 * covers never-created, failed-start rollback and the no-audio backend. */
bool jce_audio_master_device_live_internal(void);
void jce_audio_master_session_open_internal(void);
void jce_audio_master_session_close_internal(void);
/* Close callback admission and wait up to two seconds for actual borrowers.
 * Refusal retains all targets and the device, with admission still closed.
 * After quiescence, native uninit may have platform-specific join latency.
 * Called before app.exit; never from Play/Stop or last-graph destruction. */
bool jce_audio_master_try_retire_internal(void);
JCE_EXTERN_C_END
#endif
