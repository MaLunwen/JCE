/*
 * test_jce_morph.c — Unit tests for FEATURE 3.1 morph targets / blendshapes.
 *
 * Exercises the REAL dynamic paths, not a mock:
 *
 *   (1) Morph CPU core (jce_morph.c):
 *       - build JceMorphData with known POSITION/NORMAL deltas + base weights;
 *       - apply weighted deltas to a base vertex buffer through the REAL
 *         jce_morph_apply and assert out == base + sum(w_i * delta_i);
 *       - sample a REAL JceMorphWeightTrack and assert it matches the keyframes
 *         (endpoints + interior LINEAR/STEP interpolation).
 *
 *   (2) Real glTF import (jce_gltf_loader.c, decode-only / bgfx-free):
 *       - feed a synthesized in-memory .gltf with 1 primitive, 2 morph targets
 *         (target 0: POSITION + NORMAL deltas, target 1: POSITION only), mesh
 *         base weights, and a "weights" animation channel to the REAL
 *         jce_gltf_decode_cpu_memory;
 *       - assert prim->targets were read (delta values + base weights);
 *       - assert the (previously dropped) weights channel imported into a
 *         JceMorphWeightTrack with the correct keyframes;
 *       - close the loop: morph the imported base mesh with weights sampled
 *         from the imported track and assert the deformed positions.
 *
 * Links jce_core + jce_animation + jce_renderer.  The decode path touches no
 * bgfx (GPU upload is a separate step), so this runs without a render context —
 * exactly like test_jce_skin_palette which also links jce_renderer.  Needs
 * engine/src on the include path for the loader's internal header.
 */

#include "unity.h"

#include <jce/middleware/animation/jce_morph.h>
#include <jce/os/core/jce_math.h>

/* Internal loader header (CPU-intermediate morph accessors). */
#include "renderer/jce_gltf_loader.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-4f

/* ------------------------------------------------------------------ */
/* (1a) CPU evaluator                                                  */
/* ------------------------------------------------------------------ */

/* A simple interleaved vertex matching the morph test data: pos + normal. */
typedef struct { float pos[3]; float normal[3]; } TestVtx;

