/* Compile the actual queue; control only capacity and submission boundaries. */
#include "unity.h"
#include <jce/renderer/jce_render_queue.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <bgfx/c99/bgfx.h>
#include <jce/os/core/jce_thread.h>
#include "renderer/jce_shader_deferred.h"
#include <string.h>
#include <stdlib.h>
#include "renderer/jce_instance_upload.h"
static unsigned available, allocated, submitted, instances, bound;
static unsigned char storage[4096], captured[4096], dynamic_data[8][4096];
static uint16_t dynamic_stride[8], next_handle;
static unsigned captured_bytes, index_binds, materials[128], material_count;
static uint64_t last_state;
static uint32_t last_stencil;
static bool fail_staging;
static JceInstanceUploadOwner test_owner;
static JceInstanceUploadOwner *owner_selected;
static uint16_t resolved(uint16_t program) { return program == 9 ? UINT16_MAX : program; }
static void submit(bgfx_view_id_t view, bgfx_program_handle_t program, uint32_t depth, uint8_t flags);
static void encoder_submit(bgfx_encoder_t *encoder, bgfx_view_id_t view,
    bgfx_program_handle_t program, uint32_t depth, uint8_t flags);
static void no_texture(uint8_t stage, bgfx_uniform_handle_t sampler,
    bgfx_texture_handle_t texture, uint32_t flags)
{ (void)stage; (void)sampler; (void)texture; (void)flags; TEST_FAIL_MESSAGE("queue fixture must not bind textures"); }
static void no_encoder_texture(bgfx_encoder_t *encoder, uint8_t stage,
    bgfx_uniform_handle_t sampler, bgfx_texture_handle_t texture, uint32_t flags)
{ (void)encoder; no_texture(stage, sampler, texture, flags); }
static void discard(uint8_t flags) { TEST_ASSERT_EQUAL_HEX8(BGFX_DISCARD_ALL, flags); }
static void encoder_discard(bgfx_encoder_t *encoder, uint8_t flags)
{ (void)encoder; discard(flags); }
/* The queue reaches the actual encoder's deferred resolve. */
#define jce_shader_deferred_resolve resolved
#define bgfx_submit submit
#define bgfx_encoder_submit encoder_submit
#define bgfx_set_texture no_texture
#define bgfx_encoder_set_texture no_encoder_texture
#define bgfx_discard discard
#define bgfx_encoder_discard encoder_discard
#include "renderer/jce_render_encoder.h"
#undef bgfx_encoder_discard
#undef bgfx_discard
#undef bgfx_encoder_set_texture
#undef bgfx_set_texture
#undef bgfx_encoder_submit
#undef bgfx_submit
#undef jce_shader_deferred_resolve
#if defined(_MSC_VER)
__declspec(thread) bgfx_encoder_t *jce_tls_encoder;
#else
__thread bgfx_encoder_t *jce_tls_encoder;
#endif
uint32_t jce_dbg_xform_matrices;
static unsigned char pool_token, encoder_token;
static bool pool_enabled, encoder_available, workers_only;
static uint64_t current_tid;
static unsigned parallel_calls, encoder_submits, encoder_ends;
static uint64_t JCE_CALL thread_id(void) { return current_tid; }
static JceThreadPool *shared_pool(void)
{ return pool_enabled ? (JceThreadPool *)&pool_token : NULL; }
static int JCE_CALL worker_count(const JceThreadPool *pool)
{ TEST_ASSERT_EQUAL_PTR(&pool_token, pool); return 3; }
static void JCE_CALL parallel_for(JceThreadPool *pool, const char *name,
    uint32_t count, uint32_t chunk, JceTaskRangeFn fn, void *arg);
