/*
 * jce_host_shell.h  Host OS shell helpers — opening paths/URLs in
 * native handlers via SDL3.  Wraps SDL_OpenURL and SDL_CreateProcess
 * so editor/game code never branches on _WIN32 / __APPLE__ / ...
 *
 * Layer: Platform (Layer 1 — depends on jce_core, uses SDL3 internally).
 */

#ifndef JCE_HOST_SHELL_H
#define JCE_HOST_SHELL_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Open a URL (http/https/file/mailto/...) in the user's default handler.
   Returns true on success.  Implemented with SDL_OpenURL. */
bool jce_host_open_url(const char *url);

/* Reveal a file or folder in the host file manager.
   - If path is a file: opens its parent directory (selection of the
     specific file is best-effort and not guaranteed cross-platform).
   - If path is a directory: opens that directory.
   Returns false if path is empty / does not exist. */
bool jce_host_reveal_path(const char *path);

/* Open a path in a text editor.  Tries `code <path>` (VS Code) via
   SDL_CreateProcess; on failure, falls back to SDL_OpenURL("file://...")
   so the OS default editor / file handler picks it up.
   Returns true if either path succeeded. */
bool jce_host_open_in_text_editor(const char *path);

/* Open a host OS terminal/console with `cwd` as the working directory.
   On Windows this launches the user's default terminal (Windows Terminal
   if installed, otherwise cmd.exe).  On macOS it launches Terminal.app.
   On Linux it tries x-terminal-emulator / gnome-terminal / xterm in order.
   Returns false if no terminal could be launched. */
bool jce_host_open_terminal(const char *cwd);

JCE_EXTERN_C_END

#endif /* JCE_HOST_SHELL_H */
