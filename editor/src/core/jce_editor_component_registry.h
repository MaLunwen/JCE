/*
 * jce_editor_component_registry.h  Editor-side component identity table.
 *
 * Each editor descriptor row is backed by an ENGINE component-registry row
 * (jce_component_registry.h): `engine_name` is the canonical registry name
 * and `comp_id` is resolved from it at first use.  Presence, raw access,
 * removal, the per-component enable toggle, inspector fold/order state,
 * presets and the multi-select broadcast all route through the dense
 * comp_id — adding a future component takes ONE engine REG row plus ONE
 * descriptor row here (plus its draw/default adapters).
 *
 * The synthetic JCE_EDITOR_COMP_SLOT_* constants (the pre-registry
 * workaround for the exhausted 64-bit flag space) are GONE: no editor
 * state is keyed on them anymore.  Their raw values survive only inside
 * jce_editor_presets.cpp's legacy-header migration table (old .preset
 * files persisted them).  The `slot` / `legacy_flag` fields remain solely
 * as a flag-compat lookup surface for call sites that still hold a
 * JCE_COMP_FLAG_* value (slot == legacy_flag there; rows without a flag
 * carry slot 0 and are NOT addressable by slot).  Do not key new code on
 * slots or legacy flags — use comp_id / engine_name.
 */

#ifndef JCE_EDITOR_COMPONENT_REGISTRY_H
#define JCE_EDITOR_COMPONENT_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_component_registry.h>
}

typedef uint64_t JceEditorComponentSlot;

#define JCE_EDITOR_COMPONENT_LIGHT_MASK \
    (JCE_COMP_FLAG_DIR_LIGHT | JCE_COMP_FLAG_POINT_LIGHT | JCE_COMP_FLAG_SPOT_LIGHT)

/* Per-row behavior hooks, registered at runtime (the registry TU cannot
 * reference panel symbols, so panels/core install these via the
 * jce_editor_component_set_*_fn calls below). */
typedef void (*JceEditorCompDrawFn)(JceScene *scene, JceEntity entity,
                                    uint32_t entity_id);
typedef void (*JceEditorCompAddDefaultFn)(JceScene *scene, JceEntity entity);
typedef void (*JceEditorCompDupFixupFn)(JceScene *scene, JceEntity dst);
typedef void (*JceEditorCompPreRemoveFn)(JceScene *scene, JceEntity entity);

typedef struct {
    JceEditorComponentSlot slot;        /* DEPRECATED flag-compat id: equals
                                         * legacy_flag, 0 when the row has
                                         * no flag (not slot-addressable). */
    uint64_t               legacy_flag; /* DEPRECATED; 0 for post-64 rows  */
    const char            *engine_name; /* canonical engine registry name  */
    const char            *display_name;
    const char            *i18n_key;
    bool                   addable;
    bool                   removable;
    bool                   multi_edit_supported;

    /* Runtime-resolved / runtime-registered (never in the table literal). */
    int                        comp_id;     /* engine row; JCE_COMP_ID_INVALID
                                             * until the engine registry is
                                             * populated (first scene). */
    JceEditorCompDrawFn        draw;        /* inspector section body      */
    JceEditorCompAddDefaultFn  add_default; /* default-init on add         */
    JceEditorCompDupFixupFn    dup_fixup;   /* post-copy fixup on duplicate */
    JceEditorCompPreRemoveFn   pre_remove;  /* runtime cleanup before remove */
} JceEditorComponentDescriptor;

int jce_editor_component_descriptor_count(void);
const JceEditorComponentDescriptor *jce_editor_component_descriptor_at(int index);

/* Canonical lookups. */
const JceEditorComponentDescriptor *jce_editor_component_find_by_id(int comp_id);
const JceEditorComponentDescriptor *jce_editor_component_find_by_name(
    const char *engine_name);

/* DEPRECATED flag-compat lookup for call sites that still hold a
 * JCE_COMP_FLAG_* value.  Returns NULL for slot 0. */
const JceEditorComponentDescriptor *jce_editor_component_find(
    JceEditorComponentSlot slot);

/* Engine comp_id for a legacy flag value (resolved lazily from
 * engine_name).  JCE_COMP_ID_INVALID for unknown slots or before the
 * engine registry is populated. */
int jce_editor_component_comp_id(JceEditorComponentSlot slot);

/* Default inspector section order, as dense comp_ids.  Entries resolve to
 * JCE_COMP_ID_INVALID before the engine registry is populated. */
int jce_editor_component_default_order_count(void);
int jce_editor_component_default_order_comp_id(int index);

/* Lights stay a UI aggregation (one "Light" section over the three engine
 * light components): the group is the engine's unified "Light" row, whose
 * presence answers "any of the three". */
bool jce_editor_component_id_is_light_group(int comp_id);

/* Hook installation, keyed by the canonical engine name (idempotent
 * overwrite). */
void jce_editor_component_set_draw_fn(const char *engine_name,
                                      JceEditorCompDrawFn fn);
void jce_editor_component_set_add_default_fn(const char *engine_name,
                                             JceEditorCompAddDefaultFn fn);
void jce_editor_component_set_dup_fixup_fn(const char *engine_name,
                                           JceEditorCompDupFixupFn fn);
void jce_editor_component_set_pre_remove_fn(const char *engine_name,
                                            JceEditorCompPreRemoveFn fn);

/* Installs every add_default / dup_fixup / pre_remove hook (idempotent).
 * Implemented in jce_editor_component_defaults.cpp; called by the state
 * layer before any registry-routed add/remove/duplicate. */
void jce_editor_component_defaults_ensure_registered(void);

void jce_editor_component_compound_default(
    JceCompoundColliderComponent *out);

#endif /* JCE_EDITOR_COMPONENT_REGISTRY_H */
