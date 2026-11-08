/* Exact prepass target owner with counted first-party C99 ports.
 * These controls prove cache identity and resource ownership, not GPU pixels. */
#include "unity.h"
#include "jce_sr_prepass_targets.h"
#include <jce/os/core/jce_trace.h>
#include <stddef.h>
#include <string.h>

#define HANDLE_LIMIT 512u

typedef struct TextureRecord {
    bool live;
    uint16_t width, height, format;
} TextureRecord;

typedef struct FrameBufferRecord {
    bool live;
    uint8_t count;
    uint16_t attachments[4];
} FrameBufferRecord;

typedef struct Fixture {
    TextureRecord textures[HANDLE_LIMIT];
    FrameBufferRecord frame_buffers[HANDLE_LIMIT];
    uint16_t next_texture, next_frame_buffer;
    unsigned texture_calls, frame_buffer_calls;
    unsigned textures_created, frame_buffers_created;
    unsigned textures_destroyed, frame_buffers_destroyed;
    unsigned fail_texture_call, fail_frame_buffer_call;
    bool reject_depth_only;
} Fixture;

static Fixture fixture;
static SrPrepassTargets owners[2];
static SrPrepassTargetProvider provider;

static uint16_t create_texture(uint16_t width, uint16_t height,
                               uint16_t format, void *user)
{
    Fixture *f = user;
    TEST_ASSERT_EQUAL_PTR(&fixture, f);
    TEST_ASSERT_TRUE(width > 0 && height > 0);
    if (++f->texture_calls == f->fail_texture_call) return UINT16_MAX;
    TEST_ASSERT_TRUE(f->next_texture < HANDLE_LIMIT);
    const uint16_t handle = f->next_texture++;
    TextureRecord *record = &f->textures[handle];
    TEST_ASSERT_FALSE(record->live);
    *record = (TextureRecord){true, width, height, format};
    ++f->textures_created;
    return handle;
}

static uint16_t create_frame_buffer(const uint16_t *attachments,
                                    uint8_t count, void *user)
{
    Fixture *f = user;
    TEST_ASSERT_EQUAL_PTR(&fixture, f);
    TEST_ASSERT_NOT_NULL(attachments);
    TEST_ASSERT_TRUE(count == 1 || count == 3 || count == 4);
    for (uint8_t i = 0; i < count; ++i) {
        TEST_ASSERT_TRUE(attachments[i] < HANDLE_LIMIT);
        const TextureRecord *record = &f->textures[attachments[i]];
        TEST_ASSERT_TRUE_MESSAGE(record->live, "Only live textures may be attached");
        for (uint8_t j = 0; j < i; ++j)
            TEST_ASSERT_TRUE(attachments[i] != attachments[j]);
        TEST_ASSERT_EQUAL_UINT16(f->textures[attachments[0]].width, record->width);
        TEST_ASSERT_EQUAL_UINT16(f->textures[attachments[0]].height, record->height);
    }
    if (++f->frame_buffer_calls == f->fail_frame_buffer_call ||
        (count == 1 && f->reject_depth_only)) return UINT16_MAX;
    TEST_ASSERT_TRUE(f->next_frame_buffer < HANDLE_LIMIT);
    const uint16_t handle = f->next_frame_buffer++;
    FrameBufferRecord *record = &f->frame_buffers[handle];
    TEST_ASSERT_FALSE(record->live);
    record->live = true;
    record->count = count;
    memcpy(record->attachments, attachments, count * sizeof(*attachments));
    ++f->frame_buffers_created;
    return handle;
}

static void destroy_texture(uint16_t texture, void *user)
{
    Fixture *f = user;
    TEST_ASSERT_EQUAL_PTR(&fixture, f);
    TEST_ASSERT_TRUE(texture < HANDLE_LIMIT);
    TEST_ASSERT_TRUE_MESSAGE(f->textures[texture].live,
        "Every texture, including handle zero, must retire exactly once");
    for (unsigned i = 0; i < HANDLE_LIMIT; ++i) {
        const FrameBufferRecord *record = &f->frame_buffers[i];
        if (!record->live) continue;
        for (uint8_t j = 0; j < record->count; ++j)
            TEST_ASSERT_TRUE_MESSAGE(record->attachments[j] != texture,
                "A borrowing framebuffer must retire before its textures");
    }
    f->textures[texture].live = false;
    ++f->textures_destroyed;
}

static void destroy_frame_buffer(uint16_t frame_buffer, void *user)
{
    Fixture *f = user;
    TEST_ASSERT_EQUAL_PTR(&fixture, f);
    TEST_ASSERT_TRUE(frame_buffer < HANDLE_LIMIT);
    FrameBufferRecord *record = &f->frame_buffers[frame_buffer];
    TEST_ASSERT_TRUE_MESSAGE(record->live,
        "Every framebuffer, including handle zero, must retire exactly once");
    for (uint8_t i = 0; i < record->count; ++i)
        TEST_ASSERT_TRUE_MESSAGE(f->textures[record->attachments[i]].live,
            "Destroying a framebuffer does not destroy its borrowed attachments");
    record->live = false;
    ++f->frame_buffers_destroyed;
}

static unsigned live_textures(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < HANDLE_LIMIT; ++i)
        if (fixture.textures[i].live) ++count;
    return count;
}

static unsigned live_frame_buffers(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < HANDLE_LIMIT; ++i)
        if (fixture.frame_buffers[i].live) ++count;
    return count;
}

static void assert_live(unsigned textures, unsigned frame_buffers)
{
    TEST_ASSERT_EQUAL_UINT(textures, live_textures());
    TEST_ASSERT_EQUAL_UINT(frame_buffers, live_frame_buffers());
}

static SrPrepassTargetRequest request(uint16_t view, uint32_t width,
                                      uint32_t height, bool velocity)
{
    return (SrPrepassTargetRequest){view, width, height, 101, 202, 303, velocity, false};
}

static const SrPrepassTarget *acquire(unsigned owner,
                                     const SrPrepassTargetRequest *wanted)
{
    TEST_ASSERT_TRUE(owner < 2);
    return sr_prepass_targets_acquire(&owners[owner], wanted, &provider,
                                      &owners[owner]);
}

static void release(unsigned owner, SrPrepassTargetReleaseReason reason)
{
    sr_prepass_targets_release(&owners[owner], &provider, &owners[owner], reason);
}

