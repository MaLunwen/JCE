/*
 * test_jce_ragdoll_scene.c  Headless self-test for the RAGDOLL SCENE-PASS
 *                           WIRING (component + transient pose relay + sync).
 *
 * The ragdoll CORE is unit-tested in test_jce_ragdoll.c.  THIS test closes the
 * scene-pass last-mile: the bridge the runtime + renderer use to author, hand
 * off, and consume a ragdoll pose WITHOUT the renderer ever touching physics.
 *
 *   1. JceRagdoll COMPONENT round-trip: set/get/has/remove the presence-gated
 *      component, plus a REAL JSON serialize->deserialize round-trip
 *      (jce_scene_save_json -> jce_scene_load_json), asserting every field
 *      survives (mirrors test_jce_scene_morph_authoring).
 *   2. POSE-RELAY round-trip: publish a known mat4[] pose via
 *      jce_scene_set_ragdoll_pose, read it back byte-identical via
 *      jce_scene_get_ragdoll_pose; assert has() is true; AND assert the relay
 *      is NOT serialized (absent after a scene save/load round-trip — it is
 *      runtime-transient).
 *   3. PURE SYNC MATH: a 3-bone skeleton + real Bullet world, step with
 *      blend_weight=0 (full physics), jce_ragdoll_sync_to_pose into locals,
 *      jce_skeleton_evaluate into a palette, and assert everything is finite +
 *      the chain stayed connected.  This is exactly the math the runtime's
 *      rt_ragdoll_sync_to publishes into the relay.
 *
 * No GPU.  Deterministic.
 */

#include "unity.h"

#include "middleware/animation/jce_ragdoll.h"   /* INTERNAL src header */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_json.h>

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Entity-by-name helper (mirrors test_jce_scene_morph_authoring) ──── */

typedef struct {
    const char *want;
    JceEntity   found;
} FindCtx;

static void find_cb(JceScene *s, JceEntity e, void *ud)
{
    FindCtx *ctx = (FindCtx *)ud;
    const char *nm = jce_scene_entity_registered_name(s, e);
    if (nm && strcmp(nm, ctx->want) == 0)
        ctx->found = e;
}

static JceEntity find_by_name(JceScene *s, const char *name)
{
    FindCtx ctx = { name, JCE_ENTITY_INVALID };
    jce_scene_each_entity(s, find_cb, &ctx);
    return ctx.found;
}

/* True only if every float in the matrix array is finite. */
static bool all_finite(const jce_mat4 *m, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        const float *f = (const float *)&m[i].raw[0][0];
        for (int k = 0; k < 16; ++k)
            if (!isfinite(f[k]))
                return false;
    }
    return true;
}

/* ================================================================== */
/* Test 1: JceRagdoll component round-trip (in-memory + JSON)          */
/* ================================================================== */

static void test_ragdoll_component_round_trip(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceEntity e = jce_scene_create_entity(src, "Dummy");

    /* In-memory set/get/has. */
    JceRagdollComponent rc;
    memset(&rc, 0, sizeof rc);
    rc.enable       = true;
    rc.blend_weight = 0.5f;
    rc.radius       = 0.1f;
    rc.height_scale = 1.2f;
    jce_scene_set_ragdoll(src, e, &rc);

    TEST_ASSERT_TRUE(jce_scene_has_ragdoll(src, e));
    JceRagdollComponent *got = jce_scene_get_ragdoll(src, e);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_TRUE(got->enable);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, got->blend_weight);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.1f, got->radius);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.2f, got->height_scale);

    /* JSON save/load round-trip. */
    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Dummy");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_TRUE(jce_scene_has_ragdoll(dst, ne));
    JceRagdollComponent *out = jce_scene_get_ragdoll(dst, ne);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(out->enable);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, out->blend_weight);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.1f, out->radius);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.2f, out->height_scale);

    /* remove() clears has(). */
    jce_scene_remove_ragdoll(src, e);
    TEST_ASSERT_FALSE(jce_scene_has_ragdoll(src, e));

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* An entity with NO ragdoll component reloads with none (byte-identical). */
static void test_no_ragdoll_default(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Plain");
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(src, e, &t);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Plain");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_FALSE(jce_scene_has_ragdoll(dst, ne));

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* ================================================================== */
/* Test 2: pose-relay round-trip (transient, NOT serialized)          */
/* ================================================================== */

static void test_ragdoll_pose_relay_round_trip(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity e = jce_scene_create_entity(s, "Relay");

    enum { N = 4 };
    jce_mat4 in[N];
    for (int j = 0; j < N; ++j) {
        /* Distinct, known matrices so a byte-compare is meaningful. */
        in[j] = jce_m4_from_trs(jce_v3((float)j, (float)(j * 2), (float)(j * 3)),
                                jce_q_identity(),
                                jce_v3(1.0f, 1.0f, 1.0f));
    }

    /* No relay before the runtime publishes. */
    TEST_ASSERT_FALSE(jce_scene_has_ragdoll_pose(s, e));

    jce_scene_set_ragdoll_pose(s, e, in, (uint32_t)N);
    TEST_ASSERT_TRUE(jce_scene_has_ragdoll_pose(s, e));

    jce_mat4 out[JCE_MAX_BONES];
    uint32_t cnt = 0;
    bool ok = jce_scene_get_ragdoll_pose(s, e, out, &cnt);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)N, cnt);
    TEST_ASSERT_EQUAL_MEMORY(in, out, (size_t)N * sizeof(jce_mat4));

    /* The relay must NOT survive a scene save/load round-trip (transient). */
    JceScene *dst = jce_scene_create();
    JceJson  *root = jce_scene_save_json(s);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Relay");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_FALSE(jce_scene_has_ragdoll_pose(dst, ne));

    jce_scene_destroy(s);
    jce_scene_destroy(dst);
}

