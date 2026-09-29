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
    JCE_SORT_FOR_INSTANCING,  /* group identical (program+vbh+ibh+material) for
                                 auto-instancing during flush */
} JceSortMode;

/* ================================================================== */
/* Draw command                                                        */
/* ================================================================== */

typedef struct {
    uint16_t view_id;         /* bgfx view */
    uint16_t program;         /* bgfx instance-variant program (used when batched as instanced) */
    uint16_t program_single;  /* bgfx non-instance program (used for n=1 fallback). 0xFFFF → reuse `program` */
    uint16_t _pad;
    uint32_t mesh_vbh;        /* vertex buffer handle */
    uint32_t mesh_ibh;        /* index buffer handle */
    uint32_t index_count;
    jce_mat4 transform;       /* model matrix (single-instance / first instance) */
    float    depth;           /* camera-space Z for sorting */
    uint32_t material_key;    /* hash for material grouping */
    uint64_t state;           /* bgfx render state (blend/cull/write); 0 → BGFX_STATE_DEFAULT */

    /* Author's tie-breaker for the transparent pass (Unity's
     * Material.renderQueue, Godot's render_priority, UE's Translucency Sort
     * Priority).  Sorted BEFORE depth in JCE_SORT_BACK_TO_FRONT, so a higher
     * value draws later -- on top -- regardless of distance.
     *
     * Transparents sorted by camera depth ALONE, which has no answer for the
     * cases authors actually hit: two coplanar quads, a decal that must land
     * on top of the glass it sits inside, interpenetrating water and a
     * windscreen.  Those flicker as the camera moves and nothing could say
     * which wins, because the queue had no field to say it with.
     *
     * APPENDED, not folded into the `_pad` above.  Reusing padding costs no
     * bytes and would have been tempting, but this struct is in a public
     * header: check_abi_snapshot reads a changed member at an existing index
     * as a break, and it is right to -- a consumer compiled against the old
     * layout has a `_pad` there, and only the APPEND rule keeps that consumer
     * correct without recompiling.  Zero is the neutral value, which every
     * memset'd command already carries.
     *
     * Ignored by every other sort mode: opaque order is a performance
     * decision (front-to-back, to kill overdraw), not an authoring one. */
    int16_t  priority;

    /* The material's bgfx STENCIL word; 0 (BGFX_STENCIL_NONE) = no stencil.
     * Separate from `state` because bgfx keeps them separate -- set_stencil
     * is its own call -- and carried on the COMMAND rather than applied in
     * the material bind because bgfx resets the stencil after every submit
     * exactly as it resets the state (RenderDraw::clear, BGFX_DISCARD_STATE).
     * A per-material-run bind would therefore reach the run's first draw and
     * nothing else.  APPENDED; zero is what every memset'd command holds. */
    uint32_t stencil;

    /* WHERE in the index buffer this draw starts.  The queue could only ever
     * draw from 0, which is right for a mesh and wrong for a merged static
     * batch: its members occupy known slices of ONE buffer (jce_static_batch.h
     * writes the table), and submitting the whole thing whenever any member is
     * on screen is the cost merging quietly adds.  With this, the colour pass
     * pushes one command per visible RUN.
     *
     * APPENDED for the reason `priority` above says in full: a changed member
     * at an existing index is an ABI break, an append is not.  Zero is the
     * neutral value every memset'd command already carries, so every existing
     * push still means "from the start", byte for byte.
     *
     * It is part of the BATCHING KEY on purpose (see the merge predicate in
     * jce_render_queue.c): two runs of the same mesh are different draws and
     * must not be folded into one instanced batch that would draw the first
     * run's range for both. */
    uint32_t first_index;
} JceDrawCmd;

/* ================================================================== */
/* GPU instancing                                                      */
/* ================================================================== */

/* Per-instance payload uploaded to the GPU. The bgfx convention is
 * vec4[N] (16-byte chunks).  The smallest useful payload is a 4x4
 * model matrix = 4 vec4 = 64 bytes; callers may extend with extra
 * vec4 columns (e.g. tint colour) by widening `stride_vec4`. */