static const SrPrepassTargetSlot *find_slot(unsigned owner, uint16_t view)
{
    for (unsigned i = 0; i < SR_PREPASS_TARGET_CAPACITY; ++i) {
        const SrPrepassTargetSlot *slot = &owners[owner].slots[i];
        if (slot->used && slot->target.request.view_base == view) return slot;
    }
    return NULL;
}

static unsigned used_slots(unsigned owner)
{
    unsigned count = 0;
    for (unsigned i = 0; i < SR_PREPASS_TARGET_CAPACITY; ++i)
        if (owners[owner].slots[i].used) ++count;
    return count;
}

static void assert_request(const SrPrepassTargetRequest *expected,
                            const SrPrepassTargetRequest *actual)
{
    TEST_ASSERT_EQUAL_UINT16(expected->view_base, actual->view_base);
    TEST_ASSERT_EQUAL_UINT32(expected->width, actual->width);
    TEST_ASSERT_EQUAL_UINT32(expected->height, actual->height);
    TEST_ASSERT_EQUAL_UINT16(expected->depth_format, actual->depth_format);
    TEST_ASSERT_EQUAL_UINT16(expected->color_format, actual->color_format);
    TEST_ASSERT_EQUAL_UINT16(expected->velocity_format, actual->velocity_format);
    TEST_ASSERT_EQUAL(expected->velocity, actual->velocity);
    TEST_ASSERT_EQUAL(expected->prefer_depth_only, actual->prefer_depth_only);
}

static void assert_same_target(const SrPrepassTarget *expected,
                                const SrPrepassTarget *actual)
{
    TEST_ASSERT_NOT_NULL(actual);
    assert_request(&expected->request, &actual->request);
    TEST_ASSERT_EQUAL_UINT16(expected->depth, actual->depth);
    TEST_ASSERT_EQUAL_UINT16(expected->normal, actual->normal);
    TEST_ASSERT_EQUAL_UINT16(expected->albedo, actual->albedo);
    TEST_ASSERT_EQUAL_UINT16(expected->velocity, actual->velocity);
    TEST_ASSERT_EQUAL_UINT16(expected->frame_buffer, actual->frame_buffer);
    TEST_ASSERT_EQUAL_UINT64(expected->generation, actual->generation);
}

static void assert_texture(uint16_t handle, uint16_t format,
                            const SrPrepassTargetRequest *wanted)
{
    TEST_ASSERT_TRUE(handle < HANDLE_LIMIT);
    const TextureRecord *record = &fixture.textures[handle];
    TEST_ASSERT_TRUE(record->live);
    TEST_ASSERT_EQUAL_UINT16(wanted->width, record->width);
    TEST_ASSERT_EQUAL_UINT16(wanted->height, record->height);
    TEST_ASSERT_EQUAL_UINT16(format, record->format);
}

static void assert_target(const SrPrepassTarget *target,
                           const SrPrepassTargetRequest *wanted)
{
    TEST_ASSERT_NOT_NULL(target);
    assert_request(wanted, &target->request);
    TEST_ASSERT_TRUE(target->generation > 0);
    if (wanted->prefer_depth_only && !wanted->velocity && target->normal == UINT16_MAX) {
        TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->albedo);
        TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->velocity);
        assert_texture(target->depth, wanted->depth_format, wanted);
        TEST_ASSERT_TRUE(target->frame_buffer < HANDLE_LIMIT);
        const FrameBufferRecord *record = &fixture.frame_buffers[target->frame_buffer];
        TEST_ASSERT_TRUE(record->live);
        TEST_ASSERT_EQUAL_UINT8(1, record->count);
        TEST_ASSERT_EQUAL_UINT16(target->depth, record->attachments[0]);
        return;
    }
    assert_texture(target->normal, wanted->color_format, wanted);
    assert_texture(target->albedo, wanted->color_format, wanted);
    assert_texture(target->depth, wanted->depth_format, wanted);
    if (wanted->velocity)
        assert_texture(target->velocity, wanted->velocity_format, wanted);
    else
        TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->velocity);
    TEST_ASSERT_TRUE(target->frame_buffer < HANDLE_LIMIT);
    const FrameBufferRecord *record = &fixture.frame_buffers[target->frame_buffer];
    TEST_ASSERT_TRUE(record->live);
    TEST_ASSERT_EQUAL_UINT8(wanted->velocity ? 4 : 3, record->count);
    TEST_ASSERT_EQUAL_UINT16(target->normal, record->attachments[0]);
    TEST_ASSERT_EQUAL_UINT16(target->albedo, record->attachments[1]);
    if (wanted->velocity) {
        TEST_ASSERT_EQUAL_UINT16(target->velocity, record->attachments[2]);
        TEST_ASSERT_EQUAL_UINT16(target->depth, record->attachments[3]);
    } else {
        TEST_ASSERT_EQUAL_UINT16(target->depth, record->attachments[2]);
    }
}

static void assert_retired(const SrPrepassTarget *target)
{
    if (target->normal != UINT16_MAX)
        TEST_ASSERT_FALSE(fixture.textures[target->normal].live);
    if (target->albedo != UINT16_MAX)
        TEST_ASSERT_FALSE(fixture.textures[target->albedo].live);
    TEST_ASSERT_FALSE(fixture.textures[target->depth].live);
    if (target->velocity != UINT16_MAX)
        TEST_ASSERT_FALSE(fixture.textures[target->velocity].live);
    TEST_ASSERT_FALSE(fixture.frame_buffers[target->frame_buffer].live);
}

static void fail_ordinal(unsigned texture_count, unsigned ordinal)
{
    fixture.fail_texture_call = fixture.fail_frame_buffer_call = 0;
    if (ordinal <= texture_count)
        fixture.fail_texture_call = fixture.texture_calls + ordinal;
    else
        fixture.fail_frame_buffer_call = fixture.frame_buffer_calls + 1;
}

static void permit_creation(void)
{
    fixture.fail_texture_call = fixture.fail_frame_buffer_call = 0;
}

static JceTraceEvent trace_events[128];
static uint32_t trace_count;

static void capture_start(void)
{
    jce_trace_set_enabled(true);
    jce_trace_reset();
    trace_count = 0;
}

