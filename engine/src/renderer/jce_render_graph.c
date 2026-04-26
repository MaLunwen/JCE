/*
 * jce_render_graph.c  Declarative frame graph implementation.
 *
 * Render passes declare which virtual resources they read/write.
 * At compile time the graph:
 *   1. Topologically sorts passes (Kahn's algorithm).
 *   2. Culls passes that do not contribute to any output.
 *   3. Allocates transient bgfx textures for virtual resources.
 * At execute time each pass receives a bgfx view ID and runs its
 * callback in the compiled order.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_render_graph.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "render_graph"

#define MAX_PASSES    64
#define MAX_RESOURCES 64
#define MAX_DEPS      256

/* ── Resource record ──────────────────────────────────────────────── */

typedef struct {
    bool             active;
    bool             imported;
    JceRGResourceDesc desc;

    /* Imported resource: external bgfx handle. */
    uint16_t         external_handle;

    /* Transient resource: created per-frame during execute. */
    bgfx_texture_handle_t transient_tex;
    bool             transient_allocated;

    /* Reference counting: number of passes that write to this. */
    uint16_t         writer_count;
    /* Number of passes that read this. */
    uint16_t         reader_count;
} ResourceRecord;

/* ── Pass record ──────────────────────────────────────────────────── */

typedef struct {
    bool                active;
    char                name[64];
    JceRGPassExecuteFn  fn;
    void               *userdata;

    /* Dependency edges: resources this pass reads/writes. */
    uint16_t reads[MAX_RESOURCES];
    uint16_t read_count;
    uint16_t writes[MAX_RESOURCES];
    uint16_t write_count;

    /* Topological sort state. */
    uint16_t in_degree;
    bool     culled;

    /* Assigned bgfx view at execution. */
    uint16_t bgfx_view;
} PassRecord;

/* ── Graph struct ─────────────────────────────────────────────────── */

struct JceRenderGraph {
    ResourceRecord resources[MAX_RESOURCES];
    uint16_t       resource_count;

    PassRecord     passes[MAX_PASSES];
    uint16_t       pass_count;

    /* Compiled execution order. */
    uint16_t       exec_order[MAX_PASSES];
    uint16_t       exec_count;
    bool           compiled;

    /* Starting bgfx view ID for pass allocation. */
    uint16_t       base_view_id;
};

/* ── Helpers ──────────────────────────────────────────────────────── */

static bgfx_texture_format_t to_bgfx_fmt(JceRGFormat fmt)
{
    switch (fmt) {
    case JCE_RG_FORMAT_RGBA8:              return BGFX_TEXTURE_FORMAT_RGBA8;
    case JCE_RG_FORMAT_RGBA16F:            return BGFX_TEXTURE_FORMAT_RGBA16F;
    case JCE_RG_FORMAT_DEPTH24_STENCIL8:   return BGFX_TEXTURE_FORMAT_D24S8;
    case JCE_RG_FORMAT_DEPTH32F:           return BGFX_TEXTURE_FORMAT_D32F;
    case JCE_RG_FORMAT_R32F:               return BGFX_TEXTURE_FORMAT_R32F;
    default:                               return BGFX_TEXTURE_FORMAT_RGBA8;
    }
}

/* ── Create / Destroy ─────────────────────────────────────────────── */

JceRenderGraph *jce_rg_create(void)
{
    JceRenderGraph *rg = (JceRenderGraph *)JCE_CALLOC(1, sizeof(*rg));
    if (!rg) return NULL;

    /* Reserve low views for engine use. Graph views start at 100. */
    rg->base_view_id = 100;
    return rg;
}

void jce_rg_destroy(JceRenderGraph *rg)
{
    if (!rg) return;

    /* Destroy any lingering transient textures. */
    for (uint16_t i = 0; i < rg->resource_count; i++) {
        ResourceRecord *r = &rg->resources[i];
        if (r->transient_allocated && BGFX_HANDLE_IS_VALID(r->transient_tex)) {
            bgfx_destroy_texture(r->transient_tex);
        }
    }

    JCE_FREE(rg);
}

/* ── Resource management ──────────────────────────────────────────── */

JceRGResource jce_rg_create_resource(JceRenderGraph *rg,
                                      const JceRGResourceDesc *desc)
{
    JceRGResource invalid = { UINT16_MAX };
    if (!rg || !desc || rg->resource_count >= MAX_RESOURCES) return invalid;

    uint16_t idx = rg->resource_count++;
    ResourceRecord *r = &rg->resources[idx];
    memset(r, 0, sizeof(*r));
    r->active   = true;
    r->imported = false;
    r->desc     = *desc;
    r->transient_tex.idx = UINT16_MAX;

    return (JceRGResource){ idx };
}

