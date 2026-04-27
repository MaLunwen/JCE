/*
 * jce_render_queue.h  Sorted draw-call submission queue.
 *
 * Collects draw commands from the scene, sorts them by material,
 * depth, and pass, then submits to bgfx in optimal order to
 * minimize state changes.
 *
 * Layer: Render Abstraction (Layer 4).
 *
 * STATUS: Implemented — sorted queue with front-to-back, back-to-front,
 *         by-material, and sequential sort modes.
 */

#ifndef JCE_RENDER_QUEUE_H
#define JCE_RENDER_QUEUE_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRenderQueue JceRenderQueue;
typedef struct JceRenderer    JceRenderer;

/* ================================================================== */
/* Sort keys                                                           */
/* ================================================================== */

typedef enum {
    JCE_SORT_FRONT_TO_BACK,   /* opaque geometry (minimize overdraw) */
    JCE_SORT_BACK_TO_FRONT,   /* transparent geometry */
    JCE_SORT_BY_MATERIAL,     /* minimize state changes */
    JCE_SORT_SEQUENTIAL,      /* submission order preserved */
} JceSortMode;

/* ================================================================== */
/* Draw command                                                        */
/* ================================================================== */

typedef struct {
    uint16_t view_id;         /* bgfx view */
    uint16_t program;         /* bgfx program handle */
    uint32_t mesh_vbh;        /* vertex buffer handle */
    uint32_t mesh_ibh;        /* index buffer handle */
    uint32_t index_count;
    jce_mat4 transform;       /* model matrix */
    float    depth;           /* camera-space Z for sorting */
    uint32_t material_key;    /* hash for material grouping */
} JceDrawCmd;

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

/* Create a render queue with initial capacity. */
JCE_API JceRenderQueue *jce_rq_create(uint32_t initial_capacity);
JCE_API void            jce_rq_destroy(JceRenderQueue *rq);

/* ================================================================== */
/* Submission                                                          */
/* ================================================================== */

/* Push a draw command into the queue. */
JCE_API void jce_rq_push(JceRenderQueue *rq, const JceDrawCmd *cmd);

/* Sort queued commands by the given mode. */
JCE_API void jce_rq_sort(JceRenderQueue *rq, JceSortMode mode);

/* Submit all queued commands to bgfx and clear the queue. */
JCE_API void jce_rq_flush(JceRenderQueue *rq, const JceRenderer *renderer);

/* Discard all queued commands without submitting. */
JCE_API void jce_rq_clear(JceRenderQueue *rq);

/* Current number of queued draw commands. */
JCE_API uint32_t jce_rq_count(const JceRenderQueue *rq);

JCE_EXTERN_C_END

#endif /* JCE_RENDER_QUEUE_H */
