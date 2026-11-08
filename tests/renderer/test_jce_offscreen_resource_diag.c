/* Complete actual offscreen owner and renderer session admission/accessors.
 * Typed CPU ports count backend calls; no device, GPU or log worker is started. */
#include "unity.h"
#include <jce/os/core/jce_env.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_primitives.h>
#include "renderer/jce_offscreen_target_internal.h"
#include "renderer/jce_offscreen_retirement_internal.h"
#include "renderer/jce_renderer_admission_internal.h"
#include "os/core/jce_log_ring.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct JceRenderer { JceOffscreenTarget *offscreen_targets; bool is_fallback; };
static JceRenderer renderer;
static struct {
    JceRendererAdmission admission;
    bool render_resource_diag;
    uint32_t offscreen_submission_index;
} s_device;
static bool main_owner;
static const char *environment_value;
static unsigned environment_reads;
static char *environment_port(const char *name)
{
    TEST_ASSERT_EQUAL_STRING("JCE_RENDER_RESOURCE_DIAG", name);
    ++environment_reads;
    return (char *)environment_value;
}
static bool main_port(void) { return main_owner; }
#define getenv environment_port
#define jce_thread_is_main main_port
#include JCE_TEST_RESOURCE_SESSION_OWNER
#undef jce_thread_is_main
#undef getenv
#include JCE_TEST_RESOURCE_ACCESSORS_OWNER

enum { HANDLES = 64, RECORDS = 96, VIEWS = 256 };
static bgfx_caps_t caps;
static unsigned texture_refs[HANDLES];
static bool framebuffers[HANDLES];
static uint16_t attachments[HANDLES][2];
static unsigned texture_calls, destroy_calls, fail_texture_call;
static bool fail_fbo, fail_query, trace_suppressed;
static unsigned rect_calls, fbo_calls, framebuffer_create_calls;
static struct {
    uint16_t width, height;
    bgfx_texture_format_t format;
    uint64_t flags;
} texture_metadata[HANDLES];
static struct {
    uint16_t width, height, fbo;
    bool rect_returned, fbo_returned;
    uint64_t binding_watermark;
} views[VIEWS];
/* Only the reassembled pair is larger. Every individual production message
 * first passes the actual JceLogMessage buffer below, never a larger port. */
static char records[RECORDS][2 * sizeof(((JceLogMessage *)0)->message)];
static char pending_message[sizeof(((JceLogMessage *)0)->message)];
static unsigned record_count, message_count, exhausted_count, max_message_size;
static JceOffscreenTarget *target;
static const float identity[16] = {
    1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
};

static uint64_t field(const char *line, const char *key)
{
    char token[80];
    snprintf(token, sizeof token, " %s=", key);
    const char *p = strstr(line, token);
    TEST_ASSERT_NOT_NULL_MESSAGE(p, key);
    return p ? strtoull(p + strlen(token), NULL, 10) : UINT64_MAX;
}

static void log_port(JceLogLevel level, const char *tag, const char *file,
    int line, const char *format, ...)
{
    (void)tag; (void)file; (void)line;
    if (strncmp(format, "render-resource:", 16) != 0) return;
    TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_INFO, level);
    JceLogMessage message;
    va_list args; va_start(args, format);
    const int count = vsnprintf(message.message, sizeof message.message, format, args);
    va_end(args);
    TEST_ASSERT_EQUAL_UINT(512, sizeof message.message);
    TEST_ASSERT_TRUE_MESSAGE(count > 0 && (size_t)count < sizeof message.message,
        "Actual production log message would truncate");
    TEST_ASSERT_EQUAL_UINT((unsigned)count, strlen(message.message));
    if ((unsigned)count > max_message_size) max_message_size = (unsigned)count;
    ++message_count;
    TEST_ASSERT_EQUAL_UINT64(2, field(message.message, "schema"));
    const uint64_t part = field(message.message, "part");
    if (part == 0) {
        TEST_ASSERT_EQUAL_STRING("", pending_message);
        TEST_ASSERT_NOT_NULL(strstr(message.message, "phase=diagnostic_exhausted "));
        TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, field(message.message, "event_ordinal"));
        ++exhausted_count;
        return;
    }
    static const char *const part1_fields[] = {
        "event_ordinal", "creation_serial", "origin_capture_generation", "view",
        "fbo", "color", "depth", "allocated_width", "allocated_height", "color_format",
        "samples", "allocation_committed", "allocated_extent_known", "submission_ordinal"
    };
    static const char *const part2_fields[] = {
        "event_ordinal", "creation_serial", "origin_capture_generation", "view",
        "requested_extent_known", "requested_width", "requested_height", "forwarded_rect_known",
        "forwarded_rect_x", "forwarded_rect_y", "forwarded_rect_width", "forwarded_rect_height",
        "reason", "destroy_kind", "destroy_handle", "cpu_calls_returned", "gpu_completion_known"
    };
    const char *const *keys = part == 1 ? part1_fields : part2_fields;
    const unsigned key_count = part == 1 ? sizeof part1_fields / sizeof part1_fields[0]
        : sizeof part2_fields / sizeof part2_fields[0];
    for (unsigned i = 0; i < key_count; ++i) (void)field(message.message, keys[i]);
    if (part == 1) {
        TEST_ASSERT_EQUAL_STRING("", pending_message);
        TEST_ASSERT_NOT_NULL(strstr(message.message, " phase="));
        TEST_ASSERT_NOT_NULL(strstr(message.message, " owner=offscreen "));
        memcpy(pending_message, message.message, (size_t)count + 1);
        return;
    }
    TEST_ASSERT_EQUAL_UINT64(2, part);
    TEST_ASSERT_NOT_EQUAL(0, pending_message[0]);
    for (unsigned i = 0; i < 4; ++i)
        TEST_ASSERT_EQUAL_UINT64(field(pending_message, part1_fields[i]),
            field(message.message, part1_fields[i]));
    TEST_ASSERT_LESS_THAN_UINT(RECORDS, record_count);
    const int combined = snprintf(records[record_count], sizeof records[0], "%s %s",
        pending_message, message.message);
    TEST_ASSERT_TRUE(combined > 0 && (size_t)combined < sizeof records[0]);
    pending_message[0] = '\0';
    const char *record = records[record_count++];
    TEST_ASSERT_EQUAL_UINT64(0, field(record, "gpu_completion_known"));
    TEST_ASSERT_EQUAL_UINT64(1, field(record, "cpu_calls_returned"));
    if (strstr(record, "phase=view_configured ")) {
        const unsigned view = (unsigned)field(record, "view");
        TEST_ASSERT_LESS_THAN_UINT(VIEWS, view);
        TEST_ASSERT_TRUE(views[view].rect_returned);
        TEST_ASSERT_TRUE(views[view].fbo_returned);
        TEST_ASSERT_EQUAL_UINT16(views[view].width, field(record, "forwarded_rect_width"));
        TEST_ASSERT_EQUAL_UINT16(views[view].height, field(record, "forwarded_rect_height"));
        TEST_ASSERT_EQUAL_UINT16(views[view].fbo, field(record, "fbo"));
        TEST_ASSERT_EQUAL_UINT64(1, field(record, "forwarded_rect_known"));
    }
}

