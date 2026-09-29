/*
 * jce_ui_console_overlay.h  Draw a JceConsoleShell.
 *
 * This is the half of the developer console that needs a GPU, and it is
 * deliberately the half with no logic in it.  Everything a console DOES --
 * the line, history, Tab completion, the scrollback ring, which keys are
 * consumed -- is jce_console_shell in os/platform, where it runs headlessly
 * in `jce.py test`.  This file turns what the shell reports into RML and
 * nothing else, so there is no behaviour here that only a GPU can check.
 *
 * The split was forced rather than chosen: linking jce_ui into a test drags
 * RmlUi with it, and RmlUi's prebuilt objects reference an MSVC STL symbol
 * this toolchain does not provide, so the first version of this console --
 * one object that both edited a line and drew it -- could not be tested at
 * all because its test would not link.
 *
 * Element ids are the contract with engine/ui/engine_console_overlay.rml:
 *   #console-output   scrollback
 *   #console-line     the line being edited
 *
 * Thread safety: NOT thread-safe.  Call from the main thread only.
 */

#ifndef JCE_UI_CONSOLE_OVERLAY_H
#define JCE_UI_CONSOLE_OVERLAY_H

#include <jce/middleware/ui/jce_ui.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/platform/jce_console_shell.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

typedef struct JceConsoleOverlay JceConsoleOverlay;

/*
 * Bind a document to `shell`.  The overlay does NOT own the shell: a game
 * keeps one shell for the life of the process and may show it through more
 * than one surface.
 *
 * Returns NULL when `ui` or `shell` is NULL.  A missing or unparseable
 * document is NOT a failure to create -- it is logged and the overlay draws
 * nothing, because taking the console down with its stylesheet would turn a
 * missing asset into a key that silently does nothing, which reads exactly
 * like the feature never having been wired.
 */
JCE_API JceConsoleOverlay *jce_console_overlay_create(JceUIContext *ui,
                                                      JceConsoleShell *shell);
JCE_API void JCE_CALL jce_console_overlay_destroy(JceConsoleOverlay *o);

/*
 * Push the shell's current line and scrollback into the document, and show or
 * hide it to match jce_console_shell_is_visible().  Call once per frame
 * before jce_ui_update().  A no-op when the document did not load.
 */
JCE_API void JCE_CALL jce_console_overlay_update(JceConsoleOverlay *o);

/* False when the document did not load -- so an embedder can say so rather
 * than wonder why the console is invisible. */
JCE_API bool JCE_CALL jce_console_overlay_has_document(const JceConsoleOverlay *o);

JCE_EXTERN_C_END

#endif /* JCE_UI_CONSOLE_OVERLAY_H */