static void test_apply_position_only(void)
{
    /* 2 targets x 2 verts, position deltas only. */
    JceMorphData *m = jce_morph_data_create(2, 2, /*has_normals*/false);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT32(2, jce_morph_target_count(m));
    TEST_ASSERT_EQUAL_UINT32(2, jce_morph_vertex_count(m));
    TEST_ASSERT_FALSE(jce_morph_has_normals(m));

    jce_vec3 t0[2] = { { 1,0,0 }, { 2,0,0 } };  /* target 0 +X */
    jce_vec3 t1[2] = { { 0,1,0 }, { 0,3,0 } };  /* target 1 +Y */
    jce_morph_set_position_deltas(m, 0, t0, 2);
    jce_morph_set_position_deltas(m, 1, t1, 2);

    TestVtx base[2] = {
        { { 10, 20, 30 }, { 0,0,1 } },
        { { 40, 50, 60 }, { 0,0,1 } },
    };
    TestVtx out[2];

    float weights[2] = { 0.5f, 2.0f };

    bool ok = jce_morph_apply(m, weights, 2, base, out, 2,
                              (uint32_t)sizeof(TestVtx),
                              (int32_t)offsetof(TestVtx, pos),
                              -1 /* skip normals */);
    TEST_ASSERT_TRUE(ok);

    /* v0: pos + 0.5*t0[0] + 2.0*t1[0] = (10,20,30)+(0.5,0,0)+(0,2,0) */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 10.5f, out[0].pos[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 22.0f, out[0].pos[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 30.0f, out[0].pos[2]);
    /* v1: (40,50,60)+0.5*(2,0,0)+2.0*(0,3,0) = (41,56,60) */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 41.0f, out[1].pos[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 56.0f, out[1].pos[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 60.0f, out[1].pos[2]);

    jce_morph_data_destroy(m);
}

static void test_apply_zero_weights_is_copy(void)
{
    JceMorphData *m = jce_morph_data_create(1, 1, false);
    jce_vec3 d = { 100, 100, 100 };
    jce_morph_set_position_deltas(m, 0, &d, 1);

    TestVtx base = { { 1, 2, 3 }, { 0,0,1 } };
    TestVtx out;
    float w = 0.0f;
    bool ok = jce_morph_apply(m, &w, 1, &base, &out, 1,
                              (uint32_t)sizeof(TestVtx),
                              (int32_t)offsetof(TestVtx, pos), -1);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out.pos[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, out.pos[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 3.0f, out.pos[2]);
    jce_morph_data_destroy(m);
}

static void test_apply_normals_renormalized(void)
{
    JceMorphData *m = jce_morph_data_create(1, 1, /*has_normals*/true);
    TEST_ASSERT_TRUE(jce_morph_has_normals(m));
    jce_vec3 dp = { 0, 0, 0 };
    jce_vec3 dn = { 0, 1, 0 };   /* rotate normal toward +Y */
    jce_morph_set_position_deltas(m, 0, &dp, 1);
    jce_morph_set_normal_deltas(m, 0, &dn, 1);

    TestVtx base = { { 0,0,0 }, { 0,0,1 } };  /* normal +Z */
    TestVtx out;
    float w = 1.0f;
    bool ok = jce_morph_apply(m, &w, 1, &base, &out, 1,
                              (uint32_t)sizeof(TestVtx),
                              (int32_t)offsetof(TestVtx, pos),
                              (int32_t)offsetof(TestVtx, normal));
    TEST_ASSERT_TRUE(ok);
    /* normal = normalize((0,0,1)+(0,1,0)) = (0, 0.7071, 0.7071) */
    float inv = 1.0f / sqrtf(2.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out.normal[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, inv,  out.normal[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, inv,  out.normal[2]);
    jce_morph_data_destroy(m);
}

static void test_apply_in_place_aliasing(void)
{
    JceMorphData *m = jce_morph_data_create(1, 1, false);
    jce_vec3 d = { 5, 0, 0 };
    jce_morph_set_position_deltas(m, 0, &d, 1);
    TestVtx buf = { { 1, 1, 1 }, { 0,0,1 } };
    float w = 2.0f;
    bool ok = jce_morph_apply(m, &w, 1, &buf, &buf, 1,
                              (uint32_t)sizeof(TestVtx),
                              (int32_t)offsetof(TestVtx, pos), -1);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 11.0f, buf.pos[0]);
    jce_morph_data_destroy(m);
}

/* ------------------------------------------------------------------ */
/* (1b) Weight-track sampling                                          */
/* ------------------------------------------------------------------ */

static void test_weight_track_sample_linear(void)
{
    /* 2 targets, 3 keys (key-major values). */
    float ts[3] = { 0.0f, 1.0f, 2.0f };
    float vals[6] = {
        0.0f, 1.0f,   /* key0 */
        1.0f, 0.0f,   /* key1 */
        0.5f, 0.5f,   /* key2 */
    };
    JceMorphWeightTrack *t = jce_morph_weight_track_create(
        2, 3, ts, vals, JCE_MORPH_INTERP_LINEAR);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT32(2, jce_morph_weight_track_targets(t));
    TEST_ASSERT_EQUAL_UINT32(3, jce_morph_weight_track_keys(t));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, jce_morph_weight_track_duration(t));

    float out[2];

    /* Exact key0. */
    TEST_ASSERT_EQUAL_UINT32(2, jce_morph_weight_track_sample(t, 0.0f, out, 2));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[1]);

    /* Midpoint key0->key1 (t=0.5): lerp each target. */
    jce_morph_weight_track_sample(t, 0.5f, out, 2);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, out[0]);  /* 0->1 */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, out[1]);  /* 1->0 */

    /* Exact key2. */
    jce_morph_weight_track_sample(t, 2.0f, out, 2);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, out[1]);

    /* Clamp before start / after end. */
    jce_morph_weight_track_sample(t, -5.0f, out, 2);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[1]);
    jce_morph_weight_track_sample(t, 99.0f, out, 2);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, out[1]);

    jce_morph_weight_track_destroy(t);
}

