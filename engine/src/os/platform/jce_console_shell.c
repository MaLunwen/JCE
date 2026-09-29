/*
 * jce_console_shell.c  The typing half of a developer console.
 *
 * See jce_console_shell.h for why this is separate from the overlay that
 * so a console written straight onto jce_ui.h has no headless coverage and,
 * on this toolchain, does not even link.  Nothing here knows a document
 * exists.
 *
 * All line editing, history and completion is jce_console_session's -- this
 * file translates keystrokes into calls on it and never keeps a second copy
 * of the line.
 */

#include <jce/os/platform/jce_console_shell.h>

#include <jce/os/core/jce_console.h>
#include <jce/os/core/jce_console_session.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/platform/jce_keys.h>

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "console_shell"

/* Enough to read back what a `list` over a few hundred cvars printed, and
 * bounded so a script logging every frame cannot grow it without limit.
 * Oldest lines are dropped first. */
#define SHELL_SCROLLBACK 256
#define SHELL_LINE_MAX   512

struct JceConsoleShell {
    JceConsoleSession *session;
    int                toggle_key;
    bool               visible;


    /* Ring of retained output lines.  `head` is the next slot to write; when
     * `count` == SHELL_SCROLLBACK the ring has wrapped and index 0 of the
     * public API is `head`, not 0. */
    char     (*lines)[SHELL_LINE_MAX];
    uint32_t   head;
    uint32_t   count;
};

/* ── scrollback ──────────────────────────────────────────────────────── */

static void shell_push_line(JceConsoleShell *o, const char *text)
{
    if (!o || !o->lines) return;
    snprintf(o->lines[o->head], SHELL_LINE_MAX, "%s", text ? text : "");
    o->head = (o->head + 1u) % SHELL_SCROLLBACK;
    if (o->count < SHELL_SCROLLBACK) ++o->count;
}

/* jce_console_exec writes through this.  The console's sink is GLOBAL and
 * there is exactly one of it, so the overlay installs itself only for the
 * duration of a single exec and clears it afterwards.  It CANNOT restore a
 * previous sink -- jce_console_set_output has no getter -- and shell_submit
 * says so where it happens.  Two surfaces executing in one process therefore
 * each capture their own output only while they are the most recent to have
 * installed a sink. */
static void shell_sink(const char *text, void *user)
{
    shell_push_line((JceConsoleShell *)user, text);
}

void jce_console_shell_print(JceConsoleShell *o, const char *text)
{
    shell_push_line(o, text);
}

uint32_t jce_console_shell_scrollback_count(const JceConsoleShell *o)
{
    return o ? o->count : 0u;
}

const char *jce_console_shell_scrollback_at(const JceConsoleShell *o,
                                              uint32_t index)
{
    if (!o || !o->lines || index >= o->count) return NULL;
    /* Index 0 is the OLDEST line.  Once the ring has wrapped that is `head`;
     * before it wraps the ring is still in order from 0. */
    const uint32_t start = (o->count == SHELL_SCROLLBACK) ? o->head : 0u;
    return o->lines[(start + index) % SHELL_SCROLLBACK];
}

const char *jce_console_shell_line(const JceConsoleShell *o)
{
    return (o && o->session) ? jce_console_session_line(o->session) : "";
}

/* ── lifecycle ───────────────────────────────────────────────────────── */

JceConsoleShell *jce_console_shell_create(const JceConsoleShellDesc *desc)
{
    JceConsoleShell *o = (JceConsoleShell *)JCE_CALLOC(1, sizeof *o);
    if (!o) return NULL;

    o->session = jce_console_session_create();
    if (!o->session) { JCE_FREE(o); return NULL; }

    o->lines = (char (*)[SHELL_LINE_MAX])JCE_CALLOC(
        SHELL_SCROLLBACK, SHELL_LINE_MAX);
    if (!o->lines) {
        jce_console_session_destroy(o->session);
        JCE_FREE(o);
        return NULL;
    }

    o->toggle_key = (desc && desc->toggle_key) ? desc->toggle_key : JCE_KEY_GRAVE;
    return o;
}

void jce_console_shell_destroy(JceConsoleShell *o)
{
    if (!o) return;
    jce_console_session_destroy(o->session);
    JCE_FREE(o->lines);
    JCE_FREE(o);
}

void jce_console_shell_show(JceConsoleShell *o)
{
    if (!o) return;
    o->visible = true;
}

void jce_console_shell_hide(JceConsoleShell *o)
{
    if (!o) return;
    o->visible = false;
}

bool jce_console_shell_is_visible(const JceConsoleShell *o)
{
    return o && o->visible;
}

/* ── input ───────────────────────────────────────────────────────────── */

