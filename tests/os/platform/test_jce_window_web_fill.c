/* Complete real window owner with typed SDL ports. The headers keep their
 * native ABI; only the Web fill-document branch is modeled after inclusion.
 * This does not create an OS window or certify browser events/pixels. */
#include "unity.h"
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_env.h>
#include <jce/os/platform/jce_window.h>
#include "os/platform/jce_window_internal.h"
#include "os/core/jce_memory.h"
#include <SDL3/SDL_metal.h>

SDL_Window *SDLCALL fill_create_window(const char *, int, int, SDL_WindowFlags);
void SDLCALL fill_destroy_window(SDL_Window *);
bool SDLCALL fill_window_pixels(SDL_Window *, int *, int *);
bool SDLCALL fill_window_points(SDL_Window *, int *, int *);
SDL_WindowFlags SDLCALL fill_window_flags(SDL_Window *);
SDL_MetalView SDLCALL fill_metal_create(SDL_Window *);
void SDLCALL fill_metal_destroy(SDL_MetalView);
void *SDLCALL fill_metal_layer(SDL_MetalView);

#undef JCE_PLATFORM_WEB
#define JCE_PLATFORM_WEB JCE_WINDOW_FILL_WEB_CONTROL
#define SDL_CreateWindow fill_create_window
#define SDL_DestroyWindow fill_destroy_window
#define SDL_GetWindowSizeInPixels fill_window_pixels
#define SDL_GetWindowSize fill_window_points
#define SDL_GetWindowFlags fill_window_flags
#define SDL_Metal_CreateView fill_metal_create
#define SDL_Metal_DestroyView fill_metal_destroy
#define SDL_Metal_GetLayer fill_metal_layer
#ifndef JCE_WINDOW_FILL_OWNER
#define JCE_WINDOW_FILL_OWNER "os/platform/jce_window.c"
#endif
#include JCE_WINDOW_FILL_OWNER
#undef SDL_CreateWindow
#undef SDL_DestroyWindow
#undef SDL_GetWindowSizeInPixels
#undef SDL_GetWindowSize
#undef SDL_GetWindowFlags
#undef SDL_Metal_CreateView
#undef SDL_Metal_DestroyView
#undef SDL_Metal_GetLayer

static unsigned creates, destroys, point_queries, pixel_queries;
static SDL_WindowFlags creation_flags, observed_flags;
static bool reject_create;
static int points_w, points_h, pixels_w, pixels_h;
static unsigned char native_tag, metal_tag;
static JceWindow *owned;

