/* Private Engine-session admission over the existing default executor owner. */
#ifndef JCE_ASYNC_SESSION_INTERNAL_H
#define JCE_ASYNC_SESSION_INTERNAL_H
#include <jce/os/core/jce_defs.h>
JCE_EXTERN_C_BEGIN

/* Engine owner thread only. Open after successful Engine owner publication;
 * close before entering DRAINING, including deferred destruction. Close remains
 * latched between sessions; only an admitted next constructor opens it again.
 * Both transitions use the default executor guard and invoke no callbacks.
 * Closing prevents lazy creation, not use of an already borrowed executor:
 * existing pointers retain the public owner-thread shutdown lifetime contract.
 * Public default shutdown/retry and independently owned executors are unchanged. */
void jce_async_default_session_open_internal(void);
void jce_async_default_session_close_internal(void);

JCE_EXTERN_C_END
#endif
