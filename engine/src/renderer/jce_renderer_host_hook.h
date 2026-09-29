/*
 * jce_renderer_host_hook.h -- the invoker half of the automated-capture hook.
 *
 * WHY THIS IS ITS OWN HEADER AND NOT A LINE IN
 * jce_renderer_bgfx_callback.h, WHICH IS WHERE THE CODE LIVES.
 *
 * That header declares jce_rcb_callback_interface() and therefore includes
 * <bgfx/c99/bgfx.h>.  bgfx is a PRIVATE dependency of jce_renderer -- the
 * ownership matrix forbids "bgfx types in public headers" and does not
 * propagate the include path to consumers -- so a unit test that wanted the
 * invoker would have had to drag bgfx in to get one declaration of one
 * function that takes a string.
 *
 * The alternative was to forward-declare it in the test, and that is the
 * one thing not to do here: a second declaration of a C function is not
 * checked against the first by the linker, so a signature that drifted
 * would be a wrong-shaped call rather than a build error.  That is the
 * JceScriptHost hazard in miniature.
 *
 * So: ONE declaration, in a header with no dependencies, included both by
 * the translation unit that defines it and by the test that exercises it.
 */

#ifndef JCE_RENDERER_HOST_HOOK_H
#define JCE_RENDERER_HOST_HOOK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ask the host's automated-capture hook (jce_renderer_set_auto_capture_hook)
 * to take this frame's shot.
 *
 * Returns false when nobody is installed, AND when the host declined because
 * it had nothing to photograph this frame -- the editor's Game View panel can
 * be closed, and then there is no offscreen target.  Both mean the caller
 * falls back to the backbuffer, which is what every host did before the hook
 * existed.  Treating "declined" as "taken" would produce no file at all, with
 * nothing reporting it.
 *
 * Internal because a host INSTALLS a hook; it does not invoke one. */
bool jce_rcb_host_took_capture(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* JCE_RENDERER_HOST_HOOK_H */
