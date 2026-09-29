/*
 * test_jce_scene_sequence_player.c
 *
 * SequencePlayer component + sequencer↔scene integrator (P1-L):
 *   - canonical property catalogue (name ↔ id, kind, support gating),
 *   - float/color apply + read-back through the public scene accessors,
 *   - component JSON round-trip incl. the bindings[] entity-ref remap,
 *   - structured/legacy track-binding parse in the sequencer loader.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/middleware/scene/jce_scene_sequencer.h>
#include <jce/middleware/scene/jce_sequencer.h>
#include <jce/os/core/jce_json.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── Property catalogue ─────────────────────────────────────────── */

static void test_prop_catalogue_name_round_trip(void)
{
    for (int p = JCE_SEQ_PROP_NONE + 1; p < JCE_SEQ_PROP_COUNT; ++p) {
        const char *name = jce_seq_prop_name((JceSeqPropId)p);
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_TRUE(name[0] != '\0');
        TEST_ASSERT_EQUAL_INT(p, (int)jce_seq_prop_from_name(name));
    }
    TEST_ASSERT_EQUAL_INT(JCE_SEQ_PROP_NONE, jce_seq_prop_from_name(""));
    TEST_ASSERT_EQUAL_INT(JCE_SEQ_PROP_NONE, jce_seq_prop_from_name(NULL));
    TEST_ASSERT_EQUAL_INT(JCE_SEQ_PROP_NONE, jce_seq_prop_from_name("no.such.prop"));

    TEST_ASSERT_TRUE (jce_seq_prop_is_color(JCE_SEQ_PROP_LIGHT_COLOR));
    TEST_ASSERT_TRUE (jce_seq_prop_is_color(JCE_SEQ_PROP_MESH_BASE_COLOR));
    TEST_ASSERT_FALSE(jce_seq_prop_is_color(JCE_SEQ_PROP_POS_X));
    TEST_ASSERT_FALSE(jce_seq_prop_is_color(JCE_SEQ_PROP_CAMERA_FOV));
}

static void test_prop_apply_and_read_back(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Animated");

    /* Transform position (always present on created entities). */
    TEST_ASSERT_TRUE(jce_seq_prop_supported(s, e, JCE_SEQ_PROP_POS_Y));
    jce_seq_prop_apply_float(s, e, JCE_SEQ_PROP_POS_Y, 4.5f);
    float v = 0.0f;
    TEST_ASSERT_TRUE(jce_seq_prop_get_float(s, e, JCE_SEQ_PROP_POS_Y, &v));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 4.5f, v);

    /* Uniform scale fans out to all three axes. */
    jce_seq_prop_apply_float(s, e, JCE_SEQ_PROP_SCALE_UNIFORM, 2.0f);
    JceTransform *t = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, t->scale.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, t->scale.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, t->scale.z);

    /* Light props gated on the component being present; dir-light first. */
    TEST_ASSERT_FALSE(jce_seq_prop_supported(s, e, JCE_SEQ_PROP_LIGHT_INTENSITY));
    JceDirectionalLight dl;
    memset(&dl, 0, sizeof dl);
    dl.intensity = 1.0f;
    jce_scene_set_dir_light(s, e, &dl);
    TEST_ASSERT_TRUE(jce_seq_prop_supported(s, e, JCE_SEQ_PROP_LIGHT_INTENSITY));
    jce_seq_prop_apply_float(s, e, JCE_SEQ_PROP_LIGHT_INTENSITY, 3.25f);
    TEST_ASSERT_TRUE(jce_seq_prop_get_float(s, e, JCE_SEQ_PROP_LIGHT_INTENSITY, &v));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.25f, v);

    /* Color prop on the same light. */
    const float rgb_in[3] = { 0.25f, 0.5f, 0.75f };
    jce_seq_prop_apply_color(s, e, JCE_SEQ_PROP_LIGHT_COLOR, rgb_in);
    float rgb_out[3] = { 0 };
    TEST_ASSERT_TRUE(jce_seq_prop_get_color(s, e, JCE_SEQ_PROP_LIGHT_COLOR, rgb_out));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, rgb_out[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f,  rgb_out[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.75f, rgb_out[2]);

    jce_scene_destroy(s);
}

/* ── Component JSON round-trip + bindings remap ─────────────────── */

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

