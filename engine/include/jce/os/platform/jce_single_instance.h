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
JCE_API bool jce_single_instance_lock(const char *app_name);

/* Release the global process lock if held by this process. */
JCE_API void jce_single_instance_unlock(void);

/* Query whether this process currently owns the lock. */
JCE_API bool jce_single_instance_is_locked(void);

/* First instance: publish the main window's native handle (Win32 HWND)
 * once it exists, so a later second instance can activate it.  No-op on
 * platforms whose OS enforces the single-instance model itself. */
JCE_API void jce_single_instance_publish_window(void *native_window_handle);

/* Second instance (after jce_single_instance_lock returned false): bring
 * the existing instance's window to the user — restore if minimized, try
 * to foreground it, and flash its taskbar button until it gains focus
 * (the no-modal industry behavior; Windows may deny the foreground
 * switch to a background process, in which case the flash IS the
 * attention signal).  Returns true when a live window was found. */
JCE_API bool jce_single_instance_activate_existing(void);

JCE_EXTERN_C_END

#endif /* JCE_SINGLE_INSTANCE_H */
