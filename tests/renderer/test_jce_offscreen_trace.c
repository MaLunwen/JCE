#include "unity.h"

#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_render_readback.h>
#include <jce/os/core/jce_trace.h>
#include "os/core/jce_trace_scope_internal.h"
#include "renderer/jce_offscreen_retirement_internal.h"
#ifndef JCE_TEST_OFFSCREEN_TARGET_INTERNAL_HEADER
#define JCE_TEST_OFFSCREEN_TARGET_INTERNAL_HEADER "renderer/jce_offscreen_target_internal.h"
#endif
#include JCE_TEST_OFFSCREEN_TARGET_INTERNAL_HEADER
#include <string.h>

static JceRenderer *renderer;
static JceOffscreenTarget *bridge;
static JceOffscreenTarget *controlled_bridge;
static bool isolated_normalization;
static const float identity[16] = {
    1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1
};

static void assert_configure_rectangle_count(unsigned expected);

/* Counted immutable per-target GPU ports execute the production allocation
 * and retirement owner. These handles never enter actual Noop view submission. */
enum { CONTROL_HANDLES = 64 };
typedef struct OffscreenControls {
    bgfx_caps_t caps;
    bool textures[CONTROL_HANDLES], fbos[CONTROL_HANDLES];
    bool texture_owned[CONTROL_HANDLES];
    unsigned texture_refs[CONTROL_HANDLES];
    uint16_t attachments[CONTROL_HANDLES][2];
    uint8_t attachment_counts[CONTROL_HANDLES];
    uint16_t fbo_destroy_order[CONTROL_HANDLES];
    unsigned texture_calls, fbo_calls, texture_destroys, fbo_destroys;
    unsigned creator_releases, fbo_ref_acquires, fbo_ref_releases, raw_take_ownership;
    unsigned raw_ownership_requests;
    unsigned fail_texture;
    bool fail_fbo, fail_fbo_validation, fail_query, fail_alias;
} OffscreenControls;
static OffscreenControls controls;

static const bgfx_caps_t *controlled_caps(void *user)
{ return &((OffscreenControls *)user)->caps; }

static bgfx_texture_handle_t controlled_texture(uint16_t width, uint16_t height,
    bgfx_texture_format_t format, void *user)
{
    OffscreenControls *c = (OffscreenControls *)user;
    TEST_ASSERT_TRUE(width >= 16u && width <= 8192u);
    TEST_ASSERT_TRUE(height >= 16u && height <= 8192u);
    TEST_ASSERT_TRUE(format == BGFX_TEXTURE_FORMAT_RGBA16F ||
                     format == BGFX_TEXTURE_FORMAT_RGBA8 || format == BGFX_TEXTURE_FORMAT_D24S8);
    if (++c->texture_calls == c->fail_texture) return (bgfx_texture_handle_t){UINT16_MAX};
    for (uint16_t i = 0; i < CONTROL_HANDLES; ++i)
        if (!c->textures[i]) {
            c->textures[i] = true; c->texture_refs[i] = 1u; c->texture_owned[i] = false;
            return (bgfx_texture_handle_t){i};
        }
    TEST_FAIL_MESSAGE("counted texture pool unexpectedly exhausted");
    return (bgfx_texture_handle_t){UINT16_MAX};
}

static void controlled_texture_unref(bgfx_texture_handle_t handle, OffscreenControls *c)
{
    TEST_ASSERT_LESS_THAN_UINT16(CONTROL_HANDLES, handle.idx);
    TEST_ASSERT_TRUE(c->textures[handle.idx]);
    TEST_ASSERT_GREATER_THAN_UINT(0, c->texture_refs[handle.idx]);
    if (--c->texture_refs[handle.idx] == 0u) {
        c->textures[handle.idx] = false; ++c->texture_destroys;
    }
}

/* Match pinned bgfx's reference boundary, including the surprising allocation
 * failure: after validation, destroyTextures takes creator ownership even if
 * no FBO handle could be allocated. A validation failure takes no ownership. */
static bgfx_frame_buffer_handle_t controlled_create_from_attachment(uint8_t count,
    const bgfx_attachment_t *attachments, bool destroy_textures, void *user)
{
    OffscreenControls *c = (OffscreenControls *)user;
    ++c->fbo_calls;
    TEST_ASSERT_TRUE(count == 1 || count == 2);
    if (count == 1) {
        TEST_ASSERT_FALSE(destroy_textures);
        TEST_ASSERT_EQUAL_INT(BGFX_ACCESS_WRITE, attachments[0].access);
        TEST_ASSERT_EQUAL_UINT16(0, attachments[0].mip);
        TEST_ASSERT_EQUAL_UINT16(0, attachments[0].layer);
        TEST_ASSERT_EQUAL_UINT16(1, attachments[0].numLayers);
        TEST_ASSERT_EQUAL_UINT8(BGFX_RESOLVE_NONE, attachments[0].resolve);
        if (c->fail_alias) return (bgfx_frame_buffer_handle_t){UINT16_MAX};
    }
    for (unsigned i = 0; i < count; ++i) {
        TEST_ASSERT_LESS_THAN_UINT16(CONTROL_HANDLES, attachments[i].handle.idx);
        TEST_ASSERT_TRUE(c->textures[attachments[i].handle.idx]);
    }
    if (c->fail_fbo_validation) return (bgfx_frame_buffer_handle_t){UINT16_MAX};
    bgfx_frame_buffer_handle_t fbo = {UINT16_MAX};
    if (!c->fail_fbo) {
        for (uint16_t i = 0; i < CONTROL_HANDLES; ++i)
            if (!c->fbos[i]) {
                c->fbos[i] = true; fbo.idx = i;
                c->attachment_counts[i] = count;
                for (unsigned a = 0; a < count; ++a) {
                    const uint16_t texture = attachments[a].handle.idx;
                    c->attachments[i][a] = texture;
                    ++c->texture_refs[texture]; ++c->fbo_ref_acquires;
                }
                break;
            }
        TEST_ASSERT_NOT_EQUAL(UINT16_MAX, fbo.idx);
    }
    if (destroy_textures) {
        ++c->raw_ownership_requests;
        for (unsigned i = 0; i < count; ++i) {
            const bgfx_texture_handle_t texture = attachments[i].handle;
            if (!c->texture_owned[texture.idx]) {
                c->texture_owned[texture.idx] = true; ++c->raw_take_ownership;
                controlled_texture_unref(texture, c);
            }
        }
    }
    return fbo;
}

static void controlled_destroy_texture(bgfx_texture_handle_t handle, void *user)
{
    OffscreenControls *c = (OffscreenControls *)user;
    ++c->creator_releases;
    controlled_texture_unref(handle, c);
}

static bgfx_frame_buffer_handle_t controlled_fbo(const bgfx_texture_handle_t *textures, void *user)
{
    const JceOffscreenFrameBufferProvider provider = {
        controlled_create_from_attachment, controlled_destroy_texture, user
    };
    return jce_offscreen_frame_buffer_create_normalized(textures, &provider);
}

static bgfx_texture_handle_t controlled_get_texture(bgfx_frame_buffer_handle_t fbo,
    uint8_t attachment, void *user)
{
    OffscreenControls *c = (OffscreenControls *)user;
    TEST_ASSERT_LESS_THAN_UINT16(CONTROL_HANDLES, fbo.idx);
    TEST_ASSERT_TRUE(c->fbos[fbo.idx]);
    TEST_ASSERT_LESS_THAN_UINT8(c->attachment_counts[fbo.idx], attachment);
    return (bgfx_texture_handle_t){(uint16_t)(c->fail_query ? UINT16_MAX : c->attachments[fbo.idx][attachment])};
}

