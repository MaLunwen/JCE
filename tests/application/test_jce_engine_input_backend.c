/* test_jce_engine_input_backend.c
 *
 * Regression for Plan B Task 1 (523904e8): jce_input_create() was inverted to
 * install NO backend and left a comment claiming "jce_engine.c installs
 * jce_input_sdl_backend() explicitly right after this call" -- but it never
 * did.  With a NULL backend, jce_input_devices_attach() never calls
 * open_device, so SDL_OpenGamepad is never called and no gamepad ever
 * attaches.  (The function named here used to be add_gamepad(), which Task 6
 * deleted along with the pad-index array it fed; the path is otherwise the
 * same one.)
 *
 * This only breaks in the WINDOWED boot: the headless path sets e->input =
 * NULL outright (grep jce_engine.c for the `e->input = NULL;` whose comment
 * reads "no window = no raw input device" -- do NOT trust a line number here,
 * this file has already shipped two stale ones) and never reaches the install
 * line, so
 * test_jce_headless_boot.c cannot see
 * this bug no matter how it is written.  The engine's JceInput is reachable
 * from a unit test only through JceServices.input, handed to the app's
 * init() callback -- the same seam test_jce_headless_boot.c uses to observe
 * svc->window / svc->renderer / svc->audio.  Reaching it here requires a
 * real (non-headless) jce_engine_create(), so this self-ignores wherever
 * that is not available (no display/GPU, or the renderer fell back before
 * input is even created) -- the same posture as the audio master-tap test
 * (tests/middleware/audio/test_jce_audio_master_tap.c) on a deviceless CI.
 *
 * THAT ASSERTION IS NOT RELIED ON IN CI, AND THAT IS WHY THERE ARE NOW TWO
 * TESTS IN THIS FILE.  .github/workflows/ci.yml runs `ctest -L unit` on
 * ubuntu-latest, windows-latest and macos-latest.  Unity's UNITY_END
 * (UnityEnd() in tests/third_party/unity/unity.c) returns Unity.TestFailures,
 * so an ignore is a PASS -- a test that self-ignores everywhere is a green tick
 * proving nothing about the thing it names, which is the same shape as the P0
 * itself.
 *
 * WHICH BRANCH EACH RUNNER TAKES HAS NOT BEEN OBSERVED, and the earlier
 * version of this paragraph asserted it anyway.  No CI log was read; ubuntu
 * "fails outright" and windows/macos "overwhelmingly likely fall back" were
 * inferences.  One of them is worse than a guess: reaching the "renderer fell
 * back" ignore below means passing through the SDL_ShowSimpleMessageBox in
 * jce_engine_create's fallback block FIRST, and on this project's Windows
 * desktop that call blocks until dismissed (measured: a JCE_FORCE_FALLBACK=1
 * run had to be killed, exit 124, twice).  On a runner with a real session it
 * would therefore not produce an ignore at all -- ctest's per-test TIMEOUT 60
 * (tests/CMakeLists.txt, jce_add_unit_test's set_tests_properties) would kill
 * it and report a FAILURE.  So "the fallback ignore is routinely taken in CI"
 * must not be assumed in either direction; what IS true is that the headless
 * test below never touches that path.
 *
 * Simply booting THIS test headless would not fix that -- it would make it
 * worse.  The headless path sets e->input = NULL and never reaches the install
 * line, so a headless jce_engine_create() would assert about a code path the
 * bug cannot live in, and would be vacuous while looking like coverage.
 *
 * What closes it is that the install line no longer lives only on the windowed
 * boot: jce_engine_create_windowed_input()
 * (engine/src/application/jce_engine_windowed_input.h) is the one place a
 * windowed engine builds its input system, it needs no window, no GPU and no
 * SDL_Init, and test_windowed_input_construction_installs_the_sdl_backend
 * below calls it directly.  Delete the jce_input_set_backend() call inside it
 * and that test fails.
 *
 * MEASURED ON WINDOWS x64 ONLY -- plain, and under JCE_HEADLESS=1, which skips
 * the windowed half exactly as a display-less runner does and still exits 1
 * (mutation table in 6bc1ba2e).  Nothing here, mutated or unmutated, has been
 * run on Linux or macOS: that it behaves the same on ubuntu-latest, which is
 * merge-blocking, is an INFERENCE from SDL_OpenGamepad(0) being documented to
 * fail for an invalid id on every backend, not a measurement.
 *
 * The windowed test below is NOT redundant with it.  It is what covers the one
 * thing the headless test cannot see -- that jce_engine_create() calls that
 * function at all rather than jce_input_create() directly -- and on a machine
 * that can boot a window, JCE_REQUIRE_WINDOWED_INPUT=1 turns its three
 * host-limitation ignores into failures so "it ran" is checkable instead of
 * assumed.
 *
 * What the ignore may NOT swallow is a real regression.  "svc->input is NULL"
 * is only excusable when the engine legitimately never built one: headless.
 * A windowed boot that ran app init and still handed the app a NULL input is
 * a defect in the same neighbourhood as the one this file exists for, so it
 * FAILS here rather than being filed under "no display".
 *
 * Once svc->input is captured, the fix cannot be proven by reading
 * JceInput's backend field -- it is opaque, and there is no accessor for it
 * (only jce_input_set_backend(), a setter).  So this proves it by BEHAVIOUR
 * instead: submit a DEVICE_ADDED event for SDL_JoystickID 0, which the SDL3
 * header (SDL_joystick.h) documents as "The value 0 is an invalid ID" --
 * true on every platform, real hardware or none.
 *   - No backend installed (the regression): jce_input_devices_attach() builds
 *     the device record straight from the lifecycle event, unconditionally.
 *     The device count goes up.
 *   - jce_input_sdl_backend() installed (the fix): jce_input_devices_attach()
 *     calls open_device -> SDL_OpenGamepad(0), which SDL guarantees fails, so
 *     the device is refused and the count does not move.
 *
 * The count is jce_input_device_ids(), not the deleted jce_input_gamepad_count().
 * That substitution is load-bearing rather than mechanical -- see the note at
 * the assertion.
 */

