/*
 * test_jce_anim_layers.c — Unit tests for FEATURE 3.3 additive/layered
 * animation + the real per-bone avatar mask.
 *
 * Exercises the REAL dynamic paths:
 *   - jce_avatar_mask_create / set_weight / weight return AUTHORED per-bone
 *     weights (not the old unconditional 1.0 stub), with unset bones falling
 *     back to the mask default;
 *   - a .mask JSON asset round-trips through jce_avatar_mask_load (index form)
 *     and jce_avatar_mask_load_for_skeleton (name form);
 *   - jce_anim_player_blend_additive composes base + delta where mask=1 and
 *     leaves the base untouched where mask=0, through the REAL player /
 *     jce_anim_clip_sample / jce_skeleton_evaluate path;
 *   - jce_anim_player_blend_layers composes a base + a masked additive layer
 *     deterministically (a 2-entry stack).
 *
 * Links jce_core + jce_animation; the clip/skeleton constructors live in the
 * internal src header (jce_anim_clip_create / jce_skeleton_create), so this TU
 * pulls in engine/src via the test's include dir (see CMakeLists).
 */

#include "unity.h"

#include "middleware/animation/jce_animation.h"
#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/middleware/animation/jce_avatar_mask.h>
#include <jce/os/core/jce_math.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-4f
#define NJ  4u            /* root + 3 children */
#define DUR 1.0f

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

/* 4-joint chain skeleton. Rest translations are distinct so an additive delta
 * (clip - rest) is non-trivial and observable per joint. */
static JceSkeleton *make_skeleton(void)
{
    JceJoint j[NJ];
    memset(j, 0, sizeof(j));
    const char *names[NJ] = { "root", "spine", "arm", "hand" };
    for (uint32_t i = 0; i < NJ; i++) {
        strncpy(j[i].name, names[i], sizeof(j[i].name) - 1);
        j[i].parent              = (i == 0) ? -1 : (int16_t)(i - 1);
        j[i].inverse_bind_matrix = jce_m4_identity();
        j[i].local_transform     = jce_m4_identity();
        j[i].rest_translation    = jce_v3((float)i, 0.0f, 0.0f);
        j[i].rest_rotation       = jce_q_identity();
        j[i].rest_scale          = jce_v3(1.0f, 1.0f, 1.0f);
    }
    return jce_skeleton_create(j, NJ);
}

/* A clip that translates EVERY joint to a constant +Z value over [0,DUR].
 * Single keyframe per joint (STEP) so the sampled value is exactly `z`
 * regardless of time — keeps the delta math trivial to assert. */
static JceAnimClip *make_constant_z_clip(const char *name, float z)
{
    static float    ts[1] = { 0.0f };
    /* One static translation array per call distinguished by z; we keep four
     * separate statics so concurrent clips don't alias. */
    static jce_vec3 v0[NJ], v1[NJ], v2[NJ], v3[NJ];
    static int      slot = 0;
    jce_vec3 *vv = (slot == 0) ? v0 : (slot == 1) ? v1 : (slot == 2) ? v2 : v3;
    slot = (slot + 1) & 3;

    JceAnimChannel ch[NJ];
    memset(ch, 0, sizeof(ch));
    for (uint32_t i = 0; i < NJ; i++) {
        vv[i] = jce_v3(0.0f, 0.0f, z);
        ch[i].joint_index   = i;
        ch[i].target        = JCE_ANIM_TARGET_TRANSLATION;
        ch[i].interpolation = JCE_INTERP_STEP;
        ch[i].timestamps    = ts;
        ch[i].count         = 1;
        ch[i].translations  = &vv[i];
    }
    return jce_anim_clip_create(name, ch, NJ, DUR);
}

/* Translation column of joint j in a skinning palette. With an identity
 * skeleton (identity inverse-bind, parents at the origin via rest at +X but
 * inverse-bind identity) the skin matrix translation equals the joint's
 * accumulated GLOBAL translation. We instead read the LOCAL pose by using a
 * skeleton whose joints are all parented to keep things deterministic; to keep
 * the assertions about the LOCAL delta we read joint 0 (root: global==local)
 * and compare relative differences for children. */
static jce_vec3 palette_translation(const jce_mat4 *m)
{
    return jce_v3(m->raw[3][0], m->raw[3][1], m->raw[3][2]);
}

/* ------------------------------------------------------------------ */
/* Mask: authored weights, defaults, round-trip                        */
/* ------------------------------------------------------------------ */