static void test_sequence_player_round_trip_and_remap(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceEntity holder = jce_scene_create_entity(src, "Holder");
    JceEntity target = jce_scene_create_entity(src, "Target");

    JceSequencePlayerComponent sp;
    memset(&sp, 0, sizeof sp);
    strcpy(sp.seq_path, "sequences/intro.seq.json");
    sp.play_on_awake = true;
    sp.override_loop = true;
    sp.loop_override = false;
    sp.speed         = 1.5f;
    sp.binding_count = 2;
    sp.bindings[0]   = (uint64_t)target;
    sp.bindings[1]   = (uint64_t)0xDEADBEEFu;  /* dangling → must clear to 0 */
    jce_scene_set_sequence_player(src, holder, &sp);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(2, loaded);
    jce_json_free(root);

    JceEntity new_holder = find_by_name(dst, "Holder");
    JceEntity new_target = find_by_name(dst, "Target");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, new_holder);
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, new_target);

    TEST_ASSERT_TRUE(jce_scene_has_sequence_player(dst, new_holder));
    JceSequencePlayerComponent *out =
        jce_scene_get_sequence_player(dst, new_holder);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("sequences/intro.seq.json", out->seq_path);
    TEST_ASSERT_TRUE(out->play_on_awake);
    TEST_ASSERT_TRUE(out->override_loop);
    TEST_ASSERT_FALSE(out->loop_override);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.5f, out->speed);
    TEST_ASSERT_EQUAL_INT(2, out->binding_count);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)new_target, out->bindings[0]);
    TEST_ASSERT_EQUAL_UINT64(0, out->bindings[1]);   /* unresolvable → 0 */
    /* Runtime fields must come back zeroed. */
    TEST_ASSERT_NULL(out->seq);
    TEST_ASSERT_FALSE(out->started);
    TEST_ASSERT_EQUAL_UINT64(0, out->opened_hash);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* ── Sequencer track-binding parse (structured + legacy) ────────── */

static void test_sequencer_structured_and_legacy_bindings(void)
{
    static const char *json =
        "{\"duration\":2.0,\"fps\":30,\"loop\":true,\"tracks\":["
        "{\"name\":\"a\",\"type\":0,"
         "\"bindProp\":\"transform.position.x\",\"bindEntity\":7,"
         "\"bindEntityName\":\"Cube\","
         "\"keys\":[{\"t\":0,\"v\":0},{\"t\":2,\"v\":10}]},"
        "{\"name\":\"b\",\"type\":0,\"binding\":\"42/camera.fov\","
         "\"keys\":[{\"t\":0,\"v\":60}]},"
        "{\"name\":\"c\",\"type\":0,\"binding\":\"not/a structured binding\","
         "\"keys\":[]}"
        "]}";

    JceSequencer *seq = jce_sequencer_load_text(json, strlen(json));
    TEST_ASSERT_NOT_NULL(seq);
    TEST_ASSERT_EQUAL_INT(3, jce_sequencer_track_count(seq));

    /* Additive structured keys win. */
    TEST_ASSERT_EQUAL_STRING("transform.position.x",
                             jce_sequencer_track_bind_prop_name(seq, 0));
    TEST_ASSERT_EQUAL_UINT64(7, jce_sequencer_track_bind_entity_hint(seq, 0));
    TEST_ASSERT_EQUAL_STRING("Cube",
                             jce_sequencer_track_bind_entity_name(seq, 0));

    /* Legacy "<digits>/<prop>" fallback parse. */
    TEST_ASSERT_EQUAL_STRING("camera.fov",
                             jce_sequencer_track_bind_prop_name(seq, 1));
    TEST_ASSERT_EQUAL_UINT64(42, jce_sequencer_track_bind_entity_hint(seq, 1));

    /* Non-conforming legacy strings are rejected gracefully. */
    TEST_ASSERT_EQUAL_STRING("", jce_sequencer_track_bind_prop_name(seq, 2));
    TEST_ASSERT_EQUAL_UINT64(0, jce_sequencer_track_bind_entity_hint(seq, 2));

    /* Mid-track evaluation still works through the public evaluator. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5.0f,
                             jce_sequencer_track_eval_float(seq, 0, 1.0f));

    jce_sequencer_free(seq);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_prop_catalogue_name_round_trip);
    RUN_TEST(test_prop_apply_and_read_back);
    RUN_TEST(test_sequence_player_round_trip_and_remap);
    RUN_TEST(test_sequencer_structured_and_legacy_bindings);
    return UNITY_END();
}
