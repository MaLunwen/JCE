/* Exact first-party SDL adapter with controlled boundary providers. No SDL
 * initialization, OS window, desktop focus or GPU presentation is exercised. */
#define SDL_CreateWindow fixture_create_window
#define SDL_DestroyWindow fixture_destroy_window
#define SDL_GetWindowSizeInPixels fixture_window_pixels
#define SDL_GetWindowSize fixture_window_points
#define SDL_GetWindowFlags fixture_window_flags
#define SDL_Metal_CreateView fixture_metal_create
#define SDL_Metal_DestroyView fixture_metal_destroy
#define SDL_Metal_GetLayer fixture_metal_layer
#include "os/platform/jce_window.c"
#undef SDL_CreateWindow
#undef SDL_DestroyWindow
#undef SDL_GetWindowSizeInPixels
#undef SDL_GetWindowSize
#undef SDL_GetWindowFlags
#undef SDL_Metal_CreateView
#undef SDL_Metal_DestroyView
#undef SDL_Metal_GetLayer
#include "unity.h"

static unsigned created, destroyed, queried;
static SDL_WindowFlags creation_flags, observed_flags;
static unsigned char native_tag, metal_tag;

static SDL_Window *fixture_native(void)
{
    return (SDL_Window *)&native_tag;
}

SDL_Window *SDLCALL fixture_create_window(const char *title, int w, int h,
                                          SDL_WindowFlags flags)
{
    (void)title;
    TEST_ASSERT_EQUAL_INT(320, w);
    TEST_ASSERT_EQUAL_INT(200, h);
    ++created;
    creation_flags = flags;
    return fixture_native();
}

void SDLCALL fixture_destroy_window(SDL_Window *window)
{
    TEST_ASSERT_EQUAL_PTR(fixture_native(), window);
    ++destroyed;
}

bool SDLCALL fixture_window_pixels(SDL_Window *window, int *w, int *h)
{
    TEST_ASSERT_EQUAL_PTR(fixture_native(), window);
    if (w) *w = 320;
    if (h) *h = 200;
    return true;
}

bool SDLCALL fixture_window_points(SDL_Window *window, int *w, int *h)
{
    TEST_ASSERT_EQUAL_PTR(fixture_native(), window);
    if (w) *w = 320;
    if (h) *h = 200;
    return true;
}

SDL_WindowFlags SDLCALL fixture_window_flags(SDL_Window *window)
{
    TEST_ASSERT_EQUAL_PTR(fixture_native(), window);
    ++queried;
    return observed_flags;
}

SDL_MetalView SDLCALL fixture_metal_create(SDL_Window *window)
{
    TEST_ASSERT_EQUAL_PTR(fixture_native(), window);
    return (SDL_MetalView)&metal_tag;
}

void SDLCALL fixture_metal_destroy(SDL_MetalView view)
{
    TEST_ASSERT_EQUAL_PTR(&metal_tag, view);
}

void *SDLCALL fixture_metal_layer(SDL_MetalView view)
{
    TEST_ASSERT_EQUAL_PTR(&metal_tag, view);
    return &metal_tag;
}

void setUp(void)
{
    created = destroyed = queried = 0;
    creation_flags = observed_flags = 0;
}
void tearDown(void) {}

static void test_invalid_flag_query_preserves_output_without_provider_call(void)
{
    JceWindow window = {0}; uint32_t flags = UINT32_C(0x12345678);
    TEST_ASSERT_FALSE(jce_window_get_flags(NULL, &flags));
    TEST_ASSERT_FALSE(jce_window_get_flags(&window, &flags));
    window.sdl_win = fixture_native();
    TEST_ASSERT_FALSE(jce_window_get_flags(&window, NULL));
    TEST_ASSERT_EQUAL_HEX32(UINT32_C(0x12345678), flags);
    TEST_ASSERT_EQUAL_UINT(0, queried);
}

static void test_valid_zero_state_is_available_and_changes_output(void)
{
    JceWindow window = {0}; uint32_t flags = UINT32_MAX;
    window.sdl_win = fixture_native();
    TEST_ASSERT_TRUE(jce_window_get_flags(&window, &flags));
    TEST_ASSERT_EQUAL_HEX32(0, flags);
    TEST_ASSERT_EQUAL_UINT(1, queried);
}

static void test_supported_high_bit_and_read_only_focus_are_observed(void)
{
    JceWindow window = {0}; uint32_t flags = 0;
    window.sdl_win = fixture_native();
    observed_flags = SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_INPUT_FOCUS |
        SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    TEST_ASSERT_TRUE(jce_window_get_flags(&window, &flags));
    TEST_ASSERT_EQUAL_HEX32(JCE_WINDOW_NOT_FOCUSABLE | JCE_WINDOW_INPUT_FOCUS |
        JCE_WINDOW_HIDDEN | JCE_WINDOW_HIGH_PIXEL_DENSITY, flags);
    TEST_ASSERT_EQUAL_UINT(1, queried);
}

static void test_unexposed_backend_flags_are_not_narrowed_into_public_flags(void)
{
    JceWindow window = {0}; uint32_t flags = UINT32_MAX;
    window.sdl_win = fixture_native();
    observed_flags = UINT64_C(0x8000000000000000) | SDL_WINDOW_ALWAYS_ON_TOP |
        SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED;
    TEST_ASSERT_TRUE(jce_window_get_flags(&window, &flags));
    TEST_ASSERT_EQUAL_HEX32(JCE_WINDOW_RESIZABLE | JCE_WINDOW_MAXIMIZED, flags);
}

static void test_actual_create_adapter_forwards_unsigned_nonfocus_high_bit(void)
{
    JceWindowConfig config = {"window-flags-cpu-control", 320, 200,
        JCE_WINDOW_NOT_FOCUSABLE | JCE_WINDOW_RESIZABLE | JCE_WINDOW_HIGH_PIXEL_DENSITY};
    JceWindow *window = jce_window_create(&config);
    TEST_ASSERT_NOT_NULL(window);
    TEST_ASSERT_EQUAL_UINT(1, created);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)config.flags,
        creation_flags & (uint64_t)config.flags);
    TEST_ASSERT_EQUAL_UINT64(0, creation_flags & UINT64_C(0xffffffff00000000));
    uint32_t flags = UINT32_MAX;
    TEST_ASSERT_TRUE(jce_window_get_flags(window, &flags));
    TEST_ASSERT_EQUAL_HEX32(0, flags); /* Observed state is not creation intent. */
    observed_flags = SDL_WINDOW_INPUT_FOCUS;
    TEST_ASSERT_TRUE(jce_window_get_flags(window, &flags));
    TEST_ASSERT_EQUAL_HEX32(JCE_WINDOW_INPUT_FOCUS, flags);
    jce_window_destroy(window);
    TEST_ASSERT_EQUAL_UINT(1, destroyed);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_invalid_flag_query_preserves_output_without_provider_call);
    RUN_TEST(test_valid_zero_state_is_available_and_changes_output);
    RUN_TEST(test_supported_high_bit_and_read_only_focus_are_observed);
    RUN_TEST(test_unexposed_backend_flags_are_not_narrowed_into_public_flags);
    RUN_TEST(test_actual_create_adapter_forwards_unsigned_nonfocus_high_bit);
    return UNITY_END();
}
