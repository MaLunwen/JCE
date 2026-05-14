/*
 * api_save.h  Save game / snapshot subsystem.
 *
 * Pulls in the low-level binary section writer (jce_snapshot) plus
 * the higher-level multi-slot manager (jce_save_slots).  Game code
 * typically only needs jce_save_slots — the snapshot layer is for
 * the binary blob the slot ferries.
 */

#ifndef JCE_API_SAVE_H
#define JCE_API_SAVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/save/jce_snapshot.h>
#include <jce/middleware/save/jce_save_slots.h>

#ifdef __cplusplus
}
#endif
#endif /* JCE_API_SAVE_H */
