/*
 * jce_ui_dom_serialize.h  ECS UI subtree → RmlUi document text.
 *
 * Walks a tree of ECS UI components (Canvas root + Image / Text /
 * Button / Layout-Group children) and produces a self-contained RML
 * document string that RmlUi can load with `LoadDocument`.  Mirrors
 * the data flow Unity's UGUI uses (component tree → CanvasRenderer
 * meshes), expressed as text so the underlying RmlUi backend can
 * reuse its parser.
 *
 * The function does NOT touch RmlUi at all — it just emits a string.
 * Caller decides when/whether to feed it to a live document.
 *
 * Output shape:
 *   <rml><body>
 *     <div class="canvas" id="...">
 *       <img src="..." style="..."/>
 *       <p style="...">text</p>
 *       <button id="..." style="...">label</button>
 *       ...
 *     </div>
 *   </body></rml>
 *
 * Layer: middleware / ui (Layer 4) — public.
 */

#ifndef JCE_UI_DOM_SERIALIZE_H
#define JCE_UI_DOM_SERIALIZE_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/ui/jce_ui_rect.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Serialise the UI subtree rooted at `canvas_entity` into a freshly-
 * allocated, NUL-terminated RML string.  Caller frees with `free()`.
 *
 * `screen_w` / `screen_h` provide the parent rect for anchor compute.
 * If the canvas entity has a JceCanvasComponent its reference
 * resolution is honoured.
 *
 * Returns NULL on failure (entity missing, OOM). */
JCE_API char *jce_ui_dom_serialize(JceScene *scene,
                                   JceEntity canvas_entity,
                                   float     screen_w,
                                   float     screen_h);

JCE_EXTERN_C_END

#endif /* JCE_UI_DOM_SERIALIZE_H */
