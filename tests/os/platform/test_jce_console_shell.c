/*
 * test_jce_console_shell.c
 *
 * The developer console a SHIPPED GAME can open.
 *
 * debug.console.cvars-at-runtime: the cvar registry, the command table, the
 * parser and the line/history/completion session are all complete and public,
 * and the only interactive reader in the entire tree was the EDITOR's console
 * panel.  A shipped game got an FPS overlay and no way to change a cvar.
 *
 * WHY THESE CASES RUN AT ALL.  jce_ui_create needs a JceRenderer, NO test in
 * this tree creates a JceUIContext, and linking jce_ui into a test drags
 * RmlUi with it -- whose prebuilt objects reference an MSVC STL symbol this
 * toolchain does not provide, so such a test does not even link.  The first
 * version of this console was one object that both edited a line and drew it,
 * and that is exactly how it failed.  The shell is now the console's whole
 * behaviour with no document in it, living beside JceInput in os/platform;
 * jce_ui_console_overlay reads what it reports and writes RML, and has no
 * logic left to test.
 *
 * THE LOAD-BEARING CASES, neither of which is "it opens":
 *
 *   an open console must CONSUME input, and a closed one must not.  A console
 *   that lets keys through walks the player around while they type; one that
 *   eats them while closed freezes the game.  Both directions asserted,
 *   including the frame where nothing was typed.
 *
 *   scrollback index 0 is the OLDEST line AFTER THE RING WRAPS.  Before it
 *   wraps, reading from slot 0 and reading from `head` give the same answer,
 *   so a wrong implementation is invisible until a session prints more than
 *   the ring holds -- which is one `list` over a few hundred cvars.
 *
 * IN THE TREE: tests/ is tracked on this branch and gitignored on `main`.
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */

#include <jce/os/platform/jce_console_shell.h>

#include <jce/os/core/jce_console.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_event.h>
#include <jce/os/platform/jce_keys.h>

#include <stdio.h>
#include <string.h>

#include "unity.h"

static JceInput          *g_in;
static JceConsoleShell *g_ov;

void setUp(void)
{
    g_in = jce_input_create();
    JceConsoleShellDesc d;
    memset(&d, 0, sizeof d);
    g_ov = jce_console_shell_create(&d);
}

void tearDown(void)
{
    jce_console_shell_destroy(g_ov);
    jce_input_destroy(g_in);
    g_ov = NULL;
    g_in = NULL;
}

/* ── feeding input ───────────────────────────────────────────────────── */

/* One frame carrying one key event.  jce_input_update() is the frame boundary
 * the pressed/repeated edges are computed against, so every helper starts
 * with it.
 *
 * THE RELEASE IS NOT OPTIONAL, and leaving it out is how the first version of
 * this file reported a defect that was not there.  jce_input_key_pressed is a
 * STRICT RISING EDGE: a key held down across two frames fires once.  Without
 * an up event the second press of the same key is not an edge at all, so the
 * toggle appeared not to close the console -- while Backspace, which reads
 * key_REPEATED, kept working and hid the pattern.  Real input always releases;
 * a test that does not is measuring a key nobody let go of. */
static void frame_key_ev(int scancode, unsigned down)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.size         = (uint32_t)sizeof ev;
    ev.kind         = JCE_INPUT_EVENT_KEY;
    ev.key.scancode = scancode;
    ev.key.down     = (uint8_t)down;
    ev.key.repeat   = 0u;
    jce_input_update(g_in);
    jce_input_submit(g_in, &ev, 1);
}

static void frame_key(int scancode) { frame_key_ev(scancode, 1u); }
static void frame_key_up(int scancode) { frame_key_ev(scancode, 0u); }

static void frame_text(const char *utf8)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.size = (uint32_t)sizeof ev;
    ev.kind = JCE_INPUT_EVENT_TEXT;
    snprintf(ev.text.utf8, sizeof ev.text.utf8, "%s", utf8);
    jce_input_update(g_in);
    jce_input_submit(g_in, &ev, 1);
}

static void frame_nothing(void) { jce_input_update(g_in); }

static void type(const char *s)
{
    frame_text(s);
    (void)jce_console_shell_handle_input(g_ov, g_in);
}

static void press(int scancode)
{
    frame_key(scancode);
    (void)jce_console_shell_handle_input(g_ov, g_in);
    /* Let go, so the NEXT press of the same key is a fresh edge. */
    frame_key_up(scancode);
    (void)jce_console_shell_handle_input(g_ov, g_in);
}