static const JceTraceEvent *trace_event(JceTraceEventType type, const char *name)
{
    const JceTraceEvent *found = NULL;
    for (uint32_t i = 0; i < trace_count; ++i) {
        const JceTraceEvent *event = &trace_events[i];
        if (event->type != type || strcmp(event->name, name) != 0) continue;
        TEST_ASSERT_NULL_MESSAGE(found, "One lifecycle span is expected per operation");
        found = event;
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(found, name);
    return found;
}

static void assert_trace_counter(uint64_t scope, const char *name, uint64_t value)
{
    bool found = false;
    for (uint32_t i = 0; i < trace_count; ++i) {
        const JceTraceEvent *event = &trace_events[i];
        if (event->type == JCE_TRACE_EVENT_COUNTER && event->id == scope &&
            strcmp(event->name, name) == 0 && event->value_u64 == value)
            found = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(found, name);
}

static void assert_trace_target(uint64_t scope, const SrPrepassTarget *target)
{
    assert_trace_counter(scope, "sr.prepass.view", target->request.view_base);
    assert_trace_counter(scope, "sr.prepass.width", target->request.width);
    assert_trace_counter(scope, "sr.prepass.height", target->request.height);
    assert_trace_counter(scope, "sr.prepass.generation", target->generation);
    assert_trace_counter(scope, "sr.prepass.depth", target->depth);
    assert_trace_counter(scope, "sr.prepass.normal", target->normal);
    assert_trace_counter(scope, "sr.prepass.albedo", target->albedo);
    assert_trace_counter(scope, "sr.prepass.velocity", target->velocity);
    assert_trace_counter(scope, "sr.prepass.prefer_depth_only", target->request.prefer_depth_only);
    assert_trace_counter(scope, "sr.prepass.framebuffer", target->frame_buffer);
}

static uint64_t assert_trace_scope(const char *name, JceTraceTaskState state,
                                   unsigned owner, const SrPrepassTarget *target)
{
    JceTraceCursor cursor;
    jce_trace_cursor_init(&cursor);
    trace_count = jce_trace_read(&cursor, trace_events, 128);
    TEST_ASSERT_TRUE(trace_count > 0 && trace_count < 128);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    const JceTraceEvent *submit = trace_event(JCE_TRACE_EVENT_TASK_SUBMIT, name);
    const JceTraceEvent *begin = trace_event(JCE_TRACE_EVENT_TASK_BEGIN, name);
    const JceTraceEvent *end = trace_event(JCE_TRACE_EVENT_TASK_END, name);
    TEST_ASSERT_TRUE(submit->id > 0);
    TEST_ASSERT_EQUAL_UINT64(0, submit->parent_id);
    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_TASK_SERVICE, submit->value_u32[0]);
    TEST_ASSERT_EQUAL_UINT64(submit->id, begin->id);
    TEST_ASSERT_EQUAL_UINT64(submit->id, begin->parent_id);
    TEST_ASSERT_EQUAL_UINT64(submit->id, end->id);
    TEST_ASSERT_EQUAL_UINT64(submit->id, end->parent_id);
    TEST_ASSERT_EQUAL_UINT(state, end->value_u32[0]);
    TEST_ASSERT_TRUE(submit->sequence < begin->sequence && begin->sequence < end->sequence);
    unsigned counter_count = 0;
    for (uint32_t i = 0; i < trace_count; ++i) {
        const JceTraceEvent *event = &trace_events[i];
        if (event->type != JCE_TRACE_EVENT_COUNTER) continue;
        ++counter_count;
        TEST_ASSERT_EQUAL_UINT(JCE_TRACE_CATEGORY_RENDER, event->category);
        TEST_ASSERT_EQUAL_UINT64(submit->id, event->id);
        TEST_ASSERT_EQUAL_UINT64(submit->parent_id, event->parent_id);
        TEST_ASSERT_TRUE(event->sequence > begin->sequence && event->sequence < end->sequence);
        if (strcmp(event->name, "sr.prepass.owner") == 0)
            TEST_ASSERT_EQUAL_UINT64((uintptr_t)&owners[owner], event->value_u64);
    }
    TEST_ASSERT_TRUE(counter_count >= 17);
    assert_trace_counter(submit->id, "sr.prepass.owner", (uintptr_t)&owners[owner]);
    assert_trace_target(submit->id, target);
    assert_trace_counter(submit->id, "sr.prepass.requests", owners[owner].requests);
    assert_trace_counter(submit->id, "sr.prepass.creates", owners[owner].creates);
    assert_trace_counter(submit->id, "sr.prepass.reuses", owners[owner].reuses);
    assert_trace_counter(submit->id, "sr.prepass.failures", owners[owner].failures);
    assert_trace_counter(submit->id, "sr.prepass.replacements", owners[owner].replacements);
    assert_trace_counter(submit->id, "sr.prepass.evictions", owners[owner].evictions);
    assert_trace_counter(submit->id, "sr.prepass.retired", owners[owner].retired_targets);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(1, stats.work_items_completed);
    return submit->id;
}

void setUp(void)
{
    jce_trace_set_enabled(false);
    jce_trace_reset();
    memset(&fixture, 0, sizeof(fixture));
    sr_prepass_targets_init(&owners[0]);
    sr_prepass_targets_init(&owners[1]);
    provider = (SrPrepassTargetProvider){create_texture, create_frame_buffer,
        destroy_texture, destroy_frame_buffer, &fixture};
}

void tearDown(void)
{
    jce_trace_set_enabled(false);
    jce_trace_reset();
    release(0, SR_PREPASS_TARGET_DESTROY);
    release(1, SR_PREPASS_TARGET_DESTROY);
    assert_live(0, 0);
    TEST_ASSERT_EQUAL_UINT(fixture.textures_created, fixture.textures_destroyed);
    TEST_ASSERT_EQUAL_UINT(fixture.frame_buffers_created, fixture.frame_buffers_destroyed);
}

static void test_cold_and_invalid_size_requests_own_no_handles(void)
{
    release(0, SR_PREPASS_TARGET_RESET);
    release(0, SR_PREPASS_TARGET_DESTROY);
    TEST_ASSERT_EQUAL_UINT(0, fixture.textures_destroyed);
    TEST_ASSERT_EQUAL_UINT(0, fixture.frame_buffers_destroyed);
    for (unsigned i = 0; i < SR_PREPASS_TARGET_CAPACITY; ++i) {
        const SrPrepassTarget *target = &owners[0].slots[i].target;
        TEST_ASSERT_FALSE(owners[0].slots[i].used);
        TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->normal);
        TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->albedo);
        TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->depth);
        TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->velocity);
        TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->frame_buffer);
    }
    SrPrepassTargetRequest wanted = request(10, 0, 480, false);
    TEST_ASSERT_NULL(acquire(0, &wanted));
    wanted.width = 640;
    wanted.height = 0;
    TEST_ASSERT_NULL(acquire(0, &wanted));
    TEST_ASSERT_EQUAL_UINT(0, fixture.texture_calls);
    TEST_ASSERT_EQUAL_UINT(0, fixture.frame_buffer_calls);
    wanted.width = (uint32_t)UINT16_MAX + 1u;
    wanted.height = 480;
    TEST_ASSERT_NULL(acquire(0, &wanted));
    wanted.width = 640;
    wanted.height = (uint32_t)UINT16_MAX + 1u;
    TEST_ASSERT_NULL(acquire(0, &wanted));
    TEST_ASSERT_EQUAL_UINT(0, fixture.texture_calls);
    TEST_ASSERT_EQUAL_UINT(0, fixture.frame_buffer_calls);
    TEST_ASSERT_EQUAL_UINT64(4, owners[0].failures);
    assert_live(0, 0);
    wanted.width = 640;
    wanted.height = 480;
    const SrPrepassTarget *target = acquire(0, &wanted);
    assert_target(target, &wanted);
    const SrPrepassTarget previous = *target;
    const unsigned texture_calls = fixture.texture_calls;
    const unsigned frame_buffer_calls = fixture.frame_buffer_calls;
    const uint32_t invalid[4][2] = {
        {0, 480}, {640, 0}, {(uint32_t)UINT16_MAX + 1u, 480},
        {640, (uint32_t)UINT16_MAX + 1u}
    };
    for (unsigned i = 0; i < 4; ++i) {
        wanted.width = invalid[i][0];
        wanted.height = invalid[i][1];
        TEST_ASSERT_NULL(acquire(0, &wanted));
        const SrPrepassTargetSlot *slot = find_slot(0, wanted.view_base);
        TEST_ASSERT_NOT_NULL(slot);
        assert_same_target(&previous, &slot->target);
        TEST_ASSERT_EQUAL_UINT(texture_calls, fixture.texture_calls);
        TEST_ASSERT_EQUAL_UINT(frame_buffer_calls, fixture.frame_buffer_calls);
        assert_live(3, 1);
    }
    TEST_ASSERT_EQUAL_UINT64(8, owners[0].failures);
}