#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_event.h>
#include <jce/renderer/jce_renderer.h>   /* jce_renderer_is_headless */

/* engine/src is on this target's include path (tests/application/
 * CMakeLists.txt).  Not a public header, and it must not become one: it
 * declares a boot-internal seam. */
#include "application/jce_engine_windowed_input.h"

#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>   /* getenv — JCE_REQUIRE_WINDOWED_INPUT */
#include <string.h>

/* Same stub-PAK contract as test_jce_headless_boot.c: a project with no
 * cooked assets links a 1-byte stub. */
const unsigned char assets_pak_data[1] = { 0 };
const size_t        assets_pak_data_size = 0;

static bool         s_init_called;
static JceInput    *s_captured_input;
static JceRenderer *s_captured_renderer;

void setUp(void)
{
    s_init_called       = false;
    s_captured_input    = NULL;
    s_captured_renderer = NULL;
}

void tearDown(void) {}

static bool eib_init(const JceServices *svc, void *user_data)
{
    (void)user_data;
    s_init_called       = true;
    s_captured_input    = svc->input;
    /* Captured so a NULL input can be ATTRIBUTED rather than excused: only the
     * headless renderer legitimately implies e->input == NULL. */
    s_captured_renderer = svc->renderer;
    return true;
}

static void eib_update(float dt, void *user_data)
{
    (void)dt;
    (void)user_data;
}

static void eib_exit(void *user_data) { (void)user_data; }

/* The probe, in one place because both tests make exactly the same claim about
 * two different handles.
 *
 * DEVICE_ADDED for SDL_JoystickID 0, which SDL_joystick.h documents as "The
 * value 0 is an invalid ID" -- true on every platform, with real hardware
 * plugged in or none:
 *   - no backend (the regression): jce_input_devices_attach() builds the
 *     device record straight from the lifecycle event, unconditionally, and
 *     the device count goes UP.
 *   - jce_input_sdl_backend() installed (the fix): attach calls open_device ->
 *     SDL_OpenGamepad(0), which fails (unmapped id, and it fails just as
 *     surely with the joystick subsystem uninitialised), so the device is
 *     refused and the count does NOT move.  Observed on Windows -- SDL itself
 *     logs "SDL_OpenGamepad(0) failed: Couldn't find mapping for device (0)".
 *     On the evdev/udev path that graceful failure is inferred from the same
 *     SDL contract, not observed.
 * Returns the delta: 0 = a backend refused it, 1 = nothing did. */