static const bgfx_caps_t *caps_port(void) { return &caps; }
static bgfx_texture_handle_t texture_port(uint16_t width, uint16_t height,
    bool mips, uint16_t layers, bgfx_texture_format_t format, uint64_t flags,
    const bgfx_memory_t *memory, uint64_t external)
{
    TEST_ASSERT_TRUE(width >= 16 && width <= 8192 && height >= 16 && height <= 8192);
    TEST_ASSERT_FALSE(mips); TEST_ASSERT_EQUAL_UINT16(1, layers);
    TEST_ASSERT_NULL(memory); TEST_ASSERT_EQUAL_UINT64(0, external);
    if (++texture_calls == fail_texture_call) return (bgfx_texture_handle_t){UINT16_MAX};
    for (uint16_t i = 0; i < HANDLES; ++i)
        if (!texture_refs[i]) {
            texture_refs[i] = 1;
            texture_metadata[i].width = width; texture_metadata[i].height = height;
            texture_metadata[i].format = format; texture_metadata[i].flags = flags;
            return (bgfx_texture_handle_t){i};
        }
    TEST_FAIL_MESSAGE("CPU texture pool exhausted");
    return (bgfx_texture_handle_t){UINT16_MAX};
}
static void destroy_texture_port(bgfx_texture_handle_t texture)
{
    TEST_ASSERT_LESS_THAN_UINT16(HANDLES, texture.idx);
    TEST_ASSERT_GREATER_THAN_UINT(0, texture_refs[texture.idx]);
    --texture_refs[texture.idx]; ++destroy_calls;
}
static bgfx_frame_buffer_handle_t framebuffer_port(uint8_t count,
    const bgfx_attachment_t *source, bool take_ownership)
{
    TEST_ASSERT_EQUAL_UINT8(2, count); TEST_ASSERT_FALSE(take_ownership);
    ++framebuffer_create_calls;
    if (fail_fbo) return (bgfx_frame_buffer_handle_t){UINT16_MAX};
    for (uint16_t i = 0; i < HANDLES; ++i) {
        if (framebuffers[i]) continue;
        framebuffers[i] = true;
        for (unsigned a = 0; a < 2; ++a) {
            const uint16_t handle = source[a].handle.idx;
            TEST_ASSERT_LESS_THAN_UINT16(HANDLES, handle);
            TEST_ASSERT_GREATER_THAN_UINT(0, texture_refs[handle]);
            attachments[i][a] = handle; ++texture_refs[handle];
        }
        return (bgfx_frame_buffer_handle_t){i};
    }
    TEST_FAIL_MESSAGE("CPU framebuffer pool exhausted");
    return (bgfx_frame_buffer_handle_t){UINT16_MAX};
}
static bgfx_texture_handle_t get_texture_port(bgfx_frame_buffer_handle_t fbo, uint8_t attachment)
{
    TEST_ASSERT_LESS_THAN_UINT16(HANDLES, fbo.idx);
    TEST_ASSERT_TRUE(framebuffers[fbo.idx]); TEST_ASSERT_LESS_THAN_UINT8(2, attachment);
    return (bgfx_texture_handle_t){fail_query ? UINT16_MAX : attachments[fbo.idx][attachment]};
}
static void destroy_framebuffer_port(bgfx_frame_buffer_handle_t fbo)
{
    TEST_ASSERT_LESS_THAN_UINT16(HANDLES, fbo.idx); TEST_ASSERT_TRUE(framebuffers[fbo.idx]);
    framebuffers[fbo.idx] = false;
    for (unsigned a = 0; a < 2; ++a)
        destroy_texture_port((bgfx_texture_handle_t){attachments[fbo.idx][a]});
}
static void rect_port(bgfx_view_id_t view, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    TEST_ASSERT_LESS_THAN_UINT(VIEWS, view); TEST_ASSERT_EQUAL_UINT16(0, x);
    TEST_ASSERT_EQUAL_UINT16(0, y); ++rect_calls;
    views[view].width = w; views[view].height = h;
    views[view].rect_returned = true; views[view].fbo_returned = false;
}
static void binding_port(bgfx_view_id_t view, bgfx_frame_buffer_handle_t fbo)
{
    TEST_ASSERT_LESS_THAN_UINT(VIEWS, view); TEST_ASSERT_TRUE(views[view].rect_returned);
    TEST_ASSERT_LESS_THAN_UINT16(HANDLES, fbo.idx); TEST_ASSERT_TRUE(framebuffers[fbo.idx]);
    views[view].fbo = fbo.idx; views[view].fbo_returned = true; ++fbo_calls;
    JceTraceStats stats; jce_trace_get_stats(&stats);
    views[view].binding_watermark = stats.recorded_events;
}
static void name_port(bgfx_view_id_t view, const char *name, int32_t length)
{ (void)view; TEST_ASSERT_NOT_NULL(name); TEST_ASSERT_EQUAL_INT32(INT32_MAX, length); }
static void clear_port(bgfx_view_id_t view, uint16_t flags, uint32_t rgba, float depth, uint8_t stencil)
{ (void)view; (void)flags; (void)rgba; (void)depth; (void)stencil; }
static void transform_port(bgfx_view_id_t view, const void *v, const void *p)
{ (void)view; TEST_ASSERT_NOT_NULL(v); TEST_ASSERT_NOT_NULL(p); }
static void mode_port(bgfx_view_id_t view, bgfx_view_mode_t mode)
{ (void)view; TEST_ASSERT_EQUAL_INT(BGFX_VIEW_MODE_SEQUENTIAL, mode); }
static void touch_port(bgfx_view_id_t view) { (void)view; }
static void draw_port(const JceRenderer *r, uint16_t view, float x, float y,
    float w, float h, JceTexture texture, uint32_t tint, const float *uv)
{ (void)r; (void)view; (void)x; (void)y; (void)w; (void)h; (void)texture; (void)tint; (void)uv; }