static void test_first_complete_target_accepts_texture_and_framebuffer_zero(void)
{
    const SrPrepassTargetRequest wanted = request(10, 640, 480, true);
    const SrPrepassTarget *target = acquire(0, &wanted);
    assert_target(target, &wanted);
    TEST_ASSERT_EQUAL_UINT16(0, target->normal);
    TEST_ASSERT_EQUAL_UINT16(0, target->frame_buffer);
    TEST_ASSERT_EQUAL_UINT(4, fixture.textures_created);
    TEST_ASSERT_EQUAL_UINT(1, fixture.frame_buffers_created);
    assert_live(4, 1);
}

static void test_a_b_a_different_sizes_remain_two_stable_target_sets(void)
{
    const SrPrepassTargetRequest a = request(10, 640, 480, false);
    const SrPrepassTargetRequest b = request(130, 1280, 720, false);
    const SrPrepassTarget *a_target = acquire(0, &a);
    assert_target(a_target, &a);
    const SrPrepassTarget a_first = *a_target;
    const SrPrepassTarget *b_target = acquire(0, &b);
    assert_target(b_target, &b);
    const SrPrepassTarget b_first = *b_target;
    for (unsigned frame = 0; frame < 32; ++frame) {
        assert_same_target(&a_first, acquire(0, &a));
        assert_same_target(&b_first, acquire(0, &b));
        assert_same_target(&a_first, acquire(0, &a));
    }
    TEST_ASSERT_EQUAL_UINT(2, used_slots(0));
    TEST_ASSERT_EQUAL_UINT(6, fixture.textures_created);
    TEST_ASSERT_EQUAL_UINT(2, fixture.frame_buffers_created);
    TEST_ASSERT_EQUAL_UINT(0, fixture.textures_destroyed);
    TEST_ASSERT_EQUAL_UINT64(96, owners[0].reuses);
    assert_live(6, 2);
}

static void test_equal_size_views_keep_distinct_handles_and_generations(void)
{
    const SrPrepassTargetRequest a = request(10, 640, 480, true);
    const SrPrepassTargetRequest b = request(130, 640, 480, true);
    const SrPrepassTarget *target = acquire(0, &a);
    assert_target(target, &a);
    const SrPrepassTarget first = *target;
    target = acquire(0, &b);
    assert_target(target, &b);
    TEST_ASSERT_TRUE(target->normal != first.normal);
    TEST_ASSERT_TRUE(target->albedo != first.albedo);
    TEST_ASSERT_TRUE(target->depth != first.depth);
    TEST_ASSERT_TRUE(target->velocity != first.velocity);
    TEST_ASSERT_TRUE(target->frame_buffer != first.frame_buffer);
    TEST_ASSERT_TRUE(target->generation > first.generation);
    assert_same_target(&first, acquire(0, &a));
    assert_live(8, 2);
}

static void test_same_view_in_independent_owners_keeps_independent_resources(void)
{
    const SrPrepassTargetRequest wanted = request(10, 640, 480, true);
    const SrPrepassTarget *target = acquire(0, &wanted);
    assert_target(target, &wanted);
    const SrPrepassTarget first = *target;
    target = acquire(1, &wanted);
    assert_target(target, &wanted);
    const SrPrepassTarget other = *target;
    TEST_ASSERT_TRUE(other.normal != first.normal);
    TEST_ASSERT_TRUE(other.frame_buffer != first.frame_buffer);
    release(1, SR_PREPASS_TARGET_DESTROY);
    assert_retired(&other);
    assert_same_target(&first, acquire(0, &wanted));
    TEST_ASSERT_EQUAL_UINT(1, used_slots(0));
    TEST_ASSERT_EQUAL_UINT(0, used_slots(1));
    assert_live(4, 1);
}