static void test_mask_returns_authored_weights_not_one(void)
{
    JceAvatarMask *m = jce_avatar_mask_create(NJ, 1.0f);
    TEST_ASSERT_NOT_NULL(m);

    /* Default: unset bones are 1.0 (the old stub returned 1.0 for ALL bones;
     * here that must hold ONLY until we author something). */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, jce_avatar_mask_weight(m, 0));

    jce_avatar_mask_set_weight(m, 1, 0.25f);
    jce_avatar_mask_set_weight(m, 2, 0.0f);
    jce_avatar_mask_set_weight(m, 3, 0.75f);

    /* AUTHORED weights come back verbatim — proving it is no longer a stub. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f,  jce_avatar_mask_weight(m, 0)); /* unset */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.25f, jce_avatar_mask_weight(m, 1));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f,  jce_avatar_mask_weight(m, 2));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.75f, jce_avatar_mask_weight(m, 3));

    /* Out-of-range bone falls back to the mask default. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, jce_avatar_mask_weight(m, 99));

    /* set_weight clamps to [0,1]. */
    jce_avatar_mask_set_weight(m, 1, 5.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, jce_avatar_mask_weight(m, 1));
    jce_avatar_mask_set_weight(m, 1, -2.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, jce_avatar_mask_weight(m, 1));

    /* NULL mask is safe and reports the all-affected default. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, jce_avatar_mask_weight(NULL, 0));

    jce_avatar_mask_unload(m);
}

static void test_mask_default_fill_grows(void)
{
    /* Empty mask with a non-1 default: unset reads the default, growing past
     * the stored range backfills with the default. */
    JceAvatarMask *m = jce_avatar_mask_create(0, 0.5f);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_avatar_mask_count(m));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, jce_avatar_mask_weight(m, 0));

    jce_avatar_mask_set_weight(m, 3, 1.0f);          /* grows to 4 slots */
    TEST_ASSERT_EQUAL_UINT32(4u, jce_avatar_mask_count(m));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, jce_avatar_mask_weight(m, 0)); /* backfilled */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, jce_avatar_mask_weight(m, 2)); /* backfilled */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, jce_avatar_mask_weight(m, 3)); /* authored */

    jce_avatar_mask_unload(m);
}

static void write_text_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void test_mask_roundtrips_through_load_index_form(void)
{
    const char *path = "test_jce_anim_layers_idx.mask";
    /* Index-addressed entries + a non-1 default for unlisted bones. */
    write_text_file(path,
        "{ \"default\": 1.0, \"weights\": ["
        " { \"index\": 1, \"weight\": 0.25 },"
        " { \"index\": 2, \"weight\": 0.0 },"
        " { \"index\": 3, \"weight\": 0.75 } ] }");

    JceAvatarMask *m = jce_avatar_mask_load(path);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f,  jce_avatar_mask_weight(m, 0)); /* default */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.25f, jce_avatar_mask_weight(m, 1));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f,  jce_avatar_mask_weight(m, 2));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.75f, jce_avatar_mask_weight(m, 3));

    jce_avatar_mask_unload(m);
    remove(path);
}

static void test_mask_roundtrips_through_load_name_form(void)
{
    JceSkeleton *sk = make_skeleton();
    TEST_ASSERT_NOT_NULL(sk);

    const char *path = "test_jce_anim_layers_name.mask";
    /* Name-addressed entries resolved against the skeleton joint names. */
    write_text_file(path,
        "{ \"weights\": ["
        " { \"bone\": \"arm\",  \"weight\": 0.0 },"
        " { \"bone\": \"hand\", \"weight\": 0.5 } ] }");

    JceAvatarMask *m = jce_avatar_mask_load_for_skeleton(path, sk);
    TEST_ASSERT_NOT_NULL(m);
    /* Sized to the skeleton; unlisted bones default to 1.0. */
    TEST_ASSERT_EQUAL_UINT32(NJ, jce_avatar_mask_count(m));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, jce_avatar_mask_weight(m, 0)); /* root  */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, jce_avatar_mask_weight(m, 1)); /* spine */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, jce_avatar_mask_weight(m, 2)); /* arm   */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, jce_avatar_mask_weight(m, 3)); /* hand  */

    jce_avatar_mask_unload(m);
    remove(path);
    jce_skeleton_destroy(sk);
}

/* ------------------------------------------------------------------ */
/* Additive blend through the REAL player                              */
/* ------------------------------------------------------------------ */

/* base + delta where mask=1; base unchanged where mask=0. The reference is the
 * rest pose (NULL ref clip), so the per-joint delta is (add_z - rest_z=0). */