#define jce_log_write log_port
#define bgfx_get_caps caps_port
#define bgfx_create_texture_2d texture_port
#define bgfx_create_frame_buffer_from_attachment framebuffer_port
#define bgfx_get_texture get_texture_port
#define bgfx_destroy_texture destroy_texture_port
#define bgfx_destroy_frame_buffer destroy_framebuffer_port
#define bgfx_set_view_rect rect_port
#define bgfx_set_view_frame_buffer binding_port
#define bgfx_set_view_name name_port
#define bgfx_set_view_clear clear_port
#define bgfx_set_view_transform transform_port
#define bgfx_set_view_mode mode_port
#define bgfx_touch touch_port
#define jce_draw_textured_rect_view_opaque draw_port
#include JCE_TEST_OFFSCREEN_RESOURCE_OWNER
#undef jce_log_write

void setUp(void)
{
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0); jce_trace_reset();
    memset(&renderer, 0, sizeof renderer); memset(&s_device, 0, sizeof s_device);
    memset(&caps, 0, sizeof caps); memset(texture_refs, 0, sizeof texture_refs);
    memset(framebuffers, 0, sizeof framebuffers); memset(views, 0, sizeof views);
    memset(texture_metadata, 0, sizeof texture_metadata);
    memset(records, 0, sizeof records);
    memset(pending_message, 0, sizeof pending_message);
    caps.formats[BGFX_TEXTURE_FORMAT_RGBA16F] = BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER;
    environment_value = NULL; environment_reads = 0; main_owner = true;
    texture_calls = destroy_calls = fail_texture_call = rect_calls = fbo_calls = record_count = 0;
    message_count = exhausted_count = max_message_size = 0;
    fail_fbo = fail_query = trace_suppressed = false; target = NULL;
    framebuffer_create_calls = 0;
}
void tearDown(void)
{
    if (trace_suppressed) {
        TEST_ASSERT_TRUE(jce_trace_suppress_end()); trace_suppressed = false;
    }
    jce_offscreen_target_destroy(target); target = NULL;
    TEST_ASSERT_EQUAL_STRING("", pending_message);
    TEST_ASSERT_NULL(renderer.offscreen_targets);
    for (unsigned i = 0; i < HANDLES; ++i) {
        TEST_ASSERT_EQUAL_UINT(0, texture_refs[i]); TEST_ASSERT_FALSE(framebuffers[i]);
    }
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0);
}
static void start(const char *value)
{
    environment_value = value;
    TEST_ASSERT_TRUE(jce_renderer_backend_create_begin_internal());
    jce_renderer_admission_finish(&s_device.admission, true);
    target = jce_offscreen_target_create(&renderer, 30);
    TEST_ASSERT_NOT_NULL(target);
}
static bool prepare(uint32_t width, uint32_t height)
{ return jce_offscreen_target_prepare(target, width, height, identity, identity, 0, "RuntimeScene"); }
static void no_trace_events(void)
{
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
    TEST_ASSERT_EQUAL_UINT64(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(0, stats.active_waits);
}
static void test_default_off_and_session_read_once_without_steady_environment_reads(void)
{
    start(NULL); TEST_ASSERT_EQUAL_UINT(1, environment_reads);
    environment_value = "1"; /* A live environment change cannot alter this session. */
    for (unsigned i = 0; i < 4; ++i) TEST_ASSERT_TRUE(prepare(64, 48));
    TEST_ASSERT_EQUAL_UINT(0, record_count); TEST_ASSERT_EQUAL_UINT(4, rect_calls);
    TEST_ASSERT_EQUAL_UINT(1, environment_reads);
    TEST_ASSERT_FALSE(jce_renderer_backend_create_begin_internal());
    TEST_ASSERT_EQUAL_UINT(1, environment_reads); no_trace_events();
}
static void test_admission_refusal_and_existing_flag_policy(void)
{
    main_owner = false; environment_value = "1";
    TEST_ASSERT_FALSE(jce_renderer_backend_create_begin_internal());
    TEST_ASSERT_EQUAL_UINT(0, environment_reads); main_owner = true;
    static const char *values[] = {NULL, "", "0", "0yes", "1", "false"};
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; ++i) {
        environment_value = values[i];
        TEST_ASSERT_TRUE(jce_renderer_backend_create_begin_internal());
        TEST_ASSERT_EQUAL_INT(i >= 4, jce_renderer_offscreen_diag_enabled(&renderer));
        TEST_ASSERT_FALSE(jce_renderer_offscreen_diag_enabled(NULL));
        renderer.is_fallback = true;
        TEST_ASSERT_FALSE(jce_renderer_offscreen_diag_enabled(&renderer));
        renderer.is_fallback = false;
        jce_renderer_admission_finish(&s_device.admission, false);
    }
    TEST_ASSERT_EQUAL_UINT(6, environment_reads);
}
static void test_trace_off_clamped_allocation_and_actual_forwarded_tuple_are_distinct(void)
{
    start("1"); TEST_ASSERT_TRUE(prepare(1, 9000));
    TEST_ASSERT_EQUAL_UINT(2, record_count);
    TEST_ASSERT_NOT_NULL(strstr(records[0], "phase=create_committed "));
    TEST_ASSERT_EQUAL_UINT64(1, field(records[0], "requested_width"));
    TEST_ASSERT_EQUAL_UINT64(9000, field(records[0], "requested_height"));
    TEST_ASSERT_EQUAL_UINT64(16, field(records[0], "allocated_width"));
    TEST_ASSERT_EQUAL_UINT64(8192, field(records[0], "allocated_height"));
    TEST_ASSERT_EQUAL_UINT64(0, field(records[0], "forwarded_rect_known"));
    TEST_ASSERT_EQUAL_UINT64(jce_trace_capture_generation(),
        field(records[0], "origin_capture_generation"));
    TEST_ASSERT_EQUAL_UINT64(1, field(records[0], "requested_extent_known"));
    TEST_ASSERT_EQUAL_UINT64(1, field(records[1], "allocation_committed"));
    TEST_ASSERT_EQUAL_UINT64(1, field(records[1], "allocated_extent_known"));
    TEST_ASSERT_EQUAL_UINT64(BGFX_TEXTURE_FORMAT_RGBA16F, field(records[1], "color_format"));
    TEST_ASSERT_EQUAL_UINT64(1, field(records[1], "samples"));
    TEST_ASSERT_EQUAL_UINT64(0, field(records[1], "fbo")); /* Valid handle zero. */
    TEST_ASSERT_TRUE(prepare(2, 9001)); /* Different request, identical actual tuple. */
    TEST_ASSERT_EQUAL_UINT(2, record_count); TEST_ASSERT_EQUAL_UINT(2, rect_calls);
    TEST_ASSERT_EQUAL_UINT(1, environment_reads); no_trace_events();
}
static void test_resize_identity_destroy_and_same_tuple_reuse(void)
{
    start("1"); TEST_ASSERT_TRUE(prepare(64, 48));
    const uint64_t first = field(records[1], "creation_serial");
    s_device.offscreen_submission_index = 1;
    TEST_ASSERT_TRUE(prepare(32, 24)); TEST_ASSERT_EQUAL_UINT(4, record_count);
    TEST_ASSERT_NOT_NULL(strstr(records[2], "phase=resize_committed "));
    TEST_ASSERT_NOT_EQUAL(first, field(records[3], "creation_serial"));
    TEST_ASSERT_TRUE(prepare(32, 24)); TEST_ASSERT_EQUAL_UINT(4, record_count);
    s_device.offscreen_submission_index = 9;
    jce_offscreen_retirement_collect(&renderer, 9);
    TEST_ASSERT_EQUAL_UINT(5, record_count);
    TEST_ASSERT_NOT_NULL(strstr(records[4], "phase=destroy_submitted "));
    TEST_ASSERT_EQUAL_UINT64(first, field(records[4], "creation_serial"));
    TEST_ASSERT_EQUAL_UINT64(3, field(records[4], "destroy_kind"));
    TEST_ASSERT_EQUAL_UINT64(0, field(records[4], "forwarded_rect_known"));
    TEST_ASSERT_EQUAL_UINT64(0, field(records[4], "requested_extent_known"));
    jce_offscreen_target_destroy(target); target = NULL;
    TEST_ASSERT_EQUAL_UINT(6, record_count); no_trace_events();
}
static void test_failures_never_publish_committed_allocation_or_view(void)
{
    start("1"); fail_fbo = true;
    TEST_ASSERT_FALSE(prepare(64, 48));
    TEST_ASSERT_EQUAL_UINT(2, record_count); TEST_ASSERT_EQUAL_UINT(0, rect_calls);
    for (unsigned i = 0; i < record_count; ++i) {
        TEST_ASSERT_NOT_NULL(strstr(records[i], "phase=rollback_destroy_submitted "));
        TEST_ASSERT_EQUAL_UINT64(0, field(records[i], "allocation_committed"));
        TEST_ASSERT_EQUAL_UINT64(0, field(records[i], "allocated_extent_known"));
    }
    fail_fbo = false; TEST_ASSERT_TRUE(prepare(64, 48));
    const unsigned before = record_count;
    fail_query = true; TEST_ASSERT_FALSE(prepare(32, 24));
    TEST_ASSERT_EQUAL_UINT(before + 1, record_count);
    TEST_ASSERT_EQUAL_UINT(1, rect_calls);
    uint32_t w = 123, h = 123;
    /* Actual resize retires the old target BEFORE attempting the new one. */
    TEST_ASSERT_FALSE(jce_offscreen_target_get_extent(target, &w, &h));
    TEST_ASSERT_EQUAL_UINT32(0, w); TEST_ASSERT_EQUAL_UINT32(0, h);
    fail_query = false; TEST_ASSERT_TRUE(prepare(32, 24));
    TEST_ASSERT_EQUAL_UINT(before + 3, record_count); no_trace_events();
}
static void test_retirement_saturation_keeps_current_without_false_resize_or_view(void)
{
    start("1"); TEST_ASSERT_TRUE(prepare(32, 32));
    for (unsigned i = 0; i < JCE_OFFSCREEN_RETIRE_DELAY; ++i)
        TEST_ASSERT_TRUE(prepare(40 + i, 40 + i));
    const unsigned before = record_count, before_rects = rect_calls;
    const uint16_t current = jce_offscreen_target_get_frame_buffer(target);
    TEST_ASSERT_FALSE(prepare(200, 100));
    TEST_ASSERT_EQUAL_UINT(before, record_count); TEST_ASSERT_EQUAL_UINT(before_rects, rect_calls);
    TEST_ASSERT_EQUAL_UINT16(current, jce_offscreen_target_get_frame_buffer(target));
    TEST_ASSERT_TRUE(prepare(47, 47)); TEST_ASSERT_EQUAL_UINT(before, record_count);
    no_trace_events();
}
static void test_texture_failure_cannot_publish_or_forward(void)
{
    start("1");
    for (unsigned failed = 1; failed <= 2; ++failed) {
        fail_texture_call = texture_calls + failed;
        TEST_ASSERT_FALSE(prepare(64, 48));
        TEST_ASSERT_EQUAL_UINT(failed, record_count);
        TEST_ASSERT_EQUAL_UINT(0, rect_calls);
        TEST_ASSERT_EQUAL_UINT64(0, field(records[failed - 1], "allocation_committed"));
        TEST_ASSERT_EQUAL_UINT64(failed == 1 ? 2 : 1,
            field(records[failed - 1], "destroy_kind"));
    }
    fail_texture_call = 0;
    TEST_ASSERT_TRUE(prepare(64, 48)); TEST_ASSERT_EQUAL_UINT(4, record_count);
    no_trace_events();
}
static void test_rgba8_fallback_and_reused_handles_have_new_allocation_identity(void)
{
    start("1"); caps.formats[BGFX_TEXTURE_FORMAT_RGBA16F] = 0;
    TEST_ASSERT_TRUE(prepare(64, 48));
    TEST_ASSERT_EQUAL_UINT64(BGFX_TEXTURE_FORMAT_RGBA8, field(records[0], "color_format"));
    const uint64_t serial = field(records[0], "creation_serial");
    jce_offscreen_target_destroy(target); target = NULL;
    target = jce_offscreen_target_create(&renderer, 30);
    TEST_ASSERT_TRUE(prepare(64, 48));
    TEST_ASSERT_EQUAL_UINT(5, record_count);
    TEST_ASSERT_EQUAL_UINT64(0, field(records[4], "fbo"));
    TEST_ASSERT_NOT_EQUAL(serial, field(records[4], "creation_serial")); no_trace_events();
}
static void test_maximum_fields_fit_actual_logger_and_event_exhaustion_closes_diagnostics(void)
{
    start("1");
    JceOffscreenResourceIdentity id = jce_offscreen_identity_empty();
    id.serial = UINT64_MAX; id.capture_generation = UINT32_MAX;
    id.width = id.height = UINT32_MAX;
    id.color.idx = id.depth.idx = UINT16_MAX;
    id.color_format = (bgfx_texture_format_t)UINT32_MAX;
    target->view_id = UINT16_MAX;
    target->resource_diag_event = UINT64_MAX - 1;
    s_device.offscreen_submission_index = UINT32_MAX;
    /* Formatter boundary only: no allocation or view port consumes these
     * maximum-width values, so no invalid handle access or huge texture occurs. */
    resource_diag(target, &id, "rollback_destroy_submitted", UINT32_MAX,
        UINT32_MAX, UINT32_MAX, true, true, UINT16_MAX, UINT16_MAX, UINT16_MAX,
        UINT32_MAX, UINT16_MAX);
    TEST_ASSERT_EQUAL_UINT(1, record_count); TEST_ASSERT_EQUAL_UINT(2, message_count);
    TEST_ASSERT_EQUAL_UINT(455, max_message_size);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, field(records[0], "event_ordinal"));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, field(records[0], "creation_serial"));
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, field(records[0], "origin_capture_generation"));
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, field(records[0], "allocated_width"));
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, field(records[0], "color_format"));
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, field(records[0], "requested_height"));
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, field(records[0], "submission_ordinal"));
    TEST_ASSERT_EQUAL_UINT64(UINT16_MAX, field(records[0], "forwarded_rect_height"));
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, field(records[0], "reason"));
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, field(records[0], "destroy_kind"));
    TEST_ASSERT_EQUAL_UINT64(UINT16_MAX, field(records[0], "destroy_handle"));
    resource_diag(target, &id, "destroy_submitted", 0, 0, 0, false, false, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT(1, record_count); TEST_ASSERT_EQUAL_UINT(3, message_count);
    TEST_ASSERT_EQUAL_UINT(1, exhausted_count); TEST_ASSERT_FALSE(target->resource_diag);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, target->resource_diag_event);
    resource_diag(target, &id, "destroy_submitted", 0, 0, 0, false, false, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT(3, message_count); TEST_ASSERT_EQUAL_UINT(0, rect_calls);
    TEST_ASSERT_EQUAL_UINT(0, texture_calls); no_trace_events();
}
/* A configure packet is scoped to the actual successful view service, not
 * another appearance of its resource identity inside create/retire/cleanup. */