static void test_resize_and_velocity_edges_replace_only_the_requested_view(void)
{
    SrPrepassTargetRequest a = request(10, 640, 480, false);
    const SrPrepassTargetRequest b = request(130, 1280, 720, true);
    const SrPrepassTarget *target = acquire(0, &a);
    assert_target(target, &a);
    SrPrepassTarget previous = *target;
    target = acquire(0, &b);
    assert_target(target, &b);
    const SrPrepassTarget b_first = *target;
    a.width = 800;
    a.height = 600;
    target = acquire(0, &a);
    assert_target(target, &a);
    TEST_ASSERT_TRUE(target->generation > previous.generation);
    assert_retired(&previous);
    assert_same_target(&b_first, acquire(0, &b));
    previous = *target;
    a.velocity = true;
    target = acquire(0, &a);
    assert_target(target, &a);
    TEST_ASSERT_TRUE(target->generation > previous.generation);
    assert_retired(&previous);
    assert_same_target(&b_first, acquire(0, &b));
    previous = *target;
    a.velocity = false;
    target = acquire(0, &a);
    assert_target(target, &a);
    TEST_ASSERT_TRUE(target->generation > previous.generation);
    assert_retired(&previous);
    assert_same_target(&b_first, acquire(0, &b));
    TEST_ASSERT_EQUAL_UINT64(3, owners[0].replacements);
    TEST_ASSERT_EQUAL_UINT64(0, owners[0].evictions);
    assert_live(7, 2);
}

static void test_attachment_format_changes_require_new_complete_target(void)
{
    SrPrepassTargetRequest wanted = request(10, 640, 480, true);
    const SrPrepassTarget *target = acquire(0, &wanted);
    assert_target(target, &wanted);
    for (unsigned format = 0; format < 3; ++format) {
        const SrPrepassTarget previous = *target;
        if (format == 0) ++wanted.depth_format;
        if (format == 1) ++wanted.color_format;
        if (format == 2) ++wanted.velocity_format;
        target = acquire(0, &wanted);
        assert_target(target, &wanted);
        TEST_ASSERT_TRUE(target->generation > previous.generation);
        assert_retired(&previous);
    }
    TEST_ASSERT_EQUAL_UINT64(3, owners[0].replacements);
    assert_live(4, 1);
}

static void test_each_cold_texture_and_framebuffer_failure_rolls_back_and_retries(void)
{
    for (unsigned velocity = 0; velocity < 2; ++velocity) {
        const unsigned texture_count = velocity ? 4 : 3;
        const SrPrepassTargetRequest wanted = request(10, 640, 480, velocity != 0);
        for (unsigned ordinal = 1; ordinal <= texture_count + 1; ++ordinal) {
            const unsigned created = fixture.textures_created;
            const unsigned destroyed = fixture.textures_destroyed;
            const uint64_t generation = owners[0].generation;
            fail_ordinal(texture_count, ordinal);
            TEST_ASSERT_NULL(acquire(0, &wanted));
            TEST_ASSERT_EQUAL_UINT(0, used_slots(0));
            TEST_ASSERT_EQUAL_UINT64(generation, owners[0].generation);
            const unsigned partial = ordinal <= texture_count ? ordinal - 1 : texture_count;
            TEST_ASSERT_EQUAL_UINT(created + partial, fixture.textures_created);
            TEST_ASSERT_EQUAL_UINT(destroyed + partial, fixture.textures_destroyed);
            assert_live(0, 0);
            permit_creation();
            const SrPrepassTarget *target = acquire(0, &wanted);
            assert_target(target, &wanted);
            TEST_ASSERT_TRUE(target->generation > generation);
            assert_live(texture_count, 1);
            release(0, SR_PREPASS_TARGET_RESET);
            assert_live(0, 0);
        }
    }
    TEST_ASSERT_EQUAL_UINT64(9, owners[0].failures);
    TEST_ASSERT_EQUAL_UINT64(9, owners[0].creates);
}

static void test_each_replacement_failure_preserves_old_and_other_view_then_retries(void)
{
    for (unsigned velocity = 0; velocity < 2; ++velocity) {
        const unsigned texture_count = velocity ? 4 : 3;
        const SrPrepassTargetRequest old_request = request(10, 640, 480, velocity != 0);
        const SrPrepassTargetRequest new_request = request(10, 1280, 720, velocity != 0);
        const SrPrepassTargetRequest other_request = request(130, 320, 240, false);
        for (unsigned ordinal = 1; ordinal <= texture_count + 1; ++ordinal) {
            const SrPrepassTarget *target = acquire(0, &old_request);
            assert_target(target, &old_request);
            const SrPrepassTarget old = *target;
            target = acquire(0, &other_request);
            assert_target(target, &other_request);
            const SrPrepassTarget other = *target;
            const unsigned created = fixture.textures_created;
            const unsigned destroyed = fixture.textures_destroyed;
            const unsigned fbo_destroyed = fixture.frame_buffers_destroyed;
            const uint64_t generation = owners[0].generation;
            const uint64_t clock = owners[0].clock;
            const SrPrepassTargetSlot *old_slot = find_slot(0, old_request.view_base);
            TEST_ASSERT_NOT_NULL(old_slot);
            const uint64_t last_used = old_slot->last_used;
            fail_ordinal(texture_count, ordinal);
            TEST_ASSERT_NULL(acquire(0, &new_request));
            const SrPrepassTargetSlot *slot = find_slot(0, old_request.view_base);
            TEST_ASSERT_NOT_NULL(slot);
            assert_same_target(&old, &slot->target);
            TEST_ASSERT_EQUAL_UINT64(last_used, slot->last_used);
            slot = find_slot(0, other_request.view_base);
            TEST_ASSERT_NOT_NULL(slot);
            assert_same_target(&other, &slot->target);
            TEST_ASSERT_EQUAL_UINT64(generation, owners[0].generation);
            TEST_ASSERT_EQUAL_UINT64(clock, owners[0].clock);
            const unsigned partial = ordinal <= texture_count ? ordinal - 1 : texture_count;
            TEST_ASSERT_EQUAL_UINT(created + partial, fixture.textures_created);
            TEST_ASSERT_EQUAL_UINT(destroyed + partial, fixture.textures_destroyed);
            TEST_ASSERT_EQUAL_UINT(fbo_destroyed, fixture.frame_buffers_destroyed);
            assert_live(texture_count + 3, 2);
            permit_creation();
            assert_same_target(&old, acquire(0, &old_request));
            target = acquire(0, &new_request);
            assert_target(target, &new_request);
            TEST_ASSERT_TRUE(target->generation > generation);
            assert_retired(&old);
            assert_same_target(&other, acquire(0, &other_request));
            assert_live(texture_count + 3, 2);
            release(0, SR_PREPASS_TARGET_RESET);
            assert_live(0, 0);
        }
    }
    TEST_ASSERT_EQUAL_UINT64(9, owners[0].failures);
    TEST_ASSERT_EQUAL_UINT64(9, owners[0].replacements);
}

