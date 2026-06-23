/*
 * jce_editor_prefab_override.h  Prefab per-COMPONENT override tracking (core).
 *
 * Unity/Godot-style prefab instances: an instance stores only the
 * components that DIFFER (byte-wise) from its source prefab; load =
 * instantiate the source, then overlay the listed overridden components.
 *
 * This module is deliberately pure: every entry point takes explicit
 * JceScene* and JceEntity arguments and the JceJson facade — it touches NO
 * editor global state (`s.scene`), NO ImGui, NO file dialogs.  That keeps
 * the diff/overlay logic unit-testable in isolation (it links only
 * jce_core/jce_scene, like the engine scene tests).  The editor wrappers
 * in jce_editor_scene_serial.cpp / jce_editor_scene_parse.cpp /
 * jce_editor_prefab.cpp adapt the live scene + prefab files onto it.
 *
 * GRANULARITY: per-COMPONENT (the byte-diff via the public registry API
 * jce_scene_get_comp + memcmp) PLUS per-FIELD overrides layered on top.
 * Per-field needs no C reflection: it diffs/merges at the JSON-serialization
 * level (jce_scene_serialize_entity_components emits each component as a
 * keyed JSON object), so an instance can override a single field key (e.g.
 * Transform position) while every NON-overridden field tracks the source
 * prefab's CURRENT value on load.  See compute_field_overrides / merge_fields
 * / json_deep_equal below and the "fields" entry form on the "overrides"
 * array.  Components that cannot be field-diffed (the unified "Light", whose
 * concrete rows serialize with a NULL callback, and anything that serializes
 * to a non-object) fall back to the legacy whole-component bare-string form.
 *
 * BACKWARD COMPAT: purely additive.  The "overrides" JSON array only ever
 * appears on a prefab-instance node that actually has divergent
 * components.  Nodes without an "overrides" array take the unchanged
 * legacy full-snapshot path (see jce_prefab_override::node_has_overrides).
 */

#ifndef JCE_EDITOR_PREFAB_OVERRIDE_H
#define JCE_EDITOR_PREFAB_OVERRIDE_H

#include <string>
#include <vector>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_json.h>
}