static void controlled_destroy_fbo(bgfx_frame_buffer_handle_t handle, void *user)
{
    OffscreenControls *c = (OffscreenControls *)user;
    TEST_ASSERT_LESS_THAN_UINT16(CONTROL_HANDLES, handle.idx); TEST_ASSERT_TRUE(c->fbos[handle.idx]);
    TEST_ASSERT_LESS_THAN_UINT(CONTROL_HANDLES, c->fbo_destroys);
    c->fbo_destroy_order[c->fbo_destroys++] = handle.idx;
    c->fbos[handle.idx] = false;
    for (unsigned i = 0; i < c->attachment_counts[handle.idx]; ++i) {
        ++c->fbo_ref_releases;
        controlled_texture_unref((bgfx_texture_handle_t){c->attachments[handle.idx][i]}, c);
    }
}

static const JceOffscreenTargetProvider controlled_provider = {
    controlled_caps, controlled_texture, controlled_fbo, controlled_get_texture,
    controlled_destroy_texture, controlled_destroy_fbo, &controls,
    controlled_create_from_attachment
};

static void assert_control_resources_closed(void)
{
    for (unsigned i = 0; i < CONTROL_HANDLES; ++i) {
        TEST_ASSERT_FALSE(controls.textures[i]); TEST_ASSERT_FALSE(controls.fbos[i]);
        TEST_ASSERT_EQUAL_UINT(0, controls.texture_refs[i]);
    }
}

static void create_controlled(void)
{
    controlled_bridge = jce_offscreen_target_create_with_provider(renderer, 14, &controlled_provider);
    TEST_ASSERT_NOT_NULL(controlled_bridge);
}

static void close_controlled(void)
{
    jce_offscreen_target_destroy(controlled_bridge); controlled_bridge = NULL;
}

void setUp(void)
{
    memset(&controls, 0, sizeof controls);
    controls.caps.formats[BGFX_TEXTURE_FORMAT_RGBA16F] = BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER;
    controlled_bridge = NULL;
    if (isolated_normalization) {
        renderer = NULL; bridge = NULL;
        return;
    }
    jce_trace_set_enabled(false);
    renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
    bridge = jce_offscreen_target_create(renderer, 10);
    TEST_ASSERT_NOT_NULL(bridge);
    jce_trace_reset();
    jce_trace_set_enabled(true);
}

void tearDown(void)
{
    if (isolated_normalization) {
        assert_control_resources_closed();
        return;
    }
    jce_trace_set_enabled(false);
    close_controlled();
    jce_offscreen_target_destroy(bridge);
    jce_renderer_destroy(renderer);
    assert_control_resources_closed();
}

static unsigned view_ends(JceTraceTaskState expected)
{
    JceTraceCursor cursor;
    JceTraceEvent events[32];
    unsigned matches = 0;
    jce_trace_cursor_init(&cursor);
    uint32_t count;
    while ((count = jce_trace_read(&cursor, events, 32)) != 0u) {
        for (uint32_t i = 0; i < count; ++i) {
            if (events[i].type != JCE_TRACE_EVENT_TASK_END ||
                strcmp(events[i].name, "renderer.view_configure")) continue;
            TEST_ASSERT_EQUAL_UINT32(expected, events[i].value_u32[0]);
            ++matches;
        }
    }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    return matches;
}

static void test_delegating_prepare_has_one_success_and_restores_parent(void)
{
    JceTraceScope owner = jce_trace_scope_begin("owner");
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 64, 64, identity, identity, 0, "test"));
    TEST_ASSERT_EQUAL_UINT64(owner.id, jce_trace_task_current_id());
    jce_trace_scope_end(owner, JCE_TRACE_TASK_SUCCEEDED);
    TEST_ASSERT_EQUAL_UINT32(1, view_ends(JCE_TRACE_TASK_SUCCEEDED));
    assert_configure_rectangle_count(1);
}

static void test_keep_and_overlay_configure_actual_target(void)
{
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare_keep(bridge, 64, 64, identity, identity, 0, true, true, "keep"));
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare_overlay_view(bridge, 11, identity, identity, "overlay"));
    TEST_ASSERT_EQUAL_UINT32(2, view_ends(JCE_TRACE_TASK_SUCCEEDED));
    assert_configure_rectangle_count(2);
}

static void assert_extent(const JceOffscreenTarget *target, uint32_t expected_w,
                          uint32_t expected_h)
{
    uint32_t width = UINT32_MAX, height = UINT32_MAX;
    const bool present = jce_offscreen_target_get_extent(target, &width, &height);
    TEST_ASSERT_EQUAL(expected_w != 0u && expected_h != 0u, present);
    TEST_ASSERT_EQUAL_UINT32(expected_w, width);
    TEST_ASSERT_EQUAL_UINT32(expected_h, height);
}

static void test_extent_query_reports_only_live_clamped_owner_dimensions(void)
{
    assert_extent(NULL, 0, 0);
    assert_extent(bridge, 0, 0);
    create_controlled();
    assert_extent(controlled_bridge, 0, 0);
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 1, 9000));
    assert_extent(controlled_bridge, 16, 8192);
    const unsigned texture_calls = controls.texture_calls;
    const unsigned fbo_calls = controls.fbo_calls;
    JceTraceStats before, after;
    jce_trace_get_stats(&before);
    assert_extent(controlled_bridge, 16, 8192);
    jce_trace_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT(texture_calls, controls.texture_calls);
    TEST_ASSERT_EQUAL_UINT(fbo_calls, controls.fbo_calls);
    TEST_ASSERT_EQUAL_UINT64(before.recorded_events, after.recorded_events);
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 96, 48));
    assert_extent(controlled_bridge, 96, 48);
    TEST_ASSERT_FALSE(jce_offscreen_target_ensure_internal(controlled_bridge, 0, 48));
    assert_extent(controlled_bridge, 96, 48);

    uint32_t dimension = UINT32_MAX;
    TEST_ASSERT_FALSE(jce_offscreen_target_get_extent(controlled_bridge, NULL, &dimension));
    TEST_ASSERT_EQUAL_UINT32(0, dimension);
    dimension = UINT32_MAX;
    TEST_ASSERT_FALSE(jce_offscreen_target_get_extent(controlled_bridge, &dimension, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, dimension);
}

static void test_extent_query_follows_resize_failure_and_renderer_detach(void)
{
    create_controlled();
    controls.fail_fbo = true;
    TEST_ASSERT_FALSE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 48));
    assert_extent(controlled_bridge, 0, 0);
    controls.fail_fbo = false;
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 48));
    controls.fail_fbo = true;
    TEST_ASSERT_FALSE(jce_offscreen_target_ensure_internal(controlled_bridge, 96, 48));
    assert_extent(controlled_bridge, 0, 0);
    controls.fail_fbo = false;
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 96, 48));
    assert_extent(controlled_bridge, 96, 48);
    jce_renderer_destroy(renderer); renderer = NULL;
    assert_extent(controlled_bridge, 0, 0);
    assert_extent(bridge, 0, 0);
}

