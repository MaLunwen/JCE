/* Complete scene-video owner with real scene/ECS lifecycle and counted public
 * decoder/texture/byte ports. The render-prefix extract proves its actual wire;
 * these CPU controls do not create a backend or certify pixels/GPU completion. */
#include "unity.h"
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/video/jce_video.h>
#include <jce/renderer/jce_texture.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_trace.h>
#include "middleware/scene/jce_scene_video_internal.h"
#include <string.h>

#define PORT_CAP 32
static struct VideoPort { bool live, ended, loop; uint64_t counter; int w, h; } videos[PORT_CAP];
static bool textures[PORT_CAP], reject_create, reject_update, reject_bytes;
static unsigned loads, unloads, advances, creates, updates, destroys, reads;
static double last_dt;
static JceScene *scene;
static JceEntity entity;
static uint8_t pixels[128];

JceVideo jce_video_load_memory(const void *data, uint32_t size, const char *name)
{
    (void)name; TEST_ASSERT_NOT_NULL(data); TEST_ASSERT_EQUAL_UINT32(4, size);
    JceVideo v = ++loads; TEST_ASSERT_TRUE(v < PORT_CAP);
    videos[v].live = true; videos[v].w = videos[v].h = 2;
    return v;
}
void jce_video_unload(JceVideo v)
{
    TEST_ASSERT_TRUE(v > 0 && v < PORT_CAP); TEST_ASSERT_TRUE(videos[v].live);
    videos[v].live = false; ++unloads;
}
void jce_video_set_loop(JceVideo v, bool loop) { videos[v].loop = loop; }
bool jce_video_has_ended(JceVideo v) { return videos[v].ended; }
void jce_video_advance(JceVideo v, double dt)
{
    TEST_ASSERT_TRUE(videos[v].live); ++advances; last_dt = dt; ++videos[v].counter;
}
uint64_t jce_video_get_frame_counter(JceVideo v) { return videos[v].counter; }
const uint8_t *jce_video_get_frame_rgba(JceVideo v, int *w, int *h, double *time)
{
    TEST_ASSERT_TRUE(videos[v].live); *w = videos[v].w; *h = videos[v].h; *time = 0;
    return pixels;
}
JceTexture jce_texture_from_rgba(const void *data, uint32_t w, uint32_t h)
{
    TEST_ASSERT_NOT_NULL(data); TEST_ASSERT_TRUE(w > 0 && h > 0); ++creates;
    if (reject_create) return JCE_TEXTURE_INVALID;
    for (uint16_t i = 0; i < PORT_CAP; ++i) if (!textures[i]) {
        JceTexture t = {i}; textures[i] = true; return t;
    }
    TEST_FAIL_MESSAGE("Counted texture budget exhausted"); return JCE_TEXTURE_INVALID;
}
bool jce_texture_update_rgba_ref(JceTexture t, const void *data, uint32_t w, uint32_t h)
{
    TEST_ASSERT_TRUE(t.idx < PORT_CAP && textures[t.idx]); TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_TRUE(w > 0 && h > 0); ++updates; return !reject_update;
}
void jce_texture_destroy(JceTexture t)
{
    TEST_ASSERT_TRUE(t.idx < PORT_CAP && textures[t.idx]); textures[t.idx] = false; ++destroys;
}
void *jce_fs_host_read_all(const char *path, uint64_t *size)
{
    TEST_ASSERT_NOT_NULL(path); ++reads; *size = 0;
    if (reject_bytes) return NULL;
    void *bytes = JCE_MALLOC(4); TEST_ASSERT_NOT_NULL(bytes); memset(bytes, 0, 4);
    *size = 4; return bytes;
}

/* The same actual resource preparation body and private layout as production.
 * No async cache work is scheduled by these real-ECS video cases. */
