/*
 * test_jce_editor_prefab_override.cpp
 *
 * Deterministic, headless coverage of the prefab per-COMPONENT override
 * core (editor/src/io/jce_editor_prefab_override.cpp).  No editor globals,
 * no ImGui, no file dialogs — the module is pure (explicit JceScene*).
 *
 * Cases (mirroring the blueprint test plan):
 *   1. Override round-trip: instantiate a source, override ONE component
 *      (Transform), write the override node, assert it carries
 *      "overrides":["Transform"] and re-serializes ONLY Transform (not the
 *      unchanged MeshRenderer); overlay back and assert the override
 *      survives while the non-overridden component matches the source.
 *   2. BACKWARD COMPAT (Risk-1 guard): a legacy full-snapshot node with NO
 *      "overrides" key takes the legacy path (node_has_overrides == false)
 *      and parses to the identical entity state.
 *   3. Apply propagation: override one instance's component, push it to a
 *      shared "source", and propagate to a sibling that was NOT overriding
 *      — the sibling picks up the new value while an overriding sibling
 *      keeps its own.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "io/jce_editor_prefab_override.h"

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

/* Copy every component of (src,se) onto (dst,de) via the engine
 * serialize -> parse round-trip — the same overlay path the editor uses to
 * instantiate a prefab source in-memory. */
void clone_entity_components(JceScene *dst, JceEntity de,
                             JceScene *src, JceEntity se)
{
    JceJson *comps = jce_scene_serialize_entity_components(src, se);
    JceJson *node  = jce_json_object();
    jce_json_set_child(node, "components", comps);
    jce_scene_parse_entity_json(dst, de, node);
    jce_json_free(node);
}

/* Canonicalize an entity's components to their PARSED byte form by
 * re-parsing its own serialized output onto itself.  This makes a directly
 * mutated source comparable byte-for-byte with a serialize->parse clone
 * (any field a parser defaults differently than a raw struct-set lands on
 * the same value on both sides), so an untouched clone reports NO
 * overrides regardless of per-component serialization asymmetries. */
void normalize_entity(JceScene *scene, JceEntity e)
{
    clone_entity_components(scene, e, scene, e);
}

JceTransform make_tf(float x, float y, float z)
{
    JceTransform t;
    t.position = jce_v3(x, y, z);
    t.rotation = jce_q_identity();
    t.scale    = jce_v3(1.0f, 1.0f, 1.0f);
    return t;
}

bool overrides_contains(const std::vector<std::string> &v, const char *name)
{
    for (const std::string &s : v)
        if (s == name) return true;
    return false;
}

/* Count component objects of a given "type" inside a node's "components". */
int count_comp_type(const JceJson *node, const char *type)
{
    const JceJson *arr = jce_json_get(node, "components");
    if (!jce_json_is_array(arr)) return 0;
    int n = 0;
    for (JceJson *c = jce_json_first_child(arr); c;
         c = jce_json_next_sibling(c)) {
        const char *t = jce_json_get_string(c, "type", "");
        if (t && std::strcmp(t, type) == 0) n++;
    }
    return n;
}

} /* namespace */

