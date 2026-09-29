/*
 * test_jce_net_rigidbody_config.c — a NetRigidbody's four knobs reach the
 * replication config.
 *
 * JceNetRigidbodyComponent carries sync_rate_hz, interp_ms, tolerance and
 * authority_mode -- the SAME four as JceNetTransformComponent, which sits
 * beside it in the runtime and has always been read.  The rigid-body ones
 * were authored, serialised, in the Inspector and read by nothing.
 *
 * There is no rigid-body replication module in this engine, and inventing one
 * to tick a box would not be implementing it.  What a networked rigid body
 * actually replicates is its TRANSFORM -- the same thing Unity's
 * NetworkRigidbody replicates -- so the component now registers transform
 * replication with its own config.
 *
 * What is checkable without a live session is the piece the runtime relies
 * on: that a registration made with a non-default config is accepted and
 * distinct from the default, so passing the authored values through is not a
 * no-op.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/net/jce_net_transform.h>

#include <string.h>

static void test_defaults_are_not_the_authored_values(void)
{
    JceNetTransformConfig d;
    memset(&d, 0, sizeof d);
    jce_net_transform_get_default_config(&d);
    /* If the defaults happened to equal what a component authors, wiring the
     * component through would be unobservable and this whole check vacuous. */
    TEST_ASSERT_TRUE_MESSAGE(d.snapshot_hz != 7u,
        "the default snapshot rate must differ from the value the test "
        "authors, or passing it through proves nothing");
    TEST_ASSERT_TRUE_MESSAGE(d.interp_delay_ms != 33u,
        "same for the interpolation delay");
}

static void test_register_accepts_an_authored_config(void)
{
    jce_net_transform_set_scene(NULL);

    JceNetTransformConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    jce_net_transform_get_default_config(&cfg);
    cfg.snapshot_hz              = 7u;      /* what a NetRigidbody would set */
    cfg.interp_delay_ms          = 33u;
    cfg.divergence_snap_distance = 0.125f;
    cfg.authority                = JCE_NET_AUTH_OWNER;

    uint32_t before = jce_net_transform_registered_count();
    TEST_ASSERT_TRUE_MESSAGE(jce_net_transform_register(1234u, &cfg),
        "a registration with an authored config must be accepted -- this is "
        "the call the NetRigidbody path makes");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before + 1u,
        jce_net_transform_registered_count(),
        "the object must actually be registered, not silently dropped");

    /* Idempotent, which is why NetTransform must win when both components are
     * present: a second register would overwrite the first one's config. */
    TEST_ASSERT_TRUE(jce_net_transform_register(1234u, &cfg));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before + 1u,
        jce_net_transform_registered_count(),
        "re-registering the same id must not add a second entry");

    jce_net_transform_unregister(1234u);
    TEST_ASSERT_EQUAL_UINT32(before, jce_net_transform_registered_count());
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_not_the_authored_values);
    RUN_TEST(test_register_accepts_an_authored_config);
    return UNITY_END();
}