static int eib_invalid_device_delta(JceInput *in)
{
    JceDeviceId   ids[JCE_INPUT_MAX_DEVICES];
    JceInputEvent ev;
    int before, after;

    memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_ADDED;
    ev.device.instance = 0;
    /* Class and layout are stated rather than left at mk-zero, and that is not
     * cosmetic.  This test used to count through jce_input_gamepad_count(),
     * which after f8179027 only counted GAMEPAD-LAYOUT devices -- and a zeroed
     * event reads as a RAW keyboard, so the count could not move whether or
     * not a backend was installed.  MEASURED, not inferred: with the backend's
     * refusal ignored -- the exact defect this file exists to catch -- that
     * layout-gated shape still reports PASS, while the enumeration below
     * reports "Expected 0 Was 1".  jce_input_device_ids() has no layout gate,
     * so with no backend the record is built from the event alone and the
     * count DOES move, which is what makes the assertion say something. */
    ev.device.cls    = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    ev.device.layout = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;

    before = jce_input_device_ids(in, ids, JCE_INPUT_MAX_DEVICES);
    jce_input_submit(in, &ev, 1);
    after  = jce_input_device_ids(in, ids, JCE_INPUT_MAX_DEVICES);
    return after - before;
}

/* THE CI-REACHABLE HALF.  No window, no GPU, no SDL_Init, no display: this one
 * RUNS on every runner in the matrix and on a developer desktop alike, and it
 * is the assertion that makes the P0 a gated defect rather than a documented
 * one.  It asserts about jce_engine_create_windowed_input() -- the single
 * place a windowed boot builds its input system -- so removing the install
 * from the engine reddens it here even where no window can be created
 * (measured under JCE_HEADLESS=1 on Windows x64; the platform caveat at the
 * top of this file says what that does and does not cover elsewhere).
 *
 * What it deliberately does NOT prove is that jce_engine_create() calls this
 * function; that is the windowed test below.  Two halves agreeing is not
 * evidence, so they are asked different questions. */
void test_windowed_input_construction_installs_the_sdl_backend(void)
{
    JceInput *in = jce_engine_create_windowed_input();
    TEST_ASSERT_NOT_NULL_MESSAGE(in,
        "jce_engine_create_windowed_input() returned NULL -- allocation "
        "failure is the only documented reason, and it needs no window");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, eib_invalid_device_delta(in),
        "the windowed engine's input constructor accepted a DEVICE_ADDED for "
        "SDL_JoystickID 0 (a documented-invalid id) -- no backend is "
        "installed, i.e. jce_engine_create_windowed_input() no longer calls "
        "jce_input_set_backend(in, jce_input_sdl_backend())");

    jce_input_destroy(in);
}

/* JCE_REQUIRE_WINDOWED_INPUT=1 marks a run as one that MUST reach the windowed
 * assertion below, turning this file's three host-limitation ignores into
 * failures.  Unset (CI, a laptop on a train), the ignores stay ignores and the
 * headless test above is what guards the P0. */
static bool eib_require_windowed(void)
{
    const char *v = getenv("JCE_REQUIRE_WINDOWED_INPUT");
    return v && v[0] && v[0] != '0';
}

#define EIB_SKIP_OR_FAIL(msg)                                                 \
    do {                                                                      \
        if (eib_require_windowed())                                           \
            TEST_FAIL_MESSAGE("JCE_REQUIRE_WINDOWED_INPUT=1 demanded the "    \
                              "windowed assertion, and it was not reached: "  \
                              msg);                                           \
        TEST_IGNORE_MESSAGE(msg);                                             \
    } while (0)

/* Same helper, same reason, as test_jce_headless_boot.c's. */
static void eib_set_env(const char *name, const char *value)
{
#if defined(_WIN32)
    (void)_putenv_s(name, value);
#else
    (void)setenv(name, value, 1);
#endif
}

