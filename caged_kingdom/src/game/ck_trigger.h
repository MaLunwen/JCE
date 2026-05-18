/*
 * ck_trigger.h — Caged Kingdom trigger zones.
 *
 * Loads the per-scene `ck_quest.objectives` block (top-level key in
 * a scene JSON; see caged_kingdom/SCENES_DESIGN.md appendix A.2.1)
 * and answers a single per-frame question:
 *
 *     "is the player inside any of this scene's trigger zones?"
 *
 * v1 zone type: cylindrical XZ distance check (Y ignored), so a zone
 * with center=(5,0,2) radius=2.5 fires whenever the player's XZ is
 * within 2.5 of (5,2).  This matches the data the editor places into
 * scenes today and keeps the math trivial on cold cache.
 *
 * The trigger set is read-only after load and owns no engine handles,
 * so destroying a scene does not invalidate the zone list.
 */
#ifndef CK_TRIGGER_H
#define CK_TRIGGER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceFileSystem JceFileSystem;
typedef struct CkTriggerSet  CkTriggerSet;

/* Load the trigger set from `vfs_path` (a scene JSON).  Returns an
   *empty* set when the file has no `ck_quest.objectives` block, so
   callers don't need to special-case scenes without triggers; returns
   NULL only on real failures (path missing, parse error, OOM). */
CkTriggerSet *ck_trigger_set_load_vfs(JceFileSystem *fs, const char *vfs_path);

void          ck_trigger_set_destroy(CkTriggerSet *set);

/* Quest id the trigger set belongs to (from `ck_quest.questId` in the
   scene file), or NULL if the scene had no `ck_quest` block. */
const char   *ck_trigger_set_quest_id(const CkTriggerSet *set);

/* Number of zones loaded.  Useful mostly for diagnostics. */
size_t        ck_trigger_set_count(const CkTriggerSet *set);

/* Return the id of the first zone whose XZ disc contains `pos[3]`,
   or NULL if the player is not inside any zone.  `pos` must point at
   3 floats {x, y, z}; the Y component is ignored. */
const char   *ck_trigger_set_check(const CkTriggerSet *set,
                                   const float pos[3]);

#ifdef __cplusplus
}
#endif

#endif /* CK_TRIGGER_H */
