/*
 * jce_console_shell.h  The typing half of a developer console.
 *
 * debug.console.cvars-at-runtime: the cvar registry, the command table, the
 * parser and -- since jce_console_session -- the line editing, history and Tab
 * completion are all complete and public.  The only interactive reader in the
 * whole tree was the EDITOR's console panel.  A shipped game got the FPS
 * overlay and no way to change a cvar, which is the row's own feature
 * sentence and the whole of the gap its `compared` field names.
 *
 * Nothing new is invented here.  jce_ui_debug_hud.{h,c} + engine/ui/
 * engine_debug_hud.rml is the exact template -- an engine-owned RML document
 * loaded from the PAK and already consumed by a shipped game -- and
 * jce_console_session owns every piece of console behaviour that is worth
 * testing.  This is the document, the toggle, and the wire between them.
 *
 * WHY THIS IS A SEPARATE OBJECT FROM THE OVERLAY THAT DRAWS IT, and it is the
 * same reason jce_console_session is separate from this.  jce_ui_create
 * requires a JceRenderer for bgfx draws, and NO test in the tree creates a
 * JceUIContext -- the runtime UI layer has no headless coverage at all.  A
 * console written straight onto jce_ui.h ships with nothing that runs in
 * `jce.py test`, and linking jce_ui into a test drags RmlUi with it (which is
 * how this split was forced: the first version did, and the test would not
 * link).  So the shell lives in os/platform, where JceInput already is, knows
 * nothing about drawing, and is fully exercised headlessly.  The half that
 * needs a GPU -- jce_ui_console_overlay -- has no logic in it at all.
 *
 * Thread safety: NOT thread-safe.  Call from the main thread only.
 */

#ifndef JCE_CONSOLE_SHELL_H
#define JCE_CONSOLE_SHELL_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/platform/jce_input.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceConsoleShell JceConsoleShell;

typedef struct JceConsoleShellDesc {
    /* Key that opens and closes it.  0 means JCE_KEY_GRAVE, the tilde key
     * every engine in the `compared` field uses. */
    int           toggle_key;
} JceConsoleShellDesc;

JCE_API JceConsoleShell *jce_console_shell_create(const JceConsoleShellDesc *desc);
JCE_API void               jce_console_shell_destroy(JceConsoleShell *o);

JCE_API void JCE_CALL jce_console_shell_show(JceConsoleShell *o);
JCE_API void JCE_CALL jce_console_shell_hide(JceConsoleShell *o);
JCE_API bool JCE_CALL jce_console_shell_is_visible(const JceConsoleShell *o);

/*
 * Feed one frame of input.  Returns TRUE when the shell consumed it, which
 * the caller must honour: a console that is open and still lets W/A/S/D
 * through walks the player around while they type.  The toggle key is
 * consumed in both directions, so the character that opens the console never
 * also lands in its line.
 *
 * Handled while visible: printable text (jce_input_text), Backspace, Enter
 * (submit + execute), Up/Down (history), Tab (completion), Escape (close).
 * All of it goes through jce_console_session, so the behaviour here is the
 * behaviour its 16 headless cases already pin down.
 */
JCE_API bool JCE_CALL jce_console_shell_handle_input(JceConsoleShell *o,
                                                       const JceInput *input);

/* ── What the shell is showing ────────────────────────────────────────
 *
 * This IS the console, as data.  jce_ui_console_overlay renders exactly what
 * these return and adds nothing; a different surface (an ImGui panel, a log
 * file, a remote debugger) renders the same thing without touching this file.
 */

/* The line being edited (never NULL; "" when empty). */
JCE_API const char *JCE_CALL jce_console_shell_line(const JceConsoleShell *o);

/* Scrollback: line 0 is the OLDEST retained line. */
JCE_API uint32_t    JCE_CALL jce_console_shell_scrollback_count(const JceConsoleShell *o);
JCE_API const char *JCE_CALL jce_console_shell_scrollback_at(const JceConsoleShell *o,
                                                                uint32_t index);

/* Append a line as if the console had printed it -- how a game reports its
 * own errors into the same pane the cvar output lands in. */
JCE_API void JCE_CALL jce_console_shell_print(JceConsoleShell *o, const char *text);

JCE_EXTERN_C_END

#endif /* JCE_CONSOLE_SHELL_H */
