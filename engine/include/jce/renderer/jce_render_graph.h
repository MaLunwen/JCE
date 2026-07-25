/*
 * jce_render_graph.h  Declarative frame graph for render passes.
 *
 * Abstracts render views into a directed acyclic graph of render passes
 * with automatic resource (transient texture) management.  Each pass
 * declares its inputs, outputs, and attachments; the graph compiles
 * them into an optimal execution order with barrier placement.
 *
 * Layer: Render Abstraction (Layer 4).
 *
 * STATUS: BUILT BUT NOT WIRED.  The machinery below is complete —
 *         topological sort (Kahn's), transient resource allocation, cycle
 *         detection, automatic view assignment — but jce_rg_execute has no
 *         production caller and no test.  The renderer submits its passes
 *         directly against fixed JCE_VIEW_* ids (see jce_scene_renderer.c),
 *         so pass ordering and resource lifetime are NOT graph-managed
 *         today; the frame debugger notes the same thing.
 *
 *         This distinction matters when auditing the architecture: reading
 *         "Implemented" here previously suggested the engine had
 *         graph-ordered passes, which it does not.  Wiring it up means
 *         moving the scene renderer's view assignments into passes — a real
 *         project, not a switch to flip.
 */

#ifndef JCE_RENDER_GRAPH_H
#define JCE_RENDER_GRAPH_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Opaque types                                                        */
/* ================================================================== */

typedef struct JceRenderGraph JceRenderGraph;

/* Handle to a virtual resource managed by the graph. */
typedef struct { uint16_t idx; } JceRGResource;

/* Handle to a registered render pass. */
typedef struct { uint16_t idx; } JceRGPass;

/* ================================================================== */
/* Resource descriptors                                                */
/* ================================================================== */

typedef enum {
    JCE_RG_FORMAT_RGBA8,
    JCE_RG_FORMAT_RGBA16F,
    JCE_RG_FORMAT_DEPTH24_STENCIL8,
    JCE_RG_FORMAT_DEPTH32F,
    JCE_RG_FORMAT_R32F,
} JceRGFormat;

typedef struct {
    uint16_t    width;      /* 0 = backbuffer-relative */
    uint16_t    height;
    JceRGFormat format;
    const char *debug_name;
} JceRGResourceDesc;

/* ================================================================== */
/* Pass setup callback                                                 */
/* ================================================================== */

/*
 * Called once per frame for each pass.  The callback should:
 *   1. Declare resource reads/writes via jce_rg_pass_read / _write.
 *   2. Record draw commands into the provided view.
 */
typedef void (*JceRGPassExecuteFn)(JceRGPass pass, uint16_t view_id,
                                   void *userdata);

/* ================================================================== */
/* Graph lifecycle                                                     */
/* ================================================================== */

JCE_API JceRenderGraph *jce_rg_create(void);
JCE_API void            jce_rg_destroy(JceRenderGraph *rg);

/* ================================================================== */
/* Resource management                                                 */
/* ================================================================== */

/* Declare a transient resource (allocated per-frame by the graph). */
JCE_API JceRGResource jce_rg_create_resource(JceRenderGraph *rg,
                                      const JceRGResourceDesc *desc);

/* Import an external resource (e.g. backbuffer, persistent texture). */
JCE_API JceRGResource jce_rg_import_resource(JceRenderGraph *rg,
                                      uint16_t texture_handle,
                                      const char *debug_name);

/* ================================================================== */
/* Pass registration                                                   */
/* ================================================================== */

/* Register a named render pass with an execution callback. */
JCE_API JceRGPass jce_rg_add_pass(JceRenderGraph *rg, const char *name,
                           JceRGPassExecuteFn fn, void *userdata);

/* Declare that a pass reads from a resource. */
JCE_API void jce_rg_pass_read(JceRenderGraph *rg, JceRGPass pass,
                       JceRGResource resource);

/* Declare that a pass writes to a resource (color or depth attachment). */
JCE_API void jce_rg_pass_write(JceRenderGraph *rg, JceRGPass pass,
                        JceRGResource resource);

/* ================================================================== */
/* Compilation & execution                                             */
/* ================================================================== */

/* Compile the graph: topological sort, cull unused passes, allocate
 * transient resources.  Returns false on cycle or error. */
JCE_API bool jce_rg_compile(JceRenderGraph *rg);

/* Execute all passes in compiled order.  Call within a
 * frame boundary (after begin_frame / before end_frame). */
JCE_API void jce_rg_execute(JceRenderGraph *rg);

/* Reset the graph for the next frame.  Clears all passes and transient
 * resource declarations.  Imported resources survive. */
JCE_API void jce_rg_reset(JceRenderGraph *rg);

/* ================================================================== */
/* Frame debug capture (Sprint 3 #10)                                   */
/* ================================================================== */

/*
 * One captured pass entry. Filled when a capture is requested via
 * jce_rg_frame_debug_request_capture() and the next jce_rg_execute()
 * runs. The buffer is owned by the render-graph module; copy out
 * before issuing another capture.
 */
typedef struct {
    char     name[64];
    uint16_t view_id;
    uint16_t read_count;
    uint16_t write_count;
    bool     culled;
    /* Up to 8 read / 8 write resource debug names per pass; longer
       lists are truncated and the count fields still reflect the true
       counts. */
    char     read_names[8][32];
    char     write_names[8][32];
} JceRGFrameDebugPass;

/* Request a single-frame capture on the *next* execute. Cheap; safe
   to call from any thread that owns the graph. */
JCE_API void jce_rg_frame_debug_request_capture(void);

/* Copy out the most recent capture (if any). Returns true if a
   capture is available; *out_count set to number of passes copied
   (≤ cap). Returns false if no capture has run yet. */
JCE_API bool jce_rg_frame_debug_get(JceRGFrameDebugPass *out_passes,
                                     uint32_t cap, uint32_t *out_count);

/* Returns true while a capture has been requested but not yet
   fulfilled (i.e. waiting for the next execute). */
JCE_API bool jce_rg_frame_debug_pending(void);

/* ================================================================== */
/* Self-test                                                            */
/* ================================================================== */

/* Build a small DAG (3 passes, 2 resources, including a known cycle in
 * a sibling sub-graph), run topo-sort + cycle detection, and verify the
 * outputs.  Does NOT touch bgfx — safe to call before/after the
 * renderer is active.  Returns true if all assertions passed. */
JCE_API bool jce_rg_self_test(void);

JCE_EXTERN_C_END

#endif /* JCE_RENDER_GRAPH_H */
