/*
 * test_jce_net_rigidbody_registers.c — an authored NetRigidbody actually
 * registers transform replication.
 *
 * JceNetRigidbodyComponent carries sync_rate_hz, interp_ms, tolerance and
 * authority_mode -- the SAME four as JceNetTransformComponent, which sits
 * beside it in the runtime's spawn walk and has always been read.  The
 * rigid-body ones were authored, serialised, in the Inspector and read by
 * nothing.
 *
 * There is no rigid-body replication module in this engine, and inventing one
 * to tick a box would not be implementing it.  What a networked rigid body
 * actually replicates is its TRANSFORM, which is what Unity's
 * NetworkRigidbody replicates too, so the component registers transform
 * replication with its own config.
 *
 * This runs the REAL runtime spawn walk over a real scene file and reads
 * jce_net_transform_registered_count(), so it covers the path a unit test of
 * the net API alone cannot: scene -> component -> registration.
 *
 * It needs a SERVER session: rt_spawn_net only assigns a net object id when
 * jce_session_is_server(), so without one nothing registers -- including the
 * NetTransform path that has always worked.  The first version of this test
 * had no session and reported the feature as dead.
 *
 * Both directions are asserted -- an entity with NO net component must NOT
 * register -- so a build that registered everything fails as loudly as one
 * that registered nothing.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/application/jce_runtime.h>
#include <jce/middleware/net/jce_net_transform.h>
#include <jce/middleware/net/jce_session.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/resource/jce_scene_serial.h>

#include <stdio.h>
#include <string.h>

/* Under the build tree, not the repo root: a test that leaves files in the
 * working tree is a test that dirties `git status` for everyone after it. */
#define SCENE_PLAIN "jce_test_netrb_plain.scene.json"
#define SCENE_NETRB "jce_test_netrb_authored.scene.json"

static const char *kPlain =
"{\"contract\":\"jce.scene/1\",\"scene\":{\"version\":1,\"entities\":["
 "{\"id\":1,\"name\":\"Box\",\"parentId\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"NetworkObject\",\"owner\":0}]}"
"]}}";

/* Same entity, plus the authored NetRigidbody. */
static const char *kNetRb =
"{\"contract\":\"jce.scene/1\",\"scene\":{\"version\":1,\"entities\":["
 "{\"id\":1,\"name\":\"Box\",\"parentId\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"NetworkObject\",\"owner\":0},"
   "{\"type\":\"NetworkRigidbody\",\"syncRateHz\":7,\"interpMs\":33,"
    "\"tolerance\":0.125,\"authorityMode\":1}]}"
"]}}";

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot write the probe scene");
    fputs(text, f);
    fclose(f);
}

/* Boot the runtime on `path` and return how many objects it registered for
 * transform replication. */
static uint32_t registered_after_boot(const char *path)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE_MESSAGE(jce_scene_serial_load_file(s, path),
                             "probe scene failed to load");

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene = s;

    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);
    uint32_t n = jce_net_transform_registered_count();
    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
    remove(path);          /* leave nothing behind */
    return n;
}

static void test_plain_network_object_registers_nothing(void)
{
    write_file(SCENE_PLAIN, kPlain);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, registered_after_boot(SCENE_PLAIN),
        "a NetworkObject with neither NetTransform nor NetRigidbody must not "
        "register -- otherwise the next assertion proves nothing");
}

static void test_authored_net_rigidbody_registers(void)
{
    write_file(SCENE_NETRB, kNetRb);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, registered_after_boot(SCENE_NETRB),
        "an authored NetRigidbody must register transform replication -- if "
        "this is 0 its four knobs still reach nothing");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    JceSessionStartHostDesc host;
    memset(&host, 0, sizeof host);
    host.port        = 0;        /* ephemeral -- the header calls it test-only */
    host.max_clients = 4;
    host.server_name = "netrb-test";
    if (!jce_session_start_dedicated_server(&host)) {
        printf("could not start a server session; the registration path is "
               "gated on jce_session_is_server()\n");
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_plain_network_object_registers_nothing);
    RUN_TEST(test_authored_net_rigidbody_registers);
    int rc = UNITY_END();
    jce_session_shutdown();
    return rc;
}
