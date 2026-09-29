/* The automated capture (JCE_CAPTURE_FRAME) can be taken by the host.
 *
 * WHY THIS SEAM EXISTS.  jce_renderer_request_screenshot() photographs the
 * BACKBUFFER, which in the editor carries the ImGui layer -- so an automated
 * capture there contains the editor rather than the project, and the
 * profiler panel draws a clock into it.  Measured: 53,089 of 3,911,680
 * pixels differ between two runs of one input digest, every one inside rows
 * 1080..1526, with the rendered scene above byte-identical.
 *
 * WHY IT IS A HOOK AND NOT A FRAMEBUFFER INDEX.  The first version of this
 * was `jce_renderer_set_capture_source_fbo(uint16_t)`, and it was written
 * and REVERTED before it shipped: the editor's own UI-free capture reads a
 * TEXTURE -- the postfx output when there is one, else the bridge's colour
 * attachment -- and the y-flip depends on which of the two it is.  An index
 * would have captured the bridge attachment, which with PostFX on is not the
 * image on screen: non-blank, correctly sized, written to the right path,
 * and NOT THE SUBJECT.  Every check that version had would have called it
 * success.
 *
 * WHAT THIS FILE PINS.  The producer side needs a GPU and a real frame, so
 * it is not testable here.  The SEAM is: installed / not installed /
 * declined / cleared, and that the path and user pointer arrive intact.
 * Those are the four states the renderer branches on, and the one that
 * matters most is DECLINED -- a host that has nothing to photograph this
 * frame must fall back to the backbuffer rather than lose the capture.
 */

#include <jce/renderer/jce_renderer.h>
/* The invoker, from a header with NO dependencies.  Its sibling
 * jce_renderer_bgfx_callback.h declares it too -- by including this one --
 * but that header pulls <bgfx/c99/bgfx.h>, and bgfx is a PRIVATE dependency
 * of jce_renderer whose include path does not reach a test.  One
 * declaration, reachable from both sides; a forward declaration here would
 * be a second one that no linker checks against the first. */
#include "renderer/jce_renderer_host_hook.h"

#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void)
{
    /* Leave no hook behind: these run in one process and a survivor would
     * make the next case pass for the previous case's reason. */
    jce_renderer_set_auto_capture_hook(NULL, NULL);
}

typedef struct Seen {
    int  calls;
    char path[256];
    int  marker;
} Seen;

static bool JCE_CALL hook_takes_it(void *user, const char *path)
{
    Seen *s = (Seen *)user;
    s->calls++;
    if (path)
        snprintf(s->path, sizeof s->path, "%s", path);
    return true;
}

static bool JCE_CALL hook_declines(void *user, const char *path)
{
    Seen *s = (Seen *)user;
    (void)path;
    s->calls++;
    return false;
}

/* NO HOOK IS THE DEFAULT, and it must stay the default: every host that
 * existed before this seam relies on the backbuffer path being what happens
 * when nobody has said otherwise. */
static void test_no_hook_means_the_host_did_not_take_it(void)
{
    TEST_ASSERT_FALSE_MESSAGE(jce_rcb_host_took_capture("shot.png"),
        "with no hook installed, the host has not taken the capture and the "
        "renderer must fall back to the backbuffer");
}

static void test_an_installed_hook_takes_it_and_gets_the_path(void)
{
    Seen seen = { 0, {0}, 0 };
    jce_renderer_set_auto_capture_hook(hook_takes_it, &seen);

    TEST_ASSERT_TRUE(jce_rcb_host_took_capture("build/shot-7.png"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen.calls,
        "the hook should have been called exactly once");
    /* THE PATH, not just the call.  A hook that fires with the wrong path
     * writes a real image to the wrong file, which is the failure this whole
     * seam exists to avoid in a different costume. */
    TEST_ASSERT_EQUAL_STRING_MESSAGE("build/shot-7.png", seen.path,
        "the hook must receive the path the renderer was told to write");
}

/* DECLINING IS THE INTERESTING CASE.  The editor's Game View panel can be
 * closed, and then there is no offscreen target to read; the capture must
 * still happen, from the backbuffer.  A seam that treated "declined" as
 * "taken" would silently produce no file at all. */
static void test_a_declining_hook_falls_back(void)
{
    Seen seen = { 0, {0}, 0 };
    jce_renderer_set_auto_capture_hook(hook_declines, &seen);

    TEST_ASSERT_FALSE_MESSAGE(jce_rcb_host_took_capture("shot.png"),
        "a hook that declines must report NOT taken, so the caller falls "
        "back to the backbuffer");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen.calls,
        "and it must still have been asked -- 'declined' and 'never called' "
        "are different facts and only one of them is this test");
}

/* The user pointer is how a host reaches its own state; a seam that dropped
 * it would work perfectly for a host that needs none and fail for every
 * other, which is the kind of thing a single-consumer test never sees. */
static void test_the_user_pointer_arrives(void)
{
    Seen a = { 0, {0}, 111 };
    Seen b = { 0, {0}, 222 };

    jce_renderer_set_auto_capture_hook(hook_takes_it, &a);
    TEST_ASSERT_TRUE(jce_rcb_host_took_capture("a.png"));
    jce_renderer_set_auto_capture_hook(hook_takes_it, &b);
    TEST_ASSERT_TRUE(jce_rcb_host_took_capture("b.png"));

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, a.calls, "the first host got its call");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, b.calls, "the second host got its own");
    TEST_ASSERT_EQUAL_STRING("a.png", a.path);
    TEST_ASSERT_EQUAL_STRING("b.png", b.path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(111, a.marker,
        "installing a second hook must not write through the first pointer");
}

/* CLEARING MUST WORK, and it is not decoration: the editor clears this in
 * jce_editor_game_render_shutdown BEFORE destroying the offscreen target the
 * hook reads.  A hook that outlived its state would not fail loudly -- it
 * would read whatever is at that address now. */
static void test_clearing_restores_the_default(void)
{
    Seen seen = { 0, {0}, 0 };
    jce_renderer_set_auto_capture_hook(hook_takes_it, &seen);
    TEST_ASSERT_TRUE(jce_rcb_host_took_capture("x.png"));

    jce_renderer_set_auto_capture_hook(NULL, NULL);
    TEST_ASSERT_FALSE_MESSAGE(jce_rcb_host_took_capture("y.png"),
        "after clearing, the backbuffer path must be what happens again");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen.calls,
        "and the cleared hook must not be called -- if it were, the editor's "
        "shutdown order would be reading a destroyed target");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_hook_means_the_host_did_not_take_it);
    RUN_TEST(test_an_installed_hook_takes_it_and_gets_the_path);
    RUN_TEST(test_a_declining_hook_falls_back);
    RUN_TEST(test_the_user_pointer_arrives);
    RUN_TEST(test_clearing_restores_the_default);
    return UNITY_END();
}
