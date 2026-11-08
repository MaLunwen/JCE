/* Optional finite host acknowledgement; owner-thread only, no public ABI. */
#ifndef JCE_ENGINE_TERMINAL_INTERNAL_H
#define JCE_ENGINE_TERMINAL_INTERNAL_H
#include <jce/os/core/jce_defs.h>
#include <stdint.h>
#include <stdbool.h>
#include "os/core/jce_log_internal.h"
#include "os/core/jce_trace_capture_internal.h"
JCE_EXTERN_C_BEGIN
/* A cursor is not an owner. Only begin, after actual owner publication, admits
 * a new ticket. Invalid/absent tokens disable that session's observation. */
uint64_t jce_engine_terminal_cursor_internal(void);
uint64_t jce_engine_terminal_after_create_internal(uint64_t before);
void jce_engine_terminal_begin_internal(void);
void jce_engine_terminal_trace_internal(uint64_t cleanup_scope);
void jce_engine_terminal_capture_internal(const JceTraceCaptureResult *result);
void jce_engine_terminal_log_internal(JceLogFinalClose result);
/* Called only after logger/SDL/storage/owner release and quit-session closure. */
void jce_engine_terminal_finish_internal(void);
/* Host calls after create/destroy returned, and JNI after bridge release.
 * Empty/no-op owners cannot invent tickets. Output is consumed once even when
 * stderr fails; false never revokes completed Engine cleanup. No console opens. */
bool jce_engine_terminal_emit_internal(uint64_t session, const char *host,
                                       int app_result, bool owner_empty);
JCE_EXTERN_C_END
#endif