static void test_additive_mask_one_adds_delta_zero_keeps_base(void)
{
    JceSkeleton   *sk   = make_skeleton();
    JceAnimPlayer *p    = jce_anim_player_create(sk);
    JceAnimClip   *base = make_constant_z_clip("base", 2.0f);  /* every joint z=2 */
    JceAnimClip   *add  = make_constant_z_clip("add",  5.0f);  /* every joint z=5 */
    TEST_ASSERT_NOT_NULL(p);

    /* Mask: bone 1 fully takes the additive delta, bone 2 takes none. */
    JceAvatarMask *m = jce_avatar_mask_create(NJ, 1.0f);
    jce_avatar_mask_set_weight(m, 1, 1.0f);
    jce_avatar_mask_set_weight(m, 2, 0.0f);

    jce_mat4 pal[NJ];
    uint32_t n = jce_anim_player_blend_additive(
        p, base, 0.0f, add, 0.0f, /*ref*/NULL, 0.0f, m, /*weight*/1.0f,
        pal, NJ);
    TEST_ASSERT_TRUE(n >= NJ);

    /* The skin palette is global; with identity inverse-bind matrices the root
     * joint's column equals its local translation, and each child column is the
     * accumulated parent + local. We assert the ROOT (mask default 1.0) and use
     * differences for the masked children so parent accumulation cancels. */

    /* Recompute the same pose WITHOUT the additive layer (pure base) for a
     * reference, so we can compare per-joint columns directly. */
    jce_mat4 base_pal[NJ];
    uint32_t nb = jce_anim_player_blend_layers(p, base, 0.0f, NULL, 0, base_pal, NJ);
    TEST_ASSERT_TRUE(nb >= NJ);

    /* Bone 1 (mask=1): its LOCAL z must move by the full delta (5 - 0 = +5)
     * relative to base. The change in the child's column relative to its parent
     * isolates the local delta:  (pal[1]-pal[0]) - (base_pal[1]-base_pal[0]). */
    jce_vec3 d1 = jce_v3_sub(
        jce_v3_sub(palette_translation(&pal[1]),      palette_translation(&pal[0])),
        jce_v3_sub(palette_translation(&base_pal[1]), palette_translation(&base_pal[0])));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 5.0f, d1.z);  /* full additive delta */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, d1.x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, d1.y);

    /* Bone 2 (mask=0): LOCAL transform is byte-identical to the base — no delta
     * leaks in. Isolate joint 2's local part the same way. */
    jce_vec3 d2 = jce_v3_sub(
        jce_v3_sub(palette_translation(&pal[2]),      palette_translation(&pal[1])),
        jce_v3_sub(palette_translation(&base_pal[2]), palette_translation(&base_pal[1])));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, d2.z);  /* masked out — unchanged */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, d2.x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, d2.y);

    jce_avatar_mask_unload(m);
    jce_anim_clip_destroy(add);
    jce_anim_clip_destroy(base);
    jce_anim_player_destroy(p);
    jce_skeleton_destroy(sk);
}

/* weight=0 (or NULL clip) is byte-identical to a pure single-clip evaluation:
 * proves the additive path is inert at zero weight. */
static void test_additive_zero_weight_is_base(void)
{
    JceSkeleton   *sk   = make_skeleton();
    JceAnimPlayer *p    = jce_anim_player_create(sk);
    JceAnimClip   *base = make_constant_z_clip("base", 2.0f);
    JceAnimClip   *add  = make_constant_z_clip("add",  9.0f);

    jce_mat4 with_layer[NJ], base_only[NJ];
    jce_anim_player_blend_additive(p, base, 0.0f, add, 0.0f, NULL, 0.0f,
                                   NULL, /*weight*/0.0f, with_layer, NJ);
    jce_anim_player_blend_layers(p, base, 0.0f, NULL, 0, base_only, NJ);

    for (uint32_t i = 0; i < NJ; i++) {
        jce_vec3 a = palette_translation(&with_layer[i]);
        jce_vec3 b = palette_translation(&base_only[i]);
        TEST_ASSERT_FLOAT_WITHIN(EPS, b.x, a.x);
        TEST_ASSERT_FLOAT_WITHIN(EPS, b.y, a.y);
        TEST_ASSERT_FLOAT_WITHIN(EPS, b.z, a.z);
    }

    jce_anim_clip_destroy(add);
    jce_anim_clip_destroy(base);
    jce_anim_player_destroy(p);
    jce_skeleton_destroy(sk);
}

/* ------------------------------------------------------------------ */
/* 2-layer stack composes deterministically                            */
/* ------------------------------------------------------------------ */

/* Base then ONE masked additive layer, evaluated twice — identical results
 * (deterministic), and the masked additive contributes exactly the delta on
 * the masked-in bone while leaving the masked-out bone at the base. */
