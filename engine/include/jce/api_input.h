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
#include <jce/os/platform/jce_keys.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_INPUT_H */