static void test_invalid_matrix_and_unprepared_overlay_record_failed(void)
{
    TEST_ASSERT_FALSE(jce_offscreen_target_prepare_keep(bridge, 64, 64, NULL, identity, 0, false, false, NULL));
    TEST_ASSERT_FALSE(jce_offscreen_target_prepare_overlay_view(bridge, 11, identity, identity, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, view_ends(JCE_TRACE_TASK_FAILED));
}

static void test_null_bridge_records_cancelled(void)
{
    TEST_ASSERT_FALSE(jce_offscreen_target_prepare(NULL, 64, 64, identity, identity, 0, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, view_ends(JCE_TRACE_TASK_CANCELLED));
}

static void test_capture_off_preserves_configuration_and_emits_nothing(void)
{
    jce_trace_set_enabled(false);
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare_keep(bridge, 64, 64, identity, identity, 0, false, false, NULL));
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
}

static void test_same_frame_saturation_preserves_current_until_eight_frames(void)
{
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 64, 64, identity, identity, 0, NULL));
    for (unsigned i = 0; i < 8; ++i)
        TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 65 + i, 64, identity, identity, 0, NULL));
    uint16_t current = jce_offscreen_target_get_frame_buffer(bridge);
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 72, 64, identity, identity, 0, NULL));
    TEST_ASSERT_EQUAL_UINT16(current, jce_offscreen_target_get_frame_buffer(bridge));
    for (unsigned i = 0; i < 20; ++i)
        TEST_ASSERT_FALSE(jce_offscreen_target_prepare(bridge, 90, 64, identity, identity, 0, NULL));
    TEST_ASSERT_EQUAL_UINT16(current, jce_offscreen_target_get_frame_buffer(bridge));
    assert_extent(bridge, 72, 64); /* Retire refusal retains this allocation. */
    for (unsigned i = 0; i < 7; ++i)
        jce_renderer_submit_frame(renderer);
    TEST_ASSERT_FALSE(jce_offscreen_target_prepare(bridge, 90, 64, identity, identity, 0, NULL));
    jce_renderer_submit_frame(renderer);
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 90, 64, identity, identity, 0, NULL));
    assert_extent(bridge, 90, 64);
}

static void test_idle_targets_collect_and_shutdown_detaches_wrapper(void)
{
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 64, 64, identity, identity, 0, NULL));
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 80, 64, identity, identity, 0, NULL));
    for (unsigned i = 0; i < 4; ++i)
        jce_renderer_submit_frame(renderer);
    JceGpuStats before, after;
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_stats(&before));
    for (unsigned i = 0; i < 8; ++i)
        jce_renderer_submit_frame(renderer);
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_stats(&after));
    TEST_ASSERT_TRUE(after.num_frame_buffers < before.num_frame_buffers);
    jce_renderer_destroy(renderer);
    renderer = NULL;
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, jce_offscreen_target_get_frame_buffer(bridge));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, jce_offscreen_target_get_color_texture(bridge));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, jce_offscreen_target_get_depth_texture(bridge));
    TEST_ASSERT_FALSE(jce_offscreen_target_is_hdr(bridge));
    TEST_ASSERT_FALSE(jce_offscreen_target_prepare(bridge, 80, 64, identity, identity, 0, NULL));
    TEST_ASSERT_FALSE(jce_offscreen_target_prepare_overlay_view(bridge, 11, identity, identity, NULL));
}

static void test_independent_targets_unlink_and_idle_collection(void)
{
    JceOffscreenTarget *middle = jce_offscreen_target_create(renderer, 12);
    JceOffscreenTarget *head = jce_offscreen_target_create(renderer, 13);
    TEST_ASSERT_NOT_NULL(middle);
    TEST_ASSERT_NOT_NULL(head);
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 64, 64, identity, identity, 0, NULL));
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(middle, 64, 64, identity, identity, 0, NULL));
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(head, 64, 64, identity, identity, 0, NULL));
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(middle, 80, 64, identity, identity, 0, NULL));
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(head, 80, 64, identity, identity, 0, NULL));
    for (unsigned i = 0; i < 4; ++i)
        jce_renderer_submit_frame(renderer);
    JceGpuStats before, after;
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_stats(&before));
    jce_offscreen_target_destroy(middle);
    for (unsigned i = 0; i < 8; ++i)
        jce_renderer_submit_frame(renderer);
    TEST_ASSERT_TRUE(jce_renderer_get_gpu_stats(&after));
    TEST_ASSERT_EQUAL_UINT16(before.num_frame_buffers - 3u, after.num_frame_buffers);
    TEST_ASSERT_NOT_EQUAL(UINT16_MAX, jce_offscreen_target_get_frame_buffer(head));
    TEST_ASSERT_NOT_EQUAL(UINT16_MAX, jce_offscreen_target_get_frame_buffer(bridge));
    jce_offscreen_target_destroy(head);
    JceOffscreenTarget *survivor = jce_offscreen_target_create(renderer, 14);
    TEST_ASSERT_NOT_NULL(survivor);
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(survivor, 64, 64, identity, identity, 0, NULL));
    jce_renderer_destroy(renderer);
    renderer = NULL;
    TEST_ASSERT_FALSE(jce_offscreen_target_prepare(survivor, 64, 64, identity, identity, 0, NULL));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, jce_offscreen_target_get_frame_buffer(survivor));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, jce_offscreen_target_get_frame_buffer(bridge));
    jce_offscreen_target_destroy(survivor);
}

static void test_new_device_restarts_retirement_clock(void)
{
    for (unsigned i = 0; i < 24; ++i)
        jce_renderer_submit_frame(renderer);
    jce_offscreen_target_destroy(bridge);
    jce_renderer_destroy(renderer);
    renderer = jce_renderer_create_headless();
    TEST_ASSERT_NOT_NULL(renderer);
    TEST_ASSERT_EQUAL_UINT32(0, jce_renderer_get_frame_index(renderer));
    bridge = jce_offscreen_target_create(renderer, 10);
    TEST_ASSERT_NOT_NULL(bridge);
    test_same_frame_saturation_preserves_current_until_eight_frames();
}

static void test_retirement_frame_wrap_and_boundary(void)
{
    TEST_ASSERT_FALSE(jce_offscreen_retirement_due(3u, UINT32_MAX - 3u));
    TEST_ASSERT_TRUE(jce_offscreen_retirement_due(4u, UINT32_MAX - 3u));
    TEST_ASSERT_FALSE(jce_offscreen_retirement_due(123u, 123u));
}

typedef struct TraceObservation {
    unsigned count;
    uint64_t id, parent, value;
    uint32_t state;
} TraceObservation;

/* Consume every resident batch, including tail events beyond the old 32-event
 * fixture. Missing/lost evidence fails; no shortened sample becomes success. */
static TraceObservation read_trace_events_matching(JceTraceEventType type, const char *name,
    uint64_t resource, bool prefix, bool match_value, uint64_t value)
{
    TraceObservation found = {0};
    JceTraceCursor cursor;
    JceTraceEvent events[32];
    jce_trace_cursor_init(&cursor);
    uint32_t count;
    while ((count = jce_trace_read(&cursor, events, 32)) != 0u)
        for (uint32_t i = 0; i < count; ++i) {
            const JceTraceEvent *event = &events[i];
            if (event->type != type || (resource && event->id != resource)) continue;
            if (match_value && event->value_u64 != value) continue;
            if (prefix ? strncmp(event->name, name, strlen(name)) != 0 : strcmp(event->name, name) != 0) continue;
            ++found.count; found.id = event->id; found.parent = event->parent_id;
            found.value = event->value_u64; found.state = event->value_u32[0];
        }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    return found;
}