static void test_two_layer_stack_composes_deterministically(void)
{
    JceSkeleton   *sk   = make_skeleton();
    JceAnimPlayer *p    = jce_anim_player_create(sk);
    JceAnimClip   *base = make_constant_z_clip("base", 1.0f);
    JceAnimClip   *add  = make_constant_z_clip("add",  4.0f);

    /* Mask: bone 3 (hand) takes the additive; bone 1 (spine) does not. */
    JceAvatarMask *m = jce_avatar_mask_create(NJ, 0.0f); /* default 0: only
                                                            authored bones move */
    jce_avatar_mask_set_weight(m, 3, 1.0f);

    JceAnimLayer layers[1];
    memset(layers, 0, sizeof(layers));
    layers[0].clip     = add;
    layers[0].time     = 0.0f;
    layers[0].weight   = 1.0f;
    layers[0].mode     = JCE_ANIM_LAYER_ADDITIVE;
    layers[0].mask     = m;
    layers[0].ref_clip = NULL;   /* rest pose reference */

    jce_mat4 r1[NJ], r2[NJ], base_pal[NJ];
    uint32_t n1 = jce_anim_player_blend_layers(p, base, 0.0f, layers, 1, r1, NJ);
    uint32_t n2 = jce_anim_player_blend_layers(p, base, 0.0f, layers, 1, r2, NJ);
    jce_anim_player_blend_layers(p, base, 0.0f, NULL, 0, base_pal, NJ);
    TEST_ASSERT_TRUE(n1 >= NJ && n2 >= NJ);

    /* Determinism: two evaluations of the same stack are bit-stable. */
    for (uint32_t i = 0; i < NJ; i++) {
        jce_vec3 a = palette_translation(&r1[i]);
        jce_vec3 b = palette_translation(&r2[i]);
        TEST_ASSERT_FLOAT_WITHIN(0.0f, a.x, b.x);
        TEST_ASSERT_FLOAT_WITHIN(0.0f, a.y, b.y);
        TEST_ASSERT_FLOAT_WITHIN(0.0f, a.z, b.z);
    }

    /* Bone 1 (mask default 0): byte-identical to base. */
    jce_vec3 d1 = jce_v3_sub(
        jce_v3_sub(palette_translation(&r1[1]),       palette_translation(&r1[0])),
        jce_v3_sub(palette_translation(&base_pal[1]), palette_translation(&base_pal[0])));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, d1.z);

    /* Bone 3 (mask=1): full additive delta (4 - 0 = +4) over base. */
    jce_vec3 d3 = jce_v3_sub(
        jce_v3_sub(palette_translation(&r1[3]),       palette_translation(&r1[2])),
        jce_v3_sub(palette_translation(&base_pal[3]), palette_translation(&base_pal[2])));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 4.0f, d3.z);

    jce_avatar_mask_unload(m);
    jce_anim_clip_destroy(add);
    jce_anim_clip_destroy(base);
    jce_anim_player_destroy(p);
    jce_skeleton_destroy(sk);
}

/* num_layers==0 with a base clip is exactly a single-clip evaluation. */
static void test_zero_layers_equals_single_clip(void)
{
    JceSkeleton   *sk   = make_skeleton();
    JceAnimPlayer *p    = jce_anim_player_create(sk);
    JceAnimClip   *base = make_constant_z_clip("base", 3.0f);

    jce_mat4 viaplayer[NJ], vialayers[NJ];

    /* Reference: drive the clip through the plain player update path. */
    jce_anim_player_play(p, base, false, 1.0f);
    jce_anim_player_set_time(p, 0.0f);
    jce_anim_player_update(p, 0.0f, viaplayer, NJ);

    jce_anim_player_blend_layers(p, base, 0.0f, NULL, 0, vialayers, NJ);

    for (uint32_t i = 0; i < NJ; i++) {
        jce_vec3 a = palette_translation(&viaplayer[i]);
        jce_vec3 b = palette_translation(&vialayers[i]);
        TEST_ASSERT_FLOAT_WITHIN(EPS, a.x, b.x);
        TEST_ASSERT_FLOAT_WITHIN(EPS, a.y, b.y);
        TEST_ASSERT_FLOAT_WITHIN(EPS, a.z, b.z);
    }

    jce_anim_clip_destroy(base);
    jce_anim_player_destroy(p);
    jce_skeleton_destroy(sk);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mask_returns_authored_weights_not_one);
    RUN_TEST(test_mask_default_fill_grows);
    RUN_TEST(test_mask_roundtrips_through_load_index_form);
    RUN_TEST(test_mask_roundtrips_through_load_name_form);
    RUN_TEST(test_additive_mask_one_adds_delta_zero_keeps_base);
    RUN_TEST(test_additive_zero_weight_is_base);
    RUN_TEST(test_two_layer_stack_composes_deterministically);
    RUN_TEST(test_zero_layers_equals_single_clip);
    return UNITY_END();
}
