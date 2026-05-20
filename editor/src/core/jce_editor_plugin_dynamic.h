/*
 * jce_editor_plugin_dynamic.h  Runtime dlopen plugin loader.
 *
 * Sits on top of jce_editor_plugin (the static registry).  A
 * dynamic plugin is a shared library exporting:
 *
 *     JCE_EDITOR_PLUGIN_API void JceEditorPluginInit(void);
 *
 * which calls jce_editor_plugin_register_panel() for each panel it
 * provides.  An optional matching JceEditorPluginShutdown() is
 * called before dlclose() to give the plugin a chance to unregister.
 *
 * Layer: editor (UI shell).
 */

#ifndef JCE_EDITOR_PLUGIN_DYNAMIC_H
#define JCE_EDITOR_PLUGIN_DYNAMIC_H

#include "core/jce_editor_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load a shared library + call its JceEditorPluginInit().  Returns
 * an opaque handle (>0) on success or 0 on failure. */
uint32_t jce_editor_plugin_load_dynamic(const char *library_path);

/* Unload by handle.  Calls JceEditorPluginShutdown() if exported,
 * then dlclose. */
bool jce_editor_plugin_unload_dynamic(uint32_t handle);

/* Convenience: scan `directory` (non-recursive) for files matching
 * the platform's shared-library extension (.so / .dll / .dylib) and
 * load each. */
uint32_t jce_editor_plugin_load_directory(const char *directory);

/* Iterate loaded plugins. */
uint32_t    jce_editor_plugin_loaded_count(void);
const char *jce_editor_plugin_loaded_path_at(uint32_t idx);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_PLUGIN_DYNAMIC_H */