TEST_CASE("prefab override: one component diffs, only it round-trips")
{
    /* ── Source prefab scene ── */
    JceScene *src = jce_scene_create();
    REQUIRE(src != nullptr);
    JceEntity se = jce_scene_create_entity(src, "Source");
    REQUIRE(se != 0);

    JceTransform st = make_tf(1.0f, 2.0f, 3.0f);
    jce_scene_set_transform(src, se, &st);

    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.mesh_path = "assets/cube.glb";   /* interned on set; a literal is fine for a test */
    mr.visible = true;
    jce_scene_set_mesh_renderer(src, se, &mr);
    normalize_entity(src, se);   /* parsed-form ground truth */

    /* ── Instance = clone of source, then override ONLY the Transform ── */
    JceScene *inst = jce_scene_create();
    REQUIRE(inst != nullptr);
    JceEntity ie = jce_scene_create_entity(inst, "Instance");
    REQUIRE(ie != 0);
    clone_entity_components(inst, ie, src, se);

    /* Sanity: a fresh clone has NO overrides. */
    {
        auto ov0 = jce_prefab_override::compute_overrides(inst, ie, src, se);
        CHECK(ov0.empty());
    }

    JceTransform it = make_tf(9.0f, 2.0f, 3.0f);   /* moved on X only */
    jce_scene_set_transform(inst, ie, &it);

    /* compute_overrides must list Transform and nothing else. */
    auto ov = jce_prefab_override::compute_overrides(inst, ie, src, se);
    CHECK(ov.size() == 1);
    CHECK(overrides_contains(ov, "Transform"));
    CHECK_FALSE(overrides_contains(ov, "MeshRenderer"));

    CHECK(jce_prefab_override::is_component_overridden(
        inst, ie, src, se, "Transform"));
    CHECK_FALSE(jce_prefab_override::is_component_overridden(
        inst, ie, src, se, "MeshRenderer"));

    /* ── Write the override node (the on-disk instance form) ── */
    JceJson *node = jce_json_object();
    jce_json_set_string(node, "name", "Instance");
    jce_json_set_bool(node, "prefabInstance", true);
    jce_json_set_string(node, "prefabPath", "Source.prefab.json");
    REQUIRE(jce_prefab_override::write_override_node(node, inst, ie, src, se));

    /* The node must carry a single overrides entry naming "Transform".
     * The entry form is now the PER-FIELD object { component, fields } when
     * the component field-diffs (a Transform move localises to posX); a
     * bare string is still emitted for whole-component / non-field-diffable
     * overrides.  Accept either form so this case validates the recorded
     * override regardless of granularity. */
    CHECK(jce_prefab_override::node_has_overrides(node));
    {
        const JceJson *ovarr = jce_json_get(node, "overrides");
        REQUIRE(jce_json_is_array(ovarr));
        CHECK(jce_json_array_size(ovarr) == 1);
        const JceJson *first = jce_json_array_at(ovarr, 0);
        std::string entry_name;
        if (jce_json_is_string(first))
            entry_name = jce_json_string_value(first, "");
        else if (jce_json_is_object(first))
            entry_name = jce_json_get_string(first, "component", "");
        CHECK(entry_name == "Transform");
    }
    /* ... and re-serialize ONLY Transform — not the unchanged MeshRenderer. */
    CHECK(count_comp_type(node, "Transform") == 1);
    CHECK(count_comp_type(node, "MeshRenderer") == 0);

    /* ── Reload: instantiate source on a fresh entity, overlay overrides ── */
    JceScene *loaded = jce_scene_create();
    REQUIRE(loaded != nullptr);
    JceEntity le = jce_scene_create_entity(loaded, "Loaded");
    clone_entity_components(loaded, le, src, se);     /* = instantiate source */
    jce_prefab_override::overlay_components(loaded, le, node);

    /* Override survived: Transform == instance's moved value. */
    JceTransform *lt = jce_scene_get_transform(loaded, le);
    REQUIRE(lt != nullptr);
    CHECK(lt->position.x == doctest::Approx(9.0f));
    CHECK(lt->position.y == doctest::Approx(2.0f));
    CHECK(lt->position.z == doctest::Approx(3.0f));

    /* Non-overridden component == source (untouched by the overlay). */
    JceMeshRenderer *lmr = jce_scene_get_mesh_renderer(loaded, le);
    REQUIRE(lmr != nullptr);
    CHECK(std::string(lmr->mesh_path) == "assets/cube.glb");

    jce_json_free(node);
    jce_scene_destroy(loaded);
    jce_scene_destroy(inst);
    jce_scene_destroy(src);
}