static bgfx_encoder_t *begin_encoder(bool worker)
{ TEST_ASSERT_TRUE(worker); return encoder_available ? (bgfx_encoder_t *)&encoder_token : NULL; }
static void end_encoder(bgfx_encoder_t *encoder)
{ TEST_ASSERT_EQUAL_PTR(&encoder_token, encoder); ++encoder_ends; }
static const char *queue_getenv(const char *key)
{
    if (strcmp(key, "JCE_PARALLEL_SUBMIT") == 0) return "1";
    if (strcmp(key, "JCE_RQ_DISABLE_INSTANCING") == 0) return "0";
    return NULL;
}
static JceInstanceUploadOwner *owner_for(const JceRenderer *r) { (void)r; return owner_selected; }
static uint32_t upload_epoch(void) { return 1; }
static void record_bytes(const uint8_t *data, uint32_t count, uint16_t stride)
{
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(sizeof captured - captured_bytes, count * stride);
    memcpy(captured + captured_bytes, data, count * stride);
    captured_bytes += count * stride;
}
static void set_state(uint64_t value, uint32_t rgba) { (void)rgba; last_state = value; }
static void set_stencil(uint32_t value, uint32_t back) { (void)back; last_stencil = value; }
static void set_indices(bgfx_index_buffer_handle_t h, uint32_t first, uint32_t count)
{ (void)h; (void)first; (void)count; ++index_binds; }
static void material(uint32_t key, void *user)
{
    (void)user;
    TEST_ASSERT_LESS_THAN_UINT32(sizeof materials / sizeof materials[0], material_count);
    materials[material_count++] = key;
}

static uint32_t capacity(uint32_t count, uint16_t stride)
{
    (void)stride;
    return count < available ? count : available;
}
static void allocate(bgfx_instance_data_buffer_t *out, uint32_t count, uint16_t stride)
{
    memset(out, 0, sizeof *out);
    out->num = count < allocated ? count : allocated;
    out->stride = stride;
    out->size = out->num * stride;
    out->data = storage;
}
static void bind_instances(const bgfx_instance_data_buffer_t *data, uint32_t start, uint32_t count)
{
    bound = count;
    record_bytes(data->data + (size_t)start * data->stride, count, data->stride);
}
static void submit(bgfx_view_id_t view, bgfx_program_handle_t program, uint32_t depth, uint8_t flags)
{
    (void)view; (void)program; (void)depth; (void)flags;
    ++submitted; instances += bound; bound = 0;
}
static void encoder_submit(bgfx_encoder_t *encoder, bgfx_view_id_t view,
    bgfx_program_handle_t program, uint32_t depth, uint8_t flags)
{
    TEST_ASSERT_EQUAL_PTR(&encoder_token, encoder);
    ++encoder_submits;
    submit(view, program, depth, flags);
}
static uint32_t provider_available(void *user, uint32_t count, uint16_t stride)
{ (void)user; return capacity(count, stride); }
static bool provider_transient(void *user, uint32_t count, uint16_t stride, JceInstanceTransient *out)
{
    (void)user;
    out->data = storage; out->num = count < allocated ? count : allocated;
    out->stride = stride; out->size = out->num * stride;
    return out->num != 0;
}
static uint16_t provider_create(void *user, uint32_t count, uint16_t stride)
{
    (void)user;
    if (next_handle >= 8 || count * stride > sizeof dynamic_data[0]) return UINT16_MAX;
    dynamic_stride[next_handle] = stride;
    return next_handle++;
}
static void provider_destroy(void *user, uint16_t handle) { (void)user; (void)handle; }
static void *provider_alloc(void *user, uint32_t bytes) { (void)user; return fail_staging ? NULL : malloc(bytes); }
static void provider_free(void *user, void *memory) { (void)user; free(memory); }
static bool provider_update(void *user, uint16_t handle, uint32_t offset, void *memory, uint32_t bytes)
{
    (void)user;
    memcpy(dynamic_data[handle] + (size_t)offset * dynamic_stride[handle], memory, bytes);
    free(memory); return true;
}
static void provider_bind_dynamic(void *user, void *encoder, uint16_t handle, uint32_t offset, uint32_t count)
{
    (void)user; (void)encoder; bound = count;
    record_bytes(dynamic_data[handle] + (size_t)offset * dynamic_stride[handle], count, dynamic_stride[handle]);
}
static void provider_bind_transient(void *user, void *encoder, const JceInstanceTransient *token, uint32_t count)
{
    (void)user; (void)encoder; bound = count;
    record_bytes(token->data, count, token->stride);
}
static const JceInstanceUploadProvider provider = {
    provider_available, provider_transient, provider_create, provider_destroy,
    provider_alloc, provider_free, provider_update, provider_bind_dynamic, provider_bind_transient, NULL
};
#define jce_renderer_instance_upload_owner owner_for
#define jce_renderer_instance_upload_epoch upload_epoch
#define jce_shader_deferred_resolve resolved
#define bgfx_encoder_begin begin_encoder
#define bgfx_encoder_end end_encoder
#define bgfx_discard discard
#define jce_thread_current_id thread_id
#define jce_thread_pool_shared shared_pool
#define jce_thread_pool_worker_count worker_count
#define jce_thread_pool_parallel_for_named parallel_for
#define getenv queue_getenv
#define jce_render_pipeline_perf_enabled(...) false
#define bgfx_get_avail_instance_data_buffer capacity
#define bgfx_alloc_instance_data_buffer allocate
#define jce_enc_set_instance_data_buffer bind_instances
#define jce_enc_set_transform(...) ((uint32_t)0)
#define jce_enc_set_vertex_buffer(...) ((void)0)
#define jce_enc_set_index_buffer set_indices
#define jce_enc_set_state set_state
#define jce_enc_set_stencil set_stencil
#define jce_enc_set_instance_data_from_dynamic_vertex_buffer(...) ((void)0)
#define jce_rq_create controlled_rq_create
#define jce_rq_set_material_binder controlled_rq_set_material_binder
#define jce_rq_set_no_batch controlled_rq_set_no_batch
#define jce_rq_destroy controlled_rq_destroy
#define jce_rq_push controlled_rq_push
#define jce_rq_push_instanced controlled_rq_push_instanced
#define jce_rq_sort controlled_rq_sort
#define jce_rq_flush controlled_rq_flush
#define jce_rq_clear controlled_rq_clear
#define jce_rq_count controlled_rq_count
#define jce_rq_last_stats controlled_rq_last_stats
#define jce_rq_analyse controlled_rq_analyse
#include "../../engine/src/renderer/jce_render_queue.c"
#undef jce_rq_flush
/* A deterministic scheduler models separate TLS contexts without native threads.
 * All range submission and deferred work stay in the actual queue owner. */