JceRGResource jce_rg_import_resource(JceRenderGraph *rg,
                                      uint16_t texture_handle,
                                      const char *debug_name)
{
    JceRGResource invalid = { UINT16_MAX };
    if (!rg || rg->resource_count >= MAX_RESOURCES) return invalid;

    uint16_t idx = rg->resource_count++;
    ResourceRecord *r = &rg->resources[idx];
    memset(r, 0, sizeof(*r));
    r->active          = true;
    r->imported        = true;
    r->external_handle = texture_handle;
    r->transient_tex.idx = UINT16_MAX;

    if (debug_name) {
        r->desc.debug_name = debug_name;
    }

    return (JceRGResource){ idx };
}

/* ── Pass registration ────────────────────────────────────────────── */

JceRGPass jce_rg_add_pass(JceRenderGraph *rg, const char *name,
                           JceRGPassExecuteFn fn, void *userdata)
{
    JceRGPass invalid = { UINT16_MAX };
    if (!rg || !fn || rg->pass_count >= MAX_PASSES) return invalid;

    uint16_t idx = rg->pass_count++;
    PassRecord *p = &rg->passes[idx];
    memset(p, 0, sizeof(*p));
    p->active   = true;
    p->fn       = fn;
    p->userdata = userdata;

    if (name) {
        snprintf(p->name, sizeof(p->name), "%s", name);
    }

    rg->compiled = false;
    return (JceRGPass){ idx };
}

void jce_rg_pass_read(JceRenderGraph *rg, JceRGPass pass,
                       JceRGResource resource)
{
    if (!rg || pass.idx >= rg->pass_count ||
        resource.idx >= rg->resource_count) return;

    PassRecord *p = &rg->passes[pass.idx];
    if (p->read_count >= MAX_RESOURCES) return;

    p->reads[p->read_count++] = resource.idx;
    rg->resources[resource.idx].reader_count++;
    rg->compiled = false;
}

void jce_rg_pass_write(JceRenderGraph *rg, JceRGPass pass,
                        JceRGResource resource)
{
    if (!rg || pass.idx >= rg->pass_count ||
        resource.idx >= rg->resource_count) return;

    PassRecord *p = &rg->passes[pass.idx];
    if (p->write_count >= MAX_RESOURCES) return;

    p->writes[p->write_count++] = resource.idx;
    rg->resources[resource.idx].writer_count++;
    rg->compiled = false;
}

/* ── Compilation ──────────────────────────────────────────────────── */

