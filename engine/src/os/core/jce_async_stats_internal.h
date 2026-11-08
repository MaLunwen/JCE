/* Private default-owner snapshot: no borrowed executor or lazy creation. */
#ifndef JCE_ASYNC_STATS_INTERNAL_H
#define JCE_ASYNC_STATS_INTERNAL_H
#include <jce/os/core/jce_async.h>
JCE_EXTERN_C_BEGIN
/* Returns false with zeroed output while absent/closing. NULL is refused.
 * Current-default guard protects the snapshot, including destroy/recreate. */
bool jce_async_default_stats_internal(JceAsyncExecutorStats *out_stats);
JCE_EXTERN_C_END
#endif
