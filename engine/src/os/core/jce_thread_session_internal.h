/* Private Engine admission for the existing shared frame-work pool. */
#ifndef JCE_THREAD_SESSION_INTERNAL_H
#define JCE_THREAD_SESSION_INTERNAL_H
#include <jce/os/core/jce_defs.h>
JCE_EXTERN_C_BEGIN
/* Owner-thread transitions under the shared-pool guard. Close only prevents
 * lazy creation; already borrowed pools retain their original lifetime contract.
 * Open only after publishing a newly admitted Engine owner. Closing persists
 * across deferred teardown and the inter-session gap. No callbacks/allocations. */
void jce_thread_pool_session_open_internal(void);
void jce_thread_pool_session_close_internal(void);
JCE_EXTERN_C_END
#endif
