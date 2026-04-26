/*
 * jce_single_instance.h  Cross-platform process single-instance lock.
 *
 * Windows: named global mutex.
 * Linux/macOS: non-blocking flock on a lock file in /tmp.
 */

#ifndef JCE_SINGLE_INSTANCE_H
#define JCE_SINGLE_INSTANCE_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Acquire the global process lock for the given app name.
 * Returns true when lock is acquired, false when another instance is running
 * (or when lock acquisition fails). */
bool jce_single_instance_lock(const char *app_name);

/* Release the global process lock if held by this process. */
void jce_single_instance_unlock(void);

/* Query whether this process currently owns the lock. */
bool jce_single_instance_is_locked(void);

JCE_EXTERN_C_END

#endif /* JCE_SINGLE_INSTANCE_H */