static void test_capacity_four_touch_a_then_e_retires_only_b_after_success(void)
{
    TEST_ASSERT_EQUAL_UINT(4, SR_PREPASS_TARGET_CAPACITY);
    SrPrepassTargetRequest wanted[5];
    SrPrepassTarget snapshots[4];
    for (unsigned i = 0; i < 5; ++i)
        wanted[i] = request((uint16_t)(10 + 20 * i), 640, 480, false);
    for (unsigned i = 0; i < 4; ++i) {
        const SrPrepassTarget *target = acquire(0, &wanted[i]);
        assert_target(target, &wanted[i]);
        snapshots[i] = *target;
    }
    assert_same_target(&snapshots[0], acquire(0, &wanted[0]));
    assert_live(12, 4);
    fail_ordinal(3, 3);
    TEST_ASSERT_NULL(acquire(0, &wanted[4]));
    TEST_ASSERT_EQUAL_UINT64(0, owners[0].evictions);
    for (unsigned i = 0; i < 4; ++i) {
        const SrPrepassTargetSlot *slot = find_slot(0, wanted[i].view_base);
        TEST_ASSERT_NOT_NULL(slot);
        assert_same_target(&snapshots[i], &slot->target);
    }
    assert_live(12, 4);
    permit_creation();
    const SrPrepassTarget *target = acquire(0, &wanted[4]);
    assert_target(target, &wanted[4]);
    TEST_ASSERT_NULL(find_slot(0, wanted[1].view_base));
    assert_retired(&snapshots[1]);
    for (unsigned i = 0; i < 4; ++i) {
        if (i == 1) continue;
        const SrPrepassTargetSlot *slot = find_slot(0, wanted[i].view_base);
        TEST_ASSERT_NOT_NULL(slot);
        assert_same_target(&snapshots[i], &slot->target);
    }
    TEST_ASSERT_EQUAL_UINT(4, used_slots(0));
    TEST_ASSERT_EQUAL_UINT64(1, owners[0].evictions);
    TEST_ASSERT_EQUAL_UINT64(0, owners[0].replacements);
    assert_live(12, 4);
}

static void test_reset_and_destroy_are_idempotent_and_generations_keep_increasing(void)
{
    const SrPrepassTargetRequest wanted = request(10, 640, 480, true);
    const SrPrepassTarget *target = acquire(0, &wanted);
    assert_target(target, &wanted);
    TEST_ASSERT_EQUAL_UINT16(0, target->normal);
    TEST_ASSERT_EQUAL_UINT16(0, target->frame_buffer);
    SrPrepassTarget previous = *target;
    release(0, SR_PREPASS_TARGET_RESET);
    release(0, SR_PREPASS_TARGET_RESET);
    assert_retired(&previous);
    assert_live(0, 0);
    TEST_ASSERT_EQUAL_UINT(4, fixture.textures_destroyed);
    TEST_ASSERT_EQUAL_UINT(1, fixture.frame_buffers_destroyed);
    target = acquire(0, &wanted);
    assert_target(target, &wanted);
    TEST_ASSERT_TRUE(target->generation > previous.generation);
    previous = *target;
    release(0, SR_PREPASS_TARGET_DESTROY);
    release(0, SR_PREPASS_TARGET_DESTROY);
    release(0, SR_PREPASS_TARGET_RESET);
    assert_retired(&previous);
    assert_live(0, 0);
    TEST_ASSERT_EQUAL_UINT(8, fixture.textures_destroyed);
    TEST_ASSERT_EQUAL_UINT(2, fixture.frame_buffers_destroyed);
    target = acquire(0, &wanted);
    assert_target(target, &wanted);
    TEST_ASSERT_TRUE(target->generation > previous.generation);
    TEST_ASSERT_EQUAL_UINT64(2, owners[0].retired_targets);
}

static void test_trace_cold_resize_layout_reuse_and_release_use_actual_owner_spans(void)
{
    SrPrepassTargetRequest wanted = request(10, 640, 480, false);
    capture_start();
    const SrPrepassTarget *target = acquire(0, &wanted);
    assert_target(target, &wanted);
    assert_trace_scope("sr.prepass.create.cold", JCE_TRACE_TASK_SUCCEEDED, 0, target);
    SrPrepassTarget previous = *target;
    capture_start();
    assert_same_target(&previous, acquire(0, &wanted));
    JceTraceCursor cursor;
    JceTraceEvent event;
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, &event, 1));
    wanted.width = 800;
    wanted.height = 600;
    capture_start();
    target = acquire(0, &wanted);
    assert_target(target, &wanted);
    uint64_t scope = assert_trace_scope("sr.prepass.create.resize",
        JCE_TRACE_TASK_SUCCEEDED, 0, target);
    assert_trace_target(scope, &previous);
    previous = *target;
    wanted.velocity = true;
    capture_start();
    target = acquire(0, &wanted);
    assert_target(target, &wanted);
    scope = assert_trace_scope("sr.prepass.create.layout", JCE_TRACE_TASK_SUCCEEDED, 0, target);
    assert_trace_target(scope, &previous);
    previous = *target;
    capture_start();
    release(0, SR_PREPASS_TARGET_RESET);
    assert_trace_scope("sr.prepass.release.reset", JCE_TRACE_TASK_SUCCEEDED, 0, &previous);
    target = acquire(0, &wanted);
    assert_target(target, &wanted);
    previous = *target;
    capture_start();
    release(0, SR_PREPASS_TARGET_DESTROY);
    assert_trace_scope("sr.prepass.release.destroy", JCE_TRACE_TASK_SUCCEEDED, 0, &previous);
    assert_live(0, 0);
}

