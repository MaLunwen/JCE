#include "jce_editor_history_entity.h"
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>
#include <algorithm>

bool jce_history_parent_valid(JceScene *scene, JceEntity child, JceEntity parent,
                              size_t entity_count)
{
    if (!scene || !child || !jce_scene_entity_alive(scene, child)) return false;
    size_t visited = 0;
    for (JceEntity cursor = parent; cursor; cursor = jce_scene_get_parent(scene, cursor)) {
        if (cursor == child || !jce_scene_entity_alive(scene, cursor) || ++visited > entity_count)
            return false;
    }
    return true;
}

bool jce_history_restore_entity(JceScene *scene, JceEntity child, JceEntity parent,
                                bool restore_parent, size_t entity_count,
                                const std::string &json, const std::vector<int> &components,
                                bool parent_required)
{
    // Parse and validate before changing hierarchy or removing components.
    if ((restore_parent && parent_required && !parent) ||
        !scene || !child || !jce_scene_entity_alive(scene, child) ||
        (restore_parent && !jce_history_parent_valid(scene, child, parent, entity_count))) return false;
    JceJson *object = jce_json_parse(json.c_str(), json.size());
    if (!object) return false;
    if (!jce_json_is_object(object) || !jce_json_is_array(jce_json_get(object, "components"))) { jce_json_free(object); return false; }
    if (restore_parent && !jce_scene_reparent(scene, child, parent, false)) {
        jce_json_free(object); return false;
    }
    int count = jce_component_count();
    for (int id = 0; id < count; ++id)
        if (jce_scene_has_comp(scene, child, id) &&
            std::find(components.begin(), components.end(), id) == components.end())
            jce_scene_remove_comp(scene, child, id);
    // Component records come from our trusted serializer; parse is a void API.
    // Shape/syntax checks do not certify arbitrary hostile component payloads.
    // The snapshot contains exact local TRS, not a recomputed world-preserve solve.
    jce_scene_parse_entity_json(scene, child, object);
    jce_json_free(object);
    return true;
}