enum { CONFIGURE_FIELDS = 22, CONFIGURE_CALLS = 16 };
static const char *const configure_fields[CONFIGURE_FIELDS] = {
    "offscreen.creation_serial", "offscreen.origin_capture_generation",
    "offscreen.current_capture_generation", "offscreen.origin_observed",
    "offscreen.origin_same_capture", "offscreen.origin_capture_gap",
    "offscreen.view_id", "offscreen.width", "offscreen.height",
    "offscreen.color_format", "offscreen.depth_format", "offscreen.samples",
    "offscreen.fbo_handle", "offscreen.color_handle", "offscreen.depth_handle",
    "offscreen.logical_bytes", "offscreen.requested_logical_bytes",
    "offscreen.retired_ordinal", "offscreen.resource_reason",
    "offscreen.forwarded_rect_width", "offscreen.forwarded_rect_height",
    "offscreen.submission_ordinal"
};
typedef struct ConfigureExpected {
    JceTraceTaskState state;
    uint64_t parent, binding_watermark, values[CONFIGURE_FIELDS];
} ConfigureExpected;

static ConfigureExpected configured(uint64_t parent, uint16_t view,
    uint16_t width, uint16_t height, bgfx_texture_format_t format,
    uint32_t ordinal, uint32_t origin, bool birth_observed, bool same_capture)
{
    ConfigureExpected expected = {0};
    expected.state = JCE_TRACE_TASK_SUCCEEDED; expected.parent = parent;
    TEST_ASSERT_LESS_THAN_UINT(VIEWS, view);
    TEST_ASSERT_TRUE(views[view].rect_returned); TEST_ASSERT_TRUE(views[view].fbo_returned);
    TEST_ASSERT_EQUAL_UINT16(width, views[view].width);
    TEST_ASSERT_EQUAL_UINT16(height, views[view].height);
    const uint16_t fbo = views[view].fbo;
    TEST_ASSERT_LESS_THAN_UINT16(HANDLES, fbo); TEST_ASSERT_TRUE(framebuffers[fbo]);
    for (unsigned a = 0; a < 2; ++a) {
        const uint16_t texture = attachments[fbo][a];
        TEST_ASSERT_LESS_THAN_UINT16(HANDLES, texture);
        TEST_ASSERT_EQUAL_UINT16(width, texture_metadata[texture].width);
        TEST_ASSERT_EQUAL_UINT16(height, texture_metadata[texture].height);
        TEST_ASSERT_EQUAL_INT(a == 0 ? format : BGFX_TEXTURE_FORMAT_D24S8,
            texture_metadata[texture].format);
        /* Exact actual allocator flags: no MSAA RT flag, independently of
         * the literal one-sample expectation in the trace packet below. */
        TEST_ASSERT_EQUAL_UINT64(BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP |
            BGFX_SAMPLER_V_CLAMP, texture_metadata[texture].flags);
    }
    const uint64_t bytes = (uint64_t)width * height *
        (format == BGFX_TEXTURE_FORMAT_RGBA16F ? 12u : 8u);
    const uint64_t values[CONFIGURE_FIELDS] = {
        target->identity.serial, origin, jce_trace_capture_generation(),
        birth_observed ? 1u : 0u, same_capture ? 1u : 0u, same_capture ? 0u : 1u,
        view, width, height, (uint64_t)format, BGFX_TEXTURE_FORMAT_D24S8, 1u,
        fbo, attachments[fbo][0], attachments[fbo][1], bytes, bytes, 0u,
        JCE_OFFSCREEN_REASON_CREATE, width, height, ordinal
    };
    TEST_ASSERT_NOT_EQUAL(0, values[0]);
    memcpy(expected.values, values, sizeof values);
    expected.binding_watermark = views[view].binding_watermark;
    return expected;
}
static ConfigureExpected refused(uint64_t parent, JceTraceTaskState state)
{
    ConfigureExpected expected = {0};
    expected.parent = parent; expected.state = state; return expected;
}

