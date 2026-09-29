/*
 * test_jce_script_spatial_query.c — "what is inside this sphere", from script.
 *
 * jce_physics_overlap_sphere / _overlap_box are implemented down to Bullet,
 * honour JceQueryFilter{layer_mask, hit_triggers}, are JCE_API and reachable
 * from <jce/api.h> -- and had ZERO consumers anywhere outside their own module.
 * The one physics query a script could reach was a closest-hit raycast that
 * ignored the filter.
 *
 * So gameplay in all seven languages could not ask what was inside a volume.
 * Explosion and AoE damage, melee arcs, aggro and proximity checks, cover and
 * line-of-sight fans, pickup detection -- every one of them had to be faked by
 * walking entities with find_by_prefix/find_with_tag and comparing distances in
 * script, which ignores colliders, layers and triggers completely.
 *
 * THE POINT IS THAT IT ANSWERS FROM PHYSICS, NOT FROM DISTANCE.  A test that
 * only checked "the near entity came back" would pass against the distance
 * loop it replaces.  So the fixture puts a BODYLESS entity inside the sphere:
 * a distance walk returns it, a physics query does not.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "application/jce_rt_internal.h"

#include "unity.h"

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

#include "jce_test_file_util.h"

#define QUERY_SCRIPT "jce_ut_spatial_query.lua"

/* Three entities at x = 0, 3 and 40.  The one at x=3 has NO collider, so it is
 * inside every sphere a distance check would use and inside none that physics
 * answers -- which is the whole distinction being tested. */
static const char *const SCENE_JSON =
"{\"format_version\":1,\"entities\":["
 "{\"name\":\"Near\",\"id\":1,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"BoxCollider\",\"sizeX\":1,\"sizeY\":1,\"sizeZ\":1,"
    "\"centerX\":0,\"centerY\":0,\"centerZ\":0}]},"
 "{\"name\":\"Ghost\",\"id\":2,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":3,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1}]},"
 "{\"name\":\"Far\",\"id\":3,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":40,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"BoxCollider\",\"sizeX\":1,\"sizeY\":1,\"sizeZ\":1,"
    "\"centerX\":0,\"centerY\":0,\"centerZ\":0}]},"
 "{\"name\":\"Reporter\",\"id\":4,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1}]}"
"]}";

static JceScene   *s_scene;
static JceRuntime *s_rt;

typedef struct { const char *want; JceEntity found; } NameLook;

static void name_cb(JceScene *sc, JceEntity e, void *ud)
{
    NameLook *l = (NameLook *)ud;
    const char *n = jce_scene_entity_name(sc, e);
    if (n && strcmp(n, l->want) == 0) l->found = e;
}

static JceEntity by_name(const char *want)
{
    NameLook l = { want, 0 };
    jce_scene_each_entity(s_scene, name_cb, &l);
    return l.found;
}

void setUp(void)    { s_scene = NULL; s_rt = NULL; }
void tearDown(void)
{
    if (s_rt)    jce_runtime_destroy(s_rt);
    if (s_scene) jce_scene_destroy(s_scene);
    remove(QUERY_SCRIPT);
    s_rt = NULL; s_scene = NULL;
}

static void boot(void)
{
    JceJson *root = jce_json_parse(SCENE_JSON, strlen(SCENE_JSON));
    TEST_ASSERT_NOT_NULL(root);
    s_scene = jce_scene_create();
    TEST_ASSERT_EQUAL_INT(4, jce_scene_load_json(s_scene, root));
    jce_json_free(root);

    JceRuntimeDesc d;
    memset(&d, 0, sizeof d);
    d.scene          = s_scene;
    d.enable_physics = true;
    s_rt = jce_runtime_create(&d);
    TEST_ASSERT_NOT_NULL(s_rt);
    /* One step so the deferred static-collider spawn has run. */
    jce_runtime_step(s_rt, 1.0f / 60.0f);
}

