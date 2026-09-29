/*
 * jce_render_graph.h  Pass-ordering graph for render passes.  ENGINE-INTERNAL.
 *
 * Layer: Render Abstraction (Layer 3).
 *
 * WHY THIS IS NOT UNDER engine/include/jce/ ANY MORE (moved 2026-08-31).
 * It was public, reachable from <jce/api.h> via api_render.h, and it cannot be
 * used.  Not "is not used" -- CANNOT be, for three structural reasons below.
 * A public header is a promise; this one could not be kept, and an
 * architecture audit reading "render graph" in the public API concluded the
 * engine had graph-ordered passes, which it does not.
 *
 * It was demoted rather than deleted.  Nothing that works breaks, because
 * nothing can work today; the correct part -- the topological sort -- is kept
 * for whoever finishes it; and re-publishing a header is one line, while
 * recovering deleted code is archaeology.
 *
 * WHAT IS ACTUALLY IMPLEMENTED, verified by running its self-test for the
 * first time on 2026-08-31.
 *
 *   WORKS
 *     - Topological ordering by READ-AFTER-WRITE (Kahn's).  Verified by
 *       jce_rg_self_test, which now runs in the unit suite.
 *     - Cycle detection: a cyclic graph fails to compile.
 *     - View assignment: pass i is given base_view_id + i.
 *
 *   WORKS ONLY FOR READ-AFTER-WRITE
 *     - WRITE-AFTER-WRITE PRODUCES NO EDGE.  compile() builds edges by walking
 *       each pass's READS and linking every writer of that resource to it; two
 *       passes that both WRITE one resource are never ordered against each
 *       other.  A clear pass and a draw pass targeting the same texture have
 *       undefined relative order.  Write-after-read is not modelled either.
 *
 *   DOES NOT WORK, despite what the resource API suggests
 *     - A PASS CANNOT REACH ITS RESOURCES.  Nothing turns a JceRGResource into
 *       a bgfx handle, and JceRGPassExecuteFn receives only
 *       (pass, view_id, userdata).  create_resource / import_resource /
 *       pass_read / pass_write therefore affect ORDERING ONLY.
 *     - NO FRAMEBUFFER IS BOUND.  jce_rg_execute never calls
 *       bgfx_set_view_frame_buffer, so a pass's view is not attached to the
 *       resource it declared it writes.
 *     - The transient textures jce_rg_execute allocates (1920x1080 RGBA8 by
 *       default, up to MAX_RESOURCES) have NO READER anywhere.
 *     - NO BARRIER PLACEMENT.  The word "barrier" does not appear in the
 *       implementation; an earlier version of this comment promised it.
 *     - NO PASS CULLING, and not implementABLE as the API stands: culling
 *       needs a designated frame output to walk back from, and there is none.
 *       PassRecord::culled is read at execute and reported to the frame
 *       debugger; nothing ever sets it.
 *     - THE ENGINE'S OWN VIEW GUARD CANNOT SEE IT.  jce_view_bands_claim(),
 *       which the scene renderer calls each frame so two subsystems cannot
 *       claim one view id unnoticed, is never called from here.
 *     - IT WOULD NOT EXPRESS THIS RENDERER'S ORDERING ANYWAY.  The scene
 *       renderer orders with bgfx_set_view_order() over a permutation from
 *       jce_scene_renderer_view_order_build(); this file never calls
 *       set_view_order.  And there, submission order and execution order
 *       deliberately DISAGREE -- SSAO and Hi-Z occlusion read the previous
 *       frame on purpose -- which a single-frame DAG models as a violation.
 *
 * To finish it, the missing pieces are a resource->handle accessor and
 * framebuffer creation in execute.  Both need a bgfx context to test, which
 * the unit suite does not have; shipping them untested is how the list above
 * came to exist.
 */

#ifndef JCE_RENDER_GRAPH_H
#define JCE_RENDER_GRAPH_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_gfx_types.h>

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

typedef JceRenderFormat JceRGFormat;
#define JCE_RG_FORMAT_RGBA8           JCE_RENDER_FORMAT_RGBA8
#define JCE_RG_FORMAT_RGBA16F         JCE_RENDER_FORMAT_RGBA16F
#define JCE_RG_FORMAT_DEPTH24_STENCIL8 JCE_RENDER_FORMAT_DEPTH24_STENCIL8
#define JCE_RG_FORMAT_DEPTH32F        JCE_RENDER_FORMAT_DEPTH32F
#define JCE_RG_FORMAT_R32F            JCE_RENDER_FORMAT_R32F

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

/* Sentinel for "the caller has not chosen a view band yet". */
#define JCE_RG_BASE_VIEW_UNSET ((uint16_t)0xFFFFu)

JceRenderGraph *jce_rg_create(void);
void            jce_rg_destroy(JceRenderGraph *rg);

/* The bgfx view id the graph's first pass is assigned; each later pass takes
 * base + 1, base + 2, ...
 *
 * THERE IS NO SAFE DEFAULT, so there is not one: jce_rg_compile() fails until
 * you call this.  bgfx view ids are a single flat 0..255 space and this engine
 * has already spoken for nearly all of it -- <jce/renderer/jce_views.h> claims
 * 3..119 for the Scene View, 120..236 for the Game View, plus 2, 20, 250, 252,
 * 253 and 254 outright.  A graph that picks its own band silently overwrites
 * whichever pass owns those ids; the symptom is corrupted shadows or a black
 * viewport, nowhere near the cause.
 *
 * Until 2026-08-31 this defaulted to 100, which lands inside the Scene View's
 * point-shadow cube band (103..119) and the Game View's core span.  Nothing
 * had ever called this API, so nothing had ever hit it.
 *
 * Pick a band yourself, knowing what else renders in your process:
 *   - a headless tool or test with no scene renderer: anything fits;
 *   - alongside jce_scene_renderer_render(): consult jce_views.h.  With both
 *     editor viewports live there is no contiguous run of 64 free ids, which
 *     is why this is your decision and not a default.
 */
void            jce_rg_set_base_view_id(JceRenderGraph *rg,
                                                uint16_t base_view_id);

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
bool jce_rg_compile(JceRenderGraph *rg);

/* Execute all passes in compiled order.  Call within a
 * frame boundary (after begin_frame / before end_frame). */
void jce_rg_execute(JceRenderGraph *rg);

/* Reset the graph for the next frame.  Clears all passes and transient
 * resource declarations.  Imported resources survive. */
void jce_rg_reset(JceRenderGraph *rg);

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
void jce_rg_frame_debug_request_capture(void);

/* Copy out the most recent capture (if any). Returns true if a
   capture is available; *out_count set to number of passes copied
   (≤ cap). Returns false if no capture has run yet. */
bool jce_rg_frame_debug_get(JceRGFrameDebugPass *out_passes,
                                     uint32_t cap, uint32_t *out_count);

/* Returns true while a capture has been requested but not yet
   fulfilled (i.e. waiting for the next execute). */
bool jce_rg_frame_debug_pending(void);

/* ================================================================== */
/* Self-test                                                            */
/* ================================================================== */

/* Build a small DAG (3 passes, 2 resources, including a known cycle in
 * a sibling sub-graph), run topo-sort + cycle detection, and verify the
 * outputs.  Does NOT touch bgfx — safe to call before/after the
 * renderer is active.  Returns true if all assertions passed. */
bool jce_rg_self_test(void);

JCE_EXTERN_C_END

#endif /* JCE_RENDER_GRAPH_H */
