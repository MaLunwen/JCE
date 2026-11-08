/* Complete production animation owner + real Scene and L5 clip/player math.
 * Model/IO/thread/GPU-morph ports are typed, counted boundaries. No backend. */
#include "unity.h"
#include "middleware/scene/jce_sr_internal.h"
#include "middleware/animation/jce_animation.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_trace.h>
#include <string.h>

static JceSceneRenderer *sr;
static JceScene *scene, *prior_scene;
static JceSkeleton *skeleton;
static JceAnimClip *clip;
static unsigned model_token;
static JceEntity actors[32];
static SrModelCache models[32];
static unsigned actor_count, model_requests, reallocations, morph_reads, deform_calls, io_reads;
static bool refuse_allocation;
static JceEntity event_entities[8];
static unsigned events_seen;
static bool add_actor_from_event, remove_actor_from_event;
static JceAnimEvent event_rows[2];

void *jce_mv_realloc(void *ptr, size_t bytes)
{
    ++reallocations;
    if (refuse_allocation) return NULL;
    return JCE_REALLOC(ptr, bytes);
}
SrModelCache *sr_get_model(JceSceneRenderer *owner, const char *path, uint32_t entity)
{
    TEST_ASSERT_EQUAL_PTR(sr, owner); TEST_ASSERT_NOT_NULL(path); ++model_requests;
    for (unsigned i = 0; i < actor_count; ++i)
        if ((uint32_t)actors[i] == entity) return &models[i];
    TEST_FAIL_MESSAGE("Unexpected model entity"); return NULL;
}
JceSkeleton *jce_model_get_skeleton(const JceModel *model)
{ TEST_ASSERT_EQUAL_PTR(&model_token, model); return skeleton; }
uint32_t jce_model_anim_count(const JceModel *model)
{ TEST_ASSERT_EQUAL_PTR(&model_token, model); return 1; }
JceAnimClip *jce_model_get_anim(const JceModel *model, uint32_t index)
{ TEST_ASSERT_EQUAL_PTR(&model_token, model); TEST_ASSERT_EQUAL_UINT32(0, index); return clip; }
JceJson *jce_json_parse_file(const char *path)
{ TEST_ASSERT_NOT_NULL(path); ++io_reads; return NULL; }
JceThreadPool *jce_thread_pool_shared(void) { return NULL; }
uint32_t sr_sample_morph_track(SrAnimInstance *instance, float *out, uint32_t cap)
{ TEST_ASSERT_NOT_NULL(instance); TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_TRUE(cap > 0); ++morph_reads; return 0; }
void sr_deform_morph_prims(JceSceneRenderer *owner, SrAnimInstance *instance)
{ (void)owner; (void)instance; ++deform_calls; TEST_FAIL_MESSAGE("Zero-morph fixture must not submit deform work"); }