static void configure_packets(const ConfigureExpected *expected, unsigned expected_count)
{
    struct {
        uint64_t id, thread, began, last;
        unsigned fields;
        bool begun, closed;
    } packets[CONFIGURE_CALLS] = {{0}};
    TEST_ASSERT_TRUE(expected_count <= CONFIGURE_CALLS);
    unsigned submitted = 0, closed = 0;
    uint64_t read_events = 0;
    JceTraceCursor cursor; JceTraceEvent events[32];
    jce_trace_cursor_init(&cursor);
    uint32_t count;
    while ((count = jce_trace_read(&cursor, events, 32)) != 0u) {
        read_events += count;
        for (uint32_t i = 0; i < count; ++i) {
            const JceTraceEvent *event = &events[i];
            if (strcmp(event->name, "renderer.view_configure") == 0) {
                TEST_ASSERT_EQUAL_INT(JCE_TRACE_CATEGORY_TASK, event->category);
                if (event->type == JCE_TRACE_EVENT_TASK_SUBMIT) {
                    TEST_ASSERT_LESS_THAN_UINT(expected_count, submitted);
                    TEST_ASSERT_EQUAL_UINT(submitted, closed);
                    TEST_ASSERT_NOT_EQUAL(0, event->id);
                    packets[submitted].id = event->id;
                    packets[submitted].thread = event->thread_id;
                    TEST_ASSERT_EQUAL_UINT64(expected[submitted].parent, event->parent_id);
                    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_SERVICE, event->value_u32[0]);
                    ++submitted;
                    continue;
                }
                TEST_ASSERT_LESS_THAN_UINT(submitted, closed);
                TEST_ASSERT_EQUAL_UINT64(packets[closed].id, event->id);
                TEST_ASSERT_EQUAL_UINT64(event->id, event->parent_id);
                TEST_ASSERT_EQUAL_UINT64(packets[closed].thread, event->thread_id);
                if (event->type == JCE_TRACE_EVENT_TASK_BEGIN) {
                    TEST_ASSERT_FALSE(packets[closed].begun);
                    packets[closed].begun = true;
                    packets[closed].began = packets[closed].last = event->timestamp_ns;
                } else {
                    TEST_ASSERT_EQUAL_INT(JCE_TRACE_EVENT_TASK_END, event->type);
                    TEST_ASSERT_TRUE(packets[closed].begun); TEST_ASSERT_FALSE(packets[closed].closed);
                    TEST_ASSERT_TRUE(event->timestamp_ns >= packets[closed].last);
                    TEST_ASSERT_EQUAL_UINT32(expected[closed].state, event->value_u32[0]);
                    TEST_ASSERT_EQUAL_UINT_MESSAGE(expected[closed].state == JCE_TRACE_TASK_SUCCEEDED
                        ? CONFIGURE_FIELDS : 0u, packets[closed].fields,
                        "configure packet requires all 22 owner fields only on success");
                    packets[closed].closed = true; ++closed;
                }
                continue;
            }
            if (event->type != JCE_TRACE_EVENT_COUNTER ||
                strncmp(event->name, "offscreen.", 10) != 0) continue;
            unsigned packet = 0;
            while (packet < submitted && packets[packet].id != event->parent_id) ++packet;
            if (packet == submitted) {
                TEST_ASSERT_TRUE_MESSAGE(strcmp(event->name, "offscreen.forwarded_rect_width") != 0 &&
                    strcmp(event->name, "offscreen.forwarded_rect_height") != 0,
                    "forwarded rectangle counter has no configure service owner");
                continue; /* An actual nested resource lifecycle packet. */
            }
            TEST_ASSERT_EQUAL_UINT(closed, packet);
            TEST_ASSERT_TRUE(packets[packet].begun); TEST_ASSERT_FALSE(packets[packet].closed);
            TEST_ASSERT_EQUAL_INT(JCE_TRACE_TASK_SUCCEEDED, expected[packet].state);
            TEST_ASSERT_EQUAL_INT(JCE_TRACE_CATEGORY_RENDER, event->category);
            TEST_ASSERT_EQUAL_UINT64(expected[packet].values[0], event->id);
            TEST_ASSERT_EQUAL_UINT64(packets[packet].thread, event->thread_id);
            TEST_ASSERT_TRUE(event->timestamp_ns >= packets[packet].began &&
                event->timestamp_ns >= packets[packet].last);
            TEST_ASSERT_TRUE_MESSAGE(event->sequence > expected[packet].binding_watermark,
                "configure fields must follow the actual returned framebuffer binding call");
            const unsigned field_index = packets[packet].fields;
            TEST_ASSERT_LESS_THAN_UINT(CONFIGURE_FIELDS, field_index);
            TEST_ASSERT_EQUAL_STRING(configure_fields[field_index], event->name);
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(expected[packet].values[field_index],
                event->value_u64, event->name);
            packets[packet].last = event->timestamp_ns; ++packets[packet].fields;
        }
    }
    TEST_ASSERT_EQUAL_UINT(expected_count, submitted); TEST_ASSERT_EQUAL_UINT(expected_count, closed);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    /* Cursor init starts at the oldest resident event; this equality also
     * rejects a prefix already overwritten before reader initialization. */
    TEST_ASSERT_EQUAL_UINT64(stats.recorded_events, read_events);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_waits);
}