static TraceObservation trace_events_matching(JceTraceEventType type, const char *name,
    uint64_t resource, bool prefix, bool match_value, uint64_t value)
{
    TraceObservation found = read_trace_events_matching(type, name, resource,
        prefix, match_value, value);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_waits);
    return found;
}

static TraceObservation trace_events(JceTraceEventType type, const char *name,
    uint64_t resource, bool prefix)
{ return trace_events_matching(type, name, resource, prefix, false, 0); }

static TraceObservation counter(const char *name, uint64_t resource)
{ return trace_events(JCE_TRACE_EVENT_COUNTER, name, resource, false); }

static void assert_configure_rectangle_count(unsigned expected)
{
    TEST_ASSERT_EQUAL_UINT(expected, counter("offscreen.forwarded_rect_width", 0).count);
    TEST_ASSERT_EQUAL_UINT(expected, counter("offscreen.forwarded_rect_height", 0).count);
}

static uint64_t resource_created(void)
{
    TraceObservation event = counter("offscreen.create_committed", 0);
    TEST_ASSERT_GREATER_THAN_UINT(0, event.count);
    TEST_ASSERT_NOT_EQUAL(0, event.id); TEST_ASSERT_NOT_EQUAL(0, event.parent);
    TEST_ASSERT_EQUAL_UINT64(1, event.value);
    return event.id;
}

static void counter_value(const char *name, uint64_t resource, uint64_t expected)
{
    TraceObservation event = counter(name, resource);
    TEST_ASSERT_GREATER_THAN_UINT(0, event.count);
    TEST_ASSERT_EQUAL_UINT64(expected, event.value);
}

static unsigned resource_scope_counter_count(uint64_t resource_scope)
{
    JceTraceCursor cursor; JceTraceEvent events[32];
    unsigned matches = 0; uint64_t read_events = 0;
    jce_trace_cursor_init(&cursor);
    uint32_t count;
    while ((count = jce_trace_read(&cursor, events, 32)) != 0u) {
        read_events += count;
        for (uint32_t i = 0; i < count; ++i)
            if (events[i].type == JCE_TRACE_EVENT_COUNTER &&
                events[i].parent_id == resource_scope &&
                strncmp(events[i].name, "offscreen.", 10) == 0) ++matches;
    }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(stats.recorded_events, read_events);
    return matches;
}

static void test_actual_creation_metadata_and_same_extent_has_no_lifecycle_events(void)
{
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 2, 64, identity, identity, 0, NULL));
    const uint64_t id = resource_created();
    const uint64_t pixel_bytes = jce_offscreen_target_is_hdr(bridge) ? 8u : 4u;
    counter_value("offscreen.width", id, 16); counter_value("offscreen.height", id, 64);
    counter_value("offscreen.samples", id, 1);
    counter_value("offscreen.logical_bytes", id, 16u * 64u * (pixel_bytes + 4u));
    counter_value("offscreen.fbo_handle", id, jce_offscreen_target_get_frame_buffer(bridge));
    counter_value("offscreen.color_handle", id, jce_offscreen_target_get_color_texture(bridge));
    counter_value("offscreen.depth_handle", id, jce_offscreen_target_get_depth_texture(bridge));
    counter_value("offscreen.origin_capture_generation", id, jce_trace_capture_generation());
    counter_value("offscreen.origin_same_capture", id, 1);
    const TraceObservation allocation = counter("offscreen.create_committed", id);
    const unsigned allocation_fields = resource_scope_counter_count(allocation.parent);
    const unsigned submits = trace_events(JCE_TRACE_EVENT_TASK_SUBMIT, "renderer.offscreen.", 0, true).count;
    const unsigned begins = trace_events(JCE_TRACE_EVENT_TASK_BEGIN, "renderer.offscreen.", 0, true).count;
    const unsigned ends = trace_events(JCE_TRACE_EVENT_TASK_END, "renderer.offscreen.", 0, true).count;
    const unsigned destroys = counter("offscreen.destroy_kind", id).count;
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.forwarded_rect_width", id).count);
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.forwarded_rect_height", id).count);
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 16, 64, identity, identity, 0, NULL));
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 2, 64, identity, identity, 0, NULL));
    TEST_ASSERT_EQUAL_UINT(allocation.count, counter("offscreen.create_committed", id).count);
    TEST_ASSERT_EQUAL_UINT(allocation_fields, resource_scope_counter_count(allocation.parent));
    TEST_ASSERT_EQUAL_UINT(submits, trace_events(JCE_TRACE_EVENT_TASK_SUBMIT, "renderer.offscreen.", 0, true).count);
    TEST_ASSERT_EQUAL_UINT(begins, trace_events(JCE_TRACE_EVENT_TASK_BEGIN, "renderer.offscreen.", 0, true).count);
    TEST_ASSERT_EQUAL_UINT(ends, trace_events(JCE_TRACE_EVENT_TASK_END, "renderer.offscreen.", 0, true).count);
    TEST_ASSERT_EQUAL_UINT(destroys, counter("offscreen.destroy_kind", id).count);
    TEST_ASSERT_EQUAL_UINT(3, counter("offscreen.forwarded_rect_width", id).count);
    TEST_ASSERT_EQUAL_UINT(3, counter("offscreen.forwarded_rect_height", id).count);
    counter_value("offscreen.forwarded_rect_width", id, 16);
    counter_value("offscreen.forwarded_rect_height", id, 64);
    TEST_ASSERT_EQUAL_UINT(3, view_ends(JCE_TRACE_TASK_SUCCEEDED));
}

static TraceObservation read_live_counter(const char *name, uint64_t resource)
{
    return read_trace_events_matching(JCE_TRACE_EVENT_COUNTER, name, resource, false, false, 0);
}

static void test_hidden_collection_and_shutdown_match_creation_once(void)
{
    /* The provider can keep its render-thread wait scope open between API
     * submissions. Check offscreen scopes here and require global quiescence
     * after the actual renderer joins the thread. */
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 64, 64, identity, identity, 0, NULL));
    const uint64_t old = read_live_counter("offscreen.create_committed", 0).id;
    const uint16_t old_fbo = jce_offscreen_target_get_frame_buffer(bridge);
    TEST_ASSERT_TRUE(jce_offscreen_target_prepare(bridge, 80, 64, identity, identity, 0, NULL));
    const uint64_t current = read_live_counter("offscreen.create_committed", 0).id;
    TEST_ASSERT_NOT_EQUAL(0, old);
    TEST_ASSERT_NOT_EQUAL(0, current);
    TEST_ASSERT_NOT_EQUAL(old, current);
    TEST_ASSERT_EQUAL_UINT64(JCE_OFFSCREEN_REASON_RESIZE_RETIRE,
        read_live_counter("offscreen.resource_reason", old).value);
    TEST_ASSERT_EQUAL_UINT(0, read_live_counter("offscreen.destroy_kind", old).count);
    for (unsigned i = 0; i < 7; ++i) jce_renderer_submit_frame(renderer);
    TEST_ASSERT_EQUAL_UINT(0, read_live_counter("offscreen.destroy_kind", old).count);
    jce_renderer_submit_frame(renderer);
    const TraceObservation destroyed = read_live_counter("offscreen.destroy_handle", old);
    TEST_ASSERT_EQUAL_UINT(1, destroyed.count);
    TEST_ASSERT_EQUAL_UINT64(old_fbo, destroyed.value);
    TEST_ASSERT_NOT_EQUAL(0, destroyed.parent);
    const TraceObservation completed = read_trace_events_matching(JCE_TRACE_EVENT_TASK_END,
        "renderer.offscreen.destroy_submit", destroyed.parent, false, false, 0);
    TEST_ASSERT_EQUAL_UINT(1, completed.count);
    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_TASK_SUCCEEDED, completed.state);
    TEST_ASSERT_EQUAL_UINT64(JCE_OFFSCREEN_REASON_RETIRE_DUE,
        read_live_counter("offscreen.resource_reason", old).value);
    TEST_ASSERT_EQUAL_UINT(1, read_live_counter("offscreen.destroy_kind", old).count);
    TEST_ASSERT_EQUAL_UINT(0, read_live_counter("offscreen.destroy_kind", current).count);
    jce_renderer_destroy(renderer); renderer = NULL;
    counter_value("offscreen.resource_reason", current, JCE_OFFSCREEN_REASON_RENDERER_CLOSE);
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.destroy_kind", current).count);
    jce_offscreen_target_destroy(bridge); bridge = NULL;
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.destroy_kind", old).count);
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.destroy_kind", current).count);
}

