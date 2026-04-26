/*
 * jce_offscreen_target.h  Engine-owned editor viewport bridge.
 *
 * Encapsulates off-screen render target lifecycle and view setup used by
 * the editor scene viewport, so panel-side code does not manage bgfx FBOs.
 */

#ifndef jce_offscreen_target_H
#define jce_offscreen_target_H


#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRenderer JceRenderer;
typedef struct JceOffscreenTarget JceOffscreenTarget;

/* Create/destroy an editor render bridge.
 * view_id=0 uses JCE_VIEW_EDITOR_SCENE. */
JceOffscreenTarget *jce_offscreen_target_create(JceRenderer *renderer,
                                                       uint16_t view_id);
void jce_offscreen_target_destroy(JceOffscreenTarget *bridge);

/* Ensure render target exists at width/height and configure the view.
 * Returns false if target allocation or setup fails. */
bool jce_offscreen_target_prepare(JceOffscreenTarget *bridge,
                                      uint32_t width,
                                      uint32_t height,
                                      const float *view16,
                                      const float *proj16,
                                      uint32_t clear_rgba,
                                      const char *view_name);

/* Query bridge-owned texture/view handles for panel integration. */
uint16_t jce_offscreen_target_get_color_texture(
    const JceOffscreenTarget *bridge);
uint16_t jce_offscreen_target_get_view_id(const JceOffscreenTarget *bridge);

JCE_EXTERN_C_END

#endif /* jce_offscreen_target_H */
