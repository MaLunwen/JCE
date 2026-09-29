/*
 * test_jce_scene_anim_authoring.c
 *
 * Editor authoring last-mile for the Animation panel (FEATURE 3.x):
 *   - SkeletalAnimator 2D blend authoring (blend_mode + blend_param_y +
 *     per-sample (x,y) positions: blend_thresholds[] = X, blend_pos_y[] = Y),
 *   - Avatar additive/override layer stack (clip + mask_path + weight + mode).
 *
 * Exercises the REAL engine component JSON serializer (jce_scene_save_json ->
 * jce_scene_load_json) so the inspector-authored fields round-trip to disk and
 * back, byte-for-byte in value.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

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

/* ── SkeletalAnimator 2D blend round-trip ───────────────────────────── */

static void test_skeletal_2d_blend_round_trip(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceEntity e = jce_scene_create_entity(src, "Mover");

    JceSkeletalAnimatorComponent sk;
    memset(&sk, 0, sizeof sk);
    strcpy(sk.skeleton_path, "models/mannequin.glb");
    sk.clip_count = 3;
    strcpy(sk.clip_names[0], "idle");
    strcpy(sk.clip_names[1], "walk");
    strcpy(sk.clip_names[2], "strafe");
    sk.speed          = 1.0f;
    sk.loop           = true;
    sk.use_blend_tree = true;
    sk.blend_mode     = 2;             /* 2D directional */
    sk.blend_param    = 0.3f;
    sk.blend_param_y  = -0.7f;
    /* X positions (blend_thresholds) + Y positions (blend_pos_y). */
    sk.blend_thresholds[0] = 0.0f;  sk.blend_pos_y[0] = 0.0f;
    sk.blend_thresholds[1] = 1.0f;  sk.blend_pos_y[1] = 0.0f;
    sk.blend_thresholds[2] = 0.0f;  sk.blend_pos_y[2] = 1.0f;
    jce_scene_set_skeletal_animator(src, e, &sk);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Mover");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_TRUE(jce_scene_has_skeletal_animator(dst, ne));
    JceSkeletalAnimatorComponent *out = jce_scene_get_skeletal_animator(dst, ne);
    TEST_ASSERT_NOT_NULL(out);

    TEST_ASSERT_TRUE(out->use_blend_tree);
    TEST_ASSERT_EQUAL_INT(2, out->blend_mode);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f,  0.3f, out->blend_param);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.7f, out->blend_param_y);
    TEST_ASSERT_EQUAL_INT(3, out->clip_count);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out->blend_thresholds[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, out->blend_thresholds[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out->blend_thresholds[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out->blend_pos_y[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out->blend_pos_y[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, out->blend_pos_y[2]);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* A 1D blend tree (blend_mode 0) must NOT emit/parse Y data and must default
 * blend_mode back to 0 / blend_param_y to 0 on reload. */
static void test_skeletal_1d_blend_omits_2d_fields(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Mover1D");
    JceSkeletalAnimatorComponent sk;
    memset(&sk, 0, sizeof sk);
    strcpy(sk.skeleton_path, "models/mannequin.glb");
    sk.clip_count = 2;
    strcpy(sk.clip_names[0], "idle");
    strcpy(sk.clip_names[1], "run");
    sk.speed          = 1.0f;
    sk.use_blend_tree = true;
    sk.blend_mode     = 0;              /* classic 1D */
    sk.blend_param    = 4.0f;
    sk.blend_thresholds[1] = 6.0f;
    jce_scene_set_skeletal_animator(src, e, &sk);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Mover1D");
    JceSkeletalAnimatorComponent *out = jce_scene_get_skeletal_animator(dst, ne);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(out->use_blend_tree);
    TEST_ASSERT_EQUAL_INT(0, out->blend_mode);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out->blend_param_y);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.0f, out->blend_thresholds[1]);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* ── SkeletalAnimator retarget-source round-trip (F12 wiring) ─────────
 *
 * The retargeted POSE itself is an F12 (renderer-only) behaviour: it needs a
 * GPU/skeleton-asset to exercise and cannot run in this headless authoring
 * harness.  What we CAN assert here is the authoring last-mile: the new optional
 * retarget_source_skeleton field round-trips through the REAL component JSON
 * serializer, and an animator that leaves it empty reloads empty (legacy byte-
 * identical — the field is absent from the emitted JSON). */

static void test_skeletal_retarget_source_round_trip(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceEntity e = jce_scene_create_entity(src, "Retargeted");

    JceSkeletalAnimatorComponent sk;
    memset(&sk, 0, sizeof sk);
    strcpy(sk.skeleton_path, "models/orc.glb");                /* dst rig    */
    strcpy(sk.retarget_source_skeleton, "models/mannequin.glb"); /* src rig  */
    sk.clip_count = 1;
    strcpy(sk.clip_names[0], "run");
    sk.active_clip = 0;
    sk.speed   = 1.0f;
    sk.loop    = true;
    sk.playing = true;
    jce_scene_set_skeletal_animator(src, e, &sk);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Retargeted");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_TRUE(jce_scene_has_skeletal_animator(dst, ne));
    JceSkeletalAnimatorComponent *out = jce_scene_get_skeletal_animator(dst, ne);
    TEST_ASSERT_NOT_NULL(out);

    TEST_ASSERT_EQUAL_STRING("models/orc.glb", out->skeleton_path);
    TEST_ASSERT_EQUAL_STRING("models/mannequin.glb", out->retarget_source_skeleton);
    TEST_ASSERT_EQUAL_INT(0, out->active_clip);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* An animator that never sets a retarget source must reload with the field
   empty (the serializer omits it entirely → legacy byte-identical). */
static void test_skeletal_no_retarget_source_default(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Plain");
    JceSkeletalAnimatorComponent sk;
    memset(&sk, 0, sizeof sk);
    strcpy(sk.skeleton_path, "models/orc.glb");
    sk.clip_count = 1;
    strcpy(sk.clip_names[0], "idle");
    sk.speed = 1.0f;
    jce_scene_set_skeletal_animator(src, e, &sk);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Plain");
    JceSkeletalAnimatorComponent *out = jce_scene_get_skeletal_animator(dst, ne);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("", out->retarget_source_skeleton);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* ── Avatar additive/override layer stack round-trip ────────────────── */

static void test_avatar_layers_round_trip(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Hero");

    JceAvatarComponent av;
    memset(&av, 0, sizeof av);
    strcpy(av.avatar_path, "rigs/hero.avatar");
    av.human_rig = true;
    av.layer_count = 2;
    strcpy(av.layers[0].clip,      "aim_overlay");
    strcpy(av.layers[0].mask_path, "masks/upper_body.mask");
    av.layers[0].weight = 0.8f;
    av.layers[0].mode   = 1;            /* masked override */
    strcpy(av.layers[1].clip,      "breathe_additive");
    av.layers[1].mask_path[0] = '\0';  /* all bones */
    av.layers[1].weight = 0.5f;
    av.layers[1].mode   = 0;            /* additive */
    jce_scene_set_avatar(src, e, &av);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Hero");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_TRUE(jce_scene_has_avatar(dst, ne));
    JceAvatarComponent *out = jce_scene_get_avatar(dst, ne);
    TEST_ASSERT_NOT_NULL(out);

    TEST_ASSERT_EQUAL_STRING("rigs/hero.avatar", out->avatar_path);
    TEST_ASSERT_EQUAL_INT(2, out->layer_count);

    TEST_ASSERT_EQUAL_STRING("aim_overlay", out->layers[0].clip);
    TEST_ASSERT_EQUAL_STRING("masks/upper_body.mask", out->layers[0].mask_path);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.8f, out->layers[0].weight);
    TEST_ASSERT_EQUAL_INT(1, out->layers[0].mode);

    TEST_ASSERT_EQUAL_STRING("breathe_additive", out->layers[1].clip);
    TEST_ASSERT_EQUAL_STRING("", out->layers[1].mask_path);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, out->layers[1].weight);
    TEST_ASSERT_EQUAL_INT(0, out->layers[1].mode);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* An avatar with no authored layers must emit no "layers" array and reload
 * with layer_count == 0 (byte-identical to legacy avatars). */
static void test_avatar_no_layers_default(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Plain");
    JceAvatarComponent av;
    memset(&av, 0, sizeof av);
    av.human_rig = true;
    jce_scene_set_avatar(src, e, &av);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Plain");
    JceAvatarComponent *out = jce_scene_get_avatar(dst, ne);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_INT(0, out->layer_count);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_skeletal_2d_blend_round_trip);
    RUN_TEST(test_skeletal_1d_blend_omits_2d_fields);
    RUN_TEST(test_skeletal_retarget_source_round_trip);
    RUN_TEST(test_skeletal_no_retarget_source_default);
    RUN_TEST(test_avatar_layers_round_trip);
    RUN_TEST(test_avatar_no_layers_default);
    return UNITY_END();
}