static void test_capture_reset_does_not_invent_old_resource_birth(void)
{
    create_controlled();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 64));
    const uint64_t old = resource_created();
    const uint32_t origin = jce_trace_capture_generation();
    jce_trace_reset();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 80, 64));
    TEST_ASSERT_EQUAL_UINT(0, counter("offscreen.create_committed", old).count);
    counter_value("offscreen.origin_capture_generation", old, origin);
    counter_value("offscreen.current_capture_generation", old, jce_trace_capture_generation());
    counter_value("offscreen.origin_observed", old, 1);
    counter_value("offscreen.origin_capture_gap", old, 1);
    close_controlled();
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.destroy_kind", old).count);
    assert_control_resources_closed();
}

static void test_untraced_birth_remains_explicit_gap_when_capture_begins(void)
{
    create_controlled();
    jce_trace_set_enabled(false);
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 64));
    JceTraceStats stats;
    jce_trace_get_stats(&stats); TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
    jce_trace_set_enabled(true);
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 80, 64));
    TraceObservation old = trace_events_matching(JCE_TRACE_EVENT_COUNTER,
        "offscreen.resource_reason", 0, false, true, JCE_OFFSCREEN_REASON_RESIZE_RETIRE);
    TEST_ASSERT_EQUAL_UINT(1, old.count);
    const uint64_t current = resource_created();
    TEST_ASSERT_NOT_EQUAL(current, old.id);
    counter_value("offscreen.origin_observed", old.id, 0);
    counter_value("offscreen.origin_same_capture", old.id, 0);
    counter_value("offscreen.origin_capture_gap", old.id, 1);
    TEST_ASSERT_EQUAL_UINT(0, counter("offscreen.create_committed", old.id).count);
    close_controlled(); assert_control_resources_closed();
}

static void exercise_creation_failure(uint32_t reason)
{
    if (reason == JCE_OFFSCREEN_REASON_COLOR_FAILED) controls.fail_texture = 1;
    if (reason == JCE_OFFSCREEN_REASON_DEPTH_FAILED) controls.fail_texture = 2;
    controls.fail_fbo = reason == JCE_OFFSCREEN_REASON_FRAMEBUFFER_FAILED;
    controls.fail_query = reason == JCE_OFFSCREEN_REASON_QUERY_FAILED;
    create_controlled();
    TEST_ASSERT_FALSE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 64));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, jce_offscreen_target_get_frame_buffer(controlled_bridge));
    TEST_ASSERT_EQUAL_UINT(0, counter("offscreen.create_committed", 0).count);
    TraceObservation created = trace_events(JCE_TRACE_EVENT_TASK_END, "renderer.offscreen.create", 0, false);
    TEST_ASSERT_EQUAL_UINT(1, created.count); TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, created.state);
    TraceObservation rollback = trace_events(JCE_TRACE_EVENT_TASK_END, "renderer.offscreen.rollback", 0, false);
    TEST_ASSERT_EQUAL_UINT(1, rollback.count); TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_SUCCEEDED, rollback.state);
    TraceObservation failed = counter("offscreen.resource_reason", 0);
    counter_value("offscreen.resource_reason", failed.id, reason);
    const unsigned expected_destroys = reason == JCE_OFFSCREEN_REASON_FRAMEBUFFER_FAILED ? 2u : 1u;
    TEST_ASSERT_EQUAL_UINT(expected_destroys, counter("offscreen.destroy_kind", failed.id).count);
    TEST_ASSERT_EQUAL_UINT(reason <= JCE_OFFSCREEN_REASON_DEPTH_FAILED ? 1u : 2u, controls.texture_destroys);
    TEST_ASSERT_EQUAL_UINT(reason == JCE_OFFSCREEN_REASON_QUERY_FAILED ? 1u : 0u, controls.fbo_destroys);
    counter_value("offscreen.logical_bytes_after_submit", failed.id, 0);
    assert_control_resources_closed();
    close_controlled();
    TEST_ASSERT_EQUAL_UINT(expected_destroys, counter("offscreen.destroy_kind", failed.id).count);
}

static void test_color_failure_rolls_back_only_real_depth(void)
{ exercise_creation_failure(JCE_OFFSCREEN_REASON_COLOR_FAILED); }
static void test_depth_failure_rolls_back_valid_zero_color(void)
{ exercise_creation_failure(JCE_OFFSCREEN_REASON_DEPTH_FAILED); }
static void test_framebuffer_failure_rolls_back_two_textures(void)
{ exercise_creation_failure(JCE_OFFSCREEN_REASON_FRAMEBUFFER_FAILED); }
static void test_query_failure_submits_one_owned_framebuffer_destroy(void)
{ exercise_creation_failure(JCE_OFFSCREEN_REASON_QUERY_FAILED); }

static void test_zero_handles_and_reuse_keep_distinct_resource_identity(void)
{
    create_controlled();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 64));
    const uint64_t first = resource_created();
    counter_value("offscreen.fbo_handle", first, 0); counter_value("offscreen.color_handle", first, 0);
    counter_value("offscreen.logical_bytes", first, 64u * 64u * 12u);
    close_controlled();
    counter_value("offscreen.destroy_handle", first, 0);
    controls.caps.formats[BGFX_TEXTURE_FORMAT_RGBA16F] = 0;
    create_controlled();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 64));
    const uint64_t second = resource_created();
    TEST_ASSERT_NOT_EQUAL(first, second);
    counter_value("offscreen.fbo_handle", second, 0); counter_value("offscreen.color_handle", second, 0);
    counter_value("offscreen.color_format", second, BGFX_TEXTURE_FORMAT_RGBA8);
    counter_value("offscreen.logical_bytes", second, 64u * 64u * 8u);
    close_controlled(); assert_control_resources_closed();
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.destroy_kind", first).count);
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.destroy_kind", second).count);
}