/* ── Per-CHILD overrides ─────────────────────────────────────────────────
 *
 * WHAT WAS LOST.  The override MVP was root-level, and the serializer said so:
 * on writing an instance whose ROOT had any override it skipped the children
 * entirely, because instantiate_prefab rebuilds the subtree on load and
 * re-serializing would duplicate it.  The consequence was not a missing
 * feature, it was DATA LOSS -- move the root one metre, and every edit the
 * author had made to any CHILD of that instance vanished on save, silently,
 * with the file still loading cleanly.
 *
 * A child has no stable id, so it is addressed by INDEX PATH from the root.
 * That is only sound while the subtree still has the source's shape:
 * jce_scene_get_children returns CREATION order (measured), but destroying a
 * middle child SWAP-REMOVES -- A B C D E becomes A B E D.  Hence
 * subtree_shape_matches, and hence most of what follows is about the paths
 * being refused rather than followed.
 */
namespace {

/* Build `n` children under `parent`, named c0..c(n-1), each with a Transform
 * at a distinct position so a diff has something to find. */
std::vector<JceEntity> make_children(JceScene *s, JceEntity parent, int n)
{
    std::vector<JceEntity> out;
    for (int i = 0; i < n; ++i) {
        char nm[16];
        std::snprintf(nm, sizeof nm, "c%d", i);
        JceEntity c = jce_scene_create_entity(s, nm);
        jce_scene_set_parent(s, c, parent);
        JceTransform t = make_tf((float)i, 0.0f, 0.0f);
        jce_scene_set_transform(s, c, &t);
        normalize_entity(s, c);
        out.push_back(c);
    }
    return out;
}

} /* namespace */

TEST_CASE("child override: an untouched subtree reports nothing")
{
    /* The baseline that makes every other case meaningful.  If a pristine
     * clone reported overrides, every save would rewrite every child and the
     * "only what differs" contract would be a lie. */
    JceScene *src = jce_scene_create();
    JceEntity se = jce_scene_create_entity(src, "Root");
    normalize_entity(src, se);
    make_children(src, se, 3);

    JceScene *inst = jce_scene_create();
    JceEntity ie = jce_scene_create_entity(inst, "Root");
    clone_entity_components(inst, ie, src, se);
    make_children(inst, ie, 3);

    CHECK(jce_prefab_override::subtree_shape_matches(inst, ie, src, se));
    CHECK(jce_prefab_override::compute_child_overrides(inst, ie, src, se).empty());

    jce_scene_destroy(inst);
    jce_scene_destroy(src);
}

TEST_CASE("child override: an edited grandchild is found at its path")
{
    /* THE CASE THE DATA LOSS WAS ABOUT.  Depth 2, so a root-only walk cannot
     * reach it and an implementation that only looked one level down would
     * pass a shallower test while still discarding this edit. */
    JceScene *src = jce_scene_create();
    JceEntity se = jce_scene_create_entity(src, "Root");
    normalize_entity(src, se);
    auto s_kids  = make_children(src, se, 2);
    auto s_gkids = make_children(src, s_kids[1], 3);
    (void)s_gkids;

    JceScene *inst = jce_scene_create();
    JceEntity ie = jce_scene_create_entity(inst, "Root");
    clone_entity_components(inst, ie, src, se);
    auto i_kids  = make_children(inst, ie, 2);
    auto i_gkids = make_children(inst, i_kids[1], 3);

    /* The author drags the middle grandchild. */
    JceTransform moved = make_tf(99.0f, 5.0f, -3.0f);
    jce_scene_set_transform(inst, i_gkids[1], &moved);

    REQUIRE(jce_prefab_override::subtree_shape_matches(inst, ie, src, se));
    auto ov = jce_prefab_override::compute_child_overrides(inst, ie, src, se);
    REQUIRE(ov.size() == 1);
    CHECK(ov[0].path == std::vector<int>{1, 1});
    CHECK(ov[0].name == "c1");
    CHECK(overrides_contains(ov[0].components, "Transform"));

    /* And the path resolves back to exactly that entity. */
    CHECK(jce_prefab_override::resolve_child_path(inst, ie, ov[0].path)
          == i_gkids[1]);

    jce_scene_destroy(inst);
    jce_scene_destroy(src);
}