#include "middleware/scene/jce_sr_internal.h"
#include "middleware/scene/jce_sr_model_diagnostics.h"
#include "renderer/jce_renderer_lifecycle_internal.h"
#include "os/core/jce_trace_scope_internal.h"
static JceSceneRenderer *video_sr;
bool jce_renderer_frame_admission_current_internal(const JceRenderer *r) { return r != NULL; }
bool jce_renderer_get_submitted_payload_stats(const JceRenderer *r, JceSubmittedPayloadStats *out)
{ memset(out, 0, sizeof(*out)); out->context_generation = 1; out->frame_index = 1; out->valid = r != NULL; return out->valid; }
uint32_t jce_renderer_get_frame_index(const JceRenderer *r) { (void)r; return 1; }
bool jce_async_task_is_terminal(const JceAsyncTask *task) { (void)task; TEST_FAIL_MESSAGE("Unexpected async video cache task"); return false; }
JceAsyncState jce_async_task_state(const JceAsyncTask *task) { (void)task; return JCE_ASYNC_STATE_INVALID; }
void jce_async_task_release(JceAsyncTask *task) { (void)task; TEST_FAIL_MESSAGE("Unexpected async task release"); }
JceTexture jce_texture_upload_cpu(JceTextureCpu *cpu) { (void)cpu; TEST_FAIL_MESSAGE("Unexpected cache texture upload"); return JCE_TEXTURE_INVALID; }
void jce_texture_cpu_free(JceTextureCpu *cpu) { (void)cpu; TEST_FAIL_MESSAGE("Unexpected cache texture free"); }
JceModel *jce_model_upload_gltf_cpu(JceModelCpu *cpu) { (void)cpu; TEST_FAIL_MESSAGE("Unexpected cache model upload"); return NULL; }
void jce_model_gltf_cpu_free(JceModelCpu *cpu) { (void)cpu; TEST_FAIL_MESSAGE("Unexpected cache model free"); }
uint64_t jce_model_gpu_bytes(const JceModel *model) { (void)model; return 0; }
void jce_texture_set_streaming_uploads(bool on) { (void)on; }
void sr_model_cache_failure_hash(JceSceneRenderer *owner, uint32_t path_hash, uint32_t entity, JceSrModelFailure reason)
{ (void)owner; (void)path_hash; (void)entity; (void)reason; TEST_FAIL_MESSAGE("Unexpected cache model failure"); }
void sr_model_cache_diagnostics_bind(JceSceneRenderer *owner, const void *bound_scene,
    uint64_t context, uint32_t backend_frame)
{
    TEST_ASSERT_EQUAL_PTR(video_sr, owner);
    TEST_ASSERT_EQUAL_PTR(scene, bound_scene);
    TEST_ASSERT_EQUAL_UINT64(1, context);
    TEST_ASSERT_EQUAL_UINT32(1, backend_frame);
}
#include JCE_SRP_OWNER
#include JCE_SCENE_VIDEO_RENDER_PREFIX