static void test_failed_resize_keeps_retired_identity_until_owner_close(void)
{
    create_controlled();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 64));
    const uint64_t old = resource_created();
    controls.fail_texture = controls.texture_calls + 1u;
    TEST_ASSERT_FALSE(jce_offscreen_target_ensure_internal(controlled_bridge, 80, 64));
    counter_value("offscreen.resource_reason", old, JCE_OFFSCREEN_REASON_RESIZE_RETIRE);
    TEST_ASSERT_EQUAL_UINT(0, counter("offscreen.destroy_kind", old).count);
    jce_offscreen_retirement_shutdown(renderer);
    counter_value("offscreen.resource_reason", old, JCE_OFFSCREEN_REASON_RENDERER_CLOSE);
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.destroy_kind", old).count);
    jce_offscreen_retirement_shutdown(renderer); close_controlled();
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.destroy_kind", old).count);
    assert_control_resources_closed();
}

static uint16_t controlled_depth_read_alias(void)
{
    const uint16_t fbo = jce_offscreen_target_get_frame_buffer(controlled_bridge);
    const uint16_t depth = jce_offscreen_target_get_depth_texture(controlled_bridge);
    uint16_t destination = UINT16_MAX;
    TEST_ASSERT_TRUE(jce_offscreen_target_depth_read_destination(
        renderer, fbo, depth, &destination));
    TEST_ASSERT_NOT_EQUAL(UINT16_MAX, destination);
    TEST_ASSERT_NOT_EQUAL(fbo, destination);
    return destination;
}

static void test_color_alias_reuses_live_generation_and_writes_only_color(void)
{
    create_controlled();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 64));
    const uint64_t id = resource_created();
    const uint16_t fbo = jce_offscreen_target_get_frame_buffer(controlled_bridge);
    const uint16_t color = jce_offscreen_target_get_color_texture(controlled_bridge);
    uint16_t destination = 0;

    TEST_ASSERT_FALSE(jce_offscreen_target_depth_read_destination(
        renderer, 600, 1, &destination));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, destination);
    TEST_ASSERT_FALSE(jce_offscreen_target_depth_read_destination(
        renderer, UINT16_MAX, 1, &destination));
    TEST_ASSERT_TRUE(jce_offscreen_target_depth_read_destination(
        renderer, fbo, color, &destination));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, destination);
    TEST_ASSERT_TRUE(jce_offscreen_target_depth_read_destination(
        renderer, fbo, 62, &destination));
    TEST_ASSERT_EQUAL_UINT16(fbo, destination);
    TEST_ASSERT_EQUAL_UINT(1, controls.fbo_calls);

    const uint16_t alias = controlled_depth_read_alias();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 64));
    TEST_ASSERT_EQUAL_UINT16(alias, controlled_depth_read_alias());
    TEST_ASSERT_EQUAL_UINT(2, controls.fbo_calls);
    TEST_ASSERT_EQUAL_UINT(2, controls.texture_calls);
    TEST_ASSERT_EQUAL_UINT8(1, controls.attachment_counts[alias]);
    TEST_ASSERT_EQUAL_UINT16(color, controls.attachments[alias][0]);
    TEST_ASSERT_EQUAL_UINT(2, controls.creator_releases);
    TEST_ASSERT_EQUAL_UINT(0, controls.raw_ownership_requests);
    TEST_ASSERT_EQUAL_UINT(2, controls.texture_refs[color]);
    counter_value("offscreen.alias_fbo_handle", id, alias);
    counter_value("offscreen.alias_origin_generation", id, jce_trace_capture_generation());
    counter_value("offscreen.alias_origin_observed", id, 1);
    counter_value("offscreen.alias_gpu_completion_known", id, 0);
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.logical_bytes", id).count);

    close_controlled();
    TEST_ASSERT_EQUAL_UINT(2, controls.fbo_destroys);
    TEST_ASSERT_EQUAL_UINT16(alias, controls.fbo_destroy_order[0]);
    TEST_ASSERT_EQUAL_UINT16(fbo, controls.fbo_destroy_order[1]);
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.destroy_kind", id).count);
    counter_value("offscreen.alias_reason", id, JCE_OFFSCREEN_REASON_WRAPPER_CLOSE);
    assert_control_resources_closed();
}

static void test_color_alias_failure_preserves_parent_and_allows_retry(void)
{
    create_controlled();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 64));
    const uint64_t id = resource_created();
    const uint16_t fbo = jce_offscreen_target_get_frame_buffer(controlled_bridge);
    const uint16_t depth = jce_offscreen_target_get_depth_texture(controlled_bridge);
    uint16_t destination = 0;
    controls.fail_alias = true;
    TEST_ASSERT_TRUE(jce_offscreen_target_depth_read_destination(
        renderer, fbo, depth, &destination));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, destination);
    TEST_ASSERT_EQUAL_UINT16(fbo, jce_offscreen_target_get_frame_buffer(controlled_bridge));
    TEST_ASSERT_EQUAL_UINT(0, controls.fbo_destroys);
    TEST_ASSERT_EQUAL_UINT(2, controls.fbo_ref_acquires);
    counter_value("offscreen.alias_reason", id, JCE_OFFSCREEN_REASON_FRAMEBUFFER_FAILED);
    TraceObservation attempt = trace_events(
        JCE_TRACE_EVENT_TASK_END, "renderer.offscreen.alias_create", 0, false);
    TEST_ASSERT_EQUAL_UINT(1, attempt.count);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, attempt.state);

    controls.fail_alias = false;
    (void)controlled_depth_read_alias();
    TEST_ASSERT_EQUAL_UINT(3, controls.fbo_calls);
    TEST_ASSERT_EQUAL_UINT(2, controls.texture_calls);
    attempt = trace_events(JCE_TRACE_EVENT_TASK_END, "renderer.offscreen.alias_create", 0, false);
    TEST_ASSERT_EQUAL_UINT(2, attempt.count);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_SUCCEEDED, attempt.state);
}

static void test_color_alias_retires_with_parent_and_handles_can_recycle(void)
{
    create_controlled();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 64));
    const uint64_t first = resource_created();
    const uint16_t old_fbo = jce_offscreen_target_get_frame_buffer(controlled_bridge);
    const uint16_t old_depth = jce_offscreen_target_get_depth_texture(controlled_bridge);
    const uint16_t old_alias = controlled_depth_read_alias();
    const uint32_t retired_at = jce_renderer_offscreen_submission_index();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 80, 64));
    TEST_ASSERT_NOT_EQUAL(old_alias, controlled_depth_read_alias());
    uint16_t destination = 0;
    TEST_ASSERT_TRUE(jce_offscreen_target_depth_read_destination(
        renderer, old_fbo, old_depth, &destination));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, destination);
    jce_offscreen_retirement_collect(renderer, retired_at + JCE_OFFSCREEN_RETIRE_DELAY - 1);
    TEST_ASSERT_EQUAL_UINT(0, controls.fbo_destroys);
    jce_offscreen_retirement_collect(renderer, retired_at + JCE_OFFSCREEN_RETIRE_DELAY);
    TEST_ASSERT_EQUAL_UINT(2, controls.fbo_destroys);
    TEST_ASSERT_EQUAL_UINT16(old_alias, controls.fbo_destroy_order[0]);
    TEST_ASSERT_EQUAL_UINT16(old_fbo, controls.fbo_destroy_order[1]);
    counter_value("offscreen.alias_reason", first, JCE_OFFSCREEN_REASON_RETIRE_DUE);
    TEST_ASSERT_FALSE(jce_offscreen_target_depth_read_destination(
        renderer, old_fbo, old_depth, &destination));
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 96, 64));
    const uint64_t recycled = resource_created();
    TEST_ASSERT_NOT_EQUAL(first, recycled);
    TEST_ASSERT_EQUAL_UINT16(old_fbo, jce_offscreen_target_get_frame_buffer(controlled_bridge));
    TEST_ASSERT_EQUAL_UINT16(old_alias, controlled_depth_read_alias());
    counter_value("offscreen.alias_fbo_handle", recycled, old_alias);
}