namespace jce_prefab_override {

/* The JSON key carrying the overridden-component name list on an
 * instance node.  Its PRESENCE is the new-vs-legacy discriminator. */
inline constexpr const char *kOverridesKey = "overrides";

/* Byte-diff every registered component of the instance (inst_scene/inst_e)
 * against the in-memory source (src_scene/src_e) and return the canonical
 * names of the components that differ.  A component present on the instance
 * but absent on the source (or vice-versa) counts as differing.  Transform
 * is always evaluated like any other row (it is the most-edited override).
 * Deterministic order: ascending registry comp_id. */
std::vector<std::string> compute_overrides(JceScene *inst_scene,
                                           JceEntity inst_e,
                                           JceScene *src_scene,
                                           JceEntity src_e);

/* Is a single named component overridden (differs from source)?  Thin
 * wrapper over the same byte-diff; used by the inspector indicator. */
bool is_component_overridden(JceScene *inst_scene, JceEntity inst_e,
                             JceScene *src_scene, JceEntity src_e,
                             const char *comp_name);

/* ── Per-FIELD override primitives (JSON-level diff/merge) ──────────────
 *
 * These operate on the serialized JSON form of components — no struct
 * reflection.  They are the foundation for the additive "fields" entry on
 * the overrides array (see kFieldsKey / kComponentKey below).
 */

/* The "overrides" array element, in the per-field form, is an OBJECT:
 *   { "component": "Transform", "fields": ["position","scale"] }
 * The PRESENCE of "fields" selects per-field; a bare string element stays
 * the legacy whole-component override.  The full overridden component still
 * lives in the node's "components" array (standard scene format) — "fields"
 * is metadata naming WHICH keys are the real overrides. */
inline constexpr const char *kComponentKey = "component";
inline constexpr const char *kFieldsKey    = "fields";

/* The sentinel a per-field result uses to mean "the whole component is the
 * override" (component object missing on one side, or serializes to a
 * non-object so individual keys cannot be diffed).  When
 * compute_field_overrides returns a vector whose single element is this
 * sentinel, the caller emits the LEGACY bare-string whole-component form. */
inline constexpr const char *kWholeComponent = "*";

/* Recursive structural equality of two JSON trees (order-independent for
 * object keys).  Both NULL ⇒ true; type mismatch ⇒ false.  Numbers compare
 * with a small epsilon (1e-6 absolute) to absorb float print/parse round-
 * trip drift; strings strcmp; bools equal; arrays same length + elementwise
 * equal; objects same key SET + per-key json_deep_equal.  Used to decide
 * whether a serialized field key actually diverges. */
bool json_deep_equal(const JceJson *a, const JceJson *b);

/* Compute the diverging FIELD KEYS of one component between an instance and
 * its source.  Serializes both entities' components, finds the object whose
 * "type" == comp_name in each array, and returns every field key (excluding
 * "type") that is absent-on-one-side OR not json_deep_equal.  Key order is
 * the instance object's serialized order (deterministic).
 *
 * Returns the singleton { kWholeComponent } when the component object is
 * missing on either side OR does not serialize to a JSON object (e.g. the
 * unified "Light") — signalling the caller to emit the legacy whole-
 * component form.  Returns an empty vector when the component is byte-equal
 * field-for-field (no divergence). */
std::vector<std::string> compute_field_overrides(JceScene *inst_scene,
                                                 JceEntity inst_e,
                                                 JceScene *src_scene,
                                                 JceEntity src_e,
                                                 const char *comp_name);

/* Build a NEW component JSON = deep-duplicate of `src_comp` with each key in
 * `keys` replaced by a deep-duplicate of `inst_comp`'s value for that key.
 * Keys present in `keys` but ABSENT on inst_comp are removed from the result
 * (the instance "overrides" the field to not-present).  Keys NOT in `keys`
 * keep the source value — so a non-overridden field tracks the current
 * source prefab.  Caller owns the returned tree (jce_json_free).  Returns
 * NULL when src_comp/inst_comp are not both objects. */
JceJson *merge_fields(const JceJson *src_comp, const JceJson *inst_comp,
                      const std::vector<std::string> &keys);

/* SAVE path.  Writes onto `node` (an instance entity node that already
 * carries name/prefabInstance/prefabPath):
 *   - "overrides": [ <element>, ... ]
 *         each element is either a bare string "CompName" (legacy whole-
 *         component override) or an object { "component":"Name",
 *         "fields":["k1","k2"] } when only those serialized field keys
 *         diverge (computed via compute_field_overrides).
 *   - "components": [ ... ]          (ONLY the overridden components, FULL)
 * Returns true when it wrote the additive override form; false when the
 * instance has no overrides at all (caller then emits nothing new — the
 * node stays diff-equal to the legacy form, just sans components, which
 * the load path reconstructs entirely from the source).
 *
 * When `overrides` is empty this still emits an empty "overrides":[] so
 * the loader takes the NEW path (instantiate source, overlay nothing)
 * rather than the legacy full path — required so a clean instance round-
 * trips to the source definition rather than a stale snapshot. */
bool write_override_node(JceJson *node, JceScene *inst_scene,
                         JceEntity inst_e, JceScene *src_scene,
                         JceEntity src_e);

/* LOAD discriminator: does this node carry the additive "overrides"
 * array?  Absent ⇒ legacy full-snapshot node (caller keeps its old path).*/
bool node_has_overrides(const JceJson *node);

/* LOAD overlay.  The caller has already instantiated the source prefab
 * into `scene`/`e`.  For each entry in the node's "overrides" array:
 *   - LEGACY bare-string entry: overlay the WHOLE stored component object
 *     (the entry's matching "components" object) onto the entity — unchanged
 *     behaviour.
 *   - PER-FIELD entry { component, fields }: serialize the entity's CURRENT
 *     (source-instantiated) component, merge ONLY the listed field keys from
 *     the stored (instance) component onto it (merge_fields), then parse the
 *     merged single component — so fields NOT in the list track the source
 *     prefab's current values.
 * Component objects whose "type" is NOT listed in "overrides" are ignored,
 * so a hand-edited node can't smuggle extra components in.  Reuses the
 * engine's jce_scene_parse_entity_json so no per-component parser is
 * duplicated. */
void overlay_components(JceScene *scene, JceEntity e, const JceJson *node);

/* Inspector helper: for a prefab instance, return which serialized field
 * keys of `comp_name` diverge from the source — the human-readable list
 * behind the inspector's per-component override badge tooltip.  Thin
 * wrapper over compute_field_overrides (returns {} when the component is
 * not overridden or is the whole-component / non-field-diffable form). */
std::vector<std::string> overridden_field_names(JceScene *inst_scene,
                                                JceEntity inst_e,
                                                JceScene *src_scene,
                                                JceEntity src_e,
                                                const char *comp_name);

/* Build a standalone single-entity object holding ONLY the named
 * component, suitable for jce_scene_parse_entity_json overlay onto
 * another entity:  { "components": [ <comp> ] }.  Returns NULL when the
 * entity lacks the component or it does not serialize.  Caller owns the
 * returned tree (jce_json_free).  Used by Apply/Revert to copy one
 * component between scenes through the proven ser/parse round-trip. */
JceJson *make_single_component_node(JceScene *scene, JceEntity e,
                                    const char *comp_name);

} /* namespace jce_prefab_override */

#endif /* JCE_EDITOR_PREFAB_OVERRIDE_H */