static void test_weight_track_sample_step(void)
{
    float ts[2] = { 0.0f, 1.0f };
    float vals[2] = { 0.0f, 1.0f };
    JceMorphWeightTrack *t = jce_morph_weight_track_create(
        1, 2, ts, vals, JCE_MORPH_INTERP_STEP);
    float out[1];
    /* STEP holds the left key's value until the next key. */
    jce_morph_weight_track_sample(t, 0.99f, out, 1);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[0]);
    jce_morph_weight_track_sample(t, 1.0f, out, 1);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[0]);
    jce_morph_weight_track_destroy(t);
}

/* ------------------------------------------------------------------ */
/* (2) Real glTF import — decode-only, bgfx-free                       */
/* ------------------------------------------------------------------ */

/* Synthesized in-memory glTF: 3-vertex mesh, POSITION+NORMAL, indices, two
 * morph targets (t0 POSITION +X & NORMAL +Y; t1 POSITION +2Y), mesh base
 * weights [0.25, 0.75], and a LINEAR "weights" animation channel with 2 keys
 * (k0=(0,0), k1=(1.0,0.5)).  Generated by .tmp_gen_morph.py. */
static const char k_morph_gltf[] =
#include "morph_gltf_inc.h"
;

static void test_import_reads_morph_targets(void)
{
    JceModelCpu *cpu = jce_gltf_decode_cpu_memory(
        k_morph_gltf, (uint32_t)(sizeof(k_morph_gltf) - 1), "morph_test");
    TEST_ASSERT_NOT_NULL(cpu);

    TEST_ASSERT_TRUE(jce_gltf_cpu_node_count(cpu) >= 1u);
    TEST_ASSERT_TRUE(jce_gltf_cpu_node_prim_count(cpu, 0) >= 1u);

    const JceMorphData *m = jce_gltf_cpu_prim_morph(cpu, 0, 0);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT32(2, jce_morph_target_count(m));
    TEST_ASSERT_EQUAL_UINT32(3, jce_morph_vertex_count(m));
    TEST_ASSERT_TRUE(jce_morph_has_normals(m));   /* target 0 carries NORMAL */

    /* Target 0 POSITION deltas: +X for all 3 verts. */
    const jce_vec3 *t0p = jce_morph_position_deltas(m, 0);
    TEST_ASSERT_NOT_NULL(t0p);
    for (int v = 0; v < 3; ++v) {
        TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, t0p[v].x);
        TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t0p[v].y);
        TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t0p[v].z);
    }

    /* Target 0 NORMAL deltas: +Y for all verts. */
    const jce_vec3 *t0n = jce_morph_normal_deltas(m, 0);
    TEST_ASSERT_NOT_NULL(t0n);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t0n[0].x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, t0n[0].y);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t0n[0].z);

    /* Target 1 POSITION deltas: +2Y. */
    const jce_vec3 *t1p = jce_morph_position_deltas(m, 1);
    TEST_ASSERT_NOT_NULL(t1p);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t1p[0].x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, t1p[0].y);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t1p[0].z);

    /* Target 1 has no NORMAL deltas -> zeroed (allocated since target 0 has). */
    const jce_vec3 *t1n = jce_morph_normal_deltas(m, 1);
    TEST_ASSERT_NOT_NULL(t1n);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t1n[0].x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t1n[0].y);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, t1n[0].z);

    /* Base weights from mesh.weights = [0.25, 0.75]. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.25f, jce_morph_base_weight(m, 0));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.75f, jce_morph_base_weight(m, 1));

    float bw[2] = { -1, -1 };
    TEST_ASSERT_EQUAL_UINT32(2, jce_morph_copy_base_weights(m, bw, 2));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.25f, bw[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.75f, bw[1]);

    jce_gltf_model_cpu_free(cpu);
}

static void test_import_undrops_weights_channel(void)
{
    JceModelCpu *cpu = jce_gltf_decode_cpu_memory(
        k_morph_gltf, (uint32_t)(sizeof(k_morph_gltf) - 1), "morph_test");
    TEST_ASSERT_NOT_NULL(cpu);

    /* The "weights" animation channel must NOT be dropped any more. */
    TEST_ASSERT_EQUAL_UINT32(1, jce_gltf_cpu_morph_anim_count(cpu));

    uint32_t anim_idx = 99, node_idx = 99;
    const JceMorphWeightTrack *t = jce_gltf_cpu_morph_anim_track(
        cpu, 0, &anim_idx, &node_idx);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT32(0, anim_idx);
    TEST_ASSERT_EQUAL_UINT32(0, node_idx);
    TEST_ASSERT_EQUAL_UINT32(2, jce_morph_weight_track_targets(t));
    TEST_ASSERT_EQUAL_UINT32(2, jce_morph_weight_track_keys(t));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, jce_morph_weight_track_duration(t));

    float out[2];
    /* key0 = (0,0). */
    jce_morph_weight_track_sample(t, 0.0f, out, 2);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[1]);
    /* key1 = (1.0, 0.5). */
    jce_morph_weight_track_sample(t, 1.0f, out, 2);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f, out[1]);
    /* midpoint t=0.5: lerp -> (0.5, 0.25). */
    jce_morph_weight_track_sample(t, 0.5f, out, 2);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f,  out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.25f, out[1]);

    jce_gltf_model_cpu_free(cpu);
}

