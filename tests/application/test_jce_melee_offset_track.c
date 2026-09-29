/* test_jce_melee_offset_track.c
 *
 * Regression (street_demo melee "player collides with the enemy but the punch
 * raycast never hits it"): an entity whose collider has a non-zero CENTER OFFSET
 * — a capsule centered at cy=0.9 so it sits on a feet-origin humanoid — must
 * KEEP that offset when the entity is moved at runtime (Lua set_position /
 * editor gizmo).  i.e. the physics collider must stay centered on the body, not
 * collapse onto the entity origin.
 *
 * ROOT CAUSE: the rigid-body spawn bakes the collider center offset into the
 * BODY ORIGIN at create time (bd.position = tf->position + rotate(center)), but
 * rt_push_external_transforms re-teleports the body to the RAW entity Transform
 * (tc->position) on the first move, DROPPING the offset.  A chest-centered
 * capsule then drops to the feet and a chest-height melee ray flies over it.
 *
 * This drives jce_runtime_create on a one-entity scene with a KINEMATIC capsule
 * (center y=0.9), raycasts at chest height (hits at spawn), MOVES the entity via
 * its scene Transform and steps (triggering rt_push_external_transforms), then
 * raycasts at chest height toward the new position.  The body must STILL be hit
 * at chest height — the offset must survive the move.
 *
 * Ray height 1.4 cleanly separates the two cases:
 *   offset preserved -> capsule center 1.0, spans 0.1..1.9 -> ray@1.4 HITS
 *   offset dropped   -> capsule center 0.1, spans -0.8..1.0 -> ray@1.4 MISSES
 *
 * Links the full engine aggregate (jce_runtime_create stands up Bullet).
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static const float DT = 1.0f / 60.0f;

/* One-entity scene: KINEMATIC capsule with a (0,0.9,0) center offset, feet at
 * (0,0.1,z) — exactly how the street_demo enemy is authored. */
static JceScene *make_scene(JceEntity *out_e, float z)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Enemy");

    JceTransform *tf = jce_scene_get_transform(s, e);
    tf->position = jce_v3(0.0f, 0.1f, z);
    tf->rotation = jce_q_identity();
    tf->scale    = jce_v3(1.0f, 1.0f, 1.0f);

    JceCapsuleColliderComponent cap;
    memset(&cap, 0, sizeof cap);
    cap.center[0] = 0.0f; cap.center[1] = 0.9f; cap.center[2] = 0.0f;  /* OFFSET */
    cap.radius    = 0.4f;
    cap.height    = 1.8f;
    cap.axis      = 1;
    cap.is_trigger = false;
    jce_scene_set_capsule_collider(s, e, &cap);

    JceRigidBodyComponent rb;
    memset(&rb, 0, sizeof rb);
    rb.mass         = 0.0f;
    rb.friction     = 0.5f;
    rb.use_gravity  = false;
    rb.is_kinematic = true;
    jce_scene_set_rigidbody(s, e, &rb);

    *out_e = e;
    return s;
}

/* Chest-height ray from the origin toward +Z; true if it hits a body. */
static bool chest_ray_hits(JceRuntime *rt)
{
    const float origin[3] = { 0.0f, 1.4f, 0.0f };
    const float dir[3]    = { 0.0f, 0.0f, 1.0f };
    float hy = 0.0f, n[3] = { 0 };
    return jce_runtime_ground_raycast(rt, origin, dir, 12.0f, &hy, n);
}

static void test_offset_preserved_after_move(void)
{
    JceEntity e;
    JceScene *s = make_scene(&e, 3.0f);

    JceRuntimeDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.scene          = s;
    rd.enable_physics = true;

    JceRuntime *rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);
    jce_runtime_step(rt, DT);

    /* At spawn the capsule is chest-centered (y=1.0): a y=1.4 ray hits. */
    bool before = chest_ray_hits(rt);
    printf("[offset-track] before-move chest(y=1.4) hit=%d\n", (int)before);
    TEST_ASSERT_TRUE_MESSAGE(before,
        "chest ray should hit at spawn (capsule centered on the body)");

    /* MOVE the entity like a Lua set_position: feet to (0,0.1,5). */
    JceTransform *tf = jce_scene_get_transform(s, e);
    tf->position = jce_v3(0.0f, 0.1f, 5.0f);
    jce_runtime_step(rt, DT);   /* rt_push_external_transforms teleports the body */

    /* After the move the capsule must STILL be chest-centered at the new pos. */
    bool after = chest_ray_hits(rt);
    printf("[offset-track] after-move  chest(y=1.4) hit=%d\n", (int)after);
    TEST_ASSERT_TRUE_MESSAGE(after,
        "BUG: chest ray misses after move — collider center offset was dropped "
        "by rt_push_external_transforms (capsule collapsed onto the feet)");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_offset_preserved_after_move);
    return UNITY_END();
}
