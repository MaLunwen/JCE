/*
 * jce_editor_ecs_adapter.h  Bridge between editor UI types and engine ECS.
 *
 * Converts JceComponentInfo (editor union) ↔ engine ECS component structs.
 * Handles type mismatches:
 *   - Transform: euler degrees (editor) ↔ quaternion (engine)
 *   - Light: unified type+fields (editor) ↔ 3 separate components (engine)
 *   - Camera: field name mapping (fov/near_clip/far_clip ↔ fov_deg/near_plane/far_plane)
 *   - SpotLight: cone degrees (editor) ↔ cos of angles (engine)
 *
 * Also provides bulk sync functions for Save/Load/Undo/Redo workflows.
 */

#ifndef JCE_EDITOR_ECS_ADAPTER_H
#define JCE_EDITOR_ECS_ADAPTER_H

#include "jce_editor_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/scene/jce_scene.h>

/* ── Single-component conversion ──────────────────────────────────── */

/*
 * Read a component from ECS and populate a JceComponentInfo.
 * For lights: pass JCE_COMP_LIGHT; the adapter checks all 3 engine types.
 * Returns true if the component was found and converted.
 */
bool jce_adapter_ecs_to_comp_info(JceScene *scene, JceEntity entity,
                                  JceComponentType type,
                                  JceComponentInfo *out);

/*
 * Write a JceComponentInfo to the ECS.
 * For lights: the adapter writes to the correct engine component type
 * based on comp->data.light.type, and removes the other light types.
 */
void jce_adapter_comp_info_to_ecs(JceScene *scene, JceEntity entity,
                                  const JceComponentInfo *comp);

/*
 * Remove a component type from ECS.
 * For lights: removes all 3 engine light types.
 */
void jce_adapter_remove_from_ecs(JceScene *scene, JceEntity entity,
                                 JceComponentType type);

/* ── Entity-level conversion ──────────────────────────────────────── */

/*
 * Populate a JceEntityInfo from an ECS entity.
 * Fills: name, enabled, tag, tag_color, parent_id (as uint32_t placeholder),
 *        prefab_instance, prefab_path, component_count.
 * Does NOT set: id, ecs_entity, children[] (caller must handle).
 */
void jce_adapter_ecs_to_entity_info(JceScene *scene, JceEntity entity,
                                    JceEntityInfo *out);

/*
 * Get the number of editor-visible components on an ECS entity.
 * Maps engine's 3 light types → 1 editor Light component.
 */
int jce_adapter_get_comp_count(JceScene *scene, JceEntity entity);

/*
 * Read all components of an ECS entity into a JceComponentInfo array.
 * Returns the number of components written (up to max_out).
 */
int jce_adapter_get_all_comps(JceScene *scene, JceEntity entity,
                              JceComponentInfo *out, int max_out);

/* ── Bulk sync ────────────────────────────────────────────────────── */

/*
 * Push all editor entity/component arrays → ECS.
 * Called before engine serialization (Save, Undo snapshot).
 * Clears and rebuilds the ECS scene from editor arrays.
 */
void jce_adapter_sync_editor_to_ecs(void);

/*
 * Pull ECS → editor entity/component arrays.
 * Called after engine deserialization (Load, Undo restore).
 * Clears and rebuilds editor arrays from ECS state.
 */
void jce_adapter_sync_ecs_to_editor(void);

/* ── Editor metadata helpers ──────────────────────────────────────── */

/*
 * Push JceEntityInfo metadata (tag, tag_color, enabled, prefab) → ECS EditorMeta.
 */
void jce_adapter_push_entity_meta(JceScene *scene, JceEntity entity,
                                  const JceEntityInfo *info);

/*
 * Pull ECS EditorMeta → JceEntityInfo metadata fields.
 */
void jce_adapter_pull_entity_meta(JceScene *scene, JceEntity entity,
                                  JceEntityInfo *info);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_ECS_ADAPTER_H */
