/* Exact production callback / PNG writer with real SDL encode and file IO.
 * Only GPU requests/readback and deliberate allocation/thread failures are
 * intercepted. A semaphore holds the writer at its actual encoder entry. */
#include "unity.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <bgfx/c99/bgfx.h>
#include <jce/os/core/jce_thread.h>
#include "os/core/jce_memory.h"
#include <jce/renderer/jce_render_readback.h>
#include <stdio.h>
#include <string.h>

static JceSemaphore *write_entered, *write_release;
static bool block_write, fail_copy, fail_worker;
static char requested_path[512];
static unsigned requests, surface_destroys, texture_destroys;
static bool readback_supported;
static unsigned capability_checks,readback_creates,readback_reads,readback_blits;
bool jce_render_readback_format_supported(JceRenderFormat format)
{
    TEST_ASSERT_EQUAL_INT(JCE_RENDER_FORMAT_RGBA16F,format);
    ++capability_checks;
    return readback_supported;
}

static uint32_t frame_index,read_delay;
static uint16_t texture_w, texture_h;
static void *gpu_borrowed_pixels;
static bool backend_stopped, freed_gpu_borrow_early;
static unsigned gpu_buffer_frees;
static const uint8_t rgba[16] = {
    255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255
};
static void test_request_screenshot(bgfx_frame_buffer_handle_t fbo, const char *path)
{
    (void)fbo;
    snprintf(requested_path, sizeof(requested_path), "%s", path);
    ++requests;
}
static bool test_save_png(SDL_Surface *surface, const char *path)
{
    if (block_write) {
        jce_semaphore_signal(write_entered);
        jce_semaphore_wait(write_release);
    }
    return IMG_SavePNG(surface, path);
}
static void test_destroy_surface(SDL_Surface *surface)
{
    ++surface_destroys;
    SDL_DestroySurface(surface);
}
static void *test_malloc(size_t size)
{
    if (fail_copy) {
        fail_copy = false;
        return NULL;
    }
    return jce__malloc_tracked(size);
}
static void test_free(void *pointer)
{
    if (pointer && pointer == gpu_borrowed_pixels) {
        ++gpu_buffer_frees;
        if (!backend_stopped)
            freed_gpu_borrow_early = true;
    }
    jce__free_tracked(pointer);
}
static JceThread *test_create_thread(JceThreadFn fn, void *arg, const char *name)
{
    return fail_worker ? NULL : jce_thread_create(fn, arg, name);
}
static bgfx_texture_handle_t test_create_texture(uint16_t w, uint16_t h,
    bool mips, uint16_t layers, bgfx_texture_format_t format, uint64_t flags,
    const bgfx_memory_t *memory, uint64_t external)
{
    bgfx_texture_handle_t handle = { 7 };
    (void)mips; (void)layers; (void)format; (void)flags;
    (void)memory; (void)external;
    ++readback_creates;
    texture_w = w; texture_h = h;
    return handle;
}
static uint32_t test_read_texture(bgfx_texture_handle_t handle, void *data,
                                   uint16_t layer, uint8_t mip)
{
    ++readback_reads;
    uint16_t *pixels = (uint16_t *)data;
    gpu_borrowed_pixels = data;
    (void)handle; (void)layer; (void)mip;
    for (size_t i = 0; i < (size_t)texture_w * texture_h * 4u; ++i)
        pixels[i] = 0x3c00; /* RGBA16F white */
    return frame_index + read_delay;
}
static void test_blit(bgfx_view_id_t view, bgfx_texture_handle_t dst,
    uint8_t dst_mip, uint16_t dx, uint16_t dy, uint16_t dz,
    bgfx_texture_handle_t src, uint8_t src_mip, uint16_t sx, uint16_t sy,
    uint16_t sz, uint16_t width, uint16_t height, uint16_t depth)
{
    ++readback_blits;
    (void)view; (void)dst; (void)dst_mip; (void)dx; (void)dy; (void)dz;
    (void)src; (void)src_mip; (void)sx; (void)sy; (void)sz;
    (void)width; (void)height; (void)depth;
}
static void test_destroy_texture(bgfx_texture_handle_t handle)
{
    (void)handle;
    ++texture_destroys;
}
uint32_t jce_rcb_host_frame_index(void) { return frame_index; }
void jce_renderer_caps_api_capture_trace(const char *line) { (void)line; }

