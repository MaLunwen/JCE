/*
 * jce_editor_render_bridge.h  Engine-owned editor viewport bridge.
 *
 * Encapsulates off-screen render target lifecycle and view setup used by
 * the editor scene viewport, so panel-side code does not manage bgfx FBOs.
 */

#ifndef JCE_EDITOR_RENDER_BRIDGE_H
#define JCE_EDITOR_RENDER_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRenderer JceRenderer;
typedef struct JceEditorRenderBridge JceEditorRenderBridge;

/* Create/destroy an editor render bridge.
 * view_id=0 uses JCE_VIEW_EDITOR_SCENE. */
JceEditorRenderBridge *jce_editor_render_bridge_create(JceRenderer *renderer,
                                                       uint16_t view_id);
void jce_editor_render_bridge_destroy(JceEditorRenderBridge *bridge);

/* Ensure render target exists at width/height and configure the view.
 * Returns false if target allocation or setup fails. */
bool jce_editor_render_bridge_prepare(JceEditorRenderBridge *bridge,
                                      uint32_t width,
                                      uint32_t height,
                                      const float *view16,
                                      const float *proj16,
                                      uint32_t clear_rgba,
                                      const char *view_name);

/* Query bridge-owned texture/view handles for panel integration. */
uint16_t jce_editor_render_bridge_get_color_texture(
    const JceEditorRenderBridge *bridge);
uint16_t jce_editor_render_bridge_get_view_id(const JceEditorRenderBridge *bridge);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_RENDER_BRIDGE_H */