/* End-to-end through the REAL import: morph the imported base mesh with weights
 * sampled from the imported track at t=1.0 (w=[1.0, 0.5]) and assert deformed
 * vertex 0:  base(0,0,0) + 1.0*t0(+X) + 0.5*t1(+2Y) = (1, 1, 0). */
static void test_import_eval_end_to_end(void)
{
    JceModelCpu *cpu = jce_gltf_decode_cpu_memory(
        k_morph_gltf, (uint32_t)(sizeof(k_morph_gltf) - 1), "morph_test");
    TEST_ASSERT_NOT_NULL(cpu);

    const JceMorphData *m = jce_gltf_cpu_prim_morph(cpu, 0, 0);
    TEST_ASSERT_NOT_NULL(m);
    const JceMorphWeightTrack *t = jce_gltf_cpu_morph_anim_track(cpu, 0, NULL, NULL);
    TEST_ASSERT_NOT_NULL(t);

    float w[2];
    jce_morph_weight_track_sample(t, 1.0f, w, 2);   /* (1.0, 0.5) */

    /* Base mesh authored positions (matches the generator): v0=(0,0,0). */
    TestVtx base = { { 0, 0, 0 }, { 0, 0, 1 } };
    TestVtx out;
    bool ok = jce_morph_apply(m, w, 2, &base, &out, 1,
                              (uint32_t)sizeof(TestVtx),
                              (int32_t)offsetof(TestVtx, pos),
                              (int32_t)offsetof(TestVtx, normal));
    TEST_ASSERT_TRUE(ok);
    /* pos = (0,0,0) + 1.0*(1,0,0) + 0.5*(0,2,0) = (1, 1, 0). */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out.pos[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out.pos[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out.pos[2]);
    /* normal = normalize((0,0,1) + 1.0*(0,1,0) + 0.5*(0,0,0)) = (0,.707,.707). */
    float inv = 1.0f / sqrtf(2.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out.normal[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, inv,  out.normal[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, inv,  out.normal[2]);

    jce_gltf_model_cpu_free(cpu);
}

/* ------------------------------------------------------------------ */
/* (3) Scene-deform parity — mirror EXACTLY what sr_deform_morph_prims  */
/*     does on a JceSkinnedVertex-layout buffer (FEATURE 3.1 GPU path). */
/* ------------------------------------------------------------------ */

/* A standalone mirror of JceSkinnedVertex (jce_skinned_mesh.h): pos@0,
 * normal@12, uv@24, tangent@32, joints@48 (uint8[4]), weights@52 (float[4]).
 * The deform must touch ONLY pos/normal and copy the rest through untouched —
 * the skinning attributes (joints/weights) must survive bit-for-bit. */
typedef struct {
    float   pos[3];
    float   normal[3];
    float   uv[2];
    float   tangent[4];
    uint8_t joints[4];
    float   weights[4];
} SkinnedVtxMirror;

/* Mirror sr_deform_morph_prims EXACTLY: copy the FULL base verts into a scratch
 * buffer (preserving uv/tangent/joints/weights), then run jce_morph_apply
 * IN-PLACE over that scratch (the scene path memcpy's into the bgfx transient
 * buffer first, then deforms in-place — jce_morph_apply only writes pos/normal,
 * so the trailing skinning attributes survive).  Assert deformed pos/normal AND
 * that the trailing joints/weights bytes are PRESERVED unchanged. */
static void test_deform_preserves_skinning_attrs(void)
{
    /* 1 target, 2 verts: position +X, normal +Y (target 0). */
    JceMorphData *m = jce_morph_data_create(1, 2, /*has_normals*/true);
    TEST_ASSERT_NOT_NULL(m);
    jce_vec3 dp[2] = { { 3,0,0 }, { 5,0,0 } };
    jce_vec3 dn[2] = { { 0,1,0 }, { 0,1,0 } };
    jce_morph_set_position_deltas(m, 0, dp, 2);
    jce_morph_set_normal_deltas(m, 0, dn, 2);

    /* Base verts with SENTINEL joints/weights so corruption is detectable. */
    SkinnedVtxMirror base[2];
    memset(base, 0, sizeof(base));
    base[0].pos[0]=10; base[0].pos[1]=20; base[0].pos[2]=30;
    base[0].normal[2]=1.0f;  /* +Z */
    base[0].uv[0]=0.1f; base[0].uv[1]=0.2f;
    base[0].tangent[0]=1.0f; base[0].tangent[3]=-1.0f;
    base[0].joints[0]=7; base[0].joints[1]=9; base[0].joints[2]=11; base[0].joints[3]=13;
    base[0].weights[0]=0.5f; base[0].weights[1]=0.25f;
    base[0].weights[2]=0.15f; base[0].weights[3]=0.10f;

    base[1].pos[0]=40; base[1].pos[1]=50; base[1].pos[2]=60;
    base[1].normal[2]=1.0f;
    base[1].uv[0]=0.3f; base[1].uv[1]=0.4f;
    base[1].tangent[1]=1.0f; base[1].tangent[3]=1.0f;
    base[1].joints[0]=1; base[1].joints[1]=2; base[1].joints[2]=3; base[1].joints[3]=4;
    base[1].weights[0]=0.7f; base[1].weights[1]=0.1f;
    base[1].weights[2]=0.1f; base[1].weights[3]=0.1f;

    /* Resolve weights the SAME way sr_resolve_morph_weights does: authored
     * static weight, override bit set, NULL track. */
    float authored[1]  = { 2.0f };
    uint32_t mask      = 0x1u;        /* override target 0 */
    float w[JCE_MORPH_MAX_WEIGHTS];
    uint32_t nw = jce_morph_resolve_weights(NULL, authored, mask, 1, w,
                                            JCE_MORPH_MAX_WEIGHTS);
    TEST_ASSERT_EQUAL_UINT32(1, nw);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, w[0]);

    /* === exact scene-path deform: full copy, then in-place apply. === */
    SkinnedVtxMirror out[2];
    memset(out, 0xCD, sizeof(out));          /* poison: prove the copy fills all */
    memcpy(out, base, sizeof(base));         /* bring ALL attributes through */
    bool ok = jce_morph_apply(m, w, nw, out, out, 2,   /* in-place (base==out) */
                              (uint32_t)sizeof(SkinnedVtxMirror),
                              (int32_t)offsetof(SkinnedVtxMirror, pos),
                              (int32_t)offsetof(SkinnedVtxMirror, normal));
    TEST_ASSERT_TRUE(ok);

    /* Deformed pos: base + 2*dp. v0=(10+6,20,30)=(16,20,30); v1=(40+10,50,60). */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 16.0f, out[0].pos[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 20.0f, out[0].pos[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 30.0f, out[0].pos[2]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 50.0f, out[1].pos[0]);

    /* Deformed normal: normalize((0,0,1)+2*(0,1,0)) = (0, 2, 1)/sqrt(5). */
    float inv5 = 1.0f / sqrtf(5.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f,        out[0].normal[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f * inv5, out[0].normal[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f * inv5, out[0].normal[2]);

    /* Skinning attributes MUST be preserved byte-for-byte. */
    for (int v = 0; v < 2; v++) {
        TEST_ASSERT_EQUAL_UINT8(base[v].joints[0], out[v].joints[0]);
        TEST_ASSERT_EQUAL_UINT8(base[v].joints[1], out[v].joints[1]);
        TEST_ASSERT_EQUAL_UINT8(base[v].joints[2], out[v].joints[2]);
        TEST_ASSERT_EQUAL_UINT8(base[v].joints[3], out[v].joints[3]);
        for (int k = 0; k < 4; k++)
            TEST_ASSERT_FLOAT_WITHIN(EPS, base[v].weights[k], out[v].weights[k]);
        /* UV + tangent (also outside pos/normal) are passed through too. */
        TEST_ASSERT_FLOAT_WITHIN(EPS, base[v].uv[0], out[v].uv[0]);
        TEST_ASSERT_FLOAT_WITHIN(EPS, base[v].uv[1], out[v].uv[1]);
        for (int k = 0; k < 4; k++)
            TEST_ASSERT_FLOAT_WITHIN(EPS, base[v].tangent[k], out[v].tangent[k]);
    }

    /* Byte-identical confirmation for joints/weights region specifically. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(base[0].joints, out[0].joints,
                                    sizeof(base[0].joints)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(base[0].weights, out[0].weights,
                                    sizeof(base[0].weights)));

    jce_morph_data_destroy(m);
}

/* Byte-identical regression guard: the scene path runs (copy base -> scratch)
 * then jce_morph_apply with morph_count == 0 (the legacy default).  With zero
 * weights the apply is a no-op deform, so the scratch must equal the base
 * buffer byte-for-byte (no skinning-attr drift) — proving morph-off content is
 * unchanged. */
static void test_deform_zero_count_is_byte_identical(void)
{
    JceMorphData *m = jce_morph_data_create(2, 3, /*has_normals*/true);
    TEST_ASSERT_NOT_NULL(m);
    jce_vec3 big[3] = { { 99,99,99 }, { 88,88,88 }, { 77,77,77 } };
    jce_morph_set_position_deltas(m, 0, big, 3);
    jce_morph_set_normal_deltas(m, 0, big, 3);

    SkinnedVtxMirror base[3];
    /* Fill with a deterministic, fully-populated bit pattern. */
    for (int v = 0; v < 3; v++) {
        unsigned char *bytes = (unsigned char *)&base[v];
        for (size_t b = 0; b < sizeof(SkinnedVtxMirror); b++)
            bytes[b] = (unsigned char)((v * 31 + (int)b * 7) & 0xFF);
    }

    /* Scene-path mirror: full copy, then in-place apply with 0 weights. */
    SkinnedVtxMirror out[3];
    memset(out, 0xCD, sizeof(out));  /* poison so a partial copy is visible */
    memcpy(out, base, sizeof(base));
    bool ok = jce_morph_apply(m, NULL, 0, out, out, 3,
                              (uint32_t)sizeof(SkinnedVtxMirror),
                              (int32_t)offsetof(SkinnedVtxMirror, pos),
                              (int32_t)offsetof(SkinnedVtxMirror, normal));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(0, memcmp(base, out, sizeof(base)));

    jce_morph_data_destroy(m);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

/* THE COMBINE THE RENDERER PERFORMS -- and the case that shipped broken.
 *
 * sr_resolve_morph_weights passed `track = NULL` and returned early unless the
 * entity authored a JceMorphWeights component.  Both halves had to be wrong
 * for the failure to be total, and both were: a model whose clip animates its
 * blendshapes resolved to all zeros, deformed nothing, and reported nothing.
 * That is facial animation baked into a glTF -- the primary use of blendshapes
 * in every engine this one is measured against -- not playing at all. */
static void test_a_clip_drives_morph_with_no_authored_component(void)
{
    JceModelCpu *cpu = jce_gltf_decode_cpu_memory(
        k_morph_gltf, (uint32_t)(sizeof(k_morph_gltf) - 1), "morph_test");
    TEST_ASSERT_NOT_NULL(cpu);
    const JceMorphWeightTrack *t =
        jce_gltf_cpu_morph_anim_track(cpu, 0, NULL, NULL);
    TEST_ASSERT_NOT_NULL(t);

    float sampled[2];
    TEST_ASSERT_EQUAL_UINT32(2, jce_morph_weight_track_sample(t, 0.5f,
                                                              sampled, 2));

    float w[JCE_MORPH_MAX_WEIGHTS];

    /* No component at all: authored NULL, mask 0.  The clip alone drives it. */
    TEST_ASSERT_EQUAL_UINT32(2, jce_morph_resolve_weights(
        sampled, NULL, 0u, 2, w, JCE_MORPH_MAX_WEIGHTS));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f,  w[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.25f, w[1]);

    /* NEGATIVE CONTROL -- this is exactly what shipped.  Same entity, same
     * clip, and the renderer's own arguments: no track, no component.  Every
     * weight is zero, so jce_morph_apply is a copy and the face never moves;
     * there is no error and no warning, because an ignored blendshape and one
     * whose weight is genuinely 0 are the same picture. */
    TEST_ASSERT_EQUAL_UINT32(2, jce_morph_resolve_weights(
        NULL, NULL, 0u, 2, w, JCE_MORPH_MAX_WEIGHTS));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, w[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, w[1]);

    /* And the designer still wins.  Pinning a blendshape OFF is the case the
     * override bitmask exists for, and it only means anything now that there
     * is a track for it to override -- against an all-zero track it was
     * indistinguishable from doing nothing. */
    float authored[2] = { 0.0f, 0.0f };
    TEST_ASSERT_EQUAL_UINT32(2, jce_morph_resolve_weights(
        sampled, authored, 1u /* bit 0 only */, 2, w, JCE_MORPH_MAX_WEIGHTS));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f,  w[0]);   /* pinned off by the author */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.25f, w[1]);   /* still follows the clip */

    jce_gltf_model_cpu_free(cpu);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_apply_position_only);
    RUN_TEST(test_apply_zero_weights_is_copy);
    RUN_TEST(test_apply_normals_renormalized);
    RUN_TEST(test_apply_in_place_aliasing);
    RUN_TEST(test_weight_track_sample_linear);
    RUN_TEST(test_weight_track_sample_step);
    RUN_TEST(test_import_reads_morph_targets);
    RUN_TEST(test_import_undrops_weights_channel);
    RUN_TEST(test_import_eval_end_to_end);
    RUN_TEST(test_deform_preserves_skinning_attrs);
    RUN_TEST(test_deform_zero_count_is_byte_identical);
    RUN_TEST(test_a_clip_drives_morph_with_no_authored_component);
    return UNITY_END();
}