TEST_CASE("child override: an added child makes the shape refuse")
{
    /* An index path is meaningless once the shapes diverge.  The caller must
     * fall back to the full snapshot -- which preserves everything -- rather
     * than write paths that point at the wrong children. */
    JceScene *src = jce_scene_create();
    JceEntity se = jce_scene_create_entity(src, "Root");
    normalize_entity(src, se);
    make_children(src, se, 2);

    JceScene *inst = jce_scene_create();
    JceEntity ie = jce_scene_create_entity(inst, "Root");
    clone_entity_components(inst, ie, src, se);
    make_children(inst, ie, 3);          /* the author added one */

    CHECK_FALSE(jce_prefab_override::subtree_shape_matches(inst, ie, src, se));

    jce_scene_destroy(inst);
    jce_scene_destroy(src);
}

TEST_CASE("child override: a divergence DEEP in the tree still refuses")
{
    /* The shallow check would pass here -- the root has the same child count
     * on both sides -- and an override written under it would address a
     * subtree that no longer matches. */
    JceScene *src = jce_scene_create();
    JceEntity se = jce_scene_create_entity(src, "Root");
    normalize_entity(src, se);
    auto s_kids = make_children(src, se, 2);
    make_children(src, s_kids[0], 2);

    JceScene *inst = jce_scene_create();
    JceEntity ie = jce_scene_create_entity(inst, "Root");
    clone_entity_components(inst, ie, src, se);
    auto i_kids = make_children(inst, ie, 2);
    make_children(inst, i_kids[0], 3);   /* deeper, and only there */

    CHECK_FALSE(jce_prefab_override::subtree_shape_matches(inst, ie, src, se));

    jce_scene_destroy(inst);
    jce_scene_destroy(src);
}

TEST_CASE("child override: an out-of-range path resolves to nothing")
{
    /* FAIL CLOSED.  A path from a file written against an older prefab must
     * resolve to 0, not to whatever entity happens to sit at that index --
     * overlaying one child's overrides onto its sibling is silent corruption
     * and the scene would still load. */
    JceScene *s = jce_scene_create();
    JceEntity root = jce_scene_create_entity(s, "Root");
    auto kids = make_children(s, root, 2);

    CHECK(jce_prefab_override::resolve_child_path(s, root, {0}) == kids[0]);
    CHECK(jce_prefab_override::resolve_child_path(s, root, {}) == root);
    CHECK(jce_prefab_override::resolve_child_path(s, root, {5}) == 0);
    CHECK(jce_prefab_override::resolve_child_path(s, root, {-1}) == 0);
    CHECK(jce_prefab_override::resolve_child_path(s, root, {0, 0}) == 0);

    jce_scene_destroy(s);
}

TEST_CASE("child override: several edited children come back in tree order")
{
    /* Deterministic order matters for the file: a set that reordered between
     * saves would churn the diff of every scene containing a prefab. */
    JceScene *src = jce_scene_create();
    JceEntity se = jce_scene_create_entity(src, "Root");
    normalize_entity(src, se);
    make_children(src, se, 4);

    JceScene *inst = jce_scene_create();
    JceEntity ie = jce_scene_create_entity(inst, "Root");
    clone_entity_components(inst, ie, src, se);
    auto kids = make_children(inst, ie, 4);

    JceTransform a = make_tf(7.0f, 0.0f, 0.0f);
    JceTransform b = make_tf(8.0f, 0.0f, 0.0f);
    jce_scene_set_transform(inst, kids[2], &a);
    jce_scene_set_transform(inst, kids[0], &b);

    auto ov = jce_prefab_override::compute_child_overrides(inst, ie, src, se);
    REQUIRE(ov.size() == 2);
    CHECK(ov[0].path == std::vector<int>{0});
    CHECK(ov[1].path == std::vector<int>{2});

    jce_scene_destroy(inst);
    jce_scene_destroy(src);
}