static void test_color_alias_shutdown_orders_live_and_retired_pairs_once(void)
{
    create_controlled();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 64));
    const uint16_t old_fbo = jce_offscreen_target_get_frame_buffer(controlled_bridge);
    const uint16_t old_alias = controlled_depth_read_alias();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 80, 64));
    const uint16_t live_fbo = jce_offscreen_target_get_frame_buffer(controlled_bridge);
    const uint16_t live_alias = controlled_depth_read_alias();

    jce_offscreen_retirement_shutdown(renderer);
    TEST_ASSERT_EQUAL_UINT(4, controls.fbo_destroys);
    TEST_ASSERT_EQUAL_UINT16(live_alias, controls.fbo_destroy_order[0]);
    TEST_ASSERT_EQUAL_UINT16(live_fbo, controls.fbo_destroy_order[1]);
    TEST_ASSERT_EQUAL_UINT16(old_alias, controls.fbo_destroy_order[2]);
    TEST_ASSERT_EQUAL_UINT16(old_fbo, controls.fbo_destroy_order[3]);
    jce_offscreen_retirement_shutdown(renderer);
    close_controlled();
    TEST_ASSERT_EQUAL_UINT(4, controls.fbo_destroys);
    assert_control_resources_closed();
}

static void test_normalized_zero_fbo_transfers_two_creator_refs_then_closes_once(void)
{
    create_controlled();
    TEST_ASSERT_TRUE(jce_offscreen_target_ensure_internal(controlled_bridge, 64, 64));
    TEST_ASSERT_EQUAL_UINT16(0, jce_offscreen_target_get_frame_buffer(controlled_bridge));
    TEST_ASSERT_EQUAL_UINT16(0, jce_offscreen_target_get_color_texture(controlled_bridge));
    TEST_ASSERT_EQUAL_UINT(2, controls.creator_releases);
    TEST_ASSERT_EQUAL_UINT(2, controls.fbo_ref_acquires);
    TEST_ASSERT_EQUAL_UINT(0, controls.fbo_ref_releases);
    TEST_ASSERT_EQUAL_UINT(0, controls.raw_ownership_requests);
    TEST_ASSERT_EQUAL_UINT(0, controls.raw_take_ownership);
    TEST_ASSERT_EQUAL_UINT(0, controls.texture_destroys);
    TEST_ASSERT_EQUAL_UINT(1, controls.texture_refs[0]);
    TEST_ASSERT_EQUAL_UINT(1, controls.texture_refs[1]);
    close_controlled();
    TEST_ASSERT_EQUAL_UINT(2, controls.creator_releases);
    TEST_ASSERT_EQUAL_UINT(2, controls.fbo_ref_releases);
    TEST_ASSERT_EQUAL_UINT(2, controls.texture_destroys);
    TEST_ASSERT_EQUAL_UINT(1, controls.fbo_destroys);
    assert_control_resources_closed();
}

/* The same witness runs in the ordinary positive case and in the old-owner
 * negative. No wrapper/renderer is created here. Release only references that
 * remain owned before making assertions, so an expected failure cannot unwind
 * through a half-published bridge or issue a second backend destroy. */
static void check_direct_normalized_pool_failure_retains_caller_refs(void)
{
    OffscreenControls observed = {0};
    observed.fail_fbo = true;
    bgfx_texture_handle_t textures[2] = {
        controlled_texture(64, 64, BGFX_TEXTURE_FORMAT_RGBA16F, &observed),
        controlled_texture(64, 64, BGFX_TEXTURE_FORMAT_D24S8, &observed)
    };
    const JceOffscreenFrameBufferProvider provider = {
        controlled_create_from_attachment, controlled_destroy_texture, &observed
    };
    const bgfx_frame_buffer_handle_t fbo =
        jce_offscreen_frame_buffer_create_normalized(textures, &provider);
    const unsigned color_refs = observed.texture_refs[textures[0].idx];
    const unsigned depth_refs = observed.texture_refs[textures[1].idx];
    const unsigned ownership_requests = observed.raw_ownership_requests;
    const unsigned ownership_taken = observed.raw_take_ownership;
    const unsigned creator_releases = observed.creator_releases;
    const unsigned acquired = observed.fbo_ref_acquires;
    for (unsigned i = 0; i < 2u; ++i)
        if (observed.texture_refs[textures[i].idx] != 0u)
            controlled_destroy_texture(textures[i], &observed);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, fbo.idx);
    TEST_ASSERT_EQUAL_UINT(1, color_refs);
    TEST_ASSERT_EQUAL_UINT(1, depth_refs);
    TEST_ASSERT_EQUAL_UINT(0, ownership_requests);
    TEST_ASSERT_EQUAL_UINT(0, ownership_taken);
    TEST_ASSERT_EQUAL_UINT(0, creator_releases);
    TEST_ASSERT_EQUAL_UINT(0, acquired);
    TEST_ASSERT_EQUAL_UINT(2, observed.creator_releases);
    TEST_ASSERT_EQUAL_UINT(2, observed.texture_destroys);
    TEST_ASSERT_EQUAL_UINT(0, observed.fbo_destroys);
    TEST_ASSERT_EQUAL_UINT(0, observed.texture_refs[textures[0].idx]);
    TEST_ASSERT_EQUAL_UINT(0, observed.texture_refs[textures[1].idx]);
}

static void test_normalized_pool_failure_preserves_creator_refs_for_rollback(void)
{
    check_direct_normalized_pool_failure_retains_caller_refs();
    /* Prove that this port exposes the old default's ownership hazard rather
     * than returning invalid without modeling bgfx's implicit releases. */
    OffscreenControls old = {0};
    old.fail_fbo = true;
    bgfx_texture_handle_t textures[2] = {
        controlled_texture(64, 64, BGFX_TEXTURE_FORMAT_RGBA16F, &old),
        controlled_texture(64, 64, BGFX_TEXTURE_FORMAT_D24S8, &old)
    };
    bgfx_attachment_t attachments[2] = {{0}, {0}};
    bgfx_attachment_init(&attachments[0], textures[0], BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_NONE);
    bgfx_attachment_init(&attachments[1], textures[1], BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_NONE);
    const bgfx_frame_buffer_handle_t old_fbo =
        controlled_create_from_attachment(2, attachments, true, &old);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, old_fbo.idx);
    TEST_ASSERT_EQUAL_UINT(2, old.raw_take_ownership);
    TEST_ASSERT_EQUAL_UINT(0, old.fbo_ref_acquires);
    TEST_ASSERT_EQUAL_UINT(2, old.texture_destroys);
    TEST_ASSERT_EQUAL_UINT(0, old.texture_refs[0]);
    TEST_ASSERT_EQUAL_UINT(0, old.texture_refs[1]);

    controls.fail_fbo = true;
    exercise_creation_failure(JCE_OFFSCREEN_REASON_FRAMEBUFFER_FAILED);
    TEST_ASSERT_EQUAL_UINT(2, controls.creator_releases);
    TEST_ASSERT_EQUAL_UINT(0, controls.fbo_ref_acquires);
    TEST_ASSERT_EQUAL_UINT(0, controls.fbo_ref_releases);
    TEST_ASSERT_EQUAL_UINT(0, controls.raw_ownership_requests);
    TEST_ASSERT_EQUAL_UINT(0, controls.raw_take_ownership);
    TEST_ASSERT_EQUAL_UINT(2, controls.texture_destroys);
    TEST_ASSERT_EQUAL_UINT(0, controls.fbo_destroys);
}

