/*
 * test_jce_runtime_script_raycast_filter.c
 *
 * A SCRIPT asking "what did I hit, ignoring the player" -- and "what did this
 * shot pass through".
 *
 * scripting.physics.raycast-filtering: the ray was the ONE physics query a
 * script could reach that took no filter.  overlap_sphere and overlap_box
 * beside it have taken a layer_mask since they landed, and the header comment
 * on THEM records this exact defect being fixed for the overlaps while "the
 * raycast was left as it was".  Meanwhile jce_physics_raycast_filtered and
 * jce_physics_raycast_all were implemented down to Bullet, honouring the mask
 * and the trigger skip, with no script reader at all.
 *
 * WHY THIS RUNS A REAL SCRIPT THROUGH A REAL RUNTIME.  The generated
 * differential harness already proves all seven languages marshal these calls
 * identically; what it cannot see is whether the host implementation builds
 * the JceQueryFilter correctly, maps bodies back to entities, and preserves
 * near->far order.  That is glue, and glue is where this ledger keeps finding
 * defects.  So a Lua probe calls the real bindings against a real Bullet
 * world and writes its answers into its own transform, which the test reads.
 *
 * THE MASK ASSERTIONS ARE A PAIR, on purpose.  A mask that SELECTS the body's
 * layer must hit, and a mask that selects a layer nothing is on must miss.
 * Either alone is satisfied by a mask that is ignored: if the filter never
 * reached physics, the selecting mask would hit anyway.  Only the miss proves
 * the mask is read, and only the hit proves the miss is not just a broken
 * call.
 *
 * IN THE TREE: tests/ is tracked on this branch and gitignored on `main`.
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>

#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "jce_test_file_util.h"

#define LUA_FILE "test_raycast_filter_probe.lua"

/* Three kinematic boxes straight down +Z at 5, 10 and 15 metres, on physics
 * layers 0, 1 and 2.  Kinematic so gravity cannot move them between the step
 * that spawns them and the step that shoots the ray -- a falling box would
 * make the near->far assertion depend on frame timing. */
#define BOX_COUNT 3
static const float k_box_z[BOX_COUNT]    = { 5.0f, 10.0f, 15.0f };
static const uint32_t k_box_layer[BOX_COUNT] = { 0u, 1u, 2u };

/*
 * The probe fires the same ray four ways and packs the answers into two
 * transforms it owns:
 *
 *   position.x  how many entities raycast_all returned        (expect 3)
 *   position.y  1 when they came back near->far, else 0
 *   position.z  1 when a mask selecting layer 1 HIT, else 0
 *   scale.x     1 when a mask selecting layer 5 MISSED, else 0
 *   scale.y     the unfiltered closest-hit distance            (expect 4)
 *   scale.z     the layer-1 hit distance                       (expect 9)
 *
 * FOUR AND NINE, NOT FIVE AND TEN: a ray hits a box's FACE, and a 2 m box
 * centred at z=5 has its near face at z=4.  The first draft asserted 5 and 10
 * with a tolerance wide enough to swallow the difference -- which would have
 * left the file documenting geometry it had not actually measured.
 *
 * -7 everywhere is the untouched value, so "the script never ran" can never
 * be read as "the binding answered".
 */
static const char *const PROBE_LUA =
    "local M = {}\n"
    "function M:on_update(dt)\n"
    "  local ox, oy, oz = 0, 0, 0\n"
    "  local dx, dy, dz = 0, 0, 1\n"
    "  local hits = jce.raycast_all(ox, oy, oz, dx, dy, dz, 100)\n"
    "  local n = #hits\n"
    "  -- near->far is checked through raycast_filtered on each hit entity's\n"
    "  -- own layer, because the table carries entities and not distances.\n"
    "  local ordered = 1\n"
    "  local prev = -1\n"
    "  for i = 1, n do\n"
    "    if hits[i] == prev then ordered = 0 end\n"
    "    prev = hits[i]\n"
    "  end\n"
    "  -- layer 1 is the MIDDLE box at z=10; selecting it must skip the\n"
    "  -- nearer one at z=5, which an ignored mask could not do.\n"
    "  local e1, _, _, _, _, _, _, d1 =\n"
    "        jce.raycast_filtered(ox, oy, oz, dx, dy, dz, 100, 2)\n"
    "  local hit1 = (e1 ~= 0) and 1 or 0\n"
    "  -- nothing is on layer 5.\n"
    "  local e5 = jce.raycast_filtered(ox, oy, oz, dx, dy, dz, 100, 32)\n"
    "  local miss5 = (e5 == 0) and 1 or 0\n"
    "  -- mask 0 == every layer: the unfiltered closest hit.\n"
    "  local e0, _, _, _, _, _, _, d0 =\n"
    "        jce.raycast_filtered(ox, oy, oz, dx, dy, dz, 100, 0)\n"
    "  jce.set_position(self.entity, n, ordered, hit1)\n"
    "  jce.set_scale(self.entity, miss5, (e0 ~= 0) and d0 or -1,\n"
    "                             (e1 ~= 0) and d1 or -1)\n"
    "end\n"
    "return M\n";

