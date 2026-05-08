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
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_keys.h>
#include <jce/os/platform/jce_single_instance.h>
#include <jce/os/platform/jce_window.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_PLATFORM_H */
