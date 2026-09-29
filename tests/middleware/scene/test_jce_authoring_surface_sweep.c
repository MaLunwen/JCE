/*
 * test_jce_authoring_surface_sweep.c — every component, added the way an SDK
 * or a script actually adds one, then saved and reloaded.
 *
 * This engine is meant to be driven from outside the editor: a user project,
 * a script binding, or a CLI that authors a scene through the public API.  All
 * three write the same thing:
 *
 *     SomeComponent c = {0};   /-* or the registry's type-erased setter *-/
 *     ...set the two fields they care about...
 *     jce_scene_set_comp(scene, e, comp_id, &c);
 *     jce_scene_serial_save(scene, &len);
 *
 * A zeroed JceMeshRenderer through exactly that path SEGFAULTED the
 * serialiser, because seven of its fields are interned strings every consumer
 * assumed were non-NULL and nothing enforced.  One component was checked by
 * hand and one defect fell out; this sweeps the whole registry so the answer
 * is about the surface rather than about the component someone thought of.
 *
 * WHAT IT ASSERTS, per component the registry can set:
 *   1. adding it from a zeroed payload does not crash;
 *   2. the scene still serialises afterwards;
 *   3. the JSON reloads into a fresh scene;
 *   4. the component is still there after the round trip -- a component that
 *      serialises to nothing is a component a user project loses on save,
 *      which is quieter than a crash and worse.
 *
 * Rule 4 has a documented exemption list, because some rows genuinely have no
 * standalone JSON form.  The list is NAMED, not a count: a silent "some
 * components do not round-trip" is exactly the shape this file exists to
 * refuse.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/os/core/jce_json.h>
#include <jce/resource/jce_scene_serial.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Does any entity in the reloaded scene carry this component? */
typedef struct { int comp_id; bool found; } SweepFind;

static void sweep_find_cb(JceScene *s, JceEntity e, void *ud)
{
    SweepFind *f = (SweepFind *)ud;
    if (!f->found && jce_scene_has_comp(s, e, f->comp_id)) f->found = true;
}

void setUp(void) {}
void tearDown(void) {}

/* Components whose registry row has a setter but no standalone round trip.
 * NAMED so a reader can check each claim; a count would hide a regression. */
static bool round_trip_exempt(const char *name)
{
    static const char *kExempt[] = {
        "Transform",     /* entity-level: written outside the components array */
        "EditorMeta",    /* name/tag/enabled live on the entity object itself  */
    };
    for (size_t i = 0; i < sizeof kExempt / sizeof kExempt[0]; ++i)
        if (strcmp(name, kExempt[i]) == 0) return true;
    return false;
}

static void test_every_component_survives_zeroed_add_and_round_trip(void)
{
    /* The registry is populated lazily by the first jce_scene_create(), so
     * asking before one exists reports zero components and sweeps nothing --
     * a sweep that passes by checking nothing is the failure mode this whole
     * file is aimed at, so it is asserted rather than assumed. */
    JceScene *warm = jce_scene_create();
    TEST_ASSERT_NOT_NULL(warm);
    const int n = jce_component_count();
    jce_scene_destroy(warm);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "the registry must have components");

    int checked = 0, no_setter = 0, exempt = 0;
    char missing[2048];
    char skipped[1024];
    missing[0] = '\0';
    skipped[0] = '\0';

    for (int cid = 0; cid < n; ++cid) {
        const char *name = jce_component_name(cid);
        if (!name || !name[0]) continue;

        JceScene *s = jce_scene_create();
        TEST_ASSERT_NOT_NULL(s);
        JceEntity e = jce_scene_create_entity(s, "sweep");

        /* A transform on every entity, as any authoring path would. */
        JceTransform t;
        memset(&t, 0, sizeof t);
        t.scale = jce_v3(1.0f, 1.0f, 1.0f);
        jce_scene_set_transform(s, e, &t);

        /* The zeroed payload, sized by the registry row itself. */
        static unsigned char zero[8192];
        memset(zero, 0, sizeof zero);
        if (!jce_scene_set_comp(s, e, cid, zero)) {
            /* NAMED, not counted.  "5 rows have no setter" is exactly the
             * kind of summary that hides the row someone needed. */
            size_t used = strlen(skipped);
            snprintf(skipped + used, sizeof skipped - used, "%s%s",
                     used ? ", " : "", name);
            no_setter++;
            jce_scene_destroy(s);
            continue;
        }
        checked++;

        char msg[192];
        snprintf(msg, sizeof msg,
                 "component '%s' (id %d): zeroed add then save", name, cid);

        size_t len = 0;
        char *json = jce_scene_serial_save(s, &len);
        TEST_ASSERT_NOT_NULL_MESSAGE(json, msg);
        TEST_ASSERT_TRUE_MESSAGE(len > 0, msg);

        JceScene *s2 = jce_scene_create();
        TEST_ASSERT_NOT_NULL(s2);
        const bool loaded = jce_scene_serial_load(s2, json, len);
        snprintf(msg, sizeof msg,
                 "component '%s' (id %d): the saved scene must reload",
                 name, cid);
        TEST_ASSERT_TRUE_MESSAGE(loaded, msg);

        if (!round_trip_exempt(name)) {
            /* Find the one entity and ask whether the component came back. */
            SweepFind find = { cid, false };
            jce_scene_each_entity(s2, sweep_find_cb, &find);
            const bool present = find.found;
            if (!present) {
                size_t used = strlen(missing);
                snprintf(missing + used, sizeof missing - used, "%s%s",
                         used ? ", " : "", name);
            }
        } else {
            exempt++;
        }

        jce_json_free_string(json);
        jce_scene_destroy(s2);
        jce_scene_destroy(s);
    }

    printf("swept %d settable component(s); %d exempt from round trip\n",
           checked, exempt);
    printf("no type-erased setter (%d): %s\n", no_setter, skipped);

    /* PINNED, not printed.  Each of these has a reason in the tree:
     *   Light        unified light row -- jce_component_registry.h says so
     *   Tag, Layer   entity-level, not components -- same header
     *   Animator     RETIRED, parse migrates onto SkeletalAnimator and it is
     *                never re-saved (jce_scene_components_json.c:2897)
     *   VfxGraph     RETIRED v0.9.9, migrates onto ParticleEmitter (:3347)
     * A row that LOSES its setter would otherwise just make the count 6 and
     * nobody would look -- which is how a generic authoring path (an SDK, a
     * script binding, a CLI) silently stops being able to add a component. */
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        "Light, Animator, VfxGraph, Tag, Layer", skipped,
        "the set of components a type-erased authoring path cannot add has "
        "changed; every name in the expected set has a documented reason, so "
        "a new one needs its own");

    char msg[2304];
    snprintf(msg, sizeof msg,
             "these components were added and then LOST by save+load, which a "
             "user project or a script would only notice after reopening: %s",
             missing);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", missing, msg);
    TEST_ASSERT_TRUE_MESSAGE(checked > 10,
        "the sweep must actually reach a meaningful number of components");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_component_survives_zeroed_add_and_round_trip);
    return UNITY_END();
}