#define bgfx_request_screen_shot test_request_screenshot
#define bgfx_create_texture_2d test_create_texture
#define bgfx_read_texture test_read_texture
#define bgfx_blit test_blit
#define bgfx_destroy_texture test_destroy_texture
#define IMG_SavePNG test_save_png
#define SDL_DestroySurface test_destroy_surface
#define jce_thread_create test_create_thread
#undef JCE_MALLOC
#define JCE_MALLOC(size) test_malloc(size)
#undef JCE_FREE
#define JCE_FREE(pointer) test_free(pointer)
#ifndef JCE_SCREENSHOT_CALLBACK_OWNER
#define JCE_SCREENSHOT_CALLBACK_OWNER "renderer/jce_renderer_bgfx_callback.c"
#endif
#include JCE_SCREENSHOT_CALLBACK_OWNER
#undef jce_thread_create
#undef SDL_DestroySurface
#undef IMG_SavePNG
#undef bgfx_destroy_texture
#undef bgfx_blit
#undef bgfx_read_texture
#undef bgfx_create_texture_2d
#undef bgfx_request_screen_shot
#include "application/jce_screenshot.c"

void setUp(void)
{
    readback_supported=true;capability_checks=readback_creates=readback_reads=readback_blits=0;
    block_write = true;
    fail_copy = fail_worker = false;
    requests = surface_destroys = texture_destroys = 0;
    frame_index = 100;
    read_delay = 1;
    gpu_borrowed_pixels = NULL;
    backend_stopped = freed_gpu_borrow_early = false;
    gpu_buffer_frees = 0;
    jce_rcb_backend_started(); /* explicit fake backend capability seam */
    write_entered = jce_semaphore_create(0);
    write_release = jce_semaphore_create(0);
    TEST_ASSERT_NOT_NULL(write_entered);
    TEST_ASSERT_NOT_NULL(write_release);
    remove("screenshot-completion.png");
    remove("screenshot-completion.bmp");
    FILE *blocker = fopen("screenshot-blocker", "wb");
    TEST_ASSERT_NOT_NULL(blocker);
    fclose(blocker);
}
void tearDown(void)
{
    jce_semaphore_signal(write_release);
    jce_renderer_readback_capture_shutdown();
    backend_stopped = true;
    jce_rcb_after_backend_shutdown();
    jce_trace_set_enabled(false);
    jce_semaphore_destroy(write_release);
    jce_semaphore_destroy(write_entered);
    remove("screenshot-completion.png");
    remove("screenshot-completion.bmp");
    remove("screenshot-blocker");
}
static void callback(const char *request, uint32_t pitch, uint32_t size)
{
    bgfx_callback_interface_t *cb = jce_rcb_callback_interface();
    cb->vtbl->screen_shot(cb, request, 2, 2, pitch,
                         BGFX_TEXTURE_FORMAT_RGBA8, rgba, size, false);
}
static void deliver(void)
{
    callback(requested_path, 8, sizeof(rgba));
    TEST_ASSERT_TRUE(jce_semaphore_wait_timeout(write_entered, 2000));
}
static void finish_write(void)
{
    jce_semaphore_signal(write_release);
    jce_renderer_readback_capture_flush();
}
static void assert_image(const char *path)
{
    SDL_Surface *image = IMG_Load(path);
    TEST_ASSERT_NOT_NULL(image);
    TEST_ASSERT_EQUAL_INT(2, image->w);
    TEST_ASSERT_EQUAL_INT(2, image->h);
    SDL_DestroySurface(image);
}

/* Private screenshot ledger protocol: RENDER COUNTER events share a namespace.
 * event.id is the accepted uint64 file identity and parent_id is zero.
 * Admit value is backbuffer=0/readback=1; terminal values use the real
 * JCE_TRACE_TASK_* enum. These counters never represent active trace tasks. */
typedef struct ScreenshotLedgerExpected {
    uint64_t identity;
    bool readback, terminal;
    JceTraceTaskState outcome;
} ScreenshotLedgerExpected;

