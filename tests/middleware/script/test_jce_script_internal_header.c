/* test_jce_script_internal_header.c
 *
 * jce_script_internal.h is the seam the GENERATED binding TU compiles
 * against.  It is engine-private (not under engine/include/), so no ABI gate
 * watches it -- which means the only thing that notices if it stops being
 * self-contained, or if the json_null token stops being shared, is this test.
 *
 * Three contracts, all load-bearing:
 *
 *   1. The header compiles standalone from a TU that includes nothing else
 *      engine-side.  It is deliberately the FIRST include below: move it
 *      after unity.h and a missing include here would be masked.
 *
 *   2. jce.json_null is the address of jce_script_json_null_token, the one
 *      extern object the header declares.  test_jce_script_asset_json pins
 *      the other end -- that the JSON reader's null compares equal to
 *      jce.json_null from inside a script -- and neither test alone pins the
 *      address: two tokens swapped consistently pass the equality test, and
 *      a reader pointed at a private token passes the address test.
 *
 *   3. The one creator installs an 81-key `jce` table.  This assertion has
 *      been through three shapes, and the current one is the only shape left:
 *      it first asserted the generated set was REFUSED (honest while the
 *      generated installer was not linked), then that BOTH sets built the
 *      same-sized table, and now -- with the hand-written 71 and the seam
 *      that selected them deleted -- that the sole creator installs 81 keys.
 *      It is deliberately COARSE, and it is not the key list: that is
 *      test_jce_script_table_shape.c, hand-authored, read with pairs() from
 *      inside a script.  This one counts the same keys from C through a TU
 *      that includes nothing engine-side but the private header, so it still
 *      fails if the generated installer stops being called at all -- which
 *      the deleted differential harness can no longer notice.
 */

#include "jce_script_internal.h"

#include "unity.h"

#include <stdio.h>
#include <string.h>

static char g_out[256];

static void rec_log(void *user, const char *msg)
{
    (void)user;
    snprintf(g_out, sizeof g_out, "%s", msg);
}

void setUp(void) { g_out[0] = '\0'; }
void tearDown(void) {}

static void test_internal_header_is_self_contained(void)
{
    /* If this TU compiled, the header pulled in everything it needs.  Prove
       the type is complete rather than merely declared. */
    TEST_ASSERT_TRUE(sizeof(struct JceScript) > sizeof(JceScriptHost));
    TEST_ASSERT_EQUAL_INT(256, JCE_SCRIPT_MAX_COROUTINES);
}

/* The `jce` table's json_null must BE &jce_script_json_null_token, not merely
   some light userdata.  Read the table out of the VM's own lua_State, which
   this test can only reach because struct JceScript is complete here. */
static void test_json_null_token_is_one_object(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.log = rec_log;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    lua_getglobal(s->L, "jce");
    TEST_ASSERT_TRUE(lua_istable(s->L, -1));
    lua_getfield(s->L, -1, "json_null");
    TEST_ASSERT_EQUAL_INT(LUA_TLIGHTUSERDATA, lua_type(s->L, -1));
    TEST_ASSERT_EQUAL_PTR(&jce_script_json_null_token, lua_touserdata(s->L, -1));
    lua_pop(s->L, 2);

    /* And it survives the trip through Lua: a script comparing against the
       table's value sees a userdata, not nil. */
    TEST_ASSERT_NOT_EQUAL(0, jce_script_instantiate_source(s, "@token",
        "jce.log(type(jce.json_null))\n"
        "local M = {}; return M\n", 1));
    TEST_ASSERT_EQUAL_STRING("userdata", g_out);

    jce_script_destroy(s);
}

/* The generated installer runs, and it produces the whole table.  A bare
   "the table exists" assertion survives an installer that registered nothing,
   so the key COUNT is the assertion; the key LIST lives in
   test_jce_script_table_shape.c and the two are derived differently on
   purpose. */
static void test_the_one_binding_set_builds_the_whole_jce_table(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.log = rec_log;

    JceScript *g = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(g);
    lua_getglobal(g->L, "jce");
    TEST_ASSERT_TRUE(lua_istable(g->L, -1));

    int keys = 0;
    lua_pushnil(g->L);
    while (lua_next(g->L, -2)) {
        keys++;
        lua_pop(g->L, 1);
    }
    lua_pop(g->L, 1);
    jce_script_destroy(g);

    /* 110: the 101 generated bindings + 8 hand-written + json_null.  A count and
     * not a list here on purpose -- test_jce_script_table_shape.c owns the
     * list, and two copies of it is two things to update. */
    TEST_ASSERT_EQUAL_INT(110, keys);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_internal_header_is_self_contained);
    RUN_TEST(test_json_null_token_is_one_object);
    RUN_TEST(test_the_one_binding_set_builds_the_whole_jce_table);
    return UNITY_END();
}