TEST_CASE("prefab override: legacy full-snapshot node has no overrides key")
{
    /* A legacy instance node = full component snapshot, NO "overrides".
     * node_has_overrides() is the SOLE discriminator the editor loader
     * uses to keep the unchanged legacy path — so it MUST report false,
     * and parsing the full node must reproduce the entity verbatim. */
    JceScene *src = jce_scene_create();
    REQUIRE(src != nullptr);
    JceEntity se = jce_scene_create_entity(src, "Legacy");
    JceTransform st = make_tf(4.0f, 5.0f, 6.0f);
    jce_scene_set_transform(src, se, &st);
    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.mesh_path = "assets/legacy.glb";   /* interned on set; a literal is fine for a test */
    mr.visible = true;
    jce_scene_set_mesh_renderer(src, se, &mr);
    normalize_entity(src, se);

    /* Build a legacy node: full components, prefabInstance set, NO overrides. */
    JceJson *legacy = jce_json_object();
    jce_json_set_string(legacy, "name", "Legacy");
    jce_json_set_bool(legacy, "prefabInstance", true);
    jce_json_set_string(legacy, "prefabPath", "Legacy.prefab.json");
    JceJson *comps = jce_scene_serialize_entity_components(src, se);
    jce_json_set_child(legacy, "components", comps);

    /* Discriminator: legacy node takes the legacy path. */
    CHECK_FALSE(jce_prefab_override::node_has_overrides(legacy));
    /* Both real components are present (full snapshot). */
    CHECK(count_comp_type(legacy, "Transform") == 1);
    CHECK(count_comp_type(legacy, "MeshRenderer") == 1);

    /* Parsing the full node reproduces the identical entity state. */
    JceScene *dst = jce_scene_create();
    JceEntity de = jce_scene_create_entity(dst, "LegacyLoaded");
    jce_scene_parse_entity_json(dst, de, legacy);

    JceTransform *dt = jce_scene_get_transform(dst, de);
    REQUIRE(dt != nullptr);
    CHECK(dt->position.x == doctest::Approx(4.0f));
    CHECK(dt->position.y == doctest::Approx(5.0f));
    CHECK(dt->position.z == doctest::Approx(6.0f));
    JceMeshRenderer *dmr = jce_scene_get_mesh_renderer(dst, de);
    REQUIRE(dmr != nullptr);
    CHECK(std::string(dmr->mesh_path) == "assets/legacy.glb");

    jce_json_free(legacy);
    jce_scene_destroy(dst);
    jce_scene_destroy(src);
}

TEST_CASE("prefab override: apply pushes to source + propagates to siblings")
{
    /* Source + two instances, all initially identical. */
    JceScene *src = jce_scene_create();
    JceEntity se = jce_scene_create_entity(src, "Src");
    JceTransform st = make_tf(0.0f, 0.0f, 0.0f);
    jce_scene_set_transform(src, se, &st);
    normalize_entity(src, se);

    JceScene *world = jce_scene_create();
    JceEntity a = jce_scene_create_entity(world, "A");
    JceEntity b = jce_scene_create_entity(world, "B");
    clone_entity_components(world, a, src, se);
    clone_entity_components(world, b, src, se);

    /* Override A's transform; B stays inheriting. */
    JceTransform at = make_tf(7.0f, 0.0f, 0.0f);
    jce_scene_set_transform(world, a, &at);

    CHECK(jce_prefab_override::is_component_overridden(
        world, a, src, se, "Transform"));
    CHECK_FALSE(jce_prefab_override::is_component_overridden(
        world, b, src, se, "Transform"));

    /* ── Simulate Apply: B is NOT overriding (== old source) so it should
     * receive A's value; the source receives A's value too. ── */
    bool b_inherits = !jce_prefab_override::is_component_overridden(
        world, b, src, se, "Transform");
    REQUIRE(b_inherits);

    /* Copy A.Transform -> source via the single-component node helper. */
    JceJson *one = jce_prefab_override::make_single_component_node(
        world, a, "Transform");
    REQUIRE(one != nullptr);
    jce_scene_parse_entity_json(src, se, one);
    /* Propagate to inheriting sibling B. */
    jce_scene_parse_entity_json(world, b, one);
    jce_json_free(one);

    /* Source now holds A's value. */
    JceTransform *stp = jce_scene_get_transform(src, se);
    REQUIRE(stp != nullptr);
    CHECK(stp->position.x == doctest::Approx(7.0f));
    /* Sibling B picked it up. */
    JceTransform *btp = jce_scene_get_transform(world, b);
    REQUIRE(btp != nullptr);
    CHECK(btp->position.x == doctest::Approx(7.0f));

    /* After apply, A no longer differs from the (now updated) source. */
    CHECK_FALSE(jce_prefab_override::is_component_overridden(
        world, a, src, se, "Transform"));
    CHECK_FALSE(jce_prefab_override::is_component_overridden(
        world, b, src, se, "Transform"));

    jce_scene_destroy(world);
    jce_scene_destroy(src);
}

