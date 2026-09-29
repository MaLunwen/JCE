/*
 * api_platform.h  Layer 2 — OS / platform abstraction.
 *
 * Window management, input devices, key codes, gamepad, touch,
 * single-instance guard, action mapping.
 * Built on SDL3 for cross-platform coverage.
 */

#ifndef JCE_API_PLATFORM_H
#define JCE_API_PLATFORM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/os/platform/jce_window_event.h>
#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_host_paths.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_input_device.h>
#include <jce/os/platform/jce_keys.h>
#include <jce/os/platform/jce_single_instance.h>
#include <jce/os/platform/jce_mmap.h>
#include <jce/os/platform/jce_window.h>

/* Completed 2026-08-31.  These headers export JCE_API symbols and were
 * reachable from NO umbrella, so §4's promise -- `#include <jce/api.h>`
 * gives you the whole engine -- did not hold for them.  Several are named
 * in §4's own table by capability.
 */
#include <jce/os/platform/jce_clipboard.h>
#include <jce/os/platform/jce_cursor.h>
#include <jce/os/platform/jce_entropy.h>
#include <jce/os/platform/jce_file_watcher.h>
#include <jce/os/platform/jce_host_dialog.h>
#include <jce/os/platform/jce_host_locale.h>
#include <jce/os/platform/jce_host_shell.h>
#include <jce/os/platform/jce_library.h>
#include <jce/os/platform/jce_tcp.h>
#include <jce/os/platform/jce_window_modal_loop.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_PLATFORM_H */