static void test_trace_capacity_correlates_old_and_new_view_generations(void)
{
    SrPrepassTargetRequest wanted = request(10, 640, 480, false);
    SrPrepassTarget oldest;
    for (unsigned i = 0; i < SR_PREPASS_TARGET_CAPACITY; ++i) {
        wanted.view_base = (uint16_t)(10 + 20 * i);
        const SrPrepassTarget *target = acquire(0, &wanted);
        assert_target(target, &wanted);
        if (i == 0) oldest = *target;
    }
    wanted.view_base = 100;
    wanted.width = 1280;
    capture_start();
    const SrPrepassTarget *target = acquire(0, &wanted);
    assert_target(target, &wanted);
    const uint64_t scope = assert_trace_scope("sr.prepass.create.capacity",
        JCE_TRACE_TASK_SUCCEEDED, 0, target);
    assert_trace_target(scope, &oldest);
    assert_retired(&oldest);
    assert_live(12, 4);
}

static void test_trace_failed_replacement_and_oversize_keep_original_request_dimensions(void)
{
    const SrPrepassTargetRequest old_request = request(10, 640, 480, false);
    const SrPrepassTarget *target = acquire(0, &old_request);
    assert_target(target, &old_request);
    const SrPrepassTarget previous = *target;
    SrPrepassTargetRequest wanted = request(10, 1280, 720, true);
    const uint16_t first = fixture.next_texture;
    SrPrepassTarget failed = {0};
    failed.request = wanted;
    failed.normal = first;
    failed.depth = (uint16_t)(first + 1);
    failed.albedo = (uint16_t)(first + 2);
    failed.velocity = (uint16_t)(first + 3);
    failed.frame_buffer = UINT16_MAX;
    fail_ordinal(4, 5);
    capture_start();
    TEST_ASSERT_NULL(acquire(0, &wanted));
    assert_trace_scope("sr.prepass.create.resize", JCE_TRACE_TASK_FAILED, 0, &failed);
    const SrPrepassTargetSlot *slot = find_slot(0, old_request.view_base);
    TEST_ASSERT_NOT_NULL(slot);
    assert_same_target(&previous, &slot->target);
    permit_creation();
    wanted.width = (uint32_t)UINT16_MAX + 1u;
    failed.request = wanted;
    failed.normal = failed.depth = failed.albedo = UINT16_MAX;
    failed.velocity = failed.frame_buffer = UINT16_MAX;
    const unsigned texture_calls = fixture.texture_calls;
    const unsigned frame_buffer_calls = fixture.frame_buffer_calls;
    capture_start();
    TEST_ASSERT_NULL(acquire(0, &wanted));
    assert_trace_scope("sr.prepass.create.resize", JCE_TRACE_TASK_FAILED, 0, &failed);
    slot = find_slot(0, old_request.view_base);
    TEST_ASSERT_NOT_NULL(slot);
    assert_same_target(&previous, &slot->target);
    TEST_ASSERT_EQUAL_UINT(texture_calls, fixture.texture_calls);
    TEST_ASSERT_EQUAL_UINT(frame_buffer_calls, fixture.frame_buffer_calls);
    assert_live(3, 1);
}

static void test_preferred_depth_only_owns_one_plane_and_reuses_handle_zero(void)
{
    SrPrepassTargetRequest wanted = request(120, 2560, 1560, false);
    wanted.prefer_depth_only = true;
    const SrPrepassTarget *target = acquire(0, &wanted);
    assert_target(target, &wanted);
    TEST_ASSERT_EQUAL_UINT16(0, target->depth);
    TEST_ASSERT_EQUAL_UINT16(0, target->frame_buffer);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->normal);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->albedo);
    assert_live(1, 1);
    const SrPrepassTarget saved = *target;
    const unsigned calls = fixture.texture_calls + fixture.frame_buffer_calls;
    assert_same_target(&saved, acquire(0, &wanted));
    TEST_ASSERT_EQUAL_UINT(calls, fixture.texture_calls + fixture.frame_buffer_calls);
    TEST_ASSERT_EQUAL_UINT64(1, owners[0].reuses);
}

static void test_preferred_storage_edges_and_velocity_preserve_fixed_mrt_slots(void)
{
    SrPrepassTargetRequest wanted = request(120, 2560, 1560, false);
    SrPrepassTarget old = *acquire(0, &wanted);
    wanted.prefer_depth_only = true;
    const SrPrepassTarget *target = acquire(0, &wanted);
    assert_target(target, &wanted); assert_retired(&old); assert_live(1, 1);
    TEST_ASSERT_TRUE(target->generation > old.generation);
    old = *target; wanted.velocity = true;
    target = acquire(0, &wanted);
    assert_target(target, &wanted); assert_retired(&old); assert_live(4, 1);
    TEST_ASSERT_TRUE(target->generation > old.generation);
    old = *target; wanted.velocity = false;
    target = acquire(0, &wanted);
    assert_target(target, &wanted); assert_retired(&old); assert_live(1, 1);
    wanted.prefer_depth_only = false;
    old = *target; target = acquire(0, &wanted);
    assert_target(target, &wanted); assert_retired(&old); assert_live(3, 1);
}

static void test_refused_depth_only_fbo_keeps_preferred_key_and_reuses_fallback(void)
{
    fixture.reject_depth_only = true;
    SrPrepassTargetRequest wanted = request(120, 2560, 1560, false);
    wanted.prefer_depth_only = true;
    const SrPrepassTarget *target = acquire(0, &wanted);
    assert_target(target, &wanted); assert_live(3, 1);
    TEST_ASSERT_EQUAL_UINT(3, fixture.texture_calls);
    TEST_ASSERT_EQUAL_UINT(2, fixture.frame_buffer_calls);
    TEST_ASSERT_TRUE(target->request.prefer_depth_only);
    TEST_ASSERT_NOT_EQUAL(UINT16_MAX, target->normal);
    TEST_ASSERT_NOT_EQUAL(UINT16_MAX, target->albedo);
    const SrPrepassTarget saved = *target;
    const unsigned calls = fixture.texture_calls + fixture.frame_buffer_calls;
    assert_same_target(&saved, acquire(0, &wanted));
    TEST_ASSERT_EQUAL_UINT(calls, fixture.texture_calls + fixture.frame_buffer_calls);
    TEST_ASSERT_EQUAL_UINT64(1, owners[0].creates);
    TEST_ASSERT_EQUAL_UINT64(1, owners[0].reuses);
}