static JceEntity add_actor(const char *name)
{
    TEST_ASSERT_TRUE(actor_count < 32);
    JceEntity entity = jce_scene_create_entity(scene, name);
    JceSkeletalAnimatorComponent animator = {0};
    strcpy(animator.skeleton_path, "memory/rig.glb");
    animator.playing = true; animator.loop = true; animator.speed = 1;
    jce_scene_set_skeletal_animator(scene, entity, &animator);
    actors[actor_count] = entity;
    models[actor_count].used = true; models[actor_count].model = (JceModel *)&model_token;
    ++actor_count; return entity;
}
static EntityList view(JceEntity *entities, int count)
{ EntityList list = {entities, count, count}; return list; }
static void tick(EntityList *list, float dt) { sr_update_skinned_anims(sr, scene, list, dt, NULL); }
static float phase(JceEntity entity)
{
    const JceSkeletalAnimatorComponent *animator = jce_scene_get_skeletal_animator(scene, entity);
    TEST_ASSERT_NOT_NULL(animator); return animator->norm_time;
}
static void event_sink(uint64_t entity, const JceAnimEvent *event, void *user)
{
    (void)event; (void)user; TEST_ASSERT_TRUE(events_seen < 8);
    event_entities[events_seen++] = (JceEntity)entity;
    if (entity == actors[0]) {
        if (add_actor_from_event && actor_count == 2) add_actor("callback actor");
        if (remove_actor_from_event) jce_scene_remove_skeletal_animator(scene, actors[1]);
    }
}
static void install_events(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        SrAnimInstance *instance = sr_find_anim_instance(sr, actors[i]); TEST_ASSERT_NOT_NULL(instance);
        event_rows[i].time = 0.15f; event_rows[i].id = i + 1;
        instance->ev_tracks[0].events = &event_rows[i]; instance->ev_tracks[0].count = 1;
        instance->ev_tracks[0].clip_duration = 1; instance->ev_track_count = 1;
    }
    sr->anim_event_fn = event_sink;
}
void setUp(void)
{
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0);
    sr = JCE_CALLOC(1, sizeof(*sr)); TEST_ASSERT_NOT_NULL(sr);
    prior_scene = NULL;
    scene = jce_scene_create(); TEST_ASSERT_NOT_NULL(scene);
    JceJoint joint = {0}; strcpy(joint.name, "root"); joint.parent = -1;
    joint.inverse_bind_matrix = joint.local_transform = jce_m4_identity();
    joint.rest_rotation = jce_q_identity(); joint.rest_scale = jce_v3(1, 1, 1);
    skeleton = jce_skeleton_create(&joint, 1); TEST_ASSERT_NOT_NULL(skeleton);
    static float times[2] = {0, 1}; static jce_vec3 positions[2];
    positions[0] = jce_v3(0, 0, 0); positions[1] = jce_v3(0, 0, 1);
    JceAnimChannel channel = {0}; channel.joint_index = 0;
    channel.target = JCE_ANIM_TARGET_TRANSLATION; channel.interpolation = JCE_INTERP_LINEAR;
    channel.timestamps = times; channel.count = 2; channel.translations = positions;
    clip = jce_anim_clip_create("walk", &channel, 1, 1); TEST_ASSERT_NOT_NULL(clip);
    actor_count = model_requests = reallocations = morph_reads = deform_calls = io_reads = events_seen = 0;
    refuse_allocation = add_actor_from_event = remove_actor_from_event = false;
    memset(models, 0, sizeof(models)); memset(event_rows, 0, sizeof(event_rows));
    add_actor("first view actor"); add_actor("second view actor");
    jce_scene_renderer_begin_velocity_frame(sr);
}
void tearDown(void)
{
    sr_anim_instances_destroy(sr); JCE_FREE(sr); sr = NULL;
    jce_scene_destroy(scene); scene = NULL;
    if (prior_scene) jce_scene_destroy(prior_scene); prior_scene = NULL;
    jce_anim_clip_destroy(clip); jce_skeleton_destroy(skeleton);
}
static void first_view_culled_actor_is_sampled_once_for_later_view(void)
{
    EntityList first = view(&actors[0], 1), second = view(&actors[1], 1);
    tick(&first, 0.1f); tick(&second, 0.1f);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.1f, phase(actors[0]));
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.1f, phase(actors[1]));
    TEST_ASSERT_EQUAL_UINT32(2, model_requests); TEST_ASSERT_EQUAL_UINT32(0, deform_calls);
    SrAnimInstance *second_pose = sr_find_anim_instance(sr, actors[1]); TEST_ASSERT_NOT_NULL(second_pose);
    TEST_ASSERT_EQUAL_UINT32(1, second_pose->skin_palette_count);
}
static void empty_first_view_still_updates_scene_actors(void)
{
    EntityList empty = view(NULL, 0); tick(&empty, 0.1f);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.1f, phase(actors[0]));
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.1f, phase(actors[1]));
}
static void generation_guard_prevents_duplicate_step_and_reuses_allocation(void)
{
    EntityList first = view(&actors[0], 1), second = view(&actors[1], 1);
    tick(&first, 0.1f); unsigned allocations = reallocations;
    tick(&second, 0.1f); jce_scene_renderer_begin_velocity_frame(sr); tick(&second, 0.1f);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.2f, phase(actors[0]));
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.2f, phase(actors[1]));
    TEST_ASSERT_EQUAL_UINT32(allocations, reallocations);
}
static void legacy_per_render_path_remains_without_velocity_driver(void)
{
    sr->velocity_frame_driven = false;
    EntityList first = view(&actors[0], 1), second = view(&actors[1], 1);
    tick(&first, 0.1f); tick(&second, 0.1f);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.2f, phase(actors[0]));
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.2f, phase(actors[1]));
}
static void disabled_animator_keeps_existing_gating(void)
{
    jce_scene_set_component_enabled(scene, actors[1], JCE_COMP_FLAG_SKELETAL_ANIMATOR, false);
    EntityList first = view(&actors[0], 1); tick(&first, 0.1f);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.1f, phase(actors[0]));
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0, phase(actors[1]));
    TEST_ASSERT_NULL(sr_find_anim_instance(sr, actors[1]));
}
static void oom_before_snapshot_does_not_stamp_and_same_round_retry_is_once(void)
{
    EntityList first = view(&actors[0], 1), second = view(&actors[1], 1);
    refuse_allocation = true; tick(&first, 0.1f);
    TEST_ASSERT_TRUE(sr->skin_anim_gen != sr->vel_frame_gen);
    TEST_ASSERT_EQUAL_UINT32(0, model_requests); TEST_ASSERT_EQUAL_UINT32(0, sr->anim_storage.count);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0, phase(actors[0]));
    refuse_allocation = false; tick(&second, 0.1f); tick(&first, 0.1f);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.1f, phase(actors[0]));
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.1f, phase(actors[1]));
}
static void scene_change_oom_does_not_reuse_prior_scene_acceptance(void)
{
    EntityList first = view(&actors[0], 1); tick(&first, 0.1f);
    const JceEntity prior_actor = actors[0];
    prior_scene = scene; scene = jce_scene_create(); TEST_ASSERT_NOT_NULL(scene);
    actor_count = 0; memset(models, 0, sizeof(models));
    while (actor_count < 20) add_actor("different scene actor");
    EntityList second_scene = view(&actors[19], 1);
    refuse_allocation = true; tick(&second_scene, 0.1f);
    TEST_ASSERT_EQUAL_PTR(scene, sr->anim_storage_scene);
    TEST_ASSERT_TRUE(sr->skin_anim_gen != sr->vel_frame_gen);
    TEST_ASSERT_EQUAL_UINT32(0, sr->anim_storage.count);
    TEST_ASSERT_EQUAL_UINT32(2, model_requests);
    refuse_allocation = false; tick(&second_scene, 0.1f); tick(&first, 0.1f);
    TEST_ASSERT_EQUAL_UINT32(20, sr->anim_storage.count);
    TEST_ASSERT_EQUAL_UINT32(22, model_requests);
    for (unsigned i = 0; i < actor_count; ++i)
        TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.1f, phase(actors[i]));
    const JceSkeletalAnimatorComponent *prior = jce_scene_get_skeletal_animator(prior_scene, prior_actor);
    TEST_ASSERT_NOT_NULL(prior); TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.1f, prior->norm_time);
}
static void callback_addition_joins_next_snapshot_without_changing_serial_event_order(void)
{
    EntityList first = view(&actors[0], 1); tick(&first, 0.1f); install_events();
    add_actor_from_event = true; morph_reads = 0;
    jce_scene_renderer_begin_velocity_frame(sr); tick(&first, 0.1f);
    TEST_ASSERT_EQUAL_UINT32(3, actor_count); TEST_ASSERT_NULL(sr_find_anim_instance(sr, actors[2]));
    TEST_ASSERT_EQUAL_UINT32(2, morph_reads); TEST_ASSERT_EQUAL_UINT32(2, events_seen);
    TEST_ASSERT_EQUAL_UINT64(actors[0], event_entities[0]); TEST_ASSERT_EQUAL_UINT64(actors[1], event_entities[1]);
    tick(&first, 0.1f); TEST_ASSERT_EQUAL_UINT32(2, events_seen);
    jce_scene_renderer_begin_velocity_frame(sr); tick(&first, 0.1f);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.1f, phase(actors[2]));
}
static void callback_removal_keeps_existing_has_get_checks_and_prunes_next_generation(void)
{
    EntityList first = view(&actors[0], 1); tick(&first, 0.1f); install_events();
    remove_actor_from_event = true; morph_reads = 0;
    jce_scene_renderer_begin_velocity_frame(sr); tick(&first, 0.1f);
    TEST_ASSERT_NULL(jce_scene_get_skeletal_animator(scene, actors[1]));
    TEST_ASSERT_EQUAL_UINT32(1, morph_reads); TEST_ASSERT_EQUAL_UINT32(2, sr->anim_storage.count);
    jce_scene_renderer_begin_velocity_frame(sr); tick(&first, 0.1f);
    TEST_ASSERT_EQUAL_UINT32(1, sr->anim_storage.count);
}
static void snapshot_growth_keeps_entity_ids_outside_parallel_request_slots(void)
{
    EntityList first = view(&actors[0], 1); tick(&first, 0.1f);
    while (actor_count < 20) add_actor("growing actor");
    jce_scene_renderer_begin_velocity_frame(sr); tick(&first, 0.1f);
    TEST_ASSERT_EQUAL_UINT32(20, sr->anim_storage.count); TEST_ASSERT_TRUE(sr->anim_request_capacity >= 20);
    for (unsigned i = 0; i < actor_count; ++i)
        TEST_ASSERT_FLOAT_WITHIN(0.00001f, i < 2 ? 0.2f : 0.1f, phase(actors[i]));
    TEST_ASSERT_EQUAL_UINT32(0, deform_calls);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(first_view_culled_actor_is_sampled_once_for_later_view);
    RUN_TEST(empty_first_view_still_updates_scene_actors);
    RUN_TEST(generation_guard_prevents_duplicate_step_and_reuses_allocation);
    RUN_TEST(legacy_per_render_path_remains_without_velocity_driver);
    RUN_TEST(disabled_animator_keeps_existing_gating);
    RUN_TEST(oom_before_snapshot_does_not_stamp_and_same_round_retry_is_once);
    RUN_TEST(scene_change_oom_does_not_reuse_prior_scene_acceptance);
    RUN_TEST(callback_addition_joins_next_snapshot_without_changing_serial_event_order);
    RUN_TEST(callback_removal_keeps_existing_has_get_checks_and_prunes_next_generation);
    RUN_TEST(snapshot_growth_keeps_entity_ids_outside_parallel_request_slots);
    return UNITY_END();
}
