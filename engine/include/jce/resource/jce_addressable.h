/*
 * jce_addressable.h  Asset addressable groups.
 *
 * Unity Addressables equivalent at the data layer.  A "group" is a
 * named bundle of asset paths/labels; the runtime can request a
 * group by name and get back its asset list to load/unload as a
 * unit.  Used for live downloadable content + scene-specific
 * preloading.
 *
 * Sits on top of jce_asset_label_map (the per-asset label store);
 * this module owns the inverted index from group → asset paths.
 *
 * Layer: resource (Layer 3) — public.
 */

#ifndef JCE_ADDRESSABLE_H
#define JCE_ADDRESSABLE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_ADDRESSABLE_GROUPS_MAX     32
#define JCE_ADDRESSABLE_GROUP_NAME_LEN 48
#define JCE_ADDRESSABLE_PATH_LEN       192
#define JCE_ADDRESSABLE_PATHS_PER_GROUP 256

typedef struct {
    char     name[JCE_ADDRESSABLE_GROUP_NAME_LEN];
    char     paths[JCE_ADDRESSABLE_PATHS_PER_GROUP]
                  [JCE_ADDRESSABLE_PATH_LEN];
    uint32_t path_count;
    /* Loading state — caller can flip these when async work begins/ends. */
    bool     loaded;
    bool     active;
} JceAddressableGroup;

/* Register or update a group.  `paths` is a NULL-terminated array of
 * NUL-terminated asset paths.  Returns the group index, or
 * UINT32_MAX on overflow. */
JCE_API uint32_t jce_addressable_register_group(const char *name,
                                                  const char *const *paths);

/* Append one path to an existing group.  Idempotent on duplicates. */
JCE_API bool     jce_addressable_add_path(uint32_t group_idx, const char *path);

/* Remove a group entirely.  Returns true if found. */
JCE_API bool     jce_addressable_unregister(const char *name);

JCE_API uint32_t jce_addressable_group_count(void);
JCE_API const JceAddressableGroup *jce_addressable_get(uint32_t idx);
JCE_API const JceAddressableGroup *jce_addressable_find(const char *name);

/* Mark / query loaded state — used by the streaming module to avoid
 * re-issuing async loads. */
JCE_API void jce_addressable_set_loaded(const char *name, bool loaded);

JCE_EXTERN_C_END

#endif /* JCE_ADDRESSABLE_H */
