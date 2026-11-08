/* Reuse the complete light/grass owners and their typed CPU recording ports. */
#define main colour_seed_unused_grass_main
#define setUp colour_seed_grass_set_up
#define tearDown colour_seed_grass_tear_down
#include "test_jce_grass_colour_binding.c"
#undef tearDown
#undef setUp
#undef main

/* Sampling providers are idle in these frame-anchored cases. The actual GI
 * owner still selects and packs the baked/dynamic coefficient arrays. */
static float unused_dynamic_sample(const JceGiProbes *gi, jce_vec3 position,
    float out[9][3])
{ (void)gi; (void)position; (void)out; TEST_FAIL_MESSAGE("Unexpected spatial sample"); return 0.0f; }
static float unused_baked_sample(const JceSrProbeSet *probes, jce_vec3 position,
    float out[9][3])
{ (void)probes; (void)position; (void)out; TEST_FAIL_MESSAGE("Unexpected spatial sample"); return 0.0f; }
#define jce_gi_probes_sample_sh9 unused_dynamic_sample
#define sr_probe_set_sample unused_baked_sample
#include JCE_TEST_COLOUR_PASS_GI_OWNER

void setUp(void)
{
    colour_seed_grass_set_up();
    renderer.tod_active = true;
    renderer.tod_state.ambient_color = jce_v3(.11f, .22f, .33f);
    renderer.u_gi_params = uniform_handle("u_giParams", BGFX_UNIFORM_TYPE_VEC4, 1u);
    renderer.u_sh9 = uniform_handle("u_sh9", BGFX_UNIFORM_TYPE_VEC4, 9u);
    renderer.u_gi_probe_box = uniform_handle("u_giProbeBox", BGFX_UNIFORM_TYPE_VEC4, 2u);
    renderer.gi_probe_active = false;
    renderer.gi_sh9_active = true;
    for (unsigned i = 0; i < 9u; ++i)
        for (unsigned c = 0; c < 3u; ++c) {
            renderer.gi_sh9[i][c] = (float)(i * 3u + c + 1u) * .01f;
            renderer.gi_dyn_sh9[i][c] = (float)(i * 3u + c + 1u) * .02f;
        }
    clear_writes();
}
void tearDown(void) { colour_seed_grass_tear_down(); }

