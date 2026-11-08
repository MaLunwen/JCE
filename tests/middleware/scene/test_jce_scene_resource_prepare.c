/* Exact production prepare, async upload polls and legacy admission prefix.
 * Uses the real private renderer layout; typed CPU ports count resource work.
 * No renderer backend, colours, compute or GPU completion is certified. */
#include "unity.h"
#include "middleware/scene/jce_sr_internal.h"
#include "middleware/scene/jce_sr_model_diagnostics.h"
#include "middleware/scene/jce_scene_video_internal.h"
#include "renderer/jce_renderer_lifecycle_internal.h"
#include "os/core/jce_trace_scope_internal.h"
#include <string.h>

enum { MODEL_PREPARE_FIXTURE_CAPACITY = 8u };

static JceSceneRenderer *sr;
static JceScene *scene, *other_scene;
static uint32_t frame;
static uint64_t context_generation;
static bool admission, facts_available, video_pending;
static unsigned video_prepares, texture_uploads, model_uploads, releases;
static unsigned streaming_on, streaming_off, failures, cpu_frees;
static struct TaskPort { bool terminal; JceAsyncState state; } tasks[MODEL_PREPARE_FIXTURE_CAPACITY];
static unsigned cpu_tokens[MODEL_PREPARE_FIXTURE_CAPACITY];
static unsigned model_tokens[MODEL_PREPARE_FIXTURE_CAPACITY];
static unsigned model_token_next;

bool jce_renderer_frame_admission_current_internal(const JceRenderer *r)
{ TEST_ASSERT_NOT_NULL(r); return admission; }
bool jce_renderer_get_submitted_payload_stats(const JceRenderer *r, JceSubmittedPayloadStats *out)
{
    TEST_ASSERT_NOT_NULL(r); memset(out, 0, sizeof(*out));
    if (!facts_available) return false;
    out->context_generation = context_generation; out->frame_index = frame;
    out->valid = context_generation != 0; return out->valid;
}
uint32_t jce_renderer_get_frame_index(const JceRenderer *r) { (void)r; return frame; }
static struct TaskPort *task_port(const JceAsyncTask *task)
{
    for (unsigned i = 0; i < MODEL_PREPARE_FIXTURE_CAPACITY; ++i)
        if (task == (const JceAsyncTask *)&tasks[i]) return &tasks[i];
    TEST_FAIL_MESSAGE("Unknown typed task token"); return &tasks[0];
}
bool jce_async_task_is_terminal(const JceAsyncTask *task) { return task_port(task)->terminal; }
JceAsyncState jce_async_task_state(const JceAsyncTask *task) { return task_port(task)->state; }
void jce_async_task_release(JceAsyncTask *task) { (void)task_port(task); ++releases; }
JceTexture jce_texture_upload_cpu(JceTextureCpu *cpu)
{ TEST_ASSERT_NOT_NULL(cpu); ++texture_uploads; JceTexture t = {(uint16_t)texture_uploads}; return t; }
void jce_texture_cpu_free(JceTextureCpu *cpu) { TEST_ASSERT_NOT_NULL(cpu); ++cpu_frees; }
JceModel *jce_model_upload_gltf_cpu(JceModelCpu *cpu)
{ TEST_ASSERT_NOT_NULL(cpu); ++model_uploads; return (JceModel *)&model_tokens[model_token_next++]; }
void jce_model_gltf_cpu_free(JceModelCpu *cpu) { TEST_ASSERT_NOT_NULL(cpu); ++cpu_frees; }
uint64_t jce_model_gpu_bytes(const JceModel *model) { TEST_ASSERT_NOT_NULL(model); return 64; }
void jce_texture_set_streaming_uploads(bool on) { if (on) ++streaming_on; else ++streaming_off; }
bool jce_scene_video_prepare_pending(JceScene *s) { TEST_ASSERT_NOT_NULL(s); return video_pending; }
void jce_scene_video_prepare_render(JceScene *s)
{ TEST_ASSERT_NOT_NULL(s); ++video_prepares; video_pending = false; }
void sr_model_cache_failure_hash(JceSceneRenderer *owner, uint32_t path_hash, uint32_t entity,
    JceSrModelFailure reason)
{ (void)path_hash; (void)entity; (void)reason; TEST_ASSERT_EQUAL_PTR(sr, owner); ++failures; }