static void shell_backspace(JceConsoleShell *o)
{
    const char *cur = jce_console_session_line(o->session);
    const size_t n  = cur ? strlen(cur) : 0u;
    if (n == 0u) return;
    char buf[JCE_CONSOLE_SESSION_LINE_MAX];
    snprintf(buf, sizeof buf, "%s", cur);
    /* One BYTE, not one codepoint.  Said rather than hidden: the line is
     * UTF-8 and a multi-byte character takes several presses to erase.  The
     * honest fix is a UTF-8 aware step, and it belongs in the session beside
     * the rest of the text handling rather than here. */
    buf[n - 1u] = '\0';
    jce_console_session_set_line(o->session, buf);
}

static void shell_append_text(JceConsoleShell *o, const char *text)
{
    if (!text || !text[0]) return;
    const char *cur = jce_console_session_line(o->session);
    char buf[JCE_CONSOLE_SESSION_LINE_MAX];
    snprintf(buf, sizeof buf, "%s%s", cur ? cur : "", text);
    jce_console_session_set_line(o->session, buf);
}

static void shell_submit(JceConsoleShell *o)
{
    const char *line = jce_console_session_line(o->session);
    if (!line || !line[0]) return;

    /* Echo the command itself first: output with no prompt above it reads as
     * if it came from nowhere, and `list` output in particular is unreadable
     * without knowing which command produced it. */
    char echo[SHELL_LINE_MAX];
    snprintf(echo, sizeof echo, "> %s", line);
    shell_push_line(o, echo);

    /* ONE GLOBAL SINK, borrowed and returned.  jce_console_set_output has no
     * getter, so the previous sink cannot be read back and restored -- this
     * clears it, which is what the editor panel already does around its own
     * exec.  Recorded here because it is a real limitation: two surfaces
     * executing in one process each capture their own output only while they
     * are the most recent to have installed a sink. */
    jce_console_set_output(shell_sink, o);
    (void)jce_console_session_submit(o->session);
    jce_console_set_output(NULL, NULL);
}

static void shell_complete(JceConsoleShell *o)
{
    const uint32_t n = jce_console_session_complete(o->session);
    if (n <= 1u) return;
    /* Ambiguous: print the candidates, the way every shell does.  The line
     * has already been extended to the common prefix by complete(). */
    const uint32_t shown = jce_console_session_match_count(o->session);
    for (uint32_t i = 0; i < shown; ++i) {
        const char *m = jce_console_session_match_at(o->session, i);
        if (m) shell_push_line(o, m);
    }
    if (n > shown) {
        /* SAY THE TRUNCATION.  complete() reports how many actually matched,
         * which can exceed what match_at can return; printing only the stored
         * subset with no note is how "these are the matches" gets said about
         * a list that is not. */
        char more[SHELL_LINE_MAX];
        snprintf(more, sizeof more, "... %u more match(es) not shown",
                 (unsigned)(n - shown));
        shell_push_line(o, more);
    }
}

bool jce_console_shell_handle_input(JceConsoleShell *o, const JceInput *in)
{
    if (!o || !in) return false;

    if (jce_input_key_pressed(in, o->toggle_key)) {
        if (o->visible) jce_console_shell_hide(o);
        else            jce_console_shell_show(o);
        /* Consumed in BOTH directions: the key that opens the console must
         * not also be typed into it, and the key that closes it must not
         * reach the game on the same frame. */
        return true;
    }
    if (!o->visible) return false;

    if (jce_input_key_pressed(in, JCE_KEY_ESCAPE)) {
        jce_console_shell_hide(o);
        return true;
    }
    if (jce_input_key_pressed(in, JCE_KEY_RETURN)) {
        shell_submit(o);
        return true;
    }
    /* REPEATED, not pressed.  jce_input_key_pressed is a strict rising edge,
     * so holding Backspace would delete exactly one byte -- which jce_input.h
     * documents as the defect that made "the shipped runtime's InputField
     * delete one byte where the editor's deleted the line".  Writing a cvar
     * name wrong and holding Backspace is the single most common thing that
     * happens in a console. */
    if (jce_input_key_repeated(in, JCE_KEY_BACKSPACE)) {
        shell_backspace(o);
        return true;
    }
    if (jce_input_key_pressed(in, JCE_KEY_TAB)) {
        shell_complete(o);
        return true;
    }
    /* Repeated too: holding Up to walk back through history is what every
     * console in the `compared` field does.  Enter, Tab, Escape and the
     * toggle stay on the strict edge -- repeating those would submit a line
     * many times, or flap the overlay open and shut while a key is held. */
    if (jce_input_key_repeated(in, JCE_KEY_UP)) {
        (void)jce_console_session_history_prev(o->session);
        return true;
    }
    if (jce_input_key_repeated(in, JCE_KEY_DOWN)) {
        (void)jce_console_session_history_next(o->session);
        return true;
    }

    shell_append_text(o, jce_input_text(in));

    /* TRUE EVEN WHEN NOTHING WAS TYPED.  An open console eats the frame: the
     * alternative is a player who opens it, types nothing for a frame, and
     * walks forward because W reached the game. */
    return true;
}
