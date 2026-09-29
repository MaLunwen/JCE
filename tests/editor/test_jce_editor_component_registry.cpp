#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_component_registry.h"

#include <string>

TEST_CASE("compound collider is a first-class editor component descriptor")
{
    JceScene *scene = jce_scene_create();
    REQUIRE(scene != nullptr);

    const JceEditorComponentDescriptor *desc =
        jce_editor_component_find_by_name("CompoundCollider");

    REQUIRE(desc != nullptr);
    REQUIRE(desc->comp_id != JCE_COMP_ID_INVALID);
    CHECK(jce_editor_component_find_by_id(desc->comp_id) == desc);
    CHECK(jce_editor_component_find(0) == nullptr);
    CHECK(desc->slot == 0);
    CHECK(desc->legacy_flag == 0);
    CHECK(desc->addable);
    CHECK(desc->removable);
    CHECK(desc->multi_edit_supported);
    CHECK(std::string(desc->display_name) == "Compound Collider");
    CHECK(std::string(desc->i18n_key) == "comp.compoundCollider");

    jce_scene_destroy(scene);
}

TEST_CASE("compound collider lives with the 3d collider default order")
{
    JceScene *scene = jce_scene_create();
    REQUIRE(scene != nullptr);

    const int mesh_id = jce_component_find("MeshCollider");
    const int compound_id = jce_component_find("CompoundCollider");
    const int collider2d_id = jce_component_find("Collider2D");
    REQUIRE(mesh_id != JCE_COMP_ID_INVALID);
    REQUIRE(compound_id != JCE_COMP_ID_INVALID);
    REQUIRE(collider2d_id != JCE_COMP_ID_INVALID);

    int mesh_idx = -1;
    int compound_idx = -1;
    int collider2d_idx = -1;

    int n = jce_editor_component_default_order_count();
    for (int i = 0; i < n; i++) {
        const int comp_id = jce_editor_component_default_order_comp_id(i);
        if (comp_id == mesh_id)
            mesh_idx = i;
        if (comp_id == compound_id)
            compound_idx = i;
        if (comp_id == collider2d_id)
            collider2d_idx = i;
    }

    REQUIRE(mesh_idx >= 0);
    REQUIRE(compound_idx >= 0);
    REQUIRE(collider2d_idx >= 0);
    CHECK(mesh_idx < compound_idx);
    CHECK(compound_idx < collider2d_idx);

    jce_scene_destroy(scene);
}

TEST_CASE("light group uses the engine registry identity")
{
    JceScene *scene = jce_scene_create();
    REQUIRE(scene != nullptr);

    const int light_id = jce_component_find("Light");
    REQUIRE(light_id != JCE_COMP_ID_INVALID);
    const JceEditorComponentDescriptor *desc =
        jce_editor_component_find_by_name("Light");

    REQUIRE(desc != nullptr);
    CHECK(desc->comp_id == light_id);
    CHECK(desc->slot == 0);
    CHECK(desc->legacy_flag == 0);
    CHECK(jce_editor_component_id_is_light_group(light_id));
    CHECK(!jce_editor_component_id_is_light_group(
        jce_component_find("DirectionalLight")));

    jce_scene_destroy(scene);
}

/* ------------------------------------------------------------------ *
 *  Engine <-> editor component parity.
 *
 *  The editor keeps its OWN descriptor table (display name, icon,
 *  category, ordering — metadata the engine has no business knowing) and
 *  binds each row to the engine registry BY NAME, via
 *  jce_component_find(d.engine_name).  That is the right split, but it
 *  fails silently in both directions:
 *
 *    - rename or drop an engine component and the editor row quietly
 *      resolves to JCE_COMP_ID_INVALID;
 *    - add an engine component and it simply never appears in the
 *      Inspector, with nothing to notice.
 *
 *  The cases above spot-check individual components by name, which does
 *  not catch either drift.  These two are exhaustive.
 * ------------------------------------------------------------------ */

TEST_CASE("every editor descriptor resolves to a real engine component")
{
    JceScene *scene = jce_scene_create();   /* populates the engine registry */
    REQUIRE(scene != nullptr);
    REQUIRE(jce_component_count() > 0);

    const int n = jce_editor_component_descriptor_count();
    REQUIRE(n > 0);

    std::string unresolved;
    for (int i = 0; i < n; ++i) {
        const JceEditorComponentDescriptor *d =
            jce_editor_component_descriptor_at(i);
        REQUIRE(d != nullptr);
        REQUIRE(d->engine_name != nullptr);
        if (jce_component_find(d->engine_name) == JCE_COMP_ID_INVALID) {
            unresolved += d->engine_name;
            unresolved += ' ';
        }
    }
    INFO("editor rows naming a component the engine does not register: "
         << unresolved);
    CHECK(unresolved.empty());

    jce_scene_destroy(scene);
}

