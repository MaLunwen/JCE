/*
 * jce_editor_command_palette.h  Ctrl+Shift+P style command launcher.
 *
 * Quick-action picker that lets the user fuzzy-search every
 * registered editor command and execute it without navigating menus.
 * Mirrors VS Code / Sublime "Command Palette" — modal overlay over
 * the editor, focuses an input field, shows ranked matches, runs
 * the chosen action.
 *
 * Commands are registered at startup via jce_editor_command_register.
 * Built-in commands (Save Scene, Toggle Play, Reset Layout, etc.) are
 * registered by the editor itself.  Plugins (B4.3) can add their own.
 *
 * Open the palette via jce_editor_command_palette_open() — wire to
 * Ctrl+Shift+P hotkey at the call site.
 */

#ifndef JCE_EDITOR_COMMAND_PALETTE_H
#define JCE_EDITOR_COMMAND_PALETTE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*JceEditorCommandFn)(void *user_data);

#define JCE_EDITOR_COMMAND_NAME_MAX 64
#define JCE_EDITOR_COMMAND_GROUP_MAX 32
#define JCE_EDITOR_COMMAND_MAX_REGISTERED 256

/* Register a command.  `id` is a stable string (e.g. "scene.save"),
 * `display_name` is what shows in the palette (e.g. "Scene: Save"),
 * `group` is the optional category for grouping ("Scene", "Window").
 * Returns true on success. */
bool jce_editor_command_register(const char *id,
                                 const char *display_name,
                                 const char *group,
                                 JceEditorCommandFn fn,
                                 void *user_data);

/* Unregister by id.  Returns true if found. */
bool jce_editor_command_unregister(const char *id);

/* Open / close the palette overlay. */
void jce_editor_command_palette_open(void);
void jce_editor_command_palette_close(void);
bool jce_editor_command_palette_is_open(void);

/* Render the palette window if open.  Call once per frame from the
 * editor main loop. */
void jce_editor_command_palette_render(void);

/* Number of commands currently registered. */
uint32_t jce_editor_command_count(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_COMMAND_PALETTE_H */