void test_engine_installs_a_gamepad_backend_on_windowed_boot(void)
{
    /* WINDOWED, BUT NOT ON THE USER'S DESKTOP.  This test needs a real
     * windowed boot -- that is its whole subject -- and it does NOT need to
     * take the focus and the keyboard of whoever is running the suite.
     * JCE_WINDOW_HIDDEN keeps every part of the windowed path this test
     * asserts about (window, device, app init, the input backend install) and
     * withholds only the mapping.
     *
     * Set HERE rather than as a ctest ENVIRONMENT property, because that is a
     * property of the LAUNCH and not of the program: running this exe directly
     * -- which is how anyone debugs it -- would open the window again. */
    eib_set_env("JCE_WINDOW_HIDDEN", "1");

    JceAppDesc desc = (JceAppDesc){0};
    desc.name     = "engine-input-backend-test";
    desc.headless = false;      /* the whole point: the headless path never
                                    reaches the install line at all */
    desc.init     = eib_init;
    desc.update   = eib_update;
    desc.exit     = eib_exit;

    jce_engine_set_app_desc(&desc);

    char  arg0[] = "test_jce_engine_input_backend";
    char *argv[] = { arg0, NULL };
    JceEngine *e = jce_engine_create(1, argv);

    /* Three distinct outcomes, named separately, because "the host cannot boot
     * a window" and "the engine booted a window and handed the app a NULL
     * input" are not the same event and must not share an exit code. */

    if (!e) {
        /* jce_engine_create() failed: no display, no GPU, or window creation
         * refused.  Nothing about the install line was reached. */
        EIB_SKIP_OR_FAIL(
            "SKIPPED (no windowed boot): jce_engine_create() returned NULL -- "
            "no display/GPU on this host, so the backend-install line was "
            "never reached");
        return;
    }

    if (!s_init_called) {
        /* The engine exists but app init never ran, which on the windowed path
         * means exactly one thing: the renderer fell back to safe mode and
         * jce_engine_create returned at that early exit, which is upstream of
         * app init -- so this test was never handed svc->input.
         *
         * NOT the same statement as before Plan B Task 8, and the difference
         * matters: input and the action map are now created ABOVE that return,
         * so on the fallback path they DO exist (that was the third defect
         * Task 8 fixed).  They are simply not observable from here, because
         * JceServices only reaches an app whose init() ran -- which is also
         * why "every JceServices.actions consumer dereferenced NULL" was never
         * true of this path: nothing is handed a JceServices on it at all.  The
         * map that now exists is loaded, not live; jce_actions_update() is
         * below the fallback branch's return in jce_engine_iterate. */
        jce_engine_destroy(e);
        EIB_SKIP_OR_FAIL(
            "SKIPPED (renderer fell back): engine created but app init never "
            "ran -- jce_engine_create returned at the fallback-renderer early "
            "exit, which is upstream of app init, so svc->input never reached "
            "this test (input itself IS created on that path)");
        return;
    }

    if (!s_captured_input) {
        /* init RAN and svc->input is still NULL.  Exactly one boot legitimately
         * does that: headless, which sets e->input = NULL by design and never
         * reaches the install line.  desc.headless is false above, but
         * JCE_HEADLESS=1 in the environment overrides it (the getenv in
         * jce_engine_create's headless-selection block -- named, not numbered,
         * because the number this comment shipped with was already stale). */
        bool headless = s_captured_renderer &&
                        jce_renderer_is_headless(s_captured_renderer);
        jce_engine_destroy(e);
        if (headless) {
            EIB_SKIP_OR_FAIL(
                "SKIPPED (headless): JCE_HEADLESS overrode desc.headless=false; "
                "the headless path sets input to NULL by design and never "
                "reaches the backend-install line");
            return;
        }
        /* Windowed, initialised, and no input handle.  Not a host limitation --
         * a regression, and it must not hide behind an ignore. */
        TEST_FAIL_MESSAGE(
            "windowed engine boot ran app init but svc->input is NULL on a "
            "non-headless renderer -- the engine built a window and no input "
            "system, which no supported boot path does");
        return;
    }

    /* The engine's OWN JceInput, reached through the same JceServices seam a
     * game gets -- so this covers the call site, not just the constructor. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0,
        eib_invalid_device_delta(s_captured_input),
        "engine's JceInput accepted a DEVICE_ADDED for SDL_JoystickID 0 "
        "(a documented-invalid id) -- no backend is installed, i.e. "
        "jce_engine_create() never went through "
        "jce_engine_create_windowed_input()");

    jce_engine_destroy(e);
}

int main(void)
{
    UNITY_BEGIN();
    /* Constructor first, engine second: the windowed test calls SDL_Init and
     * SDL_Quit around itself, and the constructor test asserts about a path
     * that must hold with SDL in ANY state -- including never initialised,
     * which is what a CI runner gives it. */
    RUN_TEST(test_windowed_input_construction_installs_the_sdl_backend);
    RUN_TEST(test_engine_installs_a_gamepad_backend_on_windowed_boot);
    return UNITY_END();
}