TEST_CASE("every engine component has an editor descriptor")
{
    JceScene *scene = jce_scene_create();
    REQUIRE(scene != nullptr);

    /* Components deliberately absent from the addable-component table,
     * because the editor exposes them through dedicated UI instead of an
     * Inspector component row.  Each entry is a decision, not an oversight —
     * a FOURTH name showing up here means someone added an engine component
     * and forgot the editor side.
     *
     *   Tag      - Inspector header: inline text field + colour combo +
     *              existing-tag dropdown (Unity-style), not an add/remove
     *              component (jce_panel_inspector.cpp).
     *   Layer    - Inspector header: layer picker combo fed by the project's
     *              32 layer slots (jce_panel_inspector.cpp).
     *   VfxGraph - authored in its own node graph panel
     *              (jce_panel_vfx_graph.cpp), not through the Inspector.
     */
    static const char *const kIntentionallyNoDescriptor[] = {
        "Tag", "Layer", "VfxGraph",
    };

    const int n = jce_component_count();
    REQUIRE(n > 0);

    std::string missing;
    for (int id = 0; id < n; ++id) {
        const char *name = jce_component_name(id);
        if (!name || !name[0])
            continue;
        bool exempt = false;
        for (const char *e : kIntentionallyNoDescriptor) {
            if (std::string(e) == name) { exempt = true; break; }
        }
        if (exempt)
            continue;
        if (jce_editor_component_find_by_name(name) == nullptr) {
            missing += name;
            missing += ' ';
        }
    }
    INFO("engine components with no editor descriptor (they cannot be added "
         "or edited in the Inspector): " << missing);
    CHECK(missing.empty());

    /* The exemptions must stay HONEST: if one of them gains a descriptor, or
     * stops being a registered component, the list is stale and should be
     * pruned rather than silently carried forever. */
    for (const char *e : kIntentionallyNoDescriptor) {
        INFO("stale exemption: " << e);
        CHECK(jce_component_find(e) != JCE_COMP_ID_INVALID);
        CHECK(jce_editor_component_find_by_name(e) == nullptr);
    }

    jce_scene_destroy(scene);
}

/* ------------------------------------------------------------------ *
 *  Default-value hooks must actually bind.
 *
 *  jce_editor_component_defaults.cpp holds a THIRD list keyed by engine
 *  name, and jce_editor_component_set_add_default_fn() silently no-ops when
 *  the name matches no descriptor (`if (d) d->add_default = fn;`).  So a
 *  typo or a rename does not fail, does not warn, and does not crash — the
 *  component is simply added with every field zeroed.  A zeroed
 *  MeshRenderer is invisible, a zeroed Light is black, a zeroed Camera has
 *  a 0 FOV; all of which read as "the editor is broken" rather than "a
 *  string stopped matching".
 * ------------------------------------------------------------------ */

TEST_CASE("every addable component has its default-value hook bound")
{
    JceScene *scene = jce_scene_create();
    REQUIRE(scene != nullptr);
    jce_editor_component_defaults_ensure_registered();

    /* Components whose "empty" state IS the correct initial state, so they
     * need no default-init hook.  Keep this list short and justified. */
    static const char *const kNoDefaultsNeeded[] = {
        "Transform",   /* identity is set by the engine on entity create */
    };

    const int n = jce_editor_component_descriptor_count();
    REQUIRE(n > 0);

    std::string unbound;
    int checked = 0, bound = 0;
    for (int i = 0; i < n; ++i) {
        const JceEditorComponentDescriptor *d =
            jce_editor_component_descriptor_at(i);
        REQUIRE(d != nullptr);
        if (!d->addable)
            continue;
        bool exempt = false;
        for (const char *e : kNoDefaultsNeeded) {
            if (std::string(e) == d->engine_name) { exempt = true; break; }
        }
        if (exempt)
            continue;
        ++checked;
        if (d->add_default == nullptr) {
            unbound += d->engine_name;
            unbound += ' ';
        } else {
            ++bound;
        }
    }
    INFO("addable components whose add_default hook never bound (they are "
         "created with all fields zeroed): " << unbound);
    CHECK(unbound.empty());

    /* Non-vacuity: this must have actually examined a meaningful number of
       addable components with hooks installed.  If `addable` were ever
       inverted, or ensure_registered() stopped running, the loop above would
       pass by checking nothing at all. */
    INFO("addable components examined: " << checked << ", bound: " << bound);
    CHECK(checked >= 20);
    CHECK(bound >= 20);

    jce_scene_destroy(scene);
}