static unsigned diagnostic_binds;
void sr_model_cache_diagnostics_bind(JceSceneRenderer *owner, const void *bound_scene,
    uint64_t context, uint32_t backend_frame)
{
    TEST_ASSERT_EQUAL_PTR(sr, owner);
    TEST_ASSERT_NOT_NULL(bound_scene);
    TEST_ASSERT_EQUAL_UINT64(context_generation, context);
    TEST_ASSERT_EQUAL_UINT32(frame, backend_frame);
    ++diagnostic_binds;
}

#include JCE_SRP_OWNER
#include JCE_SRP_RENDER_PREFIX
#include JCE_SRP_RESET_PREFIX

void setUp(void)
{
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0);
    sr = JCE_CALLOC(1, sizeof(*sr)); TEST_ASSERT_NOT_NULL(sr);
    sr->model_cache = JCE_CALLOC(MODEL_PREPARE_FIXTURE_CAPACITY,
        sizeof(*sr->model_cache));
    TEST_ASSERT_NOT_NULL(sr->model_cache);
    sr->model_cache_capacity = MODEL_PREPARE_FIXTURE_CAPACITY;
    sr->renderer = (JceRenderer *)&context_generation; sr->model_frame = 17;
    scene = jce_scene_create(); other_scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene); TEST_ASSERT_NOT_NULL(other_scene);
    frame = 9; context_generation = 1; admission = facts_available = true; video_pending = false;
    video_prepares = texture_uploads = model_uploads = releases = streaming_on = streaming_off = 0;
    diagnostic_binds = 0;
    failures = cpu_frees = model_token_next = 0; memset(tasks, 0, sizeof(tasks));
}
void tearDown(void)
{
    if (sr) {
        for (int i = 0; i < sr->tex_cache_count; ++i) if (sr->tex_cache[i].job)
            sr_tex_job_destroy(sr->tex_cache[i].job);
        if (sr->model_cache) for (uint32_t i = 0; i < sr->model_cache_capacity; ++i) {
            SrModelCache *entry = sr->model_cache[i];
            if (!entry) continue;
            if (entry->task) jce_async_task_release(entry->task);
            sr_model_job_destroy(entry->job);
            /* Uploaded model tokens are fixture values, without GPU ownership. */
            JCE_FREE(entry);
        }
        JCE_FREE(sr->model_cache);
        JCE_FREE(sr); sr = NULL;
    }
    jce_scene_destroy(scene); scene = NULL;
    jce_scene_destroy(other_scene); other_scene = NULL;
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0);
}
static void texture_job(unsigned index, bool terminal)
{
    struct SrTexJob *job = JCE_CALLOC(1, sizeof(*job)); TEST_ASSERT_NOT_NULL(job);
    job->result = (JceTextureCpu *)&cpu_tokens[index];
    sr->tex_cache[index].job = job; sr->tex_cache[index].task = (JceAsyncTask *)&tasks[index];
    sr->tex_cache[index].pending = true; sr->tex_cache_count = (int)index + 1; ++sr->tex_inflight;
    tasks[index].terminal = terminal; tasks[index].state = JCE_ASYNC_STATE_SUCCEEDED;
}
static void model_job(unsigned index, bool terminal)
{
    TEST_ASSERT_LESS_THAN_UINT32(sr->model_cache_capacity, index);
    TEST_ASSERT_LESS_THAN_UINT32(MODEL_PREPARE_FIXTURE_CAPACITY, index);
    TEST_ASSERT_NULL(sr->model_cache[index]);
    sr->model_cache[index] = JCE_CALLOC(1, sizeof(*sr->model_cache[index]));
    TEST_ASSERT_NOT_NULL(sr->model_cache[index]);
    SrModelCache *entry = sr->model_cache[index];
    entry->job = JCE_CALLOC(1, sizeof(*entry->job));
    TEST_ASSERT_NOT_NULL(entry->job);
    entry->job->cpu = (JceModelCpu *)&cpu_tokens[index];
    entry->task = (JceAsyncTask *)&tasks[index];
    entry->pending = entry->used = true;
    ++sr->model_cache_used; ++sr->model_inflight;
    tasks[index].terminal = terminal; tasks[index].state = JCE_ASYNC_STATE_SUCCEEDED;
}
static void test_invalid_or_missing_device_admission_does_not_prepare(void)
{
    TEST_ASSERT_FALSE(jce_scene_renderer_prepare_resources(NULL, scene));
    TEST_ASSERT_FALSE(jce_scene_renderer_prepare_resources(sr, NULL));
    admission = false; TEST_ASSERT_FALSE(jce_scene_renderer_prepare_resources(sr, scene));
    admission = true; facts_available = false;
    TEST_ASSERT_FALSE(jce_scene_renderer_prepare_resources(sr, scene));
    facts_available = true; context_generation = 0;
    TEST_ASSERT_FALSE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(0, video_prepares); TEST_ASSERT_EQUAL_UINT32(0, streaming_on);
    TEST_ASSERT_FALSE(sr->resource_prepared_valid);
    TEST_ASSERT_EQUAL_UINT32(0, diagnostic_binds);
}
static void test_actual_polls_consume_completed_resources_without_advancing_clock(void)
{
    texture_job(0, true); model_job(1, true);
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(1, texture_uploads); TEST_ASSERT_EQUAL_UINT32(1, model_uploads);
    TEST_ASSERT_EQUAL_UINT32(2, releases); TEST_ASSERT_EQUAL_UINT32(1, streaming_on);
    TEST_ASSERT_EQUAL_UINT32(1, streaming_off); TEST_ASSERT_EQUAL_UINT64(17, sr->model_frame);
    TEST_ASSERT_EQUAL_UINT64(17, sr->model_cache[1]->last_used_frame);
    TEST_ASSERT_EQUAL_UINT32(0, sr->tex_inflight); TEST_ASSERT_EQUAL_UINT32(0, sr->model_inflight);
}
static void test_unallocated_model_table_keeps_empty_preparation_valid(void)
{
    JCE_FREE(sr->model_cache); sr->model_cache = NULL;
    sr->model_cache_capacity = 0;
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(1, video_prepares);
    TEST_ASSERT_EQUAL_UINT32(0, model_uploads);
    TEST_ASSERT_EQUAL_UINT32(0, sr->model_inflight);
}

