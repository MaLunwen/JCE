/*
 * jce_editor_prefab_override.cpp  Prefab per-COMPONENT override core.
 *
 * Pure (no editor globals / ImGui): byte-diff an instance against an
 * in-memory source via the public component registry + scene-serial APIs.
 * See jce_editor_prefab_override.h for the design + the two guardrails
 * (backward compatibility, per-component granularity).
 */

#include "io/jce_editor_prefab_override.h"

#include <cstring>
#include <string>

extern "C" {
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
}

namespace jce_prefab_override {

namespace {

/* Entity-METADATA rows that must never be treated as overridable
 * gameplay components.  EditorMeta carries the instance's own
 * prefab_instance / prefab_path / name, which by construction differ from
 * the scratch source (instantiated as a plain entity) — so byte-diffing it
 * would ALWAYS report a spurious "EditorMeta" override and re-serialize the
 * instance's prefab link into the components array.  Tag/Layer serialize at
 * entity level, not in the components array.  These are filtered out of the
 * override computation entirely. */
bool is_metadata_row(const char *name)
{
    return name && (strcmp(name, "EditorMeta") == 0 ||
                    strcmp(name, "Tag") == 0 ||
                    strcmp(name, "Layer") == 0);
}

/* True when the instance's component `comp_id` differs (byte-wise) from
 * the source's, OR exists on exactly one of the two.  Components without a
 * direct byte accessor (unified Light, entity-level Tag/Layer rows return
 * NULL from jce_scene_get_comp) are diffed by PRESENCE only — they cannot
 * be byte-overlaid, so a presence mismatch is the only override we can
 * faithfully round-trip for them. */
bool comp_differs(JceScene *inst_scene, JceEntity inst_e,
                  JceScene *src_scene, JceEntity src_e, int comp_id)
{
    bool inst_has = jce_scene_has_comp(inst_scene, inst_e, comp_id);
    bool src_has  = jce_scene_has_comp(src_scene, src_e, comp_id);
    if (inst_has != src_has)
        return true;
    if (!inst_has)
        return false; /* absent on both */

    uint32_t isz = 0, ssz = 0;
    void *ip = jce_scene_get_comp(inst_scene, inst_e, comp_id, &isz);
    void *sp = jce_scene_get_comp(src_scene, src_e, comp_id, &ssz);

    /* No direct accessor on either side: presence already matched above,
     * so treat as not-overridden (cannot byte-compare). */
    if (!ip || !sp || isz == 0 || ssz == 0)
        return false;
    if (isz != ssz)
        return true; /* layout change — treat as override */
    return memcmp(ip, sp, isz) != 0;
}

} /* namespace */

std::vector<std::string> compute_overrides(JceScene *inst_scene,
                                           JceEntity inst_e,
                                           JceScene *src_scene,
                                           JceEntity src_e)
{
    std::vector<std::string> out;
    if (!inst_scene || !src_scene || inst_e == 0 || src_e == 0)
        return out;

    bool light_diff = false;
    const int n = jce_component_count();
    for (int id = 0; id < n; id++) {
        if (!comp_differs(inst_scene, inst_e, src_scene, src_e, id))
            continue;
        const char *name = jce_component_name(id);
        if (!name || !name[0]) continue;
        if (is_metadata_row(name)) continue;   /* never an override */

        /* The three concrete light rows (DirectionalLight/PointLight/
         * SpotLight) byte-diff faithfully, but they SERIALIZE under the
         * unified "Light" type (their own serialize callback is NULL).
         * Record the override under "Light" so write_override_node keeps
         * the matching serialized component and the loader's unified
         * parser overlays it.  The "Light" row itself has no byte
         * accessor (presence-only), so it never appears directly. */
        if (strcmp(name, "DirectionalLight") == 0 ||
            strcmp(name, "PointLight") == 0 ||
            strcmp(name, "SpotLight") == 0 ||
            strcmp(name, "Light") == 0) {
            light_diff = true;
            continue;
        }
        out.emplace_back(name);
    }
    if (light_diff)
        out.emplace_back("Light");
    return out;
}

bool is_component_overridden(JceScene *inst_scene, JceEntity inst_e,
                             JceScene *src_scene, JceEntity src_e,
                             const char *comp_name)
{
    if (!comp_name || !comp_name[0] || !inst_scene || !src_scene)
        return false;
    if (is_metadata_row(comp_name))
        return false;   /* metadata is never an override (see compute) */

    /* Light query: the unified "Light" row has no byte accessor, so check
     * the three concrete rows (any differing one means the light is
     * overridden).  Mirrors the coalescing in compute_overrides. */
    if (strcmp(comp_name, "Light") == 0) {
        static const char *kLightRows[] = {
            "DirectionalLight", "PointLight", "SpotLight"
        };
        for (const char *ln : kLightRows) {
            int lid = jce_component_find(ln);
            if (lid != JCE_COMP_ID_INVALID &&
                comp_differs(inst_scene, inst_e, src_scene, src_e, lid))
                return true;
        }
        return false;
    }

    int id = jce_component_find(comp_name);
    if (id == JCE_COMP_ID_INVALID)
        return false;
    return comp_differs(inst_scene, inst_e, src_scene, src_e, id);
}

/* ── Per-FIELD JSON diff/merge primitives ──────────────────────────────
 *
 * No struct reflection: everything works on the serialized JSON form a
 * component emits via jce_scene_serialize_entity_components.  See the
 * header for the contract on each.
 */

bool json_deep_equal(const JceJson *a, const JceJson *b)
{
    if (a == b)            return true;     /* same node, or both NULL */
    if (!a || !b)          return false;    /* exactly one NULL */

    /* Type discrimination via the facade type tests (no cJSON introspect). */
    if (jce_json_is_bool(a) || jce_json_is_bool(b)) {
        if (!(jce_json_is_bool(a) && jce_json_is_bool(b))) return false;
        /* The facade has no direct bool-NODE value accessor (only the keyed
         * jce_json_get_bool).  Print each node ("true"/"false") and compare
         * — stays within the public facade, no cJSON introspection. */
        char *pa = jce_json_print(a, false);
        char *pb = jce_json_print(b, false);
        bool eq = pa && pb && strcmp(pa, pb) == 0;
        if (pa) jce_json_free_string(pa);
        if (pb) jce_json_free_string(pb);
        return eq;
    }
    if (jce_json_is_number(a) || jce_json_is_number(b)) {
        if (!(jce_json_is_number(a) && jce_json_is_number(b))) return false;
        double da = jce_json_number_value(a, 0.0);
        double db = jce_json_number_value(b, 0.0);
        double diff = da - db;
        if (diff < 0) diff = -diff;
        /* Small absolute eps absorbs float print/parse round-trip drift. */
        return diff <= 1e-6;
    }
    if (jce_json_is_string(a) || jce_json_is_string(b)) {
        if (!(jce_json_is_string(a) && jce_json_is_string(b))) return false;
        const char *sa = jce_json_string_value(a, NULL);
        const char *sb = jce_json_string_value(b, NULL);
        if (!sa || !sb) return sa == sb;
        return strcmp(sa, sb) == 0;
    }
    if (jce_json_is_array(a) || jce_json_is_array(b)) {
        if (!(jce_json_is_array(a) && jce_json_is_array(b))) return false;
        int na = jce_json_array_size(a);
        int nb = jce_json_array_size(b);
        if (na != nb) return false;
        for (int i = 0; i < na; i++) {
            if (!json_deep_equal(jce_json_array_at(a, i),
                                 jce_json_array_at(b, i)))
                return false;
        }
        return true;
    }
    if (jce_json_is_object(a) || jce_json_is_object(b)) {
        if (!(jce_json_is_object(a) && jce_json_is_object(b))) return false;
        /* Same key SET (order-independent) + per-key equal.  Walk a's keys
         * (each must exist+match in b), then verify b has no EXTRA key. */
        int count_a = 0;
        for (JceJson *ca = jce_json_first_child(a); ca;
             ca = jce_json_next_sibling(ca)) {
            const char *key = jce_json_member_key(ca);
            if (!key) return false;   /* malformed object member */
            if (!json_deep_equal(ca, jce_json_get(b, key)))
                return false;
            count_a++;
        }
        int count_b = 0;
        for (JceJson *cb = jce_json_first_child(b); cb;
             cb = jce_json_next_sibling(cb))
            count_b++;
        return count_a == count_b;
    }

    /* Both null-typed (cJSON null) or two unknown-but-same types. */
    return true;
}

namespace {

/* Find the component object whose "type" == name inside a serialized
 * components ARRAY.  Returns NULL when absent. */
const JceJson *find_comp_by_type(const JceJson *arr, const char *name)
{
    if (!jce_json_is_array(arr) || !name) return NULL;
    for (JceJson *c = jce_json_first_child(arr); c;
         c = jce_json_next_sibling(c)) {
        const char *type = jce_json_get_string(c, "type", NULL);
        if (type && strcmp(type, name) == 0)
            return c;
    }
    return NULL;
}

/* Deep-duplicate a JSON node via print+parse (the facade has no node-dup). */
JceJson *json_dup(const JceJson *src)
{
    if (!src) return NULL;
    char *txt = jce_json_print(src, false);
    if (!txt) return NULL;
    JceJson *copy = jce_json_parse(txt, 0);
    jce_json_free_string(txt);
    return copy;
}

} /* namespace */

std::vector<std::string> compute_field_overrides(JceScene *inst_scene,
                                                 JceEntity inst_e,
                                                 JceScene *src_scene,
                                                 JceEntity src_e,
                                                 const char *comp_name)
{
    std::vector<std::string> out;
    if (!inst_scene || !src_scene || !comp_name || !comp_name[0])
        return out;

    JceJson *inst_arr = jce_scene_serialize_entity_components(inst_scene, inst_e);
    JceJson *src_arr  = jce_scene_serialize_entity_components(src_scene, src_e);

    const JceJson *inst_c = find_comp_by_type(inst_arr, comp_name);
    const JceJson *src_c  = find_comp_by_type(src_arr, comp_name);

    /* Missing on a side, or not an object (cannot key-diff): whole component.
     * The unified "Light" lands here (its concrete rows serialize NULL, so
     * "Light" never appears in the serialized array as an object). */
    if (!inst_c || !src_c ||
        !jce_json_is_object(inst_c) || !jce_json_is_object(src_c)) {
        out.emplace_back(kWholeComponent);
        jce_json_free(inst_arr);
        jce_json_free(src_arr);
        return out;
    }

    /* Walk the instance object's keys in serialized order (deterministic).
     * "type" is structural, never a divergent field.  A key diverges when it
     * is absent on the source OR not json_deep_equal. */
    for (JceJson *ic = jce_json_first_child(inst_c); ic;
         ic = jce_json_next_sibling(ic)) {
        const char *key = jce_json_member_key(ic);
        if (!key || strcmp(key, "type") == 0) continue;
        const JceJson *sc = jce_json_get(src_c, key);
        if (!json_deep_equal(ic, sc))
            out.emplace_back(key);
    }
    /* Keys present on the SOURCE but absent on the instance also diverge
     * (the instance dropped a field — overlay must restore source's view of
     * it as part of "this component's field-override set").  Append in
     * source order, skipping ones already recorded. */
    for (JceJson *sc = jce_json_first_child(src_c); sc;
         sc = jce_json_next_sibling(sc)) {
        const char *key = jce_json_member_key(sc);
        if (!key || strcmp(key, "type") == 0) continue;
        if (jce_json_has(inst_c, key)) continue;   /* handled above */
        out.emplace_back(key);
    }

    jce_json_free(inst_arr);
    jce_json_free(src_arr);
    return out;
}

JceJson *merge_fields(const JceJson *src_comp, const JceJson *inst_comp,
                      const std::vector<std::string> &keys)
{
    if (!jce_json_is_object(src_comp) || !jce_json_is_object(inst_comp))
        return NULL;

    JceJson *merged = json_dup(src_comp);
    if (!merged) return NULL;

    for (const std::string &key : keys) {
        const JceJson *iv = jce_json_get(inst_comp, key.c_str());
        /* Drop the source's key either way (idempotent — set helpers append). */
        jce_json_remove(merged, key.c_str());
        if (!iv) continue;            /* instance dropped the field */
        JceJson *dup = json_dup(iv);
        if (dup) jce_json_set_child(merged, key.c_str(), dup);
    }
    return merged;
}

std::vector<std::string> overridden_field_names(JceScene *inst_scene,
                                                JceEntity inst_e,
                                                JceScene *src_scene,
                                                JceEntity src_e,
                                                const char *comp_name)
{
    std::vector<std::string> fields =
        compute_field_overrides(inst_scene, inst_e, src_scene, src_e, comp_name);
    /* The whole-component sentinel is not a field name — strip it for the
     * human-facing list. */
    if (fields.size() == 1 && fields[0] == kWholeComponent)
        fields.clear();
    return fields;
}

bool write_override_node(JceJson *node, JceScene *inst_scene,
                         JceEntity inst_e, JceScene *src_scene,
                         JceEntity src_e)
{
    if (!node || !inst_scene || !src_scene)
        return false;

    std::vector<std::string> overrides =
        compute_overrides(inst_scene, inst_e, src_scene, src_e);

    /* Always emit an "overrides" array (possibly empty) so the LOAD path
     * takes the new instantiate-source-then-overlay branch.  Emitting
     * nothing would fall through to the legacy full-snapshot reader.
     *
     * Element form is per-field when the component field-diffs to a concrete
     * key list ({component,fields}); otherwise the legacy bare string (whole
     * component) — used for the unified "Light" and any component whose
     * serialized form is missing/non-object on either side. */
    JceJson *ov = jce_json_array();
    if (!ov) return false;
    for (const std::string &name : overrides) {
        std::vector<std::string> fields = compute_field_overrides(
            inst_scene, inst_e, src_scene, src_e, name.c_str());
        bool whole = fields.empty() ||
                     (fields.size() == 1 && fields[0] == kWholeComponent);
        if (whole) {
            /* Legacy whole-component override (also the empty-fields case:
             * compute_overrides flagged a byte difference the JSON diff
             * cannot localise — overlay the whole component to be safe). */
            jce_json_array_push_string(ov, name.c_str());
        } else {
            JceJson *entry = jce_json_object();
            JceJson *farr  = jce_json_array();
            for (const std::string &k : fields)
                jce_json_array_push_string(farr, k.c_str());
            jce_json_set_string(entry, kComponentKey, name.c_str());
            jce_json_set_child(entry, kFieldsKey, farr);
            jce_json_array_push(ov, entry);
        }
    }
    /* set_child appends, so drop any pre-existing key first (idempotent). */
    jce_json_remove(node, kOverridesKey);
    jce_json_set_child(node, kOverridesKey, ov);

    /* Serialize ONLY the overridden components.  We get the full component
     * array from the engine, then move just the overridden entries into a
     * fresh "components" array on the node.  (No json-dup in the facade, so
     * we re-print + re-parse each kept entry to detach it from the source
     * tree it was created in.) */
    JceJson *full = jce_scene_serialize_entity_components(inst_scene, inst_e);
    JceJson *kept = jce_json_array();
    if (!kept) {
        jce_json_free(full);
        return true; /* overrides array already attached */
    }

    if (jce_json_is_array(full)) {
        for (JceJson *c = jce_json_first_child(full); c;
             c = jce_json_next_sibling(c)) {
            const char *type = jce_json_get_string(c, "type", NULL);
            if (!type) continue;
            bool listed = false;
            for (const std::string &name : overrides) {
                if (name == type) { listed = true; break; }
            }
            if (!listed) continue;
            /* Detach via print + reparse (facade has no node duplicate). */
            char *txt = jce_json_print(c, false);
            if (!txt) continue;
            JceJson *copy = jce_json_parse(txt, 0);
            jce_json_free_string(txt);
            if (copy) jce_json_array_push(kept, copy);
        }
    }
    jce_json_free(full);

    jce_json_remove(node, "components");
    jce_json_set_child(node, "components", kept);
    return true;
}

bool node_has_overrides(const JceJson *node)
{
    if (!node) return false;
    const JceJson *ov = jce_json_get(node, kOverridesKey);
    return jce_json_is_array(ov);
}

void overlay_components(JceScene *scene, JceEntity e, const JceJson *node)
{
    if (!scene || e == 0 || !node) return;

    const JceJson *ov = jce_json_get(node, kOverridesKey);
    const JceJson *stored = jce_json_get(node, "components");

    /* Fast / legacy path: the overrides array carries NO per-field entry
     * (every element is a bare string).  This is every legacy file plus any
     * instance whose overrides are all whole-component — overlay the stored
     * components verbatim, byte-identical to the original behaviour. */
    bool any_field_entry = false;
    if (jce_json_is_array(ov)) {
        for (JceJson *el = jce_json_first_child(ov); el;
             el = jce_json_next_sibling(el)) {
            if (jce_json_is_object(el) && jce_json_has(el, kFieldsKey)) {
                any_field_entry = true;
                break;
            }
        }
    }
    if (!any_field_entry) {
        /* The node's "components" array already contains ONLY the overridden
         * components (write_override_node filtered them).  parse_entity_json
         * overlays each onto the existing (source-instantiated) entity. */
        jce_scene_parse_entity_json(scene, e, node);
        return;
    }

    /* Granular (per-field) path.  Serialize the entity's CURRENT components
     * (= the freshly-instantiated SOURCE) once: per-field entries merge their
     * listed keys onto these, so non-overridden fields track the source. */
    JceJson *cur_arr = jce_scene_serialize_entity_components(scene, e);

    /* Accumulate the components to parse back (whole + merged) into one
     * overlay node so the engine's per-component parsers run once. */
    JceJson *overlay = jce_json_object();
    JceJson *comps   = jce_json_array();
    if (!overlay || !comps) {
        jce_json_free(cur_arr);
        jce_json_free(overlay);
        jce_json_free(comps);
        /* Degrade to the whole-component overlay rather than losing edits. */
        jce_scene_parse_entity_json(scene, e, node);
        return;
    }

    for (JceJson *el = jce_json_first_child(ov); el;
         el = jce_json_next_sibling(el)) {
        /* Resolve the component NAME + (optional) field list for this entry. */
        const char *comp_name = NULL;
        const JceJson *fields_arr = NULL;
        if (jce_json_is_string(el)) {
            comp_name = jce_json_string_value(el, NULL);     /* legacy */
        } else if (jce_json_is_object(el)) {
            comp_name = jce_json_get_string(el, kComponentKey, NULL);
            fields_arr = jce_json_get(el, kFieldsKey);
        }
        if (!comp_name || !comp_name[0]) continue;

        const JceJson *stored_c = find_comp_by_type(stored, comp_name);
        if (!stored_c) continue;   /* malformed node: nothing to overlay */

        if (!jce_json_is_array(fields_arr)) {
            /* Whole-component entry: overlay the stored component verbatim. */
            JceJson *dup = json_dup(stored_c);
            if (dup) jce_json_array_push(comps, dup);
            continue;
        }

        /* Per-field entry: merge ONLY the listed keys from the stored
         * (instance) component onto the entity's CURRENT (source) component,
         * so every other key tracks the source prefab. */
        const JceJson *cur_c = find_comp_by_type(cur_arr, comp_name);
        if (!cur_c || !jce_json_is_object(cur_c) ||
            !jce_json_is_object(stored_c)) {
            /* Cannot field-merge — fall back to whole stored component. */
            JceJson *dup = json_dup(stored_c);
            if (dup) jce_json_array_push(comps, dup);
            continue;
        }
        std::vector<std::string> keys;
        for (JceJson *fk = jce_json_first_child(fields_arr); fk;
             fk = jce_json_next_sibling(fk)) {
            const char *k = jce_json_string_value(fk, NULL);
            if (k && k[0]) keys.emplace_back(k);
        }
        JceJson *merged = merge_fields(cur_c, stored_c, keys);
        if (merged) jce_json_array_push(comps, merged);
    }
    jce_json_free(cur_arr);

    jce_json_set_child(overlay, "components", comps);
    jce_scene_parse_entity_json(scene, e, overlay);
    jce_json_free(overlay);
}

JceJson *make_single_component_node(JceScene *scene, JceEntity e,
                                    const char *comp_name)
{
    if (!scene || e == 0 || !comp_name || !comp_name[0])
        return NULL;

    JceJson *full = jce_scene_serialize_entity_components(scene, e);
    if (!jce_json_is_array(full)) {
        jce_json_free(full);
        return NULL;
    }

    JceJson *node = jce_json_object();
    JceJson *kept = jce_json_array();
    if (!node || !kept) {
        jce_json_free(full);
        jce_json_free(node);
        jce_json_free(kept);
        return NULL;
    }

    bool found = false;
    for (JceJson *c = jce_json_first_child(full); c;
         c = jce_json_next_sibling(c)) {
        const char *type = jce_json_get_string(c, "type", NULL);
        if (!type || strcmp(type, comp_name) != 0) continue;
        char *txt = jce_json_print(c, false);
        if (!txt) continue;
        JceJson *copy = jce_json_parse(txt, 0);
        jce_json_free_string(txt);
        if (copy) { jce_json_array_push(kept, copy); found = true; }
    }
    jce_json_free(full);

    if (!found) {
        jce_json_free(node);
        jce_json_free(kept);
        return NULL;
    }
    jce_json_set_child(node, "components", kept);
    return node;
}

} /* namespace jce_prefab_override */