static JceVideoPlayerComponent *component(void) { return jce_scene_get_video_player(scene, entity); }
static void install_player(bool autoplay)
{
    JceVideoPlayerComponent c = {0}; strcpy(c.clip_path, "test.webm");
    c.autoplay = autoplay; c.output_tex = JCE_TEXTURE_INVALID;
    jce_scene_set_video_player(scene, entity, &c);
}
static void prepare(void)
{
    TEST_ASSERT_EQUAL_UINT16(7, jce_sv_test_render_prefix(video_sr, scene, NULL, 7, 0, NULL));
}
void setUp(void)
{
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0); jce_fs_set_active(NULL);
    memset(videos, 0, sizeof(videos)); memset(textures, 0, sizeof(textures));
    reject_create = reject_update = reject_bytes = false;
    loads = unloads = advances = creates = updates = destroys = reads = 0; last_dt = 0;
    video_sr = JCE_CALLOC(1, sizeof(*video_sr)); TEST_ASSERT_NOT_NULL(video_sr);
    video_sr->renderer = (JceRenderer *)video_sr;
    scene = jce_scene_create(); TEST_ASSERT_NOT_NULL(scene);
    entity = jce_scene_create_entity(scene, "video"); install_player(true);
}
void tearDown(void)
{
    jce_scene_destroy(scene); scene = NULL;
    JCE_FREE(video_sr); video_sr = NULL;
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0);
    for (unsigned i = 0; i < PORT_CAP; ++i) {
        TEST_ASSERT_FALSE(videos[i].live); TEST_ASSERT_FALSE(textures[i]);
    }
}
static void test_advance_decodes_without_create_or_update(void)
{
    for (int i = 0; i < 10; ++i) jce_scene_video_advance(scene, 0.02, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(1, loads); TEST_ASSERT_EQUAL_UINT32(10, advances);
    TEST_ASSERT_TRUE(jce_scene_video_prepare_pending(scene));
    TEST_ASSERT_EQUAL_UINT32(0, creates); TEST_ASSERT_EQUAL_UINT32(0, updates);
    TEST_ASSERT_EQUAL_UINT64(0, component()->uploaded_counter);
    TEST_ASSERT_FALSE(jce_texture_valid(component()->output_tex));
}
static void test_actual_render_prefix_prepares_latest_once_across_views(void)
{
    jce_scene_video_advance(scene, 0.02, NULL, NULL); prepare(); prepare();
    TEST_ASSERT_EQUAL_UINT32(1, creates); TEST_ASSERT_EQUAL_UINT32(0, updates);
    TEST_ASSERT_EQUAL_UINT32(1, advances); TEST_ASSERT_EQUAL_UINT64(1, component()->uploaded_counter);
    TEST_ASSERT_FALSE(jce_scene_video_prepare_pending(scene));
    jce_scene_video_advance(scene, 0.02, NULL, NULL); prepare(); prepare();
    TEST_ASSERT_EQUAL_UINT32(1, creates); TEST_ASSERT_EQUAL_UINT32(1, updates);
    TEST_ASSERT_EQUAL_UINT64(2, component()->uploaded_counter);
}
static void test_null_render_admission_does_not_prepare(void)
{
    jce_scene_video_advance(scene, 0.02, NULL, NULL);
    jce_sv_test_render_prefix(NULL, scene, NULL, 7, 0, NULL);
    jce_sv_test_render_prefix(video_sr, NULL, NULL, 7, 0, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, creates); TEST_ASSERT_EQUAL_UINT32(0, updates);
}
static void test_combined_update_keeps_immediate_output_compatibility(void)
{
    jce_scene_video_update(scene, 0.02, NULL, NULL);
    TEST_ASSERT_TRUE(jce_texture_valid(component()->output_tex));
    TEST_ASSERT_EQUAL_UINT32(1, creates); TEST_ASSERT_EQUAL_UINT32(1, advances);
    prepare(); TEST_ASSERT_EQUAL_UINT32(0, updates);
}
static void test_stopped_one_shot_prepares_final_published_frame(void)
{
    jce_scene_video_advance(scene, 0.02, NULL, NULL);
    JceVideo v = component()->video; videos[v].ended = true; videos[v].counter = 2;
    jce_scene_video_advance(scene, 0.02, NULL, NULL);
    TEST_ASSERT_FALSE(component()->playing); TEST_ASSERT_EQUAL_UINT32(1, advances);
    prepare(); TEST_ASSERT_EQUAL_UINT32(1, creates);
    TEST_ASSERT_EQUAL_UINT64(2, component()->uploaded_counter);
    prepare(); TEST_ASSERT_EQUAL_UINT32(0, updates);
}
static void test_background_path_change_retires_once_without_upload(void)
{
    jce_scene_video_update(scene, 0.02, NULL, NULL);
    strcpy(component()->clip_path, "other.webm");
    jce_scene_video_advance(scene, 0.02, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(2, loads); TEST_ASSERT_EQUAL_UINT32(1, unloads);
    TEST_ASSERT_EQUAL_UINT32(1, destroys); TEST_ASSERT_EQUAL_UINT32(1, creates);
    TEST_ASSERT_EQUAL_UINT32(0, updates); TEST_ASSERT_EQUAL_UINT64(0, component()->uploaded_counter);
    prepare(); TEST_ASSERT_EQUAL_UINT32(2, creates);
    TEST_ASSERT_EQUAL_UINT64(1, component()->uploaded_counter);
}
static void test_resize_replaces_only_on_success_and_retries_same_frame(void)
{
    jce_scene_video_update(scene, 0.02, NULL, NULL); JceTexture old = component()->output_tex;
    videos[component()->video].w = 4;
    jce_scene_video_advance(scene, 0.02, NULL, NULL); reject_create = true; prepare();
    TEST_ASSERT_EQUAL_UINT16(old.idx, component()->output_tex.idx);
    TEST_ASSERT_EQUAL_UINT64(1, component()->uploaded_counter); TEST_ASSERT_EQUAL_UINT32(0, destroys);
    reject_create = false; prepare();
    TEST_ASSERT_NOT_EQUAL(old.idx, component()->output_tex.idx);
    TEST_ASSERT_EQUAL_INT(4, component()->tex_w); TEST_ASSERT_EQUAL_UINT64(2, component()->uploaded_counter);
    TEST_ASSERT_EQUAL_UINT32(1, destroys);
}
static void test_failed_update_and_replacement_preserve_owned_output(void)
{
    jce_scene_video_update(scene, 0.02, NULL, NULL); JceTexture old = component()->output_tex;
    jce_scene_video_advance(scene, 0.02, NULL, NULL);
    reject_update = reject_create = true; prepare();
    TEST_ASSERT_EQUAL_UINT16(old.idx, component()->output_tex.idx);
    TEST_ASSERT_EQUAL_UINT64(1, component()->uploaded_counter); TEST_ASSERT_EQUAL_UINT32(0, destroys);
    reject_update = reject_create = false; prepare();
    TEST_ASSERT_EQUAL_UINT16(old.idx, component()->output_tex.idx);
    TEST_ASSERT_EQUAL_UINT64(2, component()->uploaded_counter); TEST_ASSERT_EQUAL_UINT32(2, updates);
}
static void test_initial_failed_upload_does_not_publish_counter(void)
{
    jce_scene_video_advance(scene, 0.02, NULL, NULL); reject_create = true; prepare();
    TEST_ASSERT_FALSE(jce_texture_valid(component()->output_tex));
    TEST_ASSERT_EQUAL_UINT64(0, component()->uploaded_counter);
    reject_create = false; prepare(); TEST_ASSERT_EQUAL_UINT64(1, component()->uploaded_counter);
    TEST_ASSERT_EQUAL_UINT32(2, creates);
}
static void test_pause_and_loop_keep_transport_and_render_separate(void)
{
    jce_scene_video_update(scene, 0.02, NULL, NULL); component()->playing = false;
    jce_scene_video_advance(scene, 0.02, NULL, NULL); prepare();
    TEST_ASSERT_EQUAL_UINT32(1, advances); TEST_ASSERT_EQUAL_UINT32(0, updates);
    component()->playing = true; component()->loop = true; videos[component()->video].ended = true;
    jce_scene_video_advance(scene, 10.0, NULL, NULL);
    TEST_ASSERT_TRUE(videos[component()->video].loop); TEST_ASSERT_TRUE(component()->playing);
    TEST_ASSERT_FLOAT_WITHIN(0.000001f, 0.25f, (float)last_dt); TEST_ASSERT_EQUAL_UINT32(0, updates);
}
static void test_missing_path_is_latched_then_edit_retries(void)
{
    reject_bytes = true; jce_scene_video_advance(scene, 0.02, NULL, NULL);
    jce_scene_video_advance(scene, 0.02, NULL, NULL); TEST_ASSERT_EQUAL_UINT32(1, reads);
    reject_bytes = false; strcpy(component()->clip_path, "fixed.webm");
    jce_scene_video_advance(scene, 0.02, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(2, reads); TEST_ASSERT_EQUAL_UINT32(1, loads);
    TEST_ASSERT_EQUAL_UINT32(0, creates);
}
static void test_component_remove_and_restart_release_each_owner_once(void)
{
    jce_scene_video_update(scene, 0.02, NULL, NULL); jce_scene_remove_video_player(scene, entity);
    TEST_ASSERT_EQUAL_UINT32(1, unloads); TEST_ASSERT_EQUAL_UINT32(1, destroys);
    jce_scene_remove_video_player(scene, entity);
    TEST_ASSERT_EQUAL_UINT32(1, unloads); TEST_ASSERT_EQUAL_UINT32(1, destroys);
    install_player(true); jce_scene_video_advance(scene, 0.02, NULL, NULL); prepare();
    TEST_ASSERT_EQUAL_UINT32(2, loads); TEST_ASSERT_EQUAL_UINT32(2, creates);
    jce_scene_destroy(scene); scene = NULL;
    TEST_ASSERT_EQUAL_UINT32(2, unloads); TEST_ASSERT_EQUAL_UINT32(2, destroys);
}
static void test_real_ecs_table_move_keeps_handles_until_scene_teardown(void)
{
    jce_scene_video_update(scene, 0.02, NULL, NULL);
    JceVideo v = component()->video; JceTexture t = component()->output_tex;
    JceMeshRenderer mesh = {0}; jce_scene_set_mesh_renderer(scene, entity, &mesh);
    TEST_ASSERT_EQUAL_UINT32(v, component()->video); TEST_ASSERT_EQUAL_UINT16(t.idx, component()->output_tex.idx);
    TEST_ASSERT_EQUAL_UINT32(0, unloads); TEST_ASSERT_EQUAL_UINT32(0, destroys);
}
static void test_trace_identifies_cpu_advance_upload_and_one_time_retirement(void)
{
    jce_trace_reset(); jce_trace_task_restore_id(91); jce_trace_set_enabled(true);
    jce_scene_video_advance(scene, 0.02, NULL, NULL); prepare();
    jce_scene_remove_video_player(scene, entity);
    TEST_ASSERT_EQUAL_UINT64(91, jce_trace_task_current_id());
    JceTraceEvent events[128]; JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    uint32_t count = jce_trace_read(&cursor, events, 128);
    unsigned advance = 0, upload = 0, retire = 0, texture_retired = 0;
    uint64_t resource_scope = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].type == JCE_TRACE_EVENT_TASK_SUBMIT) {
            if (strcmp(events[i].name, "scene.resources.prepare") == 0) {
                resource_scope = events[i].id;
                TEST_ASSERT_EQUAL_UINT64(91, events[i].parent_id);
            }
            advance += strcmp(events[i].name, "scene.video.advance") == 0;
            upload += strcmp(events[i].name, "scene.video.prepare") == 0;
            retire += strcmp(events[i].name, "scene.video.retire") == 0;
            if (strcmp(events[i].name, "scene.video.prepare") == 0)
                TEST_ASSERT_EQUAL_UINT64(resource_scope, events[i].parent_id);
            else if (strncmp(events[i].name, "scene.video.", 12) == 0)
                TEST_ASSERT_EQUAL_UINT64(91, events[i].parent_id);
        }
        if (events[i].type == JCE_TRACE_EVENT_COUNTER &&
            strcmp(events[i].name, "scene.video.texture.retired") == 0 &&
            events[i].value_u64 == 1) ++texture_retired;
    }
    TEST_ASSERT_EQUAL_UINT32(1, advance); TEST_ASSERT_EQUAL_UINT32(1, upload);
    TEST_ASSERT_EQUAL_UINT32(1, retire); TEST_ASSERT_EQUAL_UINT32(1, texture_retired);
}
static void test_trace_disabled_has_no_scene_video_records(void)
{
    jce_trace_reset(); jce_scene_video_advance(scene, 0.02, NULL, NULL); prepare();
    jce_scene_remove_video_player(scene, entity);
    JceTraceEvent event; JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, &event, 1));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_advance_decodes_without_create_or_update);
    RUN_TEST(test_actual_render_prefix_prepares_latest_once_across_views);
    RUN_TEST(test_null_render_admission_does_not_prepare);
    RUN_TEST(test_combined_update_keeps_immediate_output_compatibility);
    RUN_TEST(test_stopped_one_shot_prepares_final_published_frame);
    RUN_TEST(test_background_path_change_retires_once_without_upload);
    RUN_TEST(test_resize_replaces_only_on_success_and_retries_same_frame);
    RUN_TEST(test_failed_update_and_replacement_preserve_owned_output);
    RUN_TEST(test_initial_failed_upload_does_not_publish_counter);
    RUN_TEST(test_pause_and_loop_keep_transport_and_render_separate);
    RUN_TEST(test_missing_path_is_latched_then_edit_retries);
    RUN_TEST(test_component_remove_and_restart_release_each_owner_once);
    RUN_TEST(test_real_ecs_table_move_keeps_handles_until_scene_teardown);
    RUN_TEST(test_trace_identifies_cpu_advance_upload_and_one_time_retirement);
    RUN_TEST(test_trace_disabled_has_no_scene_video_records);
    return UNITY_END();
}
