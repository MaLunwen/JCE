/* API-owner-thread queue borrowing, attached to the existing renderer owner. */
#ifndef JCE_RENDER_READBACK_INTERNAL_H
#define JCE_RENDER_READBACK_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>
typedef struct JceRenderer JceRenderer;
typedef struct JceRenderReadbackQueue JceRenderReadbackQueue;
void jce_render_readback_backend_open(JceRenderer *renderer);
/* Retire handles while the backend lives; retain borrowed CPU destinations. */
void jce_render_readback_before_backend_shutdown(JceRenderer *renderer);
/* Only after bgfx shutdown returns: release destinations without GPU calls. */
void jce_render_readback_after_backend_shutdown(JceRenderer *renderer);
/* No implicit frame submission. Pending CPU destinations keep the existing
 * retire owner until an observed ready serial or the backend stop boundary. */
void jce_render_readback_destroy_deferred(JceRenderReadbackQueue *queue);
void jce_render_readback_collect(const JceRenderer *renderer, uint32_t frame);
/* Bounded admission for a producer which may cancel/restart within one frame.
 * This does not change the public queue creator's independent policy. */
bool jce_render_readback_deferred_admission(const JceRenderer *renderer,
    const JceRenderReadbackQueue *existing_queue);
#endif
