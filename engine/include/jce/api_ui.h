/*
 * api_ui.h  In-game UI.
 *
 * HTML/CSS-based UI via RmlUi C bridge.
 */

#ifndef JCE_API_UI_H
#define JCE_API_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/ui/jce_touch_hud.h>
#include <jce/middleware/ui/jce_ui.h>
#include <jce/middleware/ui/jce_ui_console_overlay.h>
#include <jce/middleware/ui/jce_ui_debug_hud.h>
#include <jce/middleware/ui/jce_ui_settings.h>
#include <jce/middleware/ui/jce_localization.h>

/* Completed 2026-08-31.  These headers export JCE_API symbols and were
 * reachable from NO umbrella, so §4's promise -- `#include <jce/api.h>`
 * gives you the whole engine -- did not hold for them.  Several are named
 * in §4's own table by capability.
 */
#include <jce/ui/jce_imgui_renderer.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_UI_H */
