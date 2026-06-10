/*
 * jce_scene_pick.h  GPU object-ID scene picking.
 *
 * Renders selectable scene geometry into an offscreen ID buffer and reads
 * back the clicked pixel asynchronously.  The GPU depth test decides the
 * front-most object, so picking follows what is visible in the viewport.
 */

#ifndef JCE_SCENE_PICK_H
#define JCE_SCENE_PICK_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_pick_id.h>
#include <jce/renderer/jce_scene_renderer.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceScenePickPass JceScenePickPass;

typedef struct JceScenePickDesc {
    JceRenderer                    *renderer;
    const JcePakArchive            *pak;
    const JceSceneRendererCallbacks *callbacks; /* optional, copied */
    uint16_t                        view_id;    /* 0 = JCE_VIEW_EDITOR_PICK */
} JceScenePickDesc;

typedef struct JceScenePickResult {
    JceEntity entity;      /* 0 = no object at the requested pixel */
    uint32_t  x;
    uint32_t  y;
    uint32_t  frame_index;
} JceScenePickResult;

JCE_API bool jce_scene_pick_supported(void);

JCE_API JceScenePickPass *jce_scene_pick_create(const JceScenePickDesc *desc);
JCE_API void              jce_scene_pick_destroy(JceScenePickPass *pass);

/* Submit the hidden ID pass. On-demand: a no-op unless a click request
 * recorded by jce_scene_pick_request() is awaiting service, in which case it
 * renders the ID buffer once and queues the async pixel readback in the same
 * frame. Call it every frame; idle frames cost a flag test. */
JCE_API bool jce_scene_pick_render(JceScenePickPass *pass,
                                   JceScene *scene,
                                   const JceCamera *camera,
                                   uint32_t width,
                                   uint32_t height);

/* Record a pick request at pixel (x, y); the next jce_scene_pick_render()
 * call services it. No prior render is required. Returns false when readback
 * is unsupported, pick shaders are unavailable, or another request is still
 * in flight. Out-of-range coordinates are clamped at service time. */
JCE_API bool jce_scene_pick_request(JceScenePickPass *pass,
                                    uint32_t x,
                                    uint32_t y);

/* Returns true exactly when a pending request has become available.
 * A ready "no hit" is reported as true with out_result->entity == 0. */
JCE_API bool jce_scene_pick_poll(JceScenePickPass *pass,
                                 JceScenePickResult *out_result);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_PICK_H */