static void test_sparse_model_bucket_consumes_terminal_arrival_in_same_round(void)
{
    const unsigned index = MODEL_PREPARE_FIXTURE_CAPACITY - 1u;
    model_job(index, false);
    TEST_ASSERT_NULL(sr->model_cache[0]);
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(0, model_uploads);
    tasks[index].terminal = true;
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(1, model_uploads);
    TEST_ASSERT_EQUAL_UINT32(2, video_prepares);
    TEST_ASSERT_EQUAL_UINT32(1, releases);
    TEST_ASSERT_EQUAL_UINT64(17, sr->model_cache[index]->last_used_frame);
    TEST_ASSERT_EQUAL_UINT32(0, sr->model_inflight);
}

static void test_same_round_and_multiple_views_reuse_preparation(void)
{
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(1, video_prepares); TEST_ASSERT_EQUAL_UINT32(1, streaming_on);
    TEST_ASSERT_EQUAL_UINT64(17, sr->model_frame);
    TEST_ASSERT_EQUAL_UINT32(3, diagnostic_binds);
}
static void test_new_terminal_texture_in_same_round_invalidates_once(void)
{
    texture_job(0, false); TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(0, texture_uploads);
    tasks[0].terminal = true; TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(1, texture_uploads); TEST_ASSERT_EQUAL_UINT32(2, video_prepares);
}
static void test_budget_blocked_model_work_does_not_repeat_and_new_round_consumes(void)
{
    model_job(0, true); model_job(1, true);
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(1, model_uploads); TEST_ASSERT_EQUAL_UINT32(1, video_prepares);
    model_job(2, false); tasks[2].terminal = true;
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(1, model_uploads); TEST_ASSERT_EQUAL_UINT32(2, video_prepares);
    ++frame; TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(2, model_uploads); TEST_ASSERT_EQUAL_UINT32(1, sr->model_inflight);
}
static void test_scene_a_b_a_does_not_reuse_the_other_scene(void)
{
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, other_scene));
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(3, video_prepares); TEST_ASSERT_EQUAL_PTR(scene, sr->resource_prepared_scene);
}
static void test_cache_generation_and_real_reset_prefix_invalidate_same_round(void)
{
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    ++sr->model_cache_generation; TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    jce_srp_test_reset_prefix(sr); TEST_ASSERT_FALSE(sr->resource_prepared_valid);
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(3, video_prepares);
}
static void test_context_change_rearms_model_budget_without_claiming_resource_recovery(void)
{
    model_job(0, true); model_job(1, true);
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    ++context_generation; TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(2, model_uploads);
    TEST_ASSERT_EQUAL_UINT64(2, sr->resource_prepared_context);
}
static void test_round_rewind_and_rejected_restore_cannot_reuse_stale_key(void)
{
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    --frame; TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    admission = false; TEST_ASSERT_FALSE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_FALSE(sr->resource_prepared_valid);
    admission = true; TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(3, video_prepares);
}
static JceModel *unused_callback(const char *path, void *ud) { (void)path; (void)ud; return NULL; }
static void test_callback_policy_keeps_existing_streaming_selection(void)
{
    sr->has_cbs = true; sr->cbs.load_model = unused_callback;
    texture_job(0, true); TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(1, texture_uploads); TEST_ASSERT_EQUAL_UINT32(0, streaming_on);
    TEST_ASSERT_EQUAL_UINT32(0, streaming_off);
}
static void test_new_video_publication_in_same_round_is_consumed(void)
{
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene)); video_pending = true;
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT32(2, video_prepares); TEST_ASSERT_FALSE(video_pending);
}
static void test_actual_legacy_render_prefix_calls_prepare_and_keeps_reference_clock(void)
{
    texture_job(0, true);
    TEST_ASSERT_EQUAL_UINT16(11, jce_srp_test_render_prefix(sr, scene, NULL, 11, 0.25f, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, texture_uploads); TEST_ASSERT_EQUAL_UINT64(18, sr->model_frame);
    TEST_ASSERT_EQUAL_UINT16(12, jce_srp_test_render_prefix(sr, scene, NULL, 12, 0.25f, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, video_prepares); TEST_ASSERT_EQUAL_UINT64(19, sr->model_frame);
}
static void test_trace_uses_existing_authority_with_round_identity_and_reuse(void)
{
    jce_trace_reset(); jce_trace_set_enabled(true); jce_trace_task_restore_id(91);
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_TRUE(jce_scene_renderer_prepare_resources(sr, scene));
    TEST_ASSERT_EQUAL_UINT64(91, jce_trace_task_current_id());
    JceTraceEvent events[64]; JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    uint32_t count = jce_trace_read(&cursor, events, 64); unsigned spans = 0, reused = 0, clocks = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].type == JCE_TRACE_EVENT_TASK_SUBMIT &&
            strcmp(events[i].name, "scene.resources.prepare") == 0) {
            ++spans; TEST_ASSERT_EQUAL_UINT64(91, events[i].parent_id);
        }
        if (events[i].type == JCE_TRACE_EVENT_COUNTER) {
            reused += strcmp(events[i].name, "scene.resources.reused") == 0 && events[i].value_u64 == 1;
            clocks += strcmp(events[i].name, "scene.resources.backend_cpu_frame") == 0 && events[i].value_u64 == 9;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(2, spans); TEST_ASSERT_EQUAL_UINT32(1, reused); TEST_ASSERT_EQUAL_UINT32(2, clocks);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_invalid_or_missing_device_admission_does_not_prepare);
    RUN_TEST(test_actual_polls_consume_completed_resources_without_advancing_clock);
    RUN_TEST(test_unallocated_model_table_keeps_empty_preparation_valid);
    RUN_TEST(test_sparse_model_bucket_consumes_terminal_arrival_in_same_round);
    RUN_TEST(test_same_round_and_multiple_views_reuse_preparation);
    RUN_TEST(test_new_terminal_texture_in_same_round_invalidates_once);
    RUN_TEST(test_budget_blocked_model_work_does_not_repeat_and_new_round_consumes);
    RUN_TEST(test_scene_a_b_a_does_not_reuse_the_other_scene);
    RUN_TEST(test_cache_generation_and_real_reset_prefix_invalidate_same_round);
    RUN_TEST(test_context_change_rearms_model_budget_without_claiming_resource_recovery);
    RUN_TEST(test_round_rewind_and_rejected_restore_cannot_reuse_stale_key);
    RUN_TEST(test_callback_policy_keeps_existing_streaming_selection);
    RUN_TEST(test_new_video_publication_in_same_round_is_consumed);
    RUN_TEST(test_actual_legacy_render_prefix_calls_prepare_and_keeps_reference_clock);
    RUN_TEST(test_trace_uses_existing_authority_with_round_identity_and_reuse);
    return UNITY_END();
}
