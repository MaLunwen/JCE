/*
 * jce_editor_prefab_overrides.cpp  Editor-side prefab override holder.
 *
 * Singleton wrapper around JcePrefabOverrideSet so Inspector buttons
 * have a stable target to read/mutate.  Persistent storage hookup
 * (auto-load on scene open, auto-save on close) is deferred —
 * exposing the data to the UI layer first lets us iterate on UX
 * without committing to a final on-disk schema.
 */

#include "jce_editor_prefab_overrides.h"

extern "C" {
#include <jce/middleware/scene/jce_prefab_overrides.h>
}

namespace {
JcePrefabOverrideSet *g_set = nullptr;
} /* namespace */

extern "C" JcePrefabOverrideSet *jce_editor_prefab_overrides(void)
{
    if (!g_set) g_set = jce_prefab_overrides_create();
    return g_set;
}

extern "C" uint32_t jce_editor_prefab_overrides_count(const char *entity_path)
{
    return jce_prefab_overrides_count_for_entity(jce_editor_prefab_overrides(),
                                                  entity_path ? entity_path : "");
}

extern "C" bool jce_editor_prefab_overrides_apply(const char *base_path)
{
    if (!base_path) return false;
    return jce_prefab_overrides_apply_to_base(jce_editor_prefab_overrides(),
                                               base_path);
}

extern "C" uint32_t jce_editor_prefab_overrides_revert(const char *entity_path)
{
    return jce_prefab_overrides_revert_entity(jce_editor_prefab_overrides(),
                                               entity_path ? entity_path : "");
}