static void test_trace_main_overlay_repeated_configures_publish_actual_complete_packets(void)
{
    start("1"); jce_trace_set_enabled(true);
    const uint32_t origin = jce_trace_capture_generation();
    ConfigureExpected expected[5];
    JceTraceScope parent = jce_trace_scope_begin("configure.owner");
    TEST_ASSERT_TRUE(prepare(1, 9000));
    expected[0] = configured(parent.id, 30, 16, 8192, BGFX_TEXTURE_FORMAT_RGBA16F, 0, origin, true, true);
    const uint64_t serial = expected[0].values[0];
    const unsigned logs = record_count, messages = message_count;
    TEST_ASSERT_EQUAL_UINT(2, logs); TEST_ASSERT_EQUAL_UINT(4, messages);
    TEST_ASSERT_EQUAL_UINT(2, texture_calls); TEST_ASSERT_EQUAL_UINT(1, framebuffer_create_calls);
    s_device.offscreen_submission_index = 7;
    TEST_ASSERT_TRUE(prepare(2, 9001));
    expected[1] = configured(parent.id, 30, 16, 8192, BGFX_TEXTURE_FORMAT_RGBA16F, 7, origin, true, true);
    s_device.offscreen_submission_index = UINT32_MAX;
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare_keep(target, 16, 8192,
        identity, identity, 0, true, true, "keep"));
    expected[2] = configured(parent.id, 30, 16, 8192, BGFX_TEXTURE_FORMAT_RGBA16F,
        UINT32_MAX, origin, true, true);
    s_device.offscreen_submission_index = 0; /* Actual uint32 provider after wrap. */
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare_overlay_view(target, 31, identity, identity, "overlay"));
    expected[3] = configured(parent.id, 31, 16, 8192, BGFX_TEXTURE_FORMAT_RGBA16F, 0, origin, true, true);
    for (unsigned i = 1; i < 4; ++i) TEST_ASSERT_EQUAL_UINT64(serial, expected[i].values[0]);
    TEST_ASSERT_EQUAL_UINT(logs, record_count); TEST_ASSERT_EQUAL_UINT(messages, message_count);
    TEST_ASSERT_EQUAL_UINT(2, texture_calls); TEST_ASSERT_EQUAL_UINT(1, framebuffer_create_calls);
    TEST_ASSERT_EQUAL_UINT(4, rect_calls); TEST_ASSERT_EQUAL_UINT(4, fbo_calls);
    TEST_ASSERT_EQUAL_UINT(1, environment_reads);
    TEST_ASSERT_TRUE(prepare(32, 24));
    expected[4] = configured(parent.id, 30, 32, 24, BGFX_TEXTURE_FORMAT_RGBA16F, 0, origin, true, true);
    TEST_ASSERT_NOT_EQUAL(serial, expected[4].values[0]);
    TEST_ASSERT_EQUAL_UINT(4, texture_calls); TEST_ASSERT_EQUAL_UINT(2, framebuffer_create_calls);
    TEST_ASSERT_EQUAL_UINT64(parent.id, jce_trace_task_current_id());
    jce_trace_scope_end(parent, JCE_TRACE_TASK_SUCCEEDED);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
    configure_packets(expected, 5);
}