static void test_each_preferred_cold_failure_retires_partial_planes_then_retries(void)
{
    SrPrepassTargetRequest wanted = request(120, 2560, 1560, false);
    wanted.prefer_depth_only = true;
    for (unsigned step = 0; step < 4; ++step) {
        fixture.reject_depth_only = step != 0;
        fixture.fail_texture_call = step < 3 ? fixture.texture_calls + step + 1 : 0;
        fixture.fail_frame_buffer_call = step == 3 ? fixture.frame_buffer_calls + 2 : 0;
        TEST_ASSERT_NULL(acquire(0, &wanted));
        assert_live(0, 0); TEST_ASSERT_EQUAL_UINT(0, used_slots(0));
        fixture.reject_depth_only = false;
        fixture.fail_texture_call = fixture.fail_frame_buffer_call = 0;
        const SrPrepassTarget *target = acquire(0, &wanted);
        assert_target(target, &wanted); assert_live(1, 1);
        release(0, SR_PREPASS_TARGET_RESET); assert_live(0, 0);
    }
    TEST_ASSERT_EQUAL_UINT64(4, owners[0].failures);
}

static void test_failed_preferred_replacement_keeps_both_views_and_original_generation(void)
{
    SrPrepassTargetRequest a = request(120, 2560, 1560, false);
    SrPrepassTargetRequest b = request(3, 640, 480, false);
    b.prefer_depth_only = true;
    const SrPrepassTarget old_a = *acquire(0, &a);
    const SrPrepassTarget old_b = *acquire(0, &b);
    a.prefer_depth_only = true;
    fixture.reject_depth_only = true;
    fixture.fail_frame_buffer_call = fixture.frame_buffer_calls + 2;
    TEST_ASSERT_NULL(acquire(0, &a));
    const SrPrepassTargetSlot *slot_a = find_slot(0, a.view_base);
    const SrPrepassTargetSlot *slot_b = find_slot(0, b.view_base);
    TEST_ASSERT_NOT_NULL(slot_a);
    TEST_ASSERT_NOT_NULL(slot_b);
    assert_same_target(&old_a, &slot_a->target);
    assert_same_target(&old_b, &slot_b->target);
    assert_live(4, 2);
    fixture.reject_depth_only = false; fixture.fail_frame_buffer_call = 0;
    const SrPrepassTarget *target = acquire(0, &a);
    assert_target(target, &a); assert_retired(&old_a); assert_live(2, 2);
    TEST_ASSERT_TRUE(target->generation > old_a.generation);
    assert_same_target(&old_b, acquire(0, &b));
}

static void test_preferred_capacity_and_independent_owners_retire_only_oldest_view(void)
{
    SrPrepassTargetRequest wanted[5]; SrPrepassTarget saved[4];
    for (unsigned i = 0; i < 5; ++i) {
        wanted[i] = request((uint16_t)(10 + i * 20), 640, 480, false);
        wanted[i].prefer_depth_only = true;
    }
    for (unsigned i = 0; i < 4; ++i) saved[i] = *acquire(0, &wanted[i]);
    const SrPrepassTarget other = *acquire(1, &wanted[0]);
    TEST_ASSERT_NOT_EQUAL(saved[0].depth, other.depth);
    assert_same_target(&saved[0], acquire(0, &wanted[0]));
    const SrPrepassTarget *added = acquire(0, &wanted[4]);
    assert_target(added, &wanted[4]); assert_retired(&saved[1]);
    TEST_ASSERT_NULL(find_slot(0, wanted[1].view_base));
    assert_same_target(&other, acquire(1, &wanted[0]));
    assert_live(5, 5);
}

static void test_trace_preferred_depth_and_refused_fallback_keep_actual_handle_facts(void)
{
    SrPrepassTargetRequest wanted = request(120, 2560, 1560, false);
    wanted.prefer_depth_only = true;
    capture_start();
    const SrPrepassTarget *target = acquire(0, &wanted);
    assert_target(target, &wanted);
    assert_trace_scope("sr.prepass.create.cold", JCE_TRACE_TASK_SUCCEEDED, 0, target);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, target->normal);
    jce_trace_set_enabled(false); release(0, SR_PREPASS_TARGET_RESET);
    fixture.reject_depth_only = true; capture_start();
    target = acquire(0, &wanted); assert_target(target, &wanted);
    assert_trace_scope("sr.prepass.create.cold", JCE_TRACE_TASK_SUCCEEDED, 0, target);
    TEST_ASSERT_NOT_EQUAL(UINT16_MAX, target->normal);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cold_and_invalid_size_requests_own_no_handles);
    RUN_TEST(test_first_complete_target_accepts_texture_and_framebuffer_zero);
    RUN_TEST(test_a_b_a_different_sizes_remain_two_stable_target_sets);
    RUN_TEST(test_equal_size_views_keep_distinct_handles_and_generations);
    RUN_TEST(test_same_view_in_independent_owners_keeps_independent_resources);
    RUN_TEST(test_resize_and_velocity_edges_replace_only_the_requested_view);
    RUN_TEST(test_attachment_format_changes_require_new_complete_target);
    RUN_TEST(test_each_cold_texture_and_framebuffer_failure_rolls_back_and_retries);
    RUN_TEST(test_each_replacement_failure_preserves_old_and_other_view_then_retries);
    RUN_TEST(test_capacity_four_touch_a_then_e_retires_only_b_after_success);
    RUN_TEST(test_reset_and_destroy_are_idempotent_and_generations_keep_increasing);
    RUN_TEST(test_trace_cold_resize_layout_reuse_and_release_use_actual_owner_spans);
    RUN_TEST(test_trace_capacity_correlates_old_and_new_view_generations);
    RUN_TEST(test_trace_failed_replacement_and_oversize_keep_original_request_dimensions);
    RUN_TEST(test_preferred_depth_only_owns_one_plane_and_reuses_handle_zero);
    RUN_TEST(test_preferred_storage_edges_and_velocity_preserve_fixed_mrt_slots);
    RUN_TEST(test_refused_depth_only_fbo_keeps_preferred_key_and_reuses_fallback);
    RUN_TEST(test_each_preferred_cold_failure_retires_partial_planes_then_retries);
    RUN_TEST(test_failed_preferred_replacement_keeps_both_views_and_original_generation);
    RUN_TEST(test_preferred_capacity_and_independent_owners_retire_only_oldest_view);
    RUN_TEST(test_trace_preferred_depth_and_refused_fallback_keep_actual_handle_facts);
    return UNITY_END();
}
