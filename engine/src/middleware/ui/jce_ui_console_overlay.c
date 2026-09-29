/*
 * jce_ui_console_overlay.c  Draw a JceConsoleShell.
 *
 * See the header: every decision a console makes lives in jce_console_shell,
 * which runs headlessly.  This file reads what the shell reports and writes
 * RML.  If a change here needs a test, it probably belongs in the shell.
 */

#include <jce/middleware/ui/jce_ui_console_overlay.h>

#include <jce/os/core/jce_console_session.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "ui_console"

/* What the pane can plausibly show.  The shell keeps far more than this;
 * pushing all of it as RML every frame costs more than it displays. */
#define OVERLAY_VISIBLE_LINES 64

struct JceConsoleOverlay {
    JceUIContext      *ui;
    JceConsoleShell   *shell;        /* borrowed, never owned */
    JceUIDocHandle     doc;
    JceUIElementHandle el_output;
    JceUIElementHandle el_line;
    bool               doc_ok;
    bool               shown;        /* what the DOCUMENT currently is */
};

JceConsoleOverlay *jce_console_overlay_create(JceUIContext *ui,
                                              JceConsoleShell *shell)
{
    if (!ui || !shell) return NULL;

    JceConsoleOverlay *o = (JceConsoleOverlay *)JCE_CALLOC(1, sizeof *o);
    if (!o) return NULL;
    o->ui    = ui;
    o->shell = shell;

    o->doc = jce_ui_doc_load_file(ui, "engine_console_overlay.rml");
    if (jce_ui_doc_valid(o->doc)) {
        o->doc_ok    = true;
        o->el_output = jce_ui_find_element(ui, o->doc, "console-output");
        o->el_line   = jce_ui_find_element(ui, o->doc, "console-line");
        jce_ui_doc_hide(ui, o->doc);
    } else {
        /* SAID OUT LOUD, and the console stays usable.  Without this line the
         * failure is a key that does nothing, which is indistinguishable from
         * the feature never having been wired -- and the shell still works,
         * so a game can read its output some other way. */
        LOG_ERROR(LOG_TAG, "engine_console_overlay.rml did not load; the "
                           "console still works but draws nothing");
    }
    return o;
}

void jce_console_overlay_destroy(JceConsoleOverlay *o)
{
    if (!o) return;
    /* The document's lifetime belongs to the UI context -- the same rule
     * jce_debug_hud_destroy documents, and for the same reason: closing one
     * document during shutdown can fault against still-live siblings.  The
     * SHELL is borrowed and is not destroyed here either. */
    JCE_FREE(o);
}

bool jce_console_overlay_has_document(const JceConsoleOverlay *o)
{
    return o && o->doc_ok;
}

void jce_console_overlay_update(JceConsoleOverlay *o)
{
    if (!o || !o->doc_ok) return;

    /* Follow the shell rather than keeping a second visibility flag: two
     * flags for one fact is how a console ends up drawn while closed. */
    const bool want = jce_console_shell_is_visible(o->shell);
    if (want != o->shown) {
        if (want) jce_ui_doc_show(o->ui, o->doc);
        else      jce_ui_doc_hide(o->ui, o->doc);
        o->shown = want;
    }
    if (!want) return;

    if (jce_ui_elem_valid(o->el_output)) {
        const uint32_t total = jce_console_shell_scrollback_count(o->shell);
        const uint32_t first = (total > OVERLAY_VISIBLE_LINES)
                                   ? (total - OVERLAY_VISIBLE_LINES) : 0u;
        char   rml[OVERLAY_VISIBLE_LINES * 128];
        size_t used = 0;
        rml[0] = '\0';
        for (uint32_t i = first; i < total; ++i) {
            const char *l = jce_console_shell_scrollback_at(o->shell, i);
            if (!l) continue;
            const int wrote = snprintf(rml + used, sizeof rml - used,
                                       "<p>%s</p>", l);
            if (wrote <= 0 || (size_t)wrote >= sizeof rml - used) break;
            used += (size_t)wrote;
        }
        jce_ui_elem_set_inner_rml(o->ui, o->el_output, rml);
    }

    if (jce_ui_elem_valid(o->el_line)) {
        char prompt[JCE_CONSOLE_SESSION_LINE_MAX + 16];
        snprintf(prompt, sizeof prompt, "&gt; %s_",
                 jce_console_shell_line(o->shell));
        jce_ui_elem_set_inner_rml(o->ui, o->el_line, prompt);
    }
}