typedef struct {
    const void *data;          /* pointer to instance_count * stride bytes
                                * (ignored when persist_vb != 0) */
    uint32_t    instance_count;/* must be >= 2 to actually instance */
    uint16_t    stride_vec4;   /* number of vec4 (16 B) per instance, >= 4 */
    /* Persistent GPU instance source (千万 S1): a bgfx dynamic-vertex-buffer
     * holding the instance attributes, stored as idx+1 (0 = none → upload `data`
     * to a transient buffer per frame as before). When set, the flush binds it
     * via set_instance_data_from_dynamic_vertex_buffer — NO per-frame CPU copy,
     * so per-frame cost is O(1) regardless of instance_count. The VB layout must
     * declare the 4 instance attributes (mat4) the instanced VS reads. */
    uint16_t    persist_vb;
} JceInstanceBatch;

/* ================================================================== */
/* Material binding (optional)                                         */
/* ================================================================== */

/* Called by jce_rq_flush at the start of every "material run" (i.e. the
 * first entry in a sequence sharing the same material_key).  Use this to
 * bind textures/uniforms that are not stored in JceDrawCmd.  Optional —
 * if no binder is set, the queue assumes callers have already bound any
 * required state externally (legacy behaviour). */
typedef void (*JceRqBindMaterialFn)(uint32_t material_key, void *user);

JCE_API void jce_rq_set_material_binder(JceRenderQueue *rq,
                                         JceRqBindMaterialFn fn,
                                         void *user);

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

/* Push an explicitly-instanced draw command.  The transform inside `cmd`
 * is ignored — per-instance transforms come from `batch`.  No batching
 * is performed during flush for these entries (they go through as a
 * single instanced submit). */
JCE_API void jce_rq_push_instanced(JceRenderQueue *rq, const JceDrawCmd *cmd,
                                    const JceInstanceBatch *batch);

/* Sort queued commands by the given mode. */
JCE_API void jce_rq_sort(JceRenderQueue *rq, JceSortMode mode);

/* Disable auto-instancing/batching for this queue.  Transparent (back-to-
 * front) queues MUST set this so adjacent same-material entries at different
 * depths are not merged into one submit, which would break draw order and
 * blend correctness.  Default (false) keeps opaque auto-instancing. */
JCE_API void jce_rq_set_no_batch(JceRenderQueue *rq, bool no_batch);

/* Submit all queued commands to bgfx and clear the queue.
 *
 * Auto-instancing: when sorted with JCE_SORT_BY_MATERIAL or
 * JCE_SORT_FOR_INSTANCING, consecutive entries that share
 * (view_id, program, vbh, ibh, material_key, index_count) are merged
 * into a single bgfx instanced submit. */
JCE_API void jce_rq_flush(JceRenderQueue *rq, const JceRenderer *renderer);

/* Statistics from the most recent jce_rq_flush call. */
typedef struct {
    uint32_t commands_in;     /* draw commands fed into flush */
    uint32_t submits_out;     /* actual bgfx_submit calls issued */
    uint32_t batches_merged;  /* number of auto-merged instance batches */
    uint32_t instances_total; /* total instances across merged batches */
} JceRenderQueueStats;

JCE_API void jce_rq_last_stats(const JceRenderQueue *rq,
                                JceRenderQueueStats *out);

/* Predict the merge result *as if* the queue were flushed now, without
 * calling bgfx.  Useful for stats panels, profilers, and validating
 * sort behaviour.  Does not modify the queue.  Sort first if you want
 * the optimistic (post-instancing) numbers. */
JCE_API void jce_rq_analyse(const JceRenderQueue *rq,
                             JceRenderQueueStats *out);

/* Discard all queued commands without submitting. */
JCE_API void jce_rq_clear(JceRenderQueue *rq);

/* Current number of queued draw commands. */
JCE_API uint32_t jce_rq_count(const JceRenderQueue *rq);

JCE_EXTERN_C_END

#endif /* JCE_RENDER_QUEUE_H */