static void begin_request_ledger(void)
{
    jce_trace_set_enabled(true);
    jce_trace_reset(); /* Called before requests, with producers quiescent. */
}
static void assert_request_ledger(const ScreenshotLedgerExpected *expected,
                                 unsigned expected_count)
{
    unsigned admits[JCE_SCREENSHOT_SCHEDULE_MAX] = {0};
    unsigned terminals[JCE_SCREENSHOT_SCHEDULE_MAX] = {0};
    JceTraceCursor cursor;
    JceTraceEvent events[32];
    JceTraceStats stats;
    unsigned total = 0;
    TEST_ASSERT_TRUE(expected_count <= JCE_SCREENSHOT_SCHEDULE_MAX);
    jce_trace_cursor_init(&cursor);
    for (;;) {
        uint32_t count = jce_trace_read(&cursor, events, 32);
        if (!count) break;
        total += count;
        TEST_ASSERT_TRUE_MESSAGE(total <= 4096, "bounded fixture trace must quiesce");
        for (uint32_t i = 0; i < count; ++i) {
            const JceTraceEvent *event = &events[i];
            bool admit = strcmp(event->name, "renderer.screenshot.admit") == 0;
            bool terminal = strcmp(event->name, "renderer.screenshot.terminal") == 0;
            if (!admit && !terminal) continue;
            TEST_ASSERT_EQUAL_INT(JCE_TRACE_EVENT_COUNTER, event->type);
            TEST_ASSERT_EQUAL_INT(JCE_TRACE_CATEGORY_RENDER, event->category);
            TEST_ASSERT_TRUE(event->id != 0);
            TEST_ASSERT_EQUAL_UINT64(0, event->parent_id);
            unsigned match = 0;
            while (match < expected_count && expected[match].identity != event->id)
                ++match;
            TEST_ASSERT_TRUE_MESSAGE(match < expected_count,
                                     "ledger event must belong to an accepted identity");
            if (admit) {
                TEST_ASSERT_EQUAL_UINT32(1, ++admits[match]);
                TEST_ASSERT_EQUAL_UINT32(0, terminals[match]);
                TEST_ASSERT_EQUAL_UINT64(expected[match].readback ? 1 : 0,
                                        event->value_u64);
            } else {
                TEST_ASSERT_TRUE(expected[match].terminal);
                TEST_ASSERT_EQUAL_UINT32(1, admits[match]);
                TEST_ASSERT_EQUAL_UINT32(1, ++terminals[match]);
                TEST_ASSERT_EQUAL_UINT64((uint64_t)expected[match].outcome,
                                        event->value_u64);
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    for (unsigned i = 0; i < expected_count; ++i) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, admits[i],
                                         "accepted request needs one identity-bearing admit");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(expected[i].terminal ? 1 : 0, terminals[i],
                                         "accepted request needs exactly its expected terminal");
    }
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
}
static void assert_one_request_ledger(uint64_t identity, bool readback,
                                     JceTraceTaskState outcome)
{
    const ScreenshotLedgerExpected expected = {identity, readback, true, outcome};
    assert_request_ledger(&expected, 1);
}

static void test_cpu_pixels_marker_precedes_write_and_excludes_invalid_callbacks(void)
{
    begin_request_ledger();
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot_fbo(17, "screenshot-completion.png"));
    const uint64_t accepted = s_file.identity;
    deliver();
    TEST_ASSERT_EQUAL_INT(0, jce_renderer_screenshot_poll());
    callback(requested_path, 8, sizeof(rgba)); /* duplicate while writer is pending */
    finish_write();
    TEST_ASSERT_EQUAL_INT(1, jce_renderer_screenshot_poll());

    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    callback(requested_path, 7, sizeof(rgba)); /* invalid pitch */
    TEST_ASSERT_EQUAL_INT(2, jce_renderer_screenshot_poll());

    JceTraceCursor cursor;
    JceTraceEvent events[16];
    bool admitted = false, terminal = false;
    unsigned pixel_markers = 0;
    jce_trace_cursor_init(&cursor);
    for (;;) {
        uint32_t count = jce_trace_read(&cursor, events, 16);
        if (!count) break;
        for (uint32_t i = 0; i < count; ++i) {
            const JceTraceEvent *event = &events[i];
            if (strcmp(event->name, "renderer.screenshot.cpu_pixels") == 0) {
                TEST_ASSERT_EQUAL_UINT64(accepted, event->id);
                TEST_ASSERT_EQUAL_UINT64(0, event->parent_id);
                TEST_ASSERT_EQUAL_INT(JCE_TRACE_EVENT_COUNTER, event->type);
                TEST_ASSERT_EQUAL_INT(JCE_TRACE_CATEGORY_RENDER, event->category);
                TEST_ASSERT_EQUAL_UINT64(17, event->value_u64);
                TEST_ASSERT_TRUE(admitted);
                TEST_ASSERT_FALSE(terminal);
                ++pixel_markers;
            } else if (event->id == accepted) {
                if (strcmp(event->name, "renderer.screenshot.admit") == 0)
                    admitted = true;
                if (strcmp(event->name, "renderer.screenshot.terminal") == 0)
                    terminal = true;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    TEST_ASSERT_TRUE(terminal);
    TEST_ASSERT_EQUAL_UINT32(1, pixel_markers);
}

static void test_pending_includes_actual_writer(void)
{
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    deliver();
    TEST_ASSERT_TRUE_MESSAGE(jce_renderer_screenshot_pending(),
                            "enqueue is not completed encode/file write");
    TEST_ASSERT_EQUAL_INT(0, jce_screenshot_poll());
}
static void test_busy_rejects_until_actual_writer_finishes(void)
{
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    deliver();
    TEST_ASSERT_FALSE(jce_renderer_request_screenshot("second-shot.png"));
    TEST_ASSERT_FALSE(jce_renderer_readback_capture_submit(4, 50, 2, 2,
                                                         "second-shot.png", 0));
    TEST_ASSERT_EQUAL_UINT32(1, requests);
}
static void test_success_is_retained_and_file_is_real_png(void)
{
    begin_request_ledger();
    TEST_ASSERT_TRUE(jce_screenshot_save("screenshot-completion.png", JCE_SCREENSHOT_PNG));
    uint64_t identity = s_file.identity;
    deliver();
    finish_write();
    TEST_ASSERT_FALSE(jce_screenshot_pending());
    TEST_ASSERT_EQUAL_INT(1, jce_screenshot_poll());
    TEST_ASSERT_EQUAL_INT(1, jce_screenshot_poll());
    TEST_ASSERT_EQUAL_UINT32(2, surface_destroys);
    assert_image("screenshot-completion.png");
    assert_one_request_ledger(identity, false, JCE_TRACE_TASK_SUCCEEDED);
}
static void test_real_write_failure_and_schedule_fail(void)
{
    begin_request_ledger();
    JceScreenshotSchedule schedule;
    char error[100];
    TEST_ASSERT_TRUE(jce_screenshot_schedule_parse("1|/tmp/fail.png", &schedule,
                                                 error, sizeof(error)));
    for (int i = 0; i < 11; ++i)
        jce_screenshot_schedule_tick(&schedule, JCE_SCREENSHOT_CLOCK_PLAYING, 0.1);
    TEST_ASSERT_TRUE(jce_screenshot_schedule_mark_submitted(&schedule));
    TEST_ASSERT_TRUE(jce_screenshot_save("screenshot-blocker/fail.png", JCE_SCREENSHOT_PNG));
    uint64_t identity = s_file.identity;
    deliver();
    finish_write();
    TEST_ASSERT_EQUAL_INT(2, jce_screenshot_poll());
    jce_screenshot_schedule_mark_complete(&schedule, jce_screenshot_poll() == 1);
    TEST_ASSERT_TRUE(schedule.failed);
    TEST_ASSERT_FALSE(jce_screenshot_schedule_finished(&schedule));
    TEST_ASSERT_NULL(jce_screenshot_schedule_due(&schedule));
    assert_one_request_ledger(identity, false, JCE_TRACE_TASK_FAILED);
}
static void test_invalid_pixels_fail_without_encode(void)
{
    begin_request_ledger();
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    uint64_t identity = s_file.identity;
    callback(requested_path, 7, sizeof(rgba));
    TEST_ASSERT_EQUAL_INT(2, jce_renderer_screenshot_poll());
    TEST_ASSERT_FALSE(jce_semaphore_wait_timeout(write_entered, 10));
    assert_one_request_ledger(identity, false, JCE_TRACE_TASK_FAILED);
}
static void test_shutdown_cancels_undelivered_and_late_identity_is_ignored(void)
{
    begin_request_ledger();
    char old_request[512];
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    snprintf(old_request, sizeof(old_request), "%s", requested_path);
    uint64_t old_identity = s_file.identity;
    jce_renderer_readback_capture_shutdown();
    backend_stopped = true;
    jce_rcb_after_backend_shutdown();
    TEST_ASSERT_EQUAL_INT(2, jce_renderer_screenshot_poll());
    TEST_ASSERT_FALSE(jce_renderer_request_screenshot("screenshot-completion.png"));
    backend_stopped = false;
    jce_rcb_backend_started();
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(old_request, requested_path));
    uint64_t new_identity = s_file.identity;
    callback(old_request, 8, sizeof(rgba));
    screenshot_complete(old_identity, false); /* Late old completion cannot fail the new request. */
    TEST_ASSERT_EQUAL_INT(0, jce_renderer_screenshot_poll());
    TEST_ASSERT_FALSE(jce_semaphore_wait_timeout(write_entered, 10));
    deliver();
    finish_write();
    TEST_ASSERT_EQUAL_INT(1, jce_renderer_screenshot_poll());
    callback(old_request, 8, sizeof(rgba));
    callback(requested_path, 8, sizeof(rgba));
    screenshot_complete(old_identity, false);
    screenshot_complete(new_identity, false); /* Duplicate completion cannot change success. */
    jce_renderer_readback_capture_shutdown();
    jce_renderer_readback_capture_shutdown();
    TEST_ASSERT_EQUAL_INT(1, jce_renderer_screenshot_poll());
    const ScreenshotLedgerExpected expected[] = {
        {old_identity, false, true, JCE_TRACE_TASK_CANCELLED},
        {new_identity, false, true, JCE_TRACE_TASK_SUCCEEDED}
    };
    assert_request_ledger(expected, 2);
}
static void test_sink_callbacks_do_not_complete_file_request(void)
{
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    callback(JCE_CAPTURE_SENTINEL, 8, sizeof(rgba));
    callback(JCE_FBO_CAPTURE_SENTINEL, 8, sizeof(rgba));
    TEST_ASSERT_EQUAL_INT(0, jce_renderer_screenshot_poll());
    deliver();
    finish_write();
    TEST_ASSERT_EQUAL_INT(1, jce_renderer_screenshot_poll());
}
static void test_readback_poll_waits_for_file_write_and_surface_has_one_owner(void)
{
    begin_request_ledger();
    TEST_ASSERT_TRUE(jce_renderer_readback_capture_submit(4, 50, 2, 2,
                                                         "screenshot-completion.png", 0));
    uint64_t identity = s_file.identity;
    TEST_ASSERT_FALSE(jce_renderer_request_screenshot("second-shot.png"));
    TEST_ASSERT_EQUAL_INT(0, jce_renderer_readback_capture_poll());
    ++frame_index;
    gpu_borrowed_pixels = NULL;
    TEST_ASSERT_EQUAL_INT(0, jce_renderer_readback_capture_poll());
    TEST_ASSERT_TRUE(jce_semaphore_wait_timeout(write_entered, 2000));
    TEST_ASSERT_EQUAL_INT(0, jce_renderer_readback_capture_poll());
    finish_write();
    TEST_ASSERT_EQUAL_INT(1, jce_renderer_readback_capture_poll());
    TEST_ASSERT_EQUAL_INT(-1, jce_renderer_readback_capture_poll());
    TEST_ASSERT_EQUAL_UINT32(1, surface_destroys);
    TEST_ASSERT_EQUAL_UINT32(1, texture_destroys);
    assert_image("screenshot-completion.png");
    assert_one_request_ledger(identity, true, JCE_TRACE_TASK_SUCCEEDED);
}
static void test_readback_real_write_failure_returns_failed(void)
{
    begin_request_ledger();
    TEST_ASSERT_TRUE(jce_renderer_readback_capture_submit(4, 50, 2, 2,
                                                         "screenshot-blocker/fail.png", 0));
    uint64_t identity = s_file.identity;
    ++frame_index;
    gpu_borrowed_pixels = NULL;
    TEST_ASSERT_EQUAL_INT(0, jce_renderer_readback_capture_poll());
    TEST_ASSERT_TRUE(jce_semaphore_wait_timeout(write_entered, 2000));
    finish_write();
    TEST_ASSERT_EQUAL_INT(2, jce_renderer_readback_capture_poll());
    assert_one_request_ledger(identity, true, JCE_TRACE_TASK_FAILED);
}
static void test_readback_frame_wrap_waits_before_then_consumes_actual_writer(void)
{
    frame_index=UINT32_MAX;
    TEST_ASSERT_TRUE(jce_renderer_readback_capture_submit(4,50,2,2,"screenshot-completion.png",0));
    TEST_ASSERT_EQUAL_INT(0,jce_renderer_readback_capture_poll());
    TEST_ASSERT_EQUAL_UINT32(0,gpu_buffer_frees);
    TEST_ASSERT_FALSE(jce_semaphore_wait_timeout(write_entered,1));
    frame_index=0;gpu_borrowed_pixels=NULL; /* Controlled provider delivered its destination. */
    TEST_ASSERT_EQUAL_INT(0,jce_renderer_readback_capture_poll());
    TEST_ASSERT_TRUE(jce_semaphore_wait_timeout(write_entered,2000));
    finish_write();TEST_ASSERT_EQUAL_INT(1,jce_renderer_readback_capture_poll());
    frame_index=1;TEST_ASSERT_EQUAL_INT(-1,jce_renderer_readback_capture_poll());
    TEST_ASSERT_EQUAL_UINT32(1,texture_destroys);assert_image("screenshot-completion.png");
}
static void test_readback_half_range_has_no_writer_or_premature_cpu_release(void)
{
    read_delay=UINT32_C(0x80000000);
    TEST_ASSERT_TRUE(jce_renderer_readback_capture_submit(4,50,2,2,"screenshot-completion.png",0));
    TEST_ASSERT_EQUAL_INT(0,jce_renderer_readback_capture_poll());
    TEST_ASSERT_FALSE(jce_semaphore_wait_timeout(write_entered,1));
    TEST_ASSERT_EQUAL_UINT32(0,gpu_buffer_frees);TEST_ASSERT_FALSE(freed_gpu_borrow_early);
    jce_renderer_readback_capture_shutdown();TEST_ASSERT_EQUAL_UINT32(0,gpu_buffer_frees);
    backend_stopped=true;jce_rcb_after_backend_shutdown();TEST_ASSERT_EQUAL_UINT32(1,gpu_buffer_frees);
}
static void release_after_delay(void *arg)
{
    (void)arg;
    jce_thread_sleep_ms(15);
    jce_semaphore_signal(write_release);
}
static void test_flush_drains_owned_cpu_write_without_closing_admission(void)
{
    begin_request_ledger();
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    uint64_t written_identity = s_file.identity;
    deliver();
    JceThread *release = jce_thread_create(release_after_delay, NULL, "shot-flush-release");
    TEST_ASSERT_NOT_NULL(release);
    jce_renderer_readback_capture_flush();
    jce_thread_join(release);
    TEST_ASSERT_EQUAL_INT(1, jce_renderer_screenshot_poll());
    assert_image("screenshot-completion.png");
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    uint64_t pending_identity = s_file.identity;
    /* CPU flush neither closes admission nor waits for an undelivered GPU callback. */
    jce_renderer_readback_capture_flush();
    TEST_ASSERT_EQUAL_INT(0, jce_renderer_screenshot_poll());
    jce_renderer_readback_capture_shutdown();
    TEST_ASSERT_EQUAL_INT(2, jce_renderer_screenshot_poll());
    const ScreenshotLedgerExpected expected[] = {
        {written_identity, false, true, JCE_TRACE_TASK_SUCCEEDED},
        {pending_identity, false, true, JCE_TRACE_TASK_CANCELLED}
    };
    assert_request_ledger(expected, 2);
}
static void test_shutdown_drains_actual_active_writer_and_reopens(void)
{
    begin_request_ledger();
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    uint64_t drained_identity = s_file.identity;
    deliver();
    JceThread *release = jce_thread_create(release_after_delay, NULL, "shot-test-release");
    TEST_ASSERT_NOT_NULL(release);
    jce_renderer_readback_capture_shutdown();
    jce_thread_join(release);
    TEST_ASSERT_EQUAL_INT(1, jce_renderer_screenshot_poll());
    assert_image("screenshot-completion.png");
    TEST_ASSERT_FALSE(jce_renderer_request_screenshot("screenshot-completion.png"));
    backend_stopped = true;
    jce_rcb_after_backend_shutdown();
    backend_stopped = false;
    jce_rcb_backend_started();
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    TEST_ASSERT_EQUAL_INT(0, jce_renderer_screenshot_poll());
    uint64_t reopened_identity = s_file.identity;
    jce_renderer_readback_capture_shutdown();
    jce_renderer_readback_capture_shutdown();
    TEST_ASSERT_EQUAL_INT(2, jce_renderer_screenshot_poll());
    const ScreenshotLedgerExpected expected[] = {
        {drained_identity, false, true, JCE_TRACE_TASK_SUCCEEDED},
        {reopened_identity, false, true, JCE_TRACE_TASK_CANCELLED}
    };
    assert_request_ledger(expected, 2);
}
static void test_inline_fallback_reports_actual_write_result(void)
{
    begin_request_ledger();
    block_write = false;
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    uint64_t success_identity = s_file.identity;
    fail_copy = true;
    callback(requested_path, 8, sizeof(rgba));
    TEST_ASSERT_EQUAL_INT(1, jce_renderer_screenshot_poll());
    assert_image("screenshot-completion.png");
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-blocker/fail.png"));
    uint64_t failed_identity = s_file.identity;
    fail_copy = true;
    callback(requested_path, 8, sizeof(rgba));
    TEST_ASSERT_EQUAL_INT(2, jce_renderer_screenshot_poll());
    const ScreenshotLedgerExpected expected[] = {
        {success_identity, false, true, JCE_TRACE_TASK_SUCCEEDED},
        {failed_identity, false, true, JCE_TRACE_TASK_FAILED}
    };
    assert_request_ledger(expected, 2);
}
static void test_no_writer_thread_fallback_is_available(void)
{
    block_write = false;
    fail_worker = true;
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    callback(requested_path, 8, sizeof(rgba));
    TEST_ASSERT_EQUAL_INT(1, jce_renderer_screenshot_poll());
    assert_image("screenshot-completion.png");
}
static void test_unimplemented_format_and_oversized_path_reject_before_gpu(void)
{
    begin_request_ledger();
    char oversized[600];
    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized)-1] = 0;
    TEST_ASSERT_FALSE(jce_screenshot_save("a.png", JCE_SCREENSHOT_BMP));
    TEST_ASSERT_FALSE(jce_screenshot_save("a.png", JCE_SCREENSHOT_TGA));
    TEST_ASSERT_FALSE(jce_screenshot_save("a.png", (JceScreenshotFormat)99));
    TEST_ASSERT_FALSE(jce_screenshot_save("a.BMP", JCE_SCREENSHOT_PNG));
    TEST_ASSERT_FALSE(jce_renderer_request_screenshot(oversized));
    TEST_ASSERT_EQUAL_UINT32(0, requests);
    assert_request_ledger(NULL, 0);
}
static void test_request_identity_exhaustion_rejects_without_alias(void)
{
    begin_request_ledger();
    uint64_t previous = s_file.serial;
    s_file.serial = UINT64_MAX;
    bool accepted = jce_renderer_request_screenshot("screenshot-completion.png");
    s_file.serial = previous;
    TEST_ASSERT_FALSE(accepted);
    TEST_ASSERT_EQUAL_UINT32(0, requests);
    assert_request_ledger(NULL, 0);
}
static void test_gpu_borrow_is_retained_until_actual_backend_stops(void)
{
    begin_request_ledger();
    TEST_ASSERT_TRUE(jce_renderer_readback_capture_submit(4, 50, 2, 2,
                                                         "screenshot-completion.png", 0));
    uint64_t identity = s_file.identity;
    TEST_ASSERT_NOT_NULL(gpu_borrowed_pixels);
    jce_renderer_readback_capture_shutdown();
    TEST_ASSERT_EQUAL_INT(2, jce_renderer_screenshot_poll());
    /* Explicit mutation control: call the real release too early. The guard
     * must fail; normal fixtures never enable this environment knob. */
    const char *mutate = getenv("JCE_TEST_SCREENSHOT_EARLY_READBACK_RELEASE");
    if (mutate && strcmp(mutate, "1") == 0)
        rb_slot_release(&s_rb.slots[0]);
    TEST_ASSERT_FALSE_MESSAGE(freed_gpu_borrow_early,
                             "shutdown cannot free storage bgfx still borrows");
    TEST_ASSERT_EQUAL_UINT32(0, gpu_buffer_frees);
    /* Simulate a backend completing its already-submitted CPU destination
     * after PNG shutdown; the original staging allocation must still exist. */
    memset(gpu_borrowed_pixels, 0, 2u * 2u * 8u);
    backend_stopped = true;
    jce_rcb_after_backend_shutdown();
    TEST_ASSERT_EQUAL_UINT32(1, gpu_buffer_frees);
    TEST_ASSERT_NULL(s_rb.slots[0].pixels);
    TEST_ASSERT_FALSE(jce_renderer_readback_capture_submit(4, 50, 2, 2,
                                                          "second-shot.png", 0));
    assert_one_request_ledger(identity, true, JCE_TRACE_TASK_CANCELLED);
}
static void test_inline_bmp_reports_actual_file_result(void)
{
    block_write = false;
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.bmp"));
    callback(requested_path, 8, sizeof(rgba));
    TEST_ASSERT_EQUAL_INT(1, jce_renderer_screenshot_poll());
    assert_image("screenshot-completion.bmp");
}
static void test_real_write_outcome_is_traced_with_no_active_work(void)
{
    JceTraceCursor cursor;
    JceTraceEvent events[100];
    JceTraceStats stats;
    bool found = false;
    jce_trace_set_enabled(true);
    jce_trace_reset();
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-blocker/fail.png"));
    uint64_t identity = s_file.identity;
    deliver();
    finish_write();
    jce_renderer_readback_capture_shutdown();
    jce_trace_cursor_init(&cursor);
    uint32_t count = jce_trace_read(&cursor, events, 100);
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].type == JCE_TRACE_EVENT_TASK_END &&
            strcmp(events[i].name, "renderer.screenshot_write") == 0) {
            TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, events[i].value_u32[0]);
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    assert_one_request_ledger(identity, false, JCE_TRACE_TASK_FAILED);
}