bool jce_rg_compile(JceRenderGraph *rg)
{
    JCE_PROFILE_ZONE_N("RenderGraph::Compile");
    if (!rg) { JCE_PROFILE_ZONE_END; return false; }

    rg->exec_count = 0;

    /* Build adjacency: pass A → pass B if A writes a resource that B reads. */
    uint16_t in_degree[MAX_PASSES];
    memset(in_degree, 0, sizeof(in_degree));

    /* For each resource, track which passes write to it. */
    uint16_t res_writers[MAX_RESOURCES][MAX_PASSES];
    uint16_t res_writer_count[MAX_RESOURCES];
    memset(res_writer_count, 0, sizeof(res_writer_count));

    for (uint16_t pi = 0; pi < rg->pass_count; pi++) {
        PassRecord *p = &rg->passes[pi];
        if (!p->active) continue;
        for (uint16_t w = 0; w < p->write_count; w++) {
            uint16_t ri = p->writes[w];
            if (res_writer_count[ri] < MAX_PASSES)
                res_writers[ri][res_writer_count[ri]++] = pi;
        }
    }

    /* Compute in-degrees: for each pass that reads a resource,
       add edges from all writers of that resource. */
    /* adjacency[a] lists passes that depend on pass a. */
    uint16_t adj[MAX_PASSES][MAX_PASSES];
    uint16_t adj_count[MAX_PASSES];
    memset(adj_count, 0, sizeof(adj_count));

    for (uint16_t pi = 0; pi < rg->pass_count; pi++) {
        PassRecord *p = &rg->passes[pi];
        if (!p->active) continue;
        for (uint16_t r = 0; r < p->read_count; r++) {
            uint16_t ri = p->reads[r];
            for (uint16_t w = 0; w < res_writer_count[ri]; w++) {
                uint16_t writer = res_writers[ri][w];
                if (writer == pi) continue; /* self-dep */
                /* Add edge writer → pi. */
                bool dup = false;
                for (uint16_t e = 0; e < adj_count[writer]; e++) {
                    if (adj[writer][e] == pi) { dup = true; break; }
                }
                if (!dup && adj_count[writer] < MAX_PASSES) {
                    adj[writer][adj_count[writer]++] = pi;
                    in_degree[pi]++;
                }
            }
        }
    }

    /* Kahn's algorithm: topological sort. */
    uint16_t queue[MAX_PASSES];
    uint16_t q_head = 0, q_tail = 0;

    for (uint16_t i = 0; i < rg->pass_count; i++) {
        if (rg->passes[i].active && in_degree[i] == 0) {
            queue[q_tail++] = i;
        }
    }

    while (q_head < q_tail) {
        uint16_t pi = queue[q_head++];
        rg->exec_order[rg->exec_count++] = pi;

        for (uint16_t e = 0; e < adj_count[pi]; e++) {
            uint16_t dep = adj[pi][e];
            in_degree[dep]--;
            if (in_degree[dep] == 0) {
                queue[q_tail++] = dep;
            }
        }
    }

    /* Check for cycles. */
    uint16_t active_count = 0;
    for (uint16_t i = 0; i < rg->pass_count; i++) {
        if (rg->passes[i].active) active_count++;
    }
    if (rg->exec_count != active_count) {
        LOG_ERROR(LOG_TAG, "cycle detected in render graph! "
                  "sorted %u of %u passes", rg->exec_count, active_count);
        rg->exec_count = 0;
        JCE_PROFILE_ZONE_END;
        return false;
    }

    /* Assign bgfx view IDs. */
    for (uint16_t i = 0; i < rg->exec_count; i++) {
        rg->passes[rg->exec_order[i]].bgfx_view =
            rg->base_view_id + i;
    }

    rg->compiled = true;
    LOG_DEBUG(LOG_TAG, "compiled %u passes, %u resources",
              rg->exec_count, rg->resource_count);
    JCE_PROFILE_ZONE_END;
    return true;
}

/* ── Execution ────────────────────────────────────────────────────── */

void jce_rg_execute(JceRenderGraph *rg)
{
    JCE_PROFILE_ZONE_N("RenderGraph::Execute");
    if (!rg || !rg->compiled || rg->exec_count == 0) { JCE_PROFILE_ZONE_END; return; }

    /* Allocate transient textures for non-imported resources. */
    for (uint16_t i = 0; i < rg->resource_count; i++) {
        ResourceRecord *r = &rg->resources[i];
        if (!r->active || r->imported) continue;
        if (r->transient_allocated) continue;

        uint16_t w = r->desc.width  ? r->desc.width  : 1920;
        uint16_t h = r->desc.height ? r->desc.height : 1080;

        r->transient_tex = bgfx_create_texture_2d(
            w, h, false, 1,
            to_bgfx_fmt(r->desc.format),
            BGFX_TEXTURE_RT,
            NULL);
        r->transient_allocated = true;
    }

    /* Execute passes in compiled order. */
    for (uint16_t i = 0; i < rg->exec_count; i++) {
        uint16_t pi = rg->exec_order[i];
        PassRecord *p = &rg->passes[pi];
        if (p->culled || !p->fn) continue;

        p->fn((JceRGPass){ pi }, p->bgfx_view, p->userdata);
    }
    JCE_PROFILE_ZONE_END;
}

/* ── Reset ────────────────────────────────────────────────────────── */

void jce_rg_reset(JceRenderGraph *rg)
{
    if (!rg) return;

    /* Destroy transient textures. */
    for (uint16_t i = 0; i < rg->resource_count; i++) {
        ResourceRecord *r = &rg->resources[i];
        if (r->transient_allocated && BGFX_HANDLE_IS_VALID(r->transient_tex)) {
            bgfx_destroy_texture(r->transient_tex);
            r->transient_tex.idx = UINT16_MAX;
            r->transient_allocated = false;
        }
    }

    /* Clear passes. */
    for (uint16_t i = 0; i < rg->pass_count; i++) {
        rg->passes[i].active = false;
    }
    rg->pass_count = 0;

    /* Clear transient resources; keep imported ones. */
    uint16_t new_count = 0;
    for (uint16_t i = 0; i < rg->resource_count; i++) {
        if (rg->resources[i].imported) {
            if (i != new_count) {
                rg->resources[new_count] = rg->resources[i];
            }
            new_count++;
        }
    }
    rg->resource_count = new_count;

    rg->exec_count = 0;
    rg->compiled   = false;
}
