/*
 * jce_editor_component_registry.h  Editor-side component identity table.
 *
 * Runtime component flags remain a uint64_t compatibility surface. The editor
 * uses slots so components without a free JCE_COMP_FLAG_* bit can still take
 * the same Inspector/Add Component path as legacy flag-backed components.
 */

#ifndef JCE_EDITOR_COMPONENT_REGISTRY_H
#define JCE_EDITOR_COMPONENT_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
}

typedef uint64_t JceEditorComponentSlot;

#define JCE_EDITOR_COMP_SLOT_LIGHT_GROUP        UINT64_MAX
#define JCE_EDITOR_COMP_SLOT_COMPOUND_COLLIDER (UINT64_MAX - UINT64_C(1))
#define JCE_EDITOR_COMP_SLOT_VIDEO_PLAYER      (UINT64_MAX - UINT64_C(2))

#define JCE_EDITOR_COMPONENT_LIGHT_MASK \
    (JCE_COMP_FLAG_DIR_LIGHT | JCE_COMP_FLAG_POINT_LIGHT | JCE_COMP_FLAG_SPOT_LIGHT)

typedef struct {
    JceEditorComponentSlot slot;
    uint64_t               legacy_flag; /* 0 for editor-only synthetic slots. */
    const char            *display_name;
    const char            *i18n_key;
    bool                   addable;
    bool                   removable;
    bool                   multi_edit_supported;
} JceEditorComponentDescriptor;

int jce_editor_component_descriptor_count(void);
const JceEditorComponentDescriptor *jce_editor_component_descriptor_at(int index);
const JceEditorComponentDescriptor *jce_editor_component_find(
    JceEditorComponentSlot slot);
const JceEditorComponentDescriptor *jce_editor_component_find_legacy(
    uint64_t legacy_flag);

int jce_editor_component_default_order_count(void);
JceEditorComponentSlot jce_editor_component_default_order_at(int index);

bool jce_editor_component_slot_is_legacy_flag(JceEditorComponentSlot slot);
bool jce_editor_component_slot_is_light_group(JceEditorComponentSlot slot);
bool jce_editor_component_slot_is_compound_collider(
    JceEditorComponentSlot slot);
bool jce_editor_component_slot_is_video_player(
    JceEditorComponentSlot slot);

const char *jce_editor_component_display_name(JceEditorComponentSlot slot);
const char *jce_editor_component_i18n_key(JceEditorComponentSlot slot);

bool jce_editor_component_slot_present(JceScene *scene,
                                       JceEntity entity,
                                       uint64_t legacy_flags,
                                       JceEditorComponentSlot slot);

void jce_editor_component_compound_default(
    JceCompoundColliderComponent *out);

#endif /* JCE_EDITOR_COMPONENT_REGISTRY_H */