static void open_console(void)
{
    press(JCE_KEY_GRAVE);
    TEST_ASSERT_TRUE_MESSAGE(jce_console_shell_is_visible(g_ov),
        "the toggle key did not open the console");
}

/* ── cases ───────────────────────────────────────────────────────────── */

static void test_it_starts_closed_and_toggles(void)
{
    TEST_ASSERT_FALSE_MESSAGE(jce_console_shell_is_visible(g_ov),
        "the console opened itself, which would cover a shipped game's first "
        "frame");

    frame_key(JCE_KEY_GRAVE);
    TEST_ASSERT_TRUE_MESSAGE(jce_console_shell_handle_input(g_ov, g_in),
        "the toggle key was not consumed on the way IN, so the character that "
        "opens the console also lands in its line");
    TEST_ASSERT_TRUE(jce_console_shell_is_visible(g_ov));
    frame_key_up(JCE_KEY_GRAVE);
    (void)jce_console_shell_handle_input(g_ov, g_in);

    frame_key(JCE_KEY_GRAVE);
    TEST_ASSERT_TRUE_MESSAGE(jce_console_shell_handle_input(g_ov, g_in),
        "the toggle key was not consumed on the way OUT, so it reaches the "
        "game on the frame the console closes");
    TEST_ASSERT_FALSE_MESSAGE(jce_console_shell_is_visible(g_ov),
        "the second press did not close it");
}

/* THE GAME MUST KEEP ITS INPUT while the console is closed, and lose all of
 * it while the console is open.  Both halves, because either alone is
 * satisfied by a handler that always returns the same answer. */
static void test_a_closed_console_consumes_nothing(void)
{
    frame_text("w");
    TEST_ASSERT_FALSE_MESSAGE(jce_console_shell_handle_input(g_ov, g_in),
        "a CLOSED console consumed input -- the game is frozen and nothing "
        "says why");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", jce_console_shell_line(g_ov),
        "a closed console recorded a keystroke into its line");
}

static void test_an_open_console_consumes_even_an_empty_frame(void)
{
    open_console();
    frame_nothing();
    TEST_ASSERT_TRUE_MESSAGE(jce_console_shell_handle_input(g_ov, g_in),
        "an OPEN console let a frame through because nothing was typed on it "
        "-- the player walks forward while the console is up");
}

static void test_typing_and_backspace_edit_the_line(void)
{
    open_console();
    type("r.taa");
    TEST_ASSERT_EQUAL_STRING("r.taa", jce_console_shell_line(g_ov));

    press(JCE_KEY_BACKSPACE);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("r.ta", jce_console_shell_line(g_ov),
        "Backspace did not delete -- and note it is bound to key_REPEATED, "
        "not key_pressed, so a strict-edge regression shows up here and not "
        "only when someone holds the key");

    /* Backspace on an empty line must not underflow. */
    for (int i = 0; i < 8; ++i) press(JCE_KEY_BACKSPACE);
    TEST_ASSERT_EQUAL_STRING("", jce_console_shell_line(g_ov));
}

static void test_escape_closes_without_submitting(void)
{
    open_console();
    type("r.taa 1");
    press(JCE_KEY_ESCAPE);
    TEST_ASSERT_FALSE_MESSAGE(jce_console_shell_is_visible(g_ov),
        "Escape did not close the console");
}

/* SUBMIT REACHES THE REGISTRY.  A cvar registered here is set through the
 * console by typing its name and value, which proves the whole chain: the
 * line, the session, jce_console_exec and the registry. */
static void test_submitting_a_line_sets_a_cvar_and_echoes_it(void)
{
    JceCvar *cv = jce_cvar_register_float("t.console_overlay", 0.0f, 0u,
                                          "a cvar this test owns");
    TEST_ASSERT_NOT_NULL(cv);

    open_console();
    type("t.console_overlay 3");
    press(JCE_KEY_RETURN);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(3.0f, jce_cvar_get_float(cv),
        "submitting a line did not reach the cvar registry -- the console "
        "draws and edits and changes nothing, which is the state this row "
        "describes");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", jce_console_shell_line(g_ov),
        "the line was not cleared after submit");

    /* The command is ECHOED into the scrollback: output with no prompt above
     * it reads as if it came from nowhere. */
    bool echoed = false;
    const uint32_t n = jce_console_shell_scrollback_count(g_ov);
    for (uint32_t i = 0; i < n; ++i) {
        const char *l = jce_console_shell_scrollback_at(g_ov, i);
        if (l && strstr(l, "t.console_overlay 3")) { echoed = true; break; }
    }
    TEST_ASSERT_TRUE_MESSAGE(echoed,
        "the submitted command was not echoed into the scrollback");
}