/* ───────────────────────── PER-FIELD overrides ─────────────────────── */

TEST_CASE("prefab field override: one field diffs -> exactly that key")
{
    JceScene *src = jce_scene_create();
    JceEntity se = jce_scene_create_entity(src, "Src");
    JceTransform st = make_tf(1.0f, 2.0f, 3.0f);
    jce_scene_set_transform(src, se, &st);
    normalize_entity(src, se);

    JceScene *inst = jce_scene_create();
    JceEntity ie = jce_scene_create_entity(inst, "Inst");
    clone_entity_components(inst, ie, src, se);

    /* Identical clone: no diverging fields. */
    {
        auto f0 = jce_prefab_override::compute_field_overrides(
            inst, ie, src, se, "Transform");
        CHECK(f0.empty());
    }

    /* Move X only -> Transform serializes posX (see ser_transform). */
    JceTransform it = make_tf(9.0f, 2.0f, 3.0f);
    jce_scene_set_transform(inst, ie, &it);

    auto fields = jce_prefab_override::compute_field_overrides(
        inst, ie, src, se, "Transform");
    CHECK(fields.size() == 1);
    CHECK(overrides_contains(fields, "posX"));
    CHECK_FALSE(overrides_contains(fields, "posY"));
    CHECK_FALSE(overrides_contains(fields, "posZ"));
    CHECK_FALSE(overrides_contains(fields, "type"));   /* never a field */

    jce_scene_destroy(inst);
    jce_scene_destroy(src);
}

TEST_CASE("prefab field override: json_deep_equal scalar/object/array")
{
    /* Objects with the SAME key set but different insertion order = equal. */
    JceJson *oa = jce_json_object();
    jce_json_set_number(oa, "a", 1.0);
    jce_json_set_number(oa, "b", 2.0);
    JceJson *ob = jce_json_object();
    jce_json_set_number(ob, "b", 2.0);   /* reversed order */
    jce_json_set_number(ob, "a", 1.0);
    CHECK(jce_prefab_override::json_deep_equal(oa, ob));

    /* Differing scalar -> false. */
    JceJson *oc = jce_json_object();
    jce_json_set_number(oc, "a", 1.0);
    jce_json_set_number(oc, "b", 99.0);
    CHECK_FALSE(jce_prefab_override::json_deep_equal(oa, oc));

    /* Extra key on one side -> false (key SET differs). */
    JceJson *od = jce_json_object();
    jce_json_set_number(od, "a", 1.0);
    jce_json_set_number(od, "b", 2.0);
    jce_json_set_number(od, "c", 3.0);
    CHECK_FALSE(jce_prefab_override::json_deep_equal(oa, od));

    /* Nested array diff -> false; identical nested array -> true. */
    JceJson *na = jce_json_object();
    JceJson *aa = jce_json_array();
    jce_json_array_push_number(aa, 1.0);
    jce_json_array_push_number(aa, 2.0);
    jce_json_set_child(na, "arr", aa);
    JceJson *nb = jce_json_object();
    JceJson *ab = jce_json_array();
    jce_json_array_push_number(ab, 1.0);
    jce_json_array_push_number(ab, 2.0);
    jce_json_set_child(nb, "arr", ab);
    CHECK(jce_prefab_override::json_deep_equal(na, nb));
    JceJson *nc = jce_json_object();
    JceJson *ac = jce_json_array();
    jce_json_array_push_number(ac, 1.0);
    jce_json_array_push_number(ac, 7.0);   /* element differs */
    jce_json_set_child(nc, "arr", ac);
    CHECK_FALSE(jce_prefab_override::json_deep_equal(na, nc));

    /* Type mismatch (number vs string) -> false. */
    JceJson *num = jce_json_number(1.0);
    JceJson *strv = jce_json_string("1");
    CHECK_FALSE(jce_prefab_override::json_deep_equal(num, strv));

    /* Both NULL -> true. */
    CHECK(jce_prefab_override::json_deep_equal(nullptr, nullptr));

    jce_json_free(oa); jce_json_free(ob); jce_json_free(oc); jce_json_free(od);
    jce_json_free(na); jce_json_free(nb); jce_json_free(nc);
    jce_json_free(num); jce_json_free(strv);
}