static void JCE_CALL parallel_for(JceThreadPool *pool, const char *name,
    uint32_t count, uint32_t chunk, JceTaskRangeFn fn, void *arg)
{
    TEST_ASSERT_EQUAL_PTR(&pool_token, pool);
    TEST_ASSERT_EQUAL_STRING("render-queue.flush", name);
    TEST_ASSERT_EQUAL_UINT32(64, count);
    TEST_ASSERT_EQUAL_UINT32(22, chunk);
    RqFlushCtx *context = (RqFlushCtx *)arg;
    ++parallel_calls;
    for (uint32_t begin = 0; begin < count; begin += chunk) {
        const uint32_t end = begin + chunk < count ? begin + chunk : count;
        const unsigned slot = begin / chunk;
        if (slot == 0 && !workers_only) {
            TEST_ASSERT_EQUAL_UINT64(context->api_tid, current_tid);
            fn(begin, end, arg);
            continue;
        }
        current_tid = context->api_tid + 1;
        encoder_available = slot != 2;
        const unsigned before_submits = submitted;
        fn(begin, end, arg);
        TEST_ASSERT_NULL(jce_tls_encoder);
        if (encoder_available) {
            TEST_ASSERT_EQUAL_UINT32(end - begin, submitted - before_submits);
        } else {
            TEST_ASSERT_EQUAL_UINT32(before_submits, submitted);
            TEST_ASSERT_EQUAL_UINT32(begin, context->defer_begin[slot]);
            TEST_ASSERT_EQUAL_UINT32(end, context->defer_end[slot]);
        }
        current_tid = context->api_tid;
    }
}
void setUp(void)
{
    available = allocated = 8;
    submitted = instances = bound = 0;
    captured_bytes = index_binds = material_count = next_handle = 0;
    last_state = last_stencil = 0; fail_staging = false; owner_selected = NULL;
    pool_enabled = encoder_available = workers_only = false;
    current_tid = 10; parallel_calls = encoder_submits = encoder_ends = 0;
    jce_tls_encoder = NULL;
    jce_instance_upload_owner_init(&test_owner, &provider, NULL, 0, 1);
}
void tearDown(void) { jce_instance_upload_owner_destroy(&test_owner); }
static JceDrawCmd command(void)
{
    JceDrawCmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.program = 1;
    cmd.program_single = UINT16_MAX;
    cmd.mesh_vbh = 1;
    cmd.mesh_ibh = UINT16_MAX;
    return cmd;
}
static void test_explicit_full_capacity_preserves_payload_count(void)
{
    float data[80] = {0};
    JceDrawCmd cmd = command();
    JceInstanceBatch batch = {0};
    batch.data = data; batch.instance_count = 4; batch.stride_vec4 = 5;
    JceRenderQueue *queue = controlled_rq_create(4);
    controlled_rq_push_instanced(queue, &cmd, &batch);
    controlled_rq_flush(queue, NULL);
    TEST_ASSERT_EQUAL_UINT32(1, submitted);
    TEST_ASSERT_EQUAL_UINT32(4, instances);
    controlled_rq_destroy(queue);
}
static void test_explicit_failed_admission_does_not_claim_submit(void)
{
    float data[80] = {0};
    JceDrawCmd cmd = command();
    JceInstanceBatch batch = {0};
    batch.data = data; batch.instance_count = 4; batch.stride_vec4 = 5;
    available = allocated = 0;
    JceRenderQueue *queue = controlled_rq_create(4);
    controlled_rq_push_instanced(queue, &cmd, &batch);
    controlled_rq_flush(queue, NULL);
    JceRenderQueueStats stats;
    controlled_rq_last_stats(queue, &stats);
    TEST_ASSERT_EQUAL_UINT32(submitted, stats.submits_out);
    controlled_rq_destroy(queue);
}
static void test_instance_only_single_never_submits_unbound(void)
{
    JceDrawCmd cmd = command();
    available = allocated = 0;
    JceRenderQueue *queue = controlled_rq_create(4);
    controlled_rq_push(queue, &cmd);
    controlled_rq_flush(queue, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, submitted);
    controlled_rq_destroy(queue);
}
static void test_optimistic_allocation_clamp_preserves_auto_remainder(void)
{
    JceDrawCmd cmd = command();
    allocated = 1;
    JceRenderQueue *queue = controlled_rq_create(4);
    for (unsigned i = 0; i < 4; ++i)
        controlled_rq_push(queue, &cmd);
    controlled_rq_flush(queue, NULL);
    TEST_ASSERT_EQUAL_UINT32(4, instances);
    JceRenderQueueStats stats; controlled_rq_last_stats(queue, &stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.batches_merged);
    controlled_rq_destroy(queue);
}
static void test_dynamic_tint_bytes_state_and_nonindexed_retry(void)
{
    float data[80];
    for (unsigned i = 0; i < 80; ++i) data[i] = (float)i + 0.25f;
    JceDrawCmd cmd = command(); cmd.state = 123; cmd.stencil = 456; cmd.material_key = 7;
    JceInstanceBatch batch = {0}; batch.data = data; batch.instance_count = 4; batch.stride_vec4 = 5;
    JceRenderQueue *queue = controlled_rq_create(4);
    controlled_rq_set_material_binder(queue, material, NULL);
    owner_selected = &test_owner; available = allocated = 0; fail_staging = true;
    controlled_rq_push_instanced(queue, &cmd, &batch);
    controlled_rq_flush(queue, (const JceRenderer *)1);
    TEST_ASSERT_EQUAL_UINT32(0, submitted);
    fail_staging = false;
    controlled_rq_push_instanced(queue, &cmd, &batch);
    controlled_rq_flush(queue, (const JceRenderer *)1);
    TEST_ASSERT_EQUAL_UINT32(4, instances);
    TEST_ASSERT_EQUAL_MEMORY(data, captured, sizeof data);
    TEST_ASSERT_EQUAL_UINT64(123, last_state);
    TEST_ASSERT_EQUAL_UINT32(456, last_stencil);
    TEST_ASSERT_EQUAL_UINT32(0, index_binds);
    TEST_ASSERT_EQUAL_UINT32(7, materials[material_count - 1]);
    controlled_rq_destroy(queue);
}
static void test_invalid_program_and_stride_do_not_claim_work(void)
{
    JceDrawCmd cmd = command(); cmd.program = 9;
    JceRenderQueue *queue = controlled_rq_create(4);
    controlled_rq_push(queue, &cmd);
    controlled_rq_flush(queue, NULL);
    JceRenderQueueStats stats; controlled_rq_last_stats(queue, &stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.submits_out);
    JceInstanceBatch batch = {0}; float value = 0;
    batch.data = &value; batch.instance_count = 1; batch.stride_vec4 = UINT16_MAX;
    controlled_rq_push_instanced(queue, &cmd, &batch);
    TEST_ASSERT_EQUAL_UINT32(0, controlled_rq_count(queue));
    controlled_rq_destroy(queue);
}
static void test_transparent_sort_and_persistent_data_keep_order(void)
{
    JceRenderQueue *queue = controlled_rq_create(4);
    controlled_rq_set_material_binder(queue, material, NULL);
    controlled_rq_set_no_batch(queue, true);
    JceDrawCmd cmd = command(); JceInstanceBatch batch = {0};
    batch.persist_vb = 1; batch.instance_count = 4; batch.stride_vec4 = 5;
    cmd.depth = 1; cmd.material_key = 1;
    controlled_rq_push_instanced(queue, &cmd, &batch);
    cmd.depth = 3; cmd.material_key = 3;
    controlled_rq_push_instanced(queue, &cmd, &batch);
    controlled_rq_sort(queue, JCE_SORT_BACK_TO_FRONT);
    controlled_rq_flush(queue, NULL);
    TEST_ASSERT_EQUAL_UINT32(2, submitted);
    TEST_ASSERT_EQUAL_UINT32(3, materials[0]);
    TEST_ASSERT_EQUAL_UINT32(1, materials[1]);
    controlled_rq_destroy(queue);
}