static void test_trace_fallback_keeps_unobserved_birth_explicit(void)
{
    start(NULL); caps.formats[BGFX_TEXTURE_FORMAT_RGBA16F] = 0;
    const uint32_t origin = jce_trace_capture_generation();
    TEST_ASSERT_TRUE(prepare(64, 48)); no_trace_events();
    jce_trace_set_enabled(true);
    JceTraceScope parent = jce_trace_scope_begin("configure.owner");
    TEST_ASSERT_TRUE(prepare(64, 48));
    ConfigureExpected expected = configured(parent.id, 30, 64, 48,
        BGFX_TEXTURE_FORMAT_RGBA8, 0, origin, false, false);
    TEST_ASSERT_EQUAL_UINT64(parent.id, jce_trace_task_current_id());
    jce_trace_scope_end(parent, JCE_TRACE_TASK_SUCCEEDED);
    configure_packets(&expected, 1);
}
static void test_trace_capture_reset_preserves_resource_origin_and_current_generation(void)
{
    start(NULL); jce_trace_set_enabled(true);
    const uint32_t origin = jce_trace_capture_generation();
    TEST_ASSERT_TRUE(prepare(64, 48));
    ConfigureExpected first = configured(0, 30, 64, 48, BGFX_TEXTURE_FORMAT_RGBA16F, 0, origin, true, true);
    configure_packets(&first, 1);
    jce_trace_reset();
    TEST_ASSERT_NOT_EQUAL(origin, jce_trace_capture_generation());
    TEST_ASSERT_TRUE(prepare(64, 48));
    ConfigureExpected second = configured(0, 30, 64, 48, BGFX_TEXTURE_FORMAT_RGBA16F, 0, origin, true, false);
    TEST_ASSERT_EQUAL_UINT64(first.values[0], second.values[0]);
    configure_packets(&second, 1);
}