TEST_CASE("prefab field override: merge_fields overlays only listed keys")
{
    /* source {a:1,b:2} + instance {a:9,b:8} overriding only [a] -> {a:9,b:2}. */
    JceJson *srcc = jce_json_object();
    jce_json_set_number(srcc, "a", 1.0);
    jce_json_set_number(srcc, "b", 2.0);
    JceJson *instc = jce_json_object();
    jce_json_set_number(instc, "a", 9.0);
    jce_json_set_number(instc, "b", 8.0);

    std::vector<std::string> keys = { "a" };
    JceJson *merged = jce_prefab_override::merge_fields(srcc, instc, keys);
    REQUIRE(merged != nullptr);
    CHECK(jce_json_get_number(merged, "a", -1.0) == doctest::Approx(9.0));
    CHECK(jce_json_get_number(merged, "b", -1.0) == doctest::Approx(2.0));

    jce_json_free(merged);
    jce_json_free(srcc);
    jce_json_free(instc);
}

TEST_CASE("prefab field override: non-overridden field tracks NEW source value")
{
    /* The per-field payoff: instance overrides one field; the SOURCE later
     * changes a DIFFERENT field; on load the instance shows the source's NEW
     * value for the non-overridden field AND keeps its own overridden one. */
    JceScene *src = jce_scene_create();
    JceEntity se = jce_scene_create_entity(src, "Src");
    JceTransform st = make_tf(1.0f, 2.0f, 3.0f);
    jce_scene_set_transform(src, se, &st);
    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.mesh_path = "assets/v1.glb";   /* interned on set; a literal is fine for a test */
    mr.metallic = 0.10f;
    mr.visible  = true;
    jce_scene_set_mesh_renderer(src, se, &mr);
    normalize_entity(src, se);

    /* Instance clones the source, then overrides ONLY metallic. */
    JceScene *inst = jce_scene_create();
    JceEntity ie = jce_scene_create_entity(inst, "Inst");
    clone_entity_components(inst, ie, src, se);
    JceMeshRenderer *imr = jce_scene_get_mesh_renderer(inst, ie);
    REQUIRE(imr != nullptr);
    imr->metallic = 0.90f;   /* the override */

    auto fields = jce_prefab_override::compute_field_overrides(
        inst, ie, src, se, "MeshRenderer");
    CHECK(overrides_contains(fields, "metallic"));
    CHECK_FALSE(overrides_contains(fields, "meshPath"));

    /* Persist the override node. */
    JceJson *node = jce_json_object();
    jce_json_set_string(node, "name", "Inst");
    jce_json_set_bool(node, "prefabInstance", true);
    jce_json_set_string(node, "prefabPath", "Src.prefab.json");
    REQUIRE(jce_prefab_override::write_override_node(node, inst, ie, src, se));

    /* The override entry for MeshRenderer must be the PER-FIELD object form
     * carrying "fields":["metallic"]. */
    {
        const JceJson *ov = jce_json_get(node, "overrides");
        REQUIRE(jce_json_is_array(ov));
        bool saw_field_entry = false;
        for (JceJson *el = jce_json_first_child(ov); el;
             el = jce_json_next_sibling(el)) {
            if (!jce_json_is_object(el)) continue;
            const char *cn = jce_json_get_string(el, "component", "");
            if (cn && std::strcmp(cn, "MeshRenderer") == 0) {
                const JceJson *farr = jce_json_get(el, "fields");
                REQUIRE(jce_json_is_array(farr));
                bool has_metallic = false;
                for (JceJson *fk = jce_json_first_child(farr); fk;
                     fk = jce_json_next_sibling(fk)) {
                    const char *k = jce_json_string_value(fk, "");
                    if (k && std::strcmp(k, "metallic") == 0) has_metallic = true;
                    CHECK(std::string(k) != "meshPath");   /* not overridden */
                }
                CHECK(has_metallic);
                saw_field_entry = true;
            }
        }
        CHECK(saw_field_entry);
    }

    /* ── SOURCE evolves: change the NON-overridden meshPath. ── */
    JceMeshRenderer mr2;
    jce_mesh_renderer_init(&mr2);
    mr2.mesh_path = "assets/v2.glb";   /* interned on set; a literal is fine for a test */
    mr2.metallic = 0.10f;     /* unchanged on source */
    mr2.visible  = true;
    jce_scene_set_mesh_renderer(src, se, &mr2);
    normalize_entity(src, se);

    /* ── Load: instantiate the (updated) source, overlay per-field. ── */
    JceScene *loaded = jce_scene_create();
    JceEntity le = jce_scene_create_entity(loaded, "Loaded");
    clone_entity_components(loaded, le, src, se);   /* = instantiate NEW source */
    jce_prefab_override::overlay_components(loaded, le, node);

    JceMeshRenderer *lmr = jce_scene_get_mesh_renderer(loaded, le);
    REQUIRE(lmr != nullptr);
    /* Non-overridden field tracked the source's NEW value. */
    CHECK(std::string(lmr->mesh_path) == "assets/v2.glb");
    /* Overridden field kept the instance's own value. */
    CHECK(lmr->metallic == doctest::Approx(0.90f));

    jce_json_free(node);
    jce_scene_destroy(loaded);
    jce_scene_destroy(inst);
    jce_scene_destroy(src);
}