/* HISTORY comes back through the same keys a shell uses. */
static void test_history_walks_back_to_a_submitted_line(void)
{
    TEST_ASSERT_NOT_NULL(jce_cvar_register_float("t.console_hist", 0.0f, 0u,
                                                 "history"));

    open_console();
    type("t.console_hist 1");
    press(JCE_KEY_RETURN);
    TEST_ASSERT_EQUAL_STRING("", jce_console_shell_line(g_ov));

    press(JCE_KEY_UP);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("t.console_hist 1",
        jce_console_shell_line(g_ov),
        "Up did not restore the last submitted line");
}

/* TAB completes against the live registry. */
static void test_tab_completes_a_registered_name(void)
{
    TEST_ASSERT_NOT_NULL(jce_cvar_register_float("t.console_unique_name", 0.0f,
                                                 0u, "tab"));

    open_console();
    type("t.console_unique_na");
    press(JCE_KEY_TAB);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("t.console_unique_name ",
        jce_console_shell_line(g_ov),
        "Tab did not complete a uniquely-matching registered name (a unique "
        "match completes to the name plus a space, ready for a value)");
}

/* THE RING.  Index 0 must stay the OLDEST line after the ring wraps -- and
 * before it wraps, reading from slot 0 and reading from `head` agree, so this
 * is invisible until a session prints more than the ring holds.  One `list`
 * over a few hundred cvars does that. */
static void test_scrollback_index_zero_is_the_oldest_after_wrap(void)
{
    enum { RING = 256 };          /* OVERLAY_SCROLLBACK */
    char buf[32];

    for (int i = 0; i < RING + 10; ++i) {
        snprintf(buf, sizeof buf, "line-%d", i);
        jce_console_shell_print(g_ov, buf);
    }

    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)RING,
        jce_console_shell_scrollback_count(g_ov),
        "the scrollback grew past its bound");

    /* 266 lines pushed, 256 kept => the oldest survivor is line-10. */
    TEST_ASSERT_EQUAL_STRING_MESSAGE("line-10",
        jce_console_shell_scrollback_at(g_ov, 0),
        "index 0 is not the OLDEST retained line after the ring wrapped -- a "
        "reader walking 0..count renders the scrollback rotated, with the "
        "newest lines in the middle");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("line-265",
        jce_console_shell_scrollback_at(g_ov, RING - 1),
        "the last index is not the NEWEST line");
    TEST_ASSERT_NULL_MESSAGE(jce_console_shell_scrollback_at(g_ov, RING),
        "reading one past the end returned something");

    /* POSITIVE CONTROL: before the wrap, index 0 is simply the first line.
     * Without it, an implementation that always returns `head + i` would
     * pass the assertions above and be wrong for every short session. */
    jce_console_shell_destroy(g_ov);
    JceConsoleShellDesc d;
    memset(&d, 0, sizeof d);
    g_ov = jce_console_shell_create(&d);
    jce_console_shell_print(g_ov, "first");
    jce_console_shell_print(g_ov, "second");
    TEST_ASSERT_EQUAL_UINT32(2u, jce_console_shell_scrollback_count(g_ov));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("first",
        jce_console_shell_scrollback_at(g_ov, 0),
        "index 0 is not the first line BEFORE the ring wraps");
}

static void test_null_arguments_are_refused(void)
{
    TEST_ASSERT_FALSE(jce_console_shell_is_visible(NULL));
    TEST_ASSERT_FALSE(jce_console_shell_handle_input(NULL, g_in));
    TEST_ASSERT_FALSE(jce_console_shell_handle_input(g_ov, NULL));
    TEST_ASSERT_EQUAL_STRING("", jce_console_shell_line(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_console_shell_scrollback_count(NULL));
    TEST_ASSERT_NULL(jce_console_shell_scrollback_at(NULL, 0));
    jce_console_shell_destroy(NULL);     /* must not crash */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_it_starts_closed_and_toggles);
    RUN_TEST(test_a_closed_console_consumes_nothing);
    RUN_TEST(test_an_open_console_consumes_even_an_empty_frame);
    RUN_TEST(test_typing_and_backspace_edit_the_line);
    RUN_TEST(test_escape_closes_without_submitting);
    RUN_TEST(test_submitting_a_line_sets_a_cvar_and_echoes_it);
    RUN_TEST(test_history_walks_back_to_a_submitted_line);
    RUN_TEST(test_tab_completes_a_registered_name);
    RUN_TEST(test_scrollback_index_zero_is_the_oldest_after_wrap);
    RUN_TEST(test_null_arguments_are_refused);
    return UNITY_END();
}