static void test_normalized_validation_failure_keeps_unreferenced_attachments(void)
{
    /* The pinned validation exit occurs before even destroyTextures=true can
     * take ownership, unlike the later FBO-handle allocation exit. */
    OffscreenControls old = {0};
    old.fail_fbo_validation = true;
    bgfx_texture_handle_t textures[2] = {
        controlled_texture(64, 64, BGFX_TEXTURE_FORMAT_RGBA16F, &old),
        controlled_texture(64, 64, BGFX_TEXTURE_FORMAT_D24S8, &old)
    };
    bgfx_attachment_t attachments[2] = {{0}, {0}};
    bgfx_attachment_init(&attachments[0], textures[0], BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_NONE);
    bgfx_attachment_init(&attachments[1], textures[1], BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_NONE);
    const bgfx_frame_buffer_handle_t old_fbo =
        controlled_create_from_attachment(2, attachments, true, &old);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, old_fbo.idx);
    TEST_ASSERT_EQUAL_UINT(0, old.raw_take_ownership);
    TEST_ASSERT_EQUAL_UINT(1, old.texture_refs[0]);
    TEST_ASSERT_EQUAL_UINT(1, old.texture_refs[1]);
    controlled_destroy_texture(textures[0], &old);
    controlled_destroy_texture(textures[1], &old);
    TEST_ASSERT_EQUAL_UINT(2, old.texture_destroys);

    controls.fail_fbo_validation = true;
    exercise_creation_failure(JCE_OFFSCREEN_REASON_FRAMEBUFFER_FAILED);
    TEST_ASSERT_EQUAL_UINT(2, controls.creator_releases);
    TEST_ASSERT_EQUAL_UINT(0, controls.fbo_ref_acquires);
    TEST_ASSERT_EQUAL_UINT(0, controls.raw_take_ownership);
    TEST_ASSERT_EQUAL_UINT(0, controls.fbo_destroys);
}

static void test_normalized_query_failure_releases_only_fbo_owned_refs(void)
{
    exercise_creation_failure(JCE_OFFSCREEN_REASON_QUERY_FAILED);
    TEST_ASSERT_EQUAL_UINT(2, controls.creator_releases);
    TEST_ASSERT_EQUAL_UINT(2, controls.fbo_ref_acquires);
    TEST_ASSERT_EQUAL_UINT(2, controls.fbo_ref_releases);
    TEST_ASSERT_EQUAL_UINT(0, controls.raw_ownership_requests);
    TEST_ASSERT_EQUAL_UINT(0, controls.raw_take_ownership);
    TEST_ASSERT_EQUAL_UINT(2, controls.texture_destroys);
    TEST_ASSERT_EQUAL_UINT(1, controls.fbo_destroys);
    TEST_ASSERT_EQUAL_UINT(1, counter("offscreen.destroy_kind", 0).count);
    TEST_ASSERT_EQUAL_UINT64(3, counter("offscreen.destroy_kind", 0).value);
}

static void test_actual_noop_backend_detaches_readback_wrapper_across_restart(void)
{
    JceRenderReadbackQueue *old_queue=jce_render_readback_create(renderer);
    TEST_ASSERT_NOT_NULL(old_queue);
    jce_offscreen_target_destroy(bridge);bridge=NULL;
    jce_renderer_destroy(renderer);renderer=NULL;
    TEST_ASSERT_NULL(jce_render_readback_create(NULL));
    renderer=jce_renderer_create_headless();TEST_ASSERT_NOT_NULL(renderer);
    JceRenderReadbackQueue *new_queue=jce_render_readback_create(renderer);
    TEST_ASSERT_NOT_NULL(new_queue);
    jce_render_readback_destroy(old_queue); /* Must not issue old/new backend work. */
    JceRenderReadbackQueue *second_queue=jce_render_readback_create(renderer);
    TEST_ASSERT_NOT_NULL(second_queue);
    jce_render_readback_destroy(second_queue);jce_render_readback_destroy(new_queue);
}

int main(int argc, char **argv)
{
    isolated_normalization = argc == 2 && strcmp(argv[1], "--normalized-failure-only") == 0;
#if defined(JCE_TEST_OFFSCREEN_ISOLATED_NORMALIZATION)
    isolated_normalization = true;
#endif
    if (argc != 1 && !isolated_normalization)
        return 2;
    UNITY_BEGIN();
    if (isolated_normalization) {
        RUN_TEST(check_direct_normalized_pool_failure_retains_caller_refs);
        return UNITY_END();
    }
    RUN_TEST(test_actual_noop_backend_detaches_readback_wrapper_across_restart);
    RUN_TEST(test_delegating_prepare_has_one_success_and_restores_parent);
    RUN_TEST(test_keep_and_overlay_configure_actual_target);
    RUN_TEST(test_extent_query_reports_only_live_clamped_owner_dimensions);
    RUN_TEST(test_extent_query_follows_resize_failure_and_renderer_detach);
    RUN_TEST(test_invalid_matrix_and_unprepared_overlay_record_failed);
    RUN_TEST(test_null_bridge_records_cancelled);
    RUN_TEST(test_capture_off_preserves_configuration_and_emits_nothing);
    RUN_TEST(test_same_frame_saturation_preserves_current_until_eight_frames);
    RUN_TEST(test_idle_targets_collect_and_shutdown_detaches_wrapper);
    RUN_TEST(test_retirement_frame_wrap_and_boundary);
    RUN_TEST(test_new_device_restarts_retirement_clock);
    RUN_TEST(test_independent_targets_unlink_and_idle_collection);
    RUN_TEST(test_actual_creation_metadata_and_same_extent_has_no_lifecycle_events);
    RUN_TEST(test_hidden_collection_and_shutdown_match_creation_once);
    RUN_TEST(test_capture_reset_does_not_invent_old_resource_birth);
    RUN_TEST(test_untraced_birth_remains_explicit_gap_when_capture_begins);
    RUN_TEST(test_color_failure_rolls_back_only_real_depth);
    RUN_TEST(test_depth_failure_rolls_back_valid_zero_color);
    RUN_TEST(test_framebuffer_failure_rolls_back_two_textures);
    RUN_TEST(test_query_failure_submits_one_owned_framebuffer_destroy);
    RUN_TEST(test_zero_handles_and_reuse_keep_distinct_resource_identity);
    RUN_TEST(test_failed_resize_keeps_retired_identity_until_owner_close);
    RUN_TEST(test_color_alias_reuses_live_generation_and_writes_only_color);
    RUN_TEST(test_color_alias_failure_preserves_parent_and_allows_retry);
    RUN_TEST(test_color_alias_retires_with_parent_and_handles_can_recycle);
    RUN_TEST(test_color_alias_shutdown_orders_live_and_retired_pairs_once);
    RUN_TEST(test_normalized_zero_fbo_transfers_two_creator_refs_then_closes_once);
    RUN_TEST(test_normalized_pool_failure_preserves_creator_refs_for_rollback);
    RUN_TEST(test_normalized_validation_failure_keeps_unreferenced_attachments);
    RUN_TEST(test_normalized_query_failure_releases_only_fbo_owned_refs);
    return UNITY_END();
}