static void test_trace_refused_main_and_overlay_entries_never_publish_success_fields(void)
{
    start(NULL); jce_trace_set_enabled(true);
    JceTraceScope parent = jce_trace_scope_begin("configure.owner");
    ConfigureExpected expected[9];
    for (unsigned i = 0; i < 9; ++i) expected[i] = refused(parent.id, JCE_TRACE_TASK_FAILED);
    expected[8].state = JCE_TRACE_TASK_CANCELLED;
    fail_texture_call = texture_calls + 1;
    TEST_ASSERT_FALSE(prepare(64, 48));
    fail_texture_call = texture_calls + 2;
    TEST_ASSERT_FALSE(prepare(64, 48));
    fail_texture_call = 0; fail_fbo = true;
    TEST_ASSERT_FALSE(prepare(64, 48));
    fail_fbo = false; fail_query = true;
    TEST_ASSERT_FALSE(prepare(64, 48));
    fail_query = false;
    TEST_ASSERT_FALSE(jce_offscreen_target_prepare_keep(target, 64, 48, NULL, identity, 0, false, false, NULL));
    TEST_ASSERT_FALSE(jce_offscreen_target_prepare_keep(target, 64, 48, identity, NULL, 0, false, false, NULL));
    TEST_ASSERT_FALSE(jce_offscreen_target_prepare_overlay_view(target, 31, identity, identity, NULL));
    TEST_ASSERT_FALSE(prepare(0, 48));
    TEST_ASSERT_FALSE(jce_offscreen_target_prepare(NULL, 64, 48, identity, identity, 0, NULL));
    TEST_ASSERT_EQUAL_UINT(0, rect_calls); TEST_ASSERT_EQUAL_UINT(0, fbo_calls);
    TEST_ASSERT_EQUAL_UINT64(parent.id, jce_trace_task_current_id());
    jce_trace_scope_end(parent, JCE_TRACE_TASK_SUCCEEDED);
    configure_packets(expected, 9);
}
static void test_trace_resize_refusal_retains_identity_without_a_false_configure_packet(void)
{
    start(NULL); jce_trace_set_enabled(true);
    const uint32_t origin = jce_trace_capture_generation();
    ConfigureExpected expected[13];
    unsigned count = 0;
    JceTraceScope parent = jce_trace_scope_begin("configure.owner");
    TEST_ASSERT_TRUE(prepare(32, 32));
    expected[count++] = configured(parent.id, 30, 32, 32, BGFX_TEXTURE_FORMAT_RGBA16F, 0, origin, true, true);
    for (unsigned i = 0; i < JCE_OFFSCREEN_RETIRE_DELAY; ++i) {
        TEST_ASSERT_TRUE(prepare(40 + i, 40 + i));
        expected[count++] = configured(parent.id, 30, (uint16_t)(40 + i), (uint16_t)(40 + i),
            BGFX_TEXTURE_FORMAT_RGBA16F, 0, origin, true, true);
    }
    const uint64_t serial = target->identity.serial;
    const unsigned rects = rect_calls, bindings = fbo_calls;
    TEST_ASSERT_FALSE(prepare(200, 100)); expected[count++] = refused(parent.id, JCE_TRACE_TASK_FAILED);
    TEST_ASSERT_EQUAL_UINT(rects, rect_calls); TEST_ASSERT_EQUAL_UINT(bindings, fbo_calls);
    TEST_ASSERT_EQUAL_UINT64(serial, target->identity.serial);
    TEST_ASSERT_TRUE(prepare(47, 47));
    expected[count++] = configured(parent.id, 30, 47, 47, BGFX_TEXTURE_FORMAT_RGBA16F, 0, origin, true, true);
    TEST_ASSERT_EQUAL_UINT64(serial, expected[count - 1].values[0]);
    jce_offscreen_retirement_shutdown(&renderer);
    const unsigned detached_rects = rect_calls, detached_bindings = fbo_calls;
    TEST_ASSERT_FALSE(prepare(47, 47)); expected[count++] = refused(parent.id, JCE_TRACE_TASK_FAILED);
    TEST_ASSERT_FALSE(jce_offscreen_target_prepare_overlay_view(target, 31, identity, identity, NULL));
    expected[count++] = refused(parent.id, JCE_TRACE_TASK_FAILED);
    TEST_ASSERT_EQUAL_UINT(detached_rects, rect_calls); TEST_ASSERT_EQUAL_UINT(detached_bindings, fbo_calls);
    TEST_ASSERT_EQUAL_UINT64(parent.id, jce_trace_task_current_id());
    jce_trace_scope_end(parent, JCE_TRACE_TASK_SUCCEEDED);
    configure_packets(expected, count);
}
static void test_suppressed_trace_keeps_main_overlay_calls_and_emits_no_packet(void)
{
    start(NULL); jce_trace_set_enabled(true);
    trace_suppressed = jce_trace_suppress_begin(); TEST_ASSERT_TRUE(trace_suppressed);
    const bool prepared = prepare(64, 48);
    const bool overlay = jce_offscreen_target_prepare_overlay_view(target, 31, identity, identity, NULL);
    const bool ended = jce_trace_suppress_end(); trace_suppressed = false;
    TEST_ASSERT_TRUE(ended); TEST_ASSERT_TRUE(prepared); TEST_ASSERT_TRUE(overlay);
    TEST_ASSERT_EQUAL_UINT(2, rect_calls); TEST_ASSERT_EQUAL_UINT(2, fbo_calls);
    TEST_ASSERT_EQUAL_UINT(2, texture_calls); TEST_ASSERT_EQUAL_UINT(1, framebuffer_create_calls);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id()); no_trace_events();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_off_and_session_read_once_without_steady_environment_reads);
    RUN_TEST(test_admission_refusal_and_existing_flag_policy);
    RUN_TEST(test_trace_off_clamped_allocation_and_actual_forwarded_tuple_are_distinct);
    RUN_TEST(test_resize_identity_destroy_and_same_tuple_reuse);
    RUN_TEST(test_failures_never_publish_committed_allocation_or_view);
    RUN_TEST(test_retirement_saturation_keeps_current_without_false_resize_or_view);
    RUN_TEST(test_texture_failure_cannot_publish_or_forward);
    RUN_TEST(test_rgba8_fallback_and_reused_handles_have_new_allocation_identity);
    RUN_TEST(test_maximum_fields_fit_actual_logger_and_event_exhaustion_closes_diagnostics);
    RUN_TEST(test_trace_main_overlay_repeated_configures_publish_actual_complete_packets);
    RUN_TEST(test_trace_fallback_keeps_unobserved_birth_explicit);
    RUN_TEST(test_trace_capture_reset_preserves_resource_origin_and_current_generation);
    RUN_TEST(test_trace_refused_main_and_overlay_entries_never_publish_success_fields);
    RUN_TEST(test_trace_resize_refusal_retains_identity_without_a_false_configure_packet);
    RUN_TEST(test_suppressed_trace_keeps_main_overlay_calls_and_emits_no_packet);
    return UNITY_END();
}
