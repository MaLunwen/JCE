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
 * STATUS: Implemented — topological sort (Kahn's), transient resource
 *         allocation, cycle detection, automatic view assignment.
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
JceRGResource jce_rg_create_resource(JceRenderGraph *rg,
                                      const JceRGResourceDesc *desc);

/* Import an external resource (e.g. backbuffer, persistent texture). */
JceRGResource jce_rg_import_resource(JceRenderGraph *rg,
                                      uint16_t texture_handle,
                                      const char *debug_name);

/* ================================================================== */
/* Pass registration                                                   */
/* ================================================================== */

/* Register a named render pass with an execution callback. */
JceRGPass jce_rg_add_pass(JceRenderGraph *rg, const char *name,
                           JceRGPassExecuteFn fn, void *userdata);

/* Declare that a pass reads from a resource. */
void jce_rg_pass_read(JceRenderGraph *rg, JceRGPass pass,
                       JceRGResource resource);

/* Declare that a pass writes to a resource (color or depth attachment). */
void jce_rg_pass_write(JceRenderGraph *rg, JceRGPass pass,
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

JCE_EXTERN_C_END

#endif /* JCE_RENDER_GRAPH_H */
