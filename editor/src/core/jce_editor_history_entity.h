/* Private scoped restore service; public scene APIs, no UI or history stack. */
#ifndef JCE_EDITOR_HISTORY_ENTITY_H
#define JCE_EDITOR_HISTORY_ENTITY_H
#include <jce/middleware/scene/jce_scene.h>
#include <string>
#include <vector>

bool jce_history_parent_valid(JceScene *scene, JceEntity child, JceEntity parent,
                              size_t entity_count);
bool jce_history_restore_entity(JceScene *scene, JceEntity child, JceEntity parent,
                                bool restore_parent, size_t entity_count,
                                const std::string &json, const std::vector<int> &components,
                                bool parent_required = false);
#endif
