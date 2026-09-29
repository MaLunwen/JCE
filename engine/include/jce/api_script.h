/*
 * api_script.h  Layer-4 facade for the gameplay scripting VM.
 *
 * Public SDK entry point for the Lua scripting subsystem; re-exports the
 * subsystem header (mirrors how <jce/api_ai.h> re-exports the AI headers).
 * Engine-internal code generally includes the subsystem header directly.
 */

#ifndef JCE_API_SCRIPT_H
#define JCE_API_SCRIPT_H

#include <jce/middleware/script/jce_script.h>

/* Completed 2026-08-31.  These headers export JCE_API symbols and were
 * reachable from NO umbrella, so §4's promise -- `#include <jce/api.h>`
 * gives you the whole engine -- did not hold for them.  Several are named
 * in §4's own table by capability.
 */
#include <jce/middleware/script/jce_script_vm.h>

#endif /* JCE_API_SCRIPT_H */
