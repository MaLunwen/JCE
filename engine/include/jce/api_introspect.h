/*
 * api_introspect.h  Machine-readable self-description.
 *
 * What the engine accepts (component types and their fields, with the
 * engine's own defaults), what a loaded scene contains, and what this process
 * is currently doing -- all as UTF-8 JSON, for a caller on the other side of
 * a process boundary.
 *
 * This is the engine half of the Automation layer (private/tools/automation/): the
 * tools there describe a project by reading its files, and this describes the
 * ENGINE by asking the engine.  The two are cross-checked against each other,
 * which is what keeps either from drifting into a second, quietly wrong,
 * description of the same components.
 *
 * Nothing here changes any engine state.  Every function is a read.
 */

#ifndef JCE_API_INTROSPECT_H
#define JCE_API_INTROSPECT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/application/jce_introspect.h>

#ifdef __cplusplus
}
#endif

#endif /* JCE_API_INTROSPECT_H */