static void prepare_current_pass(void)
{
    JceSceneRenderer *sr = &renderer;
    /* Exact CPU light-tail statements inside sr_draw_entities' existing
     * non-null light_env branch, followed by its actual GI preparation slice. */
#include JCE_TEST_COLOUR_PASS_LIGHT_PREPARE
#include JCE_TEST_COLOUR_PASS_GI_PREPARE
}
static void assert_prepared_cpu_inputs(void)
{
    TEST_ASSERT_EQUAL_FLOAT(.11f, light_env.ambient_color.x);
    TEST_ASSERT_EQUAL_FLOAT(.22f, light_env.ambient_color.y);
    TEST_ASSERT_EQUAL_FLOAT(.33f, light_env.ambient_color.z);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, light_env.ambient_intensity);
    TEST_ASSERT_EQUAL_FLOAT(camera_position.x, light_env.camera_pos.x);
    TEST_ASSERT_EQUAL_FLOAT(camera_position.y, light_env.camera_pos.y);
    TEST_ASSERT_EQUAL_FLOAT(camera_position.z, light_env.camera_pos.z);
}
static void assert_gi_once(bool dynamic)
{
    const float expected[4] = {dynamic ? 0.0f : 1.0f, 1.0f, dynamic ? 1.0f : 0.0f, 0.0f};
    UniformRecord *params = named(uniforms, "u_giParams");
    TEST_ASSERT_EQUAL_UINT(1u, params->writes);
    TEST_ASSERT_EQUAL_FLOAT_ARRAY(expected, params->value, 4u);
    UniformRecord *sh = named(uniforms, "u_sh9");
    TEST_ASSERT_EQUAL_UINT(1u, sh->writes); TEST_ASSERT_EQUAL_UINT16(9u, sh->count);
    for (unsigned i = 0; i < 9u; ++i) {
        for (unsigned c = 0; c < 3u; ++c)
            TEST_ASSERT_EQUAL_FLOAT(dynamic ? renderer.gi_dyn_sh9[i][c] : renderer.gi_sh9[i][c],
                sh->value[i * 4u + c]);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, sh->value[i * 4u + 3u]);
    }
}
static void preparation_keeps_current_cpu_inputs_without_pending_draw_writes(void)
{
    prepare_current_pass();
    assert_prepared_cpu_inputs();
    assert_no_pending_writes();
    TEST_ASSERT_EQUAL_UINT(0u, draw_count);
}
static void first_ordinary_light_and_gi_binders_publish_current_inputs_once(void)
{
    prepare_current_pass();
    /* These are the actual light/GI owners called by the ordinary queue,
     * inline, model, crowd and buffer-backed model pre-submit funnels. */
    jce_light_env_apply_for_layer(&light_env, NULL, 5u);
    sr_upload_gi_uniforms(&renderer);
    assert_prepared_cpu_inputs(); assert_gi_once(false);
    const float pos[4] = {4.0f, 8.0f, -12.0f, 0.0f};
    TEST_ASSERT_EQUAL_UINT(1u, named(uniforms, "u_cameraPos")->writes);
    TEST_ASSERT_EQUAL_FLOAT_ARRAY(pos, named(uniforms, "u_cameraPos")->value, 4u);
    TEST_ASSERT_EQUAL_UINT(1u, named(uniforms, "u_ambientColor")->writes);
    TEST_ASSERT_EQUAL_FLOAT(.11f, named(uniforms, "u_ambientColor")->value[0]);
    TEST_ASSERT_EQUAL_UINT(1u, named(uniforms, "u_dirLights")->writes);
    TEST_ASSERT_EQUAL_UINT(1u, named(uniforms, "u_lightCounts")->writes);
}
static void first_admitted_grass_draw_has_no_incoming_light_seed(void)
{
    prepare_current_pass();
    TEST_ASSERT_EQUAL_UINT32(2u, submit_range(1u, 2u, 5u).accepted);
    assert_complete_draw(0u, 1u, 2u, 9);
    TEST_ASSERT_EQUAL_UINT(0u, draws[0].uniform[renderer.u_gi_params.idx].writes);
    assert_no_pending_writes();
}
static void rejected_grass_leaves_next_admitted_producer_clean(void)
{
    prepare_current_pass(); capacity = 0u;
    TEST_ASSERT_EQUAL_UINT32(0u, submit_range(0u, 2u, 5u).accepted);
    assert_no_pending_writes(); TEST_ASSERT_EQUAL_UINT(0u, draw_count);
    capacity = ROOT_CAP;
    TEST_ASSERT_EQUAL_UINT32(2u, submit_range(2u, 2u, 5u).accepted);
    assert_complete_draw(0u, 2u, 2u, 9); assert_no_pending_writes();
}
static void dynamic_and_disabled_gi_remain_owned_by_the_admitted_binder(void)
{
    renderer.gi_sh9_active = false; renderer.gi_dyn_active = true;
    prepare_current_pass(); assert_no_pending_writes();
    sr_upload_gi_uniforms(&renderer); assert_gi_once(true);
    clear_writes(); renderer.gi_dyn_active = false;
    prepare_current_pass(); assert_no_pending_writes();
    sr_upload_gi_uniforms(&renderer);
    const float inactive[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    TEST_ASSERT_EQUAL_UINT(1u, named(uniforms, "u_giParams")->writes);
    TEST_ASSERT_EQUAL_FLOAT_ARRAY(inactive, named(uniforms, "u_giParams")->value, 4u);
    TEST_ASSERT_EQUAL_UINT(0u, named(uniforms, "u_sh9")->writes);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(preparation_keeps_current_cpu_inputs_without_pending_draw_writes);
    RUN_TEST(first_ordinary_light_and_gi_binders_publish_current_inputs_once);
    RUN_TEST(first_admitted_grass_draw_has_no_incoming_light_seed);
    RUN_TEST(rejected_grass_leaves_next_admitted_producer_clean);
    RUN_TEST(dynamic_and_disabled_gi_remain_owned_by_the_admitted_binder);
    return UNITY_END();
}