static void test_request_ledger_preserves_uint64_identity_and_task_context(void)
{
    begin_request_ledger();
    uint64_t previous = s_file.serial;
    uint64_t probe = jce_trace_next_id();
    s_file.serial = UINT64_C(0x100000007);
    jce_trace_task_restore_id(probe);
    bool accepted = jce_renderer_request_screenshot("screenshot-completion.png");
    uint64_t identity = s_file.identity;
    uint64_t after_admit = jce_trace_task_current_id();
    jce_trace_task_restore_id(0);
    s_file.serial = previous; /* Restore before any assertion can leave the fixture. */
    TEST_ASSERT_TRUE(accepted);
    TEST_ASSERT_TRUE(identity > UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT64(probe, after_admit);
    const ScreenshotLedgerExpected pending = {
        identity, false, false, JCE_TRACE_TASK_SUCCEEDED
    };
    assert_request_ledger(&pending, 1);
    deliver();
    finish_write();
    screenshot_complete(identity, false);
    TEST_ASSERT_EQUAL_INT(1, jce_renderer_screenshot_poll());
    assert_one_request_ledger(identity, false, JCE_TRACE_TASK_SUCCEEDED);
}
static void test_readback_allocation_failure_has_request_terminal(void)
{
    begin_request_ledger();
    fail_copy = true;
    TEST_ASSERT_FALSE(jce_renderer_readback_capture_submit(4, 50, 2, 2,
                                                          "screenshot-completion.png", 0));
    uint64_t identity = s_file.identity;
    TEST_ASSERT_EQUAL_INT(2, jce_renderer_screenshot_poll());
    TEST_ASSERT_NULL(gpu_borrowed_pixels);
    TEST_ASSERT_EQUAL_UINT32(1, texture_destroys);
    TEST_ASSERT_FALSE(jce_semaphore_wait_timeout(write_entered, 10));
    jce_renderer_readback_capture_shutdown();
    assert_one_request_ledger(identity, true, JCE_TRACE_TASK_FAILED);
}
static void test_untraced_admission_does_not_create_orphan_terminal(void)
{
    jce_trace_set_enabled(false);
    jce_trace_reset();
    TEST_ASSERT_TRUE(jce_renderer_request_screenshot("screenshot-completion.png"));
    jce_trace_set_enabled(true);
    deliver();
    finish_write();
    jce_renderer_readback_capture_shutdown();
    TEST_ASSERT_EQUAL_INT(1, jce_renderer_screenshot_poll());
    assert_image("screenshot-completion.png");
    assert_request_ledger(NULL, 0);
}

static void test_readback_unsupported_refuses_before_request_or_resources(void)
{
    readback_supported=false;
    TEST_ASSERT_FALSE(jce_renderer_readback_capture_submit(4,50,2,2,"screenshot-completion.png",0));
    TEST_ASSERT_FALSE(jce_renderer_readback_capture_submit_sink(4,50,2,2,0));
    TEST_ASSERT_EQUAL_UINT32(2,capability_checks);
    TEST_ASSERT_EQUAL_UINT32(0,readback_creates);TEST_ASSERT_EQUAL_UINT32(0,readback_blits);
    TEST_ASSERT_EQUAL_UINT32(0,readback_reads);TEST_ASSERT_NULL(gpu_borrowed_pixels);
    TEST_ASSERT_EQUAL_UINT64(0,s_file.readback_identity);
    TEST_ASSERT_TRUE(s_rb.format_unavailable_warned);
    FILE *file=fopen("screenshot-completion.png","rb");
    if(file)fclose(file);TEST_ASSERT_NULL(file);
}
static void test_readback_refusal_latch_resets_with_backend_owner(void)
{
    readback_supported=false;
    TEST_ASSERT_FALSE(jce_renderer_readback_capture_submit(4,50,2,2,"screenshot-completion.png",0));
    TEST_ASSERT_TRUE(s_rb.format_unavailable_warned);
    jce_renderer_readback_capture_shutdown();backend_stopped=true;jce_rcb_after_backend_shutdown();
    TEST_ASSERT_FALSE(s_rb.format_unavailable_warned);
    jce_rcb_backend_started();
    TEST_ASSERT_FALSE(jce_renderer_readback_capture_submit(4,50,2,2,"screenshot-completion.png",0));
    TEST_ASSERT_TRUE(s_rb.format_unavailable_warned);TEST_ASSERT_EQUAL_UINT32(0,readback_reads);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_readback_unsupported_refuses_before_request_or_resources);
    RUN_TEST(test_readback_refusal_latch_resets_with_backend_owner);
    RUN_TEST(test_readback_frame_wrap_waits_before_then_consumes_actual_writer);
    RUN_TEST(test_readback_half_range_has_no_writer_or_premature_cpu_release);
    RUN_TEST(test_pending_includes_actual_writer);
    RUN_TEST(test_cpu_pixels_marker_precedes_write_and_excludes_invalid_callbacks);
    RUN_TEST(test_busy_rejects_until_actual_writer_finishes);
    RUN_TEST(test_success_is_retained_and_file_is_real_png);
    RUN_TEST(test_real_write_failure_and_schedule_fail);
    RUN_TEST(test_invalid_pixels_fail_without_encode);
    RUN_TEST(test_shutdown_cancels_undelivered_and_late_identity_is_ignored);
    RUN_TEST(test_sink_callbacks_do_not_complete_file_request);
    RUN_TEST(test_readback_poll_waits_for_file_write_and_surface_has_one_owner);
    RUN_TEST(test_readback_real_write_failure_returns_failed);
    RUN_TEST(test_flush_drains_owned_cpu_write_without_closing_admission);
    RUN_TEST(test_shutdown_drains_actual_active_writer_and_reopens);
    RUN_TEST(test_inline_fallback_reports_actual_write_result);
    RUN_TEST(test_no_writer_thread_fallback_is_available);
    RUN_TEST(test_unimplemented_format_and_oversized_path_reject_before_gpu);
    RUN_TEST(test_request_identity_exhaustion_rejects_without_alias);
    RUN_TEST(test_gpu_borrow_is_retained_until_actual_backend_stops);
    RUN_TEST(test_inline_bmp_reports_actual_file_result);
    RUN_TEST(test_request_ledger_preserves_uint64_identity_and_task_context);
    RUN_TEST(test_readback_allocation_failure_has_request_terminal);
    RUN_TEST(test_untraced_admission_does_not_create_orphan_terminal);
    RUN_TEST(test_real_write_outcome_is_traced_with_no_active_work);
    return UNITY_END();
}