static SDL_Window *native(void) { return (SDL_Window *)&native_tag; }
SDL_Window *SDLCALL fill_create_window(const char *title, int w, int h,
                                      SDL_WindowFlags flags)
{
    TEST_ASSERT_EQUAL_STRING("web-fill-cpu-control", title);
    TEST_ASSERT_EQUAL_INT(320, w); TEST_ASSERT_EQUAL_INT(200, h);
    ++creates; creation_flags = flags;
    return reject_create ? NULL : native();
}
void SDLCALL fill_destroy_window(SDL_Window *window)
{
    TEST_ASSERT_EQUAL_PTR(native(), window); ++destroys;
}
bool SDLCALL fill_window_pixels(SDL_Window *window, int *w, int *h)
{
    TEST_ASSERT_EQUAL_PTR(native(), window); ++pixel_queries;
    if (w) *w = pixels_w;
    if (h) *h = pixels_h;
    return true;
}
bool SDLCALL fill_window_points(SDL_Window *window, int *w, int *h)
{
    TEST_ASSERT_EQUAL_PTR(native(), window); ++point_queries;
    if (w) *w = points_w;
    if (h) *h = points_h;
    return true;
}
SDL_WindowFlags SDLCALL fill_window_flags(SDL_Window *window)
{
    TEST_ASSERT_EQUAL_PTR(native(), window); return observed_flags;
}
SDL_MetalView SDLCALL fill_metal_create(SDL_Window *window)
{
    TEST_ASSERT_EQUAL_PTR(native(), window); return (SDL_MetalView)&metal_tag;
}
void SDLCALL fill_metal_destroy(SDL_MetalView view)
{
    TEST_ASSERT_EQUAL_PTR(&metal_tag, view);
}
void *SDLCALL fill_metal_layer(SDL_MetalView view)
{
    TEST_ASSERT_EQUAL_PTR(&metal_tag, view); return &metal_tag;
}
static void create(uint32_t flags)
{
    const JceWindowConfig cfg = {"web-fill-cpu-control", 320, 200, flags};
    owned = jce_window_create(&cfg);
}
void setUp(void)
{
    creates = destroys = point_queries = pixel_queries = 0;
    creation_flags = observed_flags = 0; reject_create = false; owned = NULL;
    points_w = 896; points_h = 504; pixels_w = 1792; pixels_h = 1008;
}
void tearDown(void)
{
    if (owned) jce_window_destroy(owned);
    owned = NULL;
    TEST_ASSERT_EQUAL_UINT(reject_create ? 0 : creates, destroys);
}
static void test_resizable_creation_uses_document_owner_only_on_web(void)
{
    const uint32_t flags = JCE_WINDOW_RESIZABLE | JCE_WINDOW_HIGH_PIXEL_DENSITY |
        JCE_WINDOW_OPENGL;
    create(flags); TEST_ASSERT_NOT_NULL(owned); TEST_ASSERT_EQUAL_UINT(1, creates);
    TEST_ASSERT_EQUAL_UINT64(flags, creation_flags & flags);
#if JCE_WINDOW_FILL_WEB_CONTROL
    TEST_ASSERT_EQUAL_UINT64(SDL_WINDOW_FILL_DOCUMENT,
        creation_flags & SDL_WINDOW_FILL_DOCUMENT);
#else
    TEST_ASSERT_EQUAL_UINT64(0, creation_flags & SDL_WINDOW_FILL_DOCUMENT);
#endif
}
static void test_fixed_creation_preserves_requested_canvas_policy(void)
{
    const uint32_t flags = JCE_WINDOW_HIDDEN | JCE_WINDOW_OPENGL;
    create(flags); TEST_ASSERT_NOT_NULL(owned);
    TEST_ASSERT_EQUAL_UINT64(flags, creation_flags & flags);
    TEST_ASSERT_EQUAL_UINT64(0, creation_flags & SDL_WINDOW_FILL_DOCUMENT);
    TEST_ASSERT_EQUAL_UINT64(0, creation_flags & SDL_WINDOW_RESIZABLE);
}
static void test_resizable_fullscreen_preserves_unsigned_creation_flags(void)
{
    const uint32_t flags = JCE_WINDOW_RESIZABLE | JCE_WINDOW_FULLSCREEN |
        JCE_WINDOW_NOT_FOCUSABLE;
    create(flags); TEST_ASSERT_NOT_NULL(owned);
    TEST_ASSERT_EQUAL_UINT64(flags, creation_flags & flags);
    TEST_ASSERT_EQUAL_UINT64(0, creation_flags & UINT64_C(0xffffffff00000000));
#if JCE_WINDOW_FILL_WEB_CONTROL
    TEST_ASSERT_EQUAL_UINT64(SDL_WINDOW_FILL_DOCUMENT,
        creation_flags & SDL_WINDOW_FILL_DOCUMENT);
#else
    TEST_ASSERT_EQUAL_UINT64(0, creation_flags & SDL_WINDOW_FILL_DOCUMENT);
#endif
}
static void test_actual_size_and_resize_keep_existing_cached_owners(void)
{
    create(JCE_WINDOW_RESIZABLE); TEST_ASSERT_NOT_NULL(owned);
    uint32_t w = 0, h = 0;
    jce_window_get_point_size(owned, &w, &h);
    TEST_ASSERT_EQUAL_UINT32(896, w); TEST_ASSERT_EQUAL_UINT32(504, h);
    jce_window_get_size(owned, &w, &h);
    TEST_ASSERT_EQUAL_UINT32(1792, w); TEST_ASSERT_EQUAL_UINT32(1008, h);
    int lw = 0, lh = 0; jce_window_get_logical(owned, &lw, &lh);
    TEST_ASSERT_EQUAL_INT(320, lw); TEST_ASSERT_EQUAL_INT(200, lh);
    points_w = 640; points_h = 360;
    jce_window_handle_resize(owned, 1280, 720);
    jce_window_get_point_size(owned, &w, &h);
    TEST_ASSERT_EQUAL_UINT32(640, w); TEST_ASSERT_EQUAL_UINT32(360, h);
    jce_window_get_size(owned, &w, &h);
    TEST_ASSERT_EQUAL_UINT32(1280, w); TEST_ASSERT_EQUAL_UINT32(720, h);
    TEST_ASSERT_EQUAL_UINT(2, point_queries); TEST_ASSERT_EQUAL_UINT(1, pixel_queries);
}
static void test_private_fill_flag_does_not_enter_public_observed_state(void)
{
    create(JCE_WINDOW_RESIZABLE); TEST_ASSERT_NOT_NULL(owned);
    observed_flags = SDL_WINDOW_FILL_DOCUMENT | SDL_WINDOW_INPUT_FOCUS |
        SDL_WINDOW_RESIZABLE;
    uint32_t flags = 0; TEST_ASSERT_TRUE(jce_window_get_flags(owned, &flags));
    TEST_ASSERT_EQUAL_HEX32(JCE_WINDOW_INPUT_FOCUS | JCE_WINDOW_RESIZABLE, flags);
}
static void test_create_refusal_has_no_size_queries_or_window_destruction(void)
{
    reject_create = true; create(JCE_WINDOW_RESIZABLE);
    TEST_ASSERT_NULL(owned); TEST_ASSERT_EQUAL_UINT(1, creates);
    TEST_ASSERT_EQUAL_UINT(0, point_queries); TEST_ASSERT_EQUAL_UINT(0, pixel_queries);
    TEST_ASSERT_EQUAL_UINT(0, destroys);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_resizable_creation_uses_document_owner_only_on_web);
    RUN_TEST(test_fixed_creation_preserves_requested_canvas_policy);
    RUN_TEST(test_resizable_fullscreen_preserves_unsigned_creation_flags);
    RUN_TEST(test_actual_size_and_resize_keep_existing_cached_owners);
    RUN_TEST(test_private_fill_flag_does_not_enter_public_observed_state);
    RUN_TEST(test_create_refusal_has_no_size_queries_or_window_destruction);
    return UNITY_END();
}