TEST_CASE("prefab field override: legacy bare-string entry overlays whole comp")
{
    /* Backward compat: an "overrides" array of BARE STRINGS (no "fields")
     * must overlay the whole stored component, unchanged from the legacy
     * per-component path. */
    JceScene *src = jce_scene_create();
    JceEntity se = jce_scene_create_entity(src, "Src");
    JceTransform st = make_tf(0.0f, 0.0f, 0.0f);
    jce_scene_set_transform(src, se, &st);
    normalize_entity(src, se);

    /* Build a node by hand: overrides:["Transform"] + a full Transform with
     * a moved value, mimicking an old on-disk instance. */
    JceScene *inst = jce_scene_create();
    JceEntity ie = jce_scene_create_entity(inst, "Inst");
    clone_entity_components(inst, ie, src, se);
    JceTransform it = make_tf(5.0f, 6.0f, 7.0f);
    jce_scene_set_transform(inst, ie, &it);

    JceJson *node = jce_json_object();
    JceJson *ov = jce_json_array();
    jce_json_array_push_string(ov, "Transform");   /* legacy BARE string */
    jce_json_set_child(node, "overrides", ov);
    /* Stored components = the full (moved) Transform from the instance. */
    JceJson *comps = jce_scene_serialize_entity_components(inst, ie);
    jce_json_set_child(node, "components", comps);

    /* Load: instantiate source, overlay via the legacy bare-string entry. */
    JceScene *loaded = jce_scene_create();
    JceEntity le = jce_scene_create_entity(loaded, "Loaded");
    clone_entity_components(loaded, le, src, se);
    jce_prefab_override::overlay_components(loaded, le, node);

    JceTransform *lt = jce_scene_get_transform(loaded, le);
    REQUIRE(lt != nullptr);
    CHECK(lt->position.x == doctest::Approx(5.0f));
    CHECK(lt->position.y == doctest::Approx(6.0f));
    CHECK(lt->position.z == doctest::Approx(7.0f));

    jce_json_free(node);
    jce_scene_destroy(loaded);
    jce_scene_destroy(inst);
    jce_scene_destroy(src);
}