static void test_matrix_remainders_preserve_bytes_and_reserve_pressure(void)
{
    JceRenderQueue *queue = controlled_rq_create(4);
    float expected[64];
    for (unsigned n = 0; n < 4; ++n) {
        JceDrawCmd cmd = command();
        for (unsigned j = 0; j < 16; ++j)
            ((float *)cmd.transform.raw)[j] = expected[n * 16 + j] = (float)(n * 16 + j);
        controlled_rq_push(queue, &cmd);
    }
    test_owner.transient_reserve_bytes = 300;
    TEST_ASSERT_FALSE(rq_parallel_fits(queue, &test_owner));
    test_owner.transient_reserve_bytes = 0;
    TEST_ASSERT_TRUE(rq_parallel_fits(queue, &test_owner));
    allocated = 1;
    controlled_rq_flush(queue, NULL);
    TEST_ASSERT_EQUAL_UINT32(4, instances);
    TEST_ASSERT_EQUAL_MEMORY(expected, captured, sizeof expected);
    controlled_rq_destroy(queue);
}
static void test_unavailable_worker_encoder_defers_without_implicit_submit(void)
{
    JceRenderQueue *queue = controlled_rq_create(4);
    JceDrawCmd cmd = command(); cmd.program_single = 1;
    controlled_rq_push(queue, &cmd);
    RqFlushCtx context; memset(&context, 0, sizeof context);
    context.rq = queue; context.per = 1;
    context.api_tid = jce_thread_current_id() + 1;
    rq_flush_chunk(0, 1, &context);
    TEST_ASSERT_EQUAL_UINT32(0, submitted);
    TEST_ASSERT_EQUAL_UINT32(1, context.defer_end[0]);
    rq_flush_range(queue, context.defer_begin[0], context.defer_end[0],
                   false, false, &context.stats[0], NULL);
    TEST_ASSERT_EQUAL_UINT32(1, submitted);
    TEST_ASSERT_EQUAL_UINT32(1, context.stats[0].submits_out);
    controlled_rq_destroy(queue);
}