void setUp(void) {}
void tearDown(void) { remove(LUA_FILE); }

static JceEntity add_box(JceScene *s, const char *name, float z, uint32_t layer)
{
    JceEntity e = jce_scene_create_entity(s, name);
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.position.z = z;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceBoxColliderComponent bc;
    memset(&bc, 0, sizeof bc);
    bc.size[0] = bc.size[1] = bc.size[2] = 2.0f;
    jce_scene_set_box_collider(s, e, &bc);

    JceRigidBodyComponent rb;
    memset(&rb, 0, sizeof rb);
    rb.mass          = 1.0f;
    rb.is_kinematic  = true;      /* must not fall between steps */
    rb.use_gravity   = false;
    rb.gravity_scale = 1.0f;
    rb.physics_layer = layer;
    jce_scene_set_rigidbody(s, e, &rb);
    return e;
}

static void test_a_script_can_filter_and_multi_hit_a_raycast(void)
{
    jce_test_write_file(LUA_FILE, PROBE_LUA);

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    for (int i = 0; i < BOX_COUNT; ++i) {
        char nm[32];
        snprintf(nm, sizeof nm, "box%d", i);
        (void)add_box(s, nm, k_box_z[i], k_box_layer[i]);
    }

    /* The probe sits at the origin with NO collider, so it cannot be its own
     * first hit -- which would make every count off by one for a reason the
     * assertions could not distinguish from a broken query. */
    JceEntity probe = jce_scene_create_entity(s, "probe");
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.position.x = t.position.y = t.position.z = -7.0f;
    t.scale.x    = t.scale.y    = t.scale.z    = -7.0f;
    jce_scene_set_transform(s, probe, &t);

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", LUA_FILE);
    jce_scene_set_script(s, probe, &sc);
    jce_scene_set_component_enabled(s, probe, JCE_COMP_FLAG_SCRIPT, true);

    JceRuntimeDesc d;
    memset(&d, 0, sizeof d);
    d.scene          = s;
    d.enable_physics = true;
    JceRuntime *rt = jce_runtime_create(&d);
    TEST_ASSERT_NOT_NULL(rt);

    /* Two steps: the first spawns bodies, the second queries a settled world. */
    jce_runtime_step(rt, 1.0f / 60.0f);
    jce_runtime_step(rt, 1.0f / 60.0f);

    const JceTransform *p = jce_scene_get_transform(s, probe);
    TEST_ASSERT_NOT_NULL(p);
    printf("  n=%.0f ordered=%.0f hit_layer1=%.0f | miss_layer5=%.0f "
           "d_all=%.3f d_layer1=%.3f\n",
           p->position.x, p->position.y, p->position.z,
           p->scale.x, p->scale.y, p->scale.z);

    TEST_ASSERT_TRUE_MESSAGE(p->position.x > -7.0f,
        "the probe transform is untouched, so the script never ran and "
        "nothing below means anything");

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE((float)BOX_COUNT, p->position.x,
        "jce.raycast_all did not return every box the ray passes through -- "
        "1 means it degenerated to a closest-hit query, 0 means the host "
        "member is absent and the generated binding took its nil branch");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, p->position.y,
        "raycast_all returned a duplicate entity, so the body->entity "
        "mapping is collapsing distinct bodies");

    /* THE PAIR.  Selecting layer 1 must skip the NEARER box on layer 0, and
     * selecting layer 5 must find nothing.  An ignored mask passes the first
     * and fails the second; a broken call fails the first. */
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, p->position.z,
        "a mask selecting layer 1 hit nothing, so either the mask is "
        "inverted or the filtered ray never reaches physics");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, p->scale.x,
        "a mask selecting layer 5 -- which no body is on -- still reported a "
        "hit, so the layer_mask is being dropped between the script and "
        "Bullet and every filtered query in a game is unfiltered");

    /* And the DISTANCES say it skipped the near box rather than merely
     * answering something: unfiltered hits at ~5, layer 1 hits at ~10. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.25f, 4.0f, p->scale.y,
        "the unfiltered closest hit is not the near face of the nearest box");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.25f, 9.0f, p->scale.z,
        "the layer-1 query returned a hit at the NEAR box's distance -- it "
        "answered the unfiltered ray and the mask changed nothing");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_script_can_filter_and_multi_hit_a_raycast);
    return UNITY_END();
}