/* ================================================================== */
/* Test 3: pure sync math (3-bone chain, real Bullet)                 */
/* ================================================================== */

enum { J_ROOT = 0, J_HIP = 1, J_KNEE = 2, J_COUNT = 3 };

static JceSkeleton *make_chain_skeleton(void)
{
    JceJoint joints[J_COUNT];
    memset(joints, 0, sizeof(joints));

    jce_vec3 local_t[J_COUNT] = {
        { 0.0f,  0.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f },
    };
    int16_t parents[J_COUNT] = { -1, J_ROOT, J_HIP };
    const char *names[J_COUNT] = { "Root", "Hip", "Knee" };

    for (int i = 0; i < J_COUNT; ++i) {
        strncpy(joints[i].name, names[i], sizeof(joints[i].name) - 1);
        joints[i].parent           = parents[i];
        joints[i].rest_translation = local_t[i];
        joints[i].rest_rotation    = jce_q_identity();
        joints[i].rest_scale       = jce_v3(1.0f, 1.0f, 1.0f);
        joints[i].local_transform  = jce_m4_from_trs(local_t[i],
                                                     jce_q_identity(),
                                                     jce_v3(1.0f, 1.0f, 1.0f));
        joints[i].inverse_bind_matrix = jce_m4_identity();
    }

    return jce_skeleton_create(joints, (uint32_t)J_COUNT);
}

static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.gravity        = jce_v3(0.0f, -9.8f, 0.0f);
    wd.max_bodies     = 256;
    wd.fixed_timestep = 1.0f / 60.0f;
    wd.max_sub_steps  = 4;
    return jce_physics_create(&wd);
}

static void test_ragdoll_sync_to_pose_math(void)
{
    JceSkeleton     *skel  = make_chain_skeleton();
    TEST_ASSERT_NOT_NULL(skel);
    JcePhysicsWorld *world = make_world();
    TEST_ASSERT_NOT_NULL(world);

    JceRagdoll *rd = jce_ragdoll_create(skel, world, 0.1f, 1.0f, 1.0f);
    TEST_ASSERT_NOT_NULL(rd);

    /* Full physics collapse — exactly what blend_weight=0 (death) does. */
    jce_ragdoll_set_blend_weight(rd, 0.0f);
    for (int i = 0; i < 20; ++i)
        jce_physics_step(world, 1.0f / 60.0f);

    /* sync_to_pose -> LOCAL transforms (the exact array rt_ragdoll_sync_to
     * publishes into the scene relay). */
    jce_mat4 locals[J_COUNT];
    jce_ragdoll_sync_to_pose(rd, locals);
    TEST_ASSERT_TRUE(all_finite(locals, (uint32_t)J_COUNT));

    /* The renderer's exact step: evaluate the relay LOCAL pose into a skin
     * palette via jce_skeleton_evaluate. */
    jce_mat4 palette[J_COUNT];
    jce_skeleton_evaluate(skel, locals, palette, (uint32_t)J_COUNT);
    TEST_ASSERT_TRUE(all_finite(palette, (uint32_t)J_COUNT));

    /* Chain stayed connected: each recovered child world origin is within ~one
     * bone length of its parent (the GENERIC6DOF anchors hold it together). */
    jce_vec3 world_pos[J_COUNT];
    for (int s = 0; s < rd->body_count; ++s) {
        jce_vec3 p = jce_v3(0.0f, 0.0f, 0.0f);
        jce_quat q = jce_q_identity();
        jce_physics_body_get_transform(world, rd->bodies[s].body, &p, &q);
        world_pos[rd->bodies[s].joint_index] = p;
        TEST_ASSERT_TRUE(isfinite(p.x) && isfinite(p.y) && isfinite(p.z));
    }
    float d_root_hip = jce_v3_len(jce_v3_sub(world_pos[J_HIP], world_pos[J_ROOT]));
    float d_hip_knee = jce_v3_len(jce_v3_sub(world_pos[J_KNEE], world_pos[J_HIP]));
    TEST_ASSERT_TRUE(d_root_hip < 1.5f);
    TEST_ASSERT_TRUE(d_hip_knee < 1.5f);

    /* And feeding sync_to_pose through the scene relay preserves the pose
     * byte-for-byte (the runtime->renderer hand-off is loss-free). */
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity e = jce_scene_create_entity(scene, "Body");
    jce_scene_set_ragdoll_pose(scene, e, locals, (uint32_t)J_COUNT);

    jce_mat4 relayed[JCE_MAX_BONES];
    uint32_t cnt = 0;
    TEST_ASSERT_TRUE(jce_scene_get_ragdoll_pose(scene, e, relayed, &cnt));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)J_COUNT, cnt);
    TEST_ASSERT_EQUAL_MEMORY(locals, relayed, (size_t)J_COUNT * sizeof(jce_mat4));

    jce_scene_destroy(scene);
    jce_ragdoll_destroy(rd);
    jce_physics_destroy(world);
    jce_skeleton_destroy(skel);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ragdoll_component_round_trip);
    RUN_TEST(test_no_ragdoll_default);
    RUN_TEST(test_ragdoll_pose_relay_round_trip);
    RUN_TEST(test_ragdoll_sync_to_pose_math);
    return UNITY_END();
}