static void test_disabled_auto_merge_retains_explicit_tint_payload(void)
{
    JceRenderQueue *queue = controlled_rq_create(4);
    controlled_rq_set_no_batch(queue, true);
    float data[80];
    for (unsigned i = 0; i < 80; ++i) data[i] = (float)i + 0.75f;
    JceDrawCmd cmd = command();
    JceInstanceBatch batch = {0}; batch.data = data; batch.instance_count = 4; batch.stride_vec4 = 5;
    controlled_rq_push_instanced(queue, &cmd, &batch);
    controlled_rq_flush(queue, NULL);
    TEST_ASSERT_EQUAL_UINT32(4, instances);
    TEST_ASSERT_EQUAL_MEMORY(data, captured, sizeof data);
    controlled_rq_destroy(queue);
}

static JceRenderQueue *parallel_queue(void)
{
    JceRenderQueue *queue = controlled_rq_create(64);
    controlled_rq_set_material_binder(queue, material, NULL);
    for (unsigned i = 0; i < 64; ++i) {
        JceDrawCmd cmd = command();
        cmd.view_id = 23; cmd.program = cmd.program_single = (uint16_t)(i % 2 ? 7 : 1);
        cmd.material_key = i; /* Keep every command a distinct terminal submit. */
        controlled_rq_push(queue, &cmd);
    }
    available = 128; pool_enabled = true;
    return queue;
}
static void test_parallel_api_worker_and_deferred_submit_exactly_once(void)
{
    JceRenderQueue *queue = parallel_queue();
    controlled_rq_flush(queue, NULL);
    TEST_ASSERT_EQUAL_UINT32(1, parallel_calls);
    TEST_ASSERT_EQUAL_UINT32(64, submitted);
    TEST_ASSERT_EQUAL_UINT32(22, encoder_submits);
    TEST_ASSERT_EQUAL_UINT32(1, encoder_ends);
    TEST_ASSERT_NULL(jce_tls_encoder);
    TEST_ASSERT_EQUAL_UINT32(0, controlled_rq_count(queue));
    JceRenderQueueStats stats; controlled_rq_last_stats(queue, &stats);
    TEST_ASSERT_EQUAL_UINT32(64, stats.submits_out);
    controlled_rq_destroy(queue);
}
static void test_parallel_worker_only_defers_without_losing_submissions(void)
{
    JceRenderQueue *queue = parallel_queue();
    workers_only = true;
    controlled_rq_flush(queue, NULL);
    TEST_ASSERT_EQUAL_UINT32(1, parallel_calls);
    TEST_ASSERT_EQUAL_UINT32(64, submitted);
    TEST_ASSERT_EQUAL_UINT32(44, encoder_submits);
    TEST_ASSERT_EQUAL_UINT32(2, encoder_ends);
    controlled_rq_destroy(queue);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_explicit_full_capacity_preserves_payload_count);
    RUN_TEST(test_explicit_failed_admission_does_not_claim_submit);
    RUN_TEST(test_instance_only_single_never_submits_unbound);
    RUN_TEST(test_optimistic_allocation_clamp_preserves_auto_remainder);
    RUN_TEST(test_dynamic_tint_bytes_state_and_nonindexed_retry);
    RUN_TEST(test_invalid_program_and_stride_do_not_claim_work);
    RUN_TEST(test_transparent_sort_and_persistent_data_keep_order);
    RUN_TEST(test_matrix_remainders_preserve_bytes_and_reserve_pressure);
    RUN_TEST(test_unavailable_worker_encoder_defers_without_implicit_submit);
    RUN_TEST(test_disabled_auto_merge_retains_explicit_tint_payload);
    RUN_TEST(test_parallel_api_worker_and_deferred_submit_exactly_once);
    RUN_TEST(test_parallel_worker_only_defers_without_losing_submissions);
    return UNITY_END();
}
