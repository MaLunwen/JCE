/* test_jce_script_netparticle.c
 *
 * Headless mock-host coverage for the networking + particle scripting bindings
 * added to the Lua VM:
 *   jce.net_is_server()           -> bool
 *   jce.net_is_client()           -> bool
 *   jce.net_spawn(prefab, x,y,z)  -> entity
 *   jce.particle_burst(entity, count)
 *   jce.particle_set_emitting(entity, on)
 *
 * The VM (jce_script.c) is a GENERIC Lua host: every engine reach goes through
 * the JceScriptHost callback table the runtime installs.  This test installs a
 * MOCK host whose callbacks record their marshalled arguments (and return canned
 * values), runs Lua source that calls each binding, and asserts the C<->Lua
 * marshalling is exact in BOTH directions.  No runtime, no net, no scene, no
 * bgfx — purely the binding glue.
 *
 * It also pins the default-safe contract: a second VM whose new callbacks are
 * all NULL must let the SAME script run without crashing, returning false / 0 /
 * doing nothing.
 */

#include <jce/middleware/script/jce_script.h>

#include "unity.h"

#include <stdio.h>   /* snprintf */
#include <string.h>

/* ── Mock host recorder ─────────────────────────────────────────────────── */
typedef struct {
    /* net role getters (return canned values) */
    int  is_server_calls;
    int  is_client_calls;
    bool is_server_canned;
    bool is_client_canned;

    /* net_spawn */
    int             spawn_calls;
    char            spawn_prefab[256];
    float           spawn_xyz[3];
    JceScriptEntity spawn_canned;   /* what the mock spawn returns */

    /* particle_burst */
    int             burst_calls;
    JceScriptEntity burst_e;
    int             burst_count;

    /* particle_set_emitting */
    int             emit_calls;
    JceScriptEntity emit_e;
    bool            emit_on;
} Recorder;

static Recorder g_rec;

/* ── Mock callbacks ─────────────────────────────────────────────────────── */
static bool mock_net_is_server(void *user)
{
    Recorder *r = (Recorder *)user;
    r->is_server_calls++;
    return r->is_server_canned;
}

static bool mock_net_is_client(void *user)
{
    Recorder *r = (Recorder *)user;
    r->is_client_calls++;
    return r->is_client_canned;
}

static JceScriptEntity mock_net_spawn(void *user, const char *prefab_path,
                                      float x, float y, float z)
{
    Recorder *r = (Recorder *)user;
    r->spawn_calls++;
    snprintf(r->spawn_prefab, sizeof r->spawn_prefab, "%s",
             prefab_path ? prefab_path : "");
    r->spawn_xyz[0] = x; r->spawn_xyz[1] = y; r->spawn_xyz[2] = z;
    return r->spawn_canned;
}

static void mock_particle_burst(void *user, JceScriptEntity e, int count)
{
    Recorder *r = (Recorder *)user;
    r->burst_calls++;
    r->burst_e = e;
    r->burst_count = count;
}

static void mock_particle_set_emitting(void *user, JceScriptEntity e, bool on)
{
    Recorder *r = (Recorder *)user;
    r->emit_calls++;
    r->emit_e = e;
    r->emit_on = on;
}

/* Build a host that wires every new binding to the recorder. */
static JceScriptHost make_mock_host(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user                  = &g_rec;
    h.net_is_server         = mock_net_is_server;
    h.net_is_client         = mock_net_is_client;
    h.net_spawn             = mock_net_spawn;
    h.particle_burst        = mock_particle_burst;
    h.particle_set_emitting = mock_particle_set_emitting;
    return h;
}

void setUp(void)    { memset(&g_rec, 0, sizeof g_rec); }
void tearDown(void) {}