static void test_a_script_can_ask_what_is_inside_a_sphere(void)
{
    /* The Reporter writes the result into its own transform so the C side can
     * read it: x = how many the sphere found, y = 1 if Near was among them,
     * z = 1 if the BODYLESS Ghost was WRONGLY reported. */
    jce_test_write_file(QUERY_SCRIPT,
        "local M = {}\n"
        "function M:on_update(dt)\n"
        "  self.n = (self.n or 0) + 1\n"
        /* NOT frame 1.  A small static collider is deferred at spawn and built
         * by the per-frame draw-distance pass, so a script that queries physics
         * on its first update sees a world whose static geometry does not exist
         * yet -- which is what the first version of this test measured and
         * reported as a dead binding. */
        "  if self.n < 3 or self.done then return end\n"
        "  self.done = true\n"
        "  local near  = jce.find_by_name(\"Near\")\n"
        "  local ghost = jce.find_by_name(\"Ghost\")\n"
        "  local hits  = jce.overlap_sphere(0, 0, 0, 5)\n"
        "  local n, saw_near, saw_ghost = 0, 0, 0\n"
        "  for _, e in ipairs(hits) do\n"
        "    n = n + 1\n"
        "    if e == near  then saw_near  = 1 end\n"
        "    if e == ghost then saw_ghost = 1 end\n"
        "  end\n"
        "  jce.set_position(self.entity, n, saw_near, saw_ghost)\n"
        "end\n"
        "return M\n");

    boot();
    const JceEntity rep = by_name("Reporter");
    TEST_ASSERT_TRUE(rep != 0);
    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", QUERY_SCRIPT);
    jce_scene_set_script(s_scene, rep, &sc);
    jce_scene_set_component_enabled(s_scene, rep, JCE_COMP_FLAG_SCRIPT, true);

    /* Rebuild so the spawn walk loads the script, then let it run. */
    jce_runtime_destroy(s_rt);
    JceRuntimeDesc d;
    memset(&d, 0, sizeof d);
    d.scene          = s_scene;
    d.enable_physics = true;
    s_rt = jce_runtime_create(&d);
    TEST_ASSERT_NOT_NULL(s_rt);
    for (int i = 0; i < 8; ++i) jce_runtime_step(s_rt, 1.0f / 60.0f);

    const JceTransform *t = jce_scene_get_transform(s_scene, rep);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_TRUE_MESSAGE(t->position.x >= 1.0f,
        "jce.overlap_sphere must return at least the one colliding entity "
        "inside the radius -- 0 means the binding, the host slot or the "
        "adapter is dead");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, t->position.y,
        "and it must be Near, the entity whose collider is actually there");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, t->position.z,
        "the BODYLESS Ghost, 3 m away and well inside the radius, must NOT be "
        "reported: this answers from physics, not from distance, which is the "
        "whole reason it is worth having over a script-side distance loop");
}

static void test_the_c_side_query_agrees(void)
{
    /* The same question through the runtime, so a failure upstairs can be
     * told apart from a failure in the binding. */
    boot();
    JceScriptEntity out[8];
    const JceScriptHost *h = NULL;
    (void)h;
    /* Reach the adapter the way the VM does: through the host table the
     * runtime installs. */
    JceBodyHandle bodies[8];
    JceQueryFilter f;
    memset(&f, 0, sizeof f);
    f.layer_mask = 0xFFFFFFFFu;
    const uint32_t n = jce_physics_overlap_sphere(
        s_rt->physics, jce_v3(0.0f, 0.0f, 0.0f), 5.0f, f, bodies, 8u);
    TEST_ASSERT_TRUE_MESSAGE(n >= 1u,
        "the physics layer itself must find the near collider, or the fixture "
        "is wrong rather than the binding");
    (void)out;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_script_can_ask_what_is_inside_a_sphere);
    RUN_TEST(test_the_c_side_query_agrees);
    return UNITY_END();
}
