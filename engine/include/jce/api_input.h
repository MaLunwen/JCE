/*
 * api_input.h  Input devices & action mapping (The-Forge IInput parity).
 *
 * Convenience re-export so client code that only needs input does not
 * have to pull the full <jce/api_platform.h> set.  Kept under the
 * Layer 2 (Platform) umbrella.
 */

#ifndef JCE_API_INPUT_H
#define JCE_API_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_input_device.h>
#include <jce/os/platform/jce_input_event.h>
/* Six JCE_API functions that no umbrella reached from 393d46eb until now: a
 * client including <jce/api_input.h> could not see the .jirc record/replay API
 * at all, though the header ships in the SDK include tree. */
#include <jce/os/platform/jce_input_record.h>
#include <jce/os/platform/jce_keys.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_INPUT_H */