/* ── net_is_server / net_is_client: both polarities ─────────────────────── */
static void test_net_role_getters(void)
{
    /* Case 1: server true, client false. */
    JceScriptHost h = make_mock_host();
    g_rec.is_server_canned = true;
    g_rec.is_client_canned = false;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance a = jce_script_instantiate_source(s, "@role_server",
        "assert(jce.net_is_server() == true,  'server should be true')\n"
        "assert(jce.net_is_client() == false, 'client should be false')\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);   /* 0 means an assert() blew the chunk up */
    TEST_ASSERT_EQUAL_INT(1, g_rec.is_server_calls);
    TEST_ASSERT_EQUAL_INT(1, g_rec.is_client_calls);

    jce_script_destroy(s);

    /* Case 2: opposite polarity — server false, client true. */
    memset(&g_rec, 0, sizeof g_rec);
    JceScriptHost h2 = make_mock_host();
    g_rec.is_server_canned = false;
    g_rec.is_client_canned = true;

    JceScript *s2 = jce_script_create(&h2);
    TEST_ASSERT_NOT_NULL(s2);

    JceScriptInstance b = jce_script_instantiate_source(s2, "@role_client",
        "assert(jce.net_is_server() == false, 'server should be false')\n"
        "assert(jce.net_is_client() == true,  'client should be true')\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, b);
    TEST_ASSERT_EQUAL_INT(1, g_rec.is_server_calls);
    TEST_ASSERT_EQUAL_INT(1, g_rec.is_client_calls);

    jce_script_destroy(s2);
}

/* ── net_spawn: records prefab + coords, returns canned entity ──────────── */
static void test_net_spawn(void)
{
    JceScriptHost h = make_mock_host();
    g_rec.spawn_canned = 99;   /* the mock spawn returns entity 99 */

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@spawn",
        "local e = jce.net_spawn('enemy.prefab', 1, 2, 3)\n"
        "assert(e == 99, 'spawned entity must be 99')\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    /* Inputs marshalled C-side. */
    TEST_ASSERT_EQUAL_INT(1, g_rec.spawn_calls);
    TEST_ASSERT_EQUAL_STRING("enemy.prefab", g_rec.spawn_prefab);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, g_rec.spawn_xyz[0]);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, g_rec.spawn_xyz[1]);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, g_rec.spawn_xyz[2]);

    jce_script_destroy(s);
}

/* ── particle_burst / particle_set_emitting: record args ────────────────── */
static void test_particle_bindings(void)
{
    JceScriptHost h = make_mock_host();
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@particles",
        "jce.particle_burst(5, 10)\n"
        "jce.particle_set_emitting(6, false)\n"
        "jce.particle_set_emitting(7, true)\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    TEST_ASSERT_EQUAL_INT(1, g_rec.burst_calls);
    TEST_ASSERT_EQUAL_UINT64(5u, g_rec.burst_e);
    TEST_ASSERT_EQUAL_INT(10, g_rec.burst_count);

    /* set_emitting was called twice; the recorder holds the last call (7,true). */
    TEST_ASSERT_EQUAL_INT(2, g_rec.emit_calls);
    TEST_ASSERT_EQUAL_UINT64(7u, g_rec.emit_e);
    TEST_ASSERT_TRUE(g_rec.emit_on);

    jce_script_destroy(s);
}

/* ── particle_set_emitting(false) is recorded as off ────────────────────── */
static void test_particle_set_emitting_off(void)
{
    JceScriptHost h = make_mock_host();
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@emit_off",
        "jce.particle_set_emitting(6, false)\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    TEST_ASSERT_EQUAL_INT(1, g_rec.emit_calls);
    TEST_ASSERT_EQUAL_UINT64(6u, g_rec.emit_e);
    TEST_ASSERT_FALSE(g_rec.emit_on);

    jce_script_destroy(s);
}

/* ── NULL-callback safety ───────────────────────────────────────────────── */
/* A host that wires NONE of the new callbacks (all NULL) must let the same
 * script run without crashing: net_is_server/client return false, net_spawn
 * returns 0, particle ops are no-ops (default-safe contract). */
static void test_null_callbacks_are_safe(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);   /* user + every callback NULL */

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@nullsafe",
        "assert(jce.net_is_server() == false, 'net_is_server no-host -> false')\n"
        "assert(jce.net_is_client() == false, 'net_is_client no-host -> false')\n"
        "assert(jce.net_spawn('x.prefab', 1, 2, 3) == 0, 'net_spawn no-host -> 0')\n"
        /* particle ops are no-ops (must not error). */
        "jce.particle_burst(1, 5)\n"
        "jce.particle_set_emitting(1, true)\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);   /* survived: no crash, no assert tripped */

    jce_script_destroy(s);
}

/* A host that wires ONLY user (so callbacks are NULL) must equally be safe and
 * leave the recorder untouched — proving the bindings guard on the callback,
 * not just on have_host. */
static void test_partial_host_records_nothing(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user = &g_rec;           /* have_host true, but new callbacks NULL */

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@partial",
        "assert(jce.net_is_server() == false, 'partial net_is_server -> false')\n"
        "assert(jce.net_spawn('x.prefab', 0, 0, 0) == 0, 'partial net_spawn -> 0')\n"
        "jce.particle_burst(1, 5)\n"
        "jce.particle_set_emitting(1, true)\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    TEST_ASSERT_EQUAL_INT(0, g_rec.is_server_calls);
    TEST_ASSERT_EQUAL_INT(0, g_rec.spawn_calls);
    TEST_ASSERT_EQUAL_INT(0, g_rec.burst_calls);
    TEST_ASSERT_EQUAL_INT(0, g_rec.emit_calls);

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_net_role_getters);
    RUN_TEST(test_net_spawn);
    RUN_TEST(test_particle_bindings);
    RUN_TEST(test_particle_set_emitting_off);
    RUN_TEST(test_null_callbacks_are_safe);
    RUN_TEST(test_partial_host_records_nothing);
    return UNITY_END();
}
