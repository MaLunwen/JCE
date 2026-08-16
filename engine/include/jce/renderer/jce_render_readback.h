#ifndef JCE_RENDERER_JCE_RENDER_READBACK_H
#define JCE_RENDERER_JCE_RENDER_READBACK_H

#include <jce/renderer/jce_gfx_types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_RENDER_READBACK_ABI_VERSION 1u
#define JCE_RENDER_READBACK_QUEUE_CAPACITY 4u
#define JCE_RENDER_READBACK_INVALID_TICKET 0u

typedef uint32_t JceRenderReadbackTicket;
typedef struct JceRenderer JceRenderer;
typedef struct JceRenderReadbackQueue JceRenderReadbackQueue;

typedef enum JceRenderReadbackState {
    JCE_RENDER_READBACK_INVALID = 0,
    JCE_RENDER_READBACK_PENDING,
    JCE_RENDER_READBACK_READY,
    JCE_RENDER_READBACK_CANCELLED,
    JCE_RENDER_READBACK_FAILED
} JceRenderReadbackState;

typedef enum JceRenderReadbackError {
    JCE_RENDER_READBACK_ERROR_NONE = 0,
    JCE_RENDER_READBACK_ERROR_ARGUMENT,
    JCE_RENDER_READBACK_ERROR_UNSUPPORTED,
    JCE_RENDER_READBACK_ERROR_QUEUE_FULL,
    JCE_RENDER_READBACK_ERROR_ALLOCATION,
    JCE_RENDER_READBACK_ERROR_COPY_SIZE
} JceRenderReadbackError;

typedef struct JceRenderReadbackDesc {
    uint32_t struct_size;
    uint32_t version;
    JceTextureHandle source;
    uint32_t format;
    uint16_t view_id;
    uint16_t reserved;
    uint32_t source_x;
    uint32_t source_y;
    uint32_t width;
    uint32_t height;
} JceRenderReadbackDesc;

typedef struct JceRenderReadbackInfo {
    uint32_t struct_size;
    uint32_t state;
    uint32_t error;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint64_t row_pitch;
    uint64_t byte_size;
    uint32_t ready_frame;
    uint32_t reserved;
} JceRenderReadbackInfo;

JCE_API JceRenderReadbackDesc jce_render_readback_desc_default(void);

/* Computes the tightly packed CPU layout. Depth formats are deliberately
 * rejected: the typed contract never performs implicit format conversion. */
JCE_API bool jce_render_readback_layout(JceRenderFormat format,
                                        uint32_t width, uint32_t height,
                                        uint64_t *out_row_pitch,
                                        uint64_t *out_byte_size);

/* Queries the active backend for same-format blit/readback support. */
JCE_API bool jce_render_readback_format_supported(JceRenderFormat format);

JCE_API JceRenderReadbackQueue *jce_render_readback_create(
    JceRenderer *renderer);
JCE_API void jce_render_readback_destroy(JceRenderReadbackQueue *queue);

/* Submission is render-thread only. Completion is asynchronous and is
 * observed through poll; no callback runs on the renderer thread. */
JCE_API bool jce_render_readback_submit(JceRenderReadbackQueue *queue,
                                        const JceRenderReadbackDesc *desc,
                                        JceRenderReadbackTicket *out_ticket);
JCE_API JceRenderReadbackState jce_render_readback_poll(
    JceRenderReadbackQueue *queue, JceRenderReadbackTicket ticket);
JCE_API bool jce_render_readback_get_info(
    const JceRenderReadbackQueue *queue, JceRenderReadbackTicket ticket,
    JceRenderReadbackInfo *out_info);
JCE_API bool jce_render_readback_copy(
    JceRenderReadbackQueue *queue, JceRenderReadbackTicket ticket,
    void *destination, size_t destination_size);
JCE_API bool jce_render_readback_cancel(
    JceRenderReadbackQueue *queue, JceRenderReadbackTicket ticket);
JCE_API bool jce_render_readback_release(
    JceRenderReadbackQueue *queue, JceRenderReadbackTicket ticket);

JCE_EXTERN_C_END

#endif /* JCE_RENDERER_JCE_RENDER_READBACK_H */
