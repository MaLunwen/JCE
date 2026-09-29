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
#include "jce_render_graph.h"

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

/* ── Frame debug capture (Sprint 3 #10) ────────────────────────────── */

#define FRAME_DEBUG_MAX_PASSES 64

static struct {
    bool                pending;       /* request set, not yet captured */
    bool                has_capture;   /* at least one capture done */
    uint32_t            count;         /* passes in current capture */
    JceRGFrameDebugPass passes[FRAME_DEBUG_MAX_PASSES];
} g_fd = { 0 };

void jce_rg_frame_debug_request_capture(void)
{
    g_fd.pending = true;
}

bool jce_rg_frame_debug_pending(void)
{
    return g_fd.pending;
}

bool jce_rg_frame_debug_get(JceRGFrameDebugPass *out_passes,
                             uint32_t cap, uint32_t *out_count)
{
    if (!g_fd.has_capture) {
        if (out_count) *out_count = 0;
        return false;
    }
    uint32_t n = g_fd.count;
    if (n > cap) n = cap;
    if (out_passes && n > 0)
        memcpy(out_passes, g_fd.passes, n * sizeof(JceRGFrameDebugPass));
    if (out_count) *out_count = n;
    return true;
}

static void fd_capture_now(JceRenderGraph *rg)
{
    g_fd.count = 0;
    if (!rg) return;
    for (uint16_t i = 0; i < rg->exec_count && g_fd.count < FRAME_DEBUG_MAX_PASSES; i++) {
        uint16_t pi = rg->exec_order[i];
        PassRecord *p = &rg->passes[pi];
        JceRGFrameDebugPass *out = &g_fd.passes[g_fd.count++];
        memset(out, 0, sizeof(*out));
        snprintf(out->name, sizeof(out->name), "%s", p->name);
        out->view_id     = p->bgfx_view;
        out->read_count  = p->read_count;
        out->write_count = p->write_count;
        out->culled      = p->culled;
        uint16_t rn = p->read_count  > 8 ? 8 : p->read_count;
        uint16_t wn = p->write_count > 8 ? 8 : p->write_count;
        for (uint16_t k = 0; k < rn; k++) {
            uint16_t ri = p->reads[k];
            const char *nm = (ri < rg->resource_count && rg->resources[ri].desc.debug_name)
                              ? rg->resources[ri].desc.debug_name : "?";
            snprintf(out->read_names[k], sizeof(out->read_names[k]), "%s", nm);
        }
        for (uint16_t k = 0; k < wn; k++) {
            uint16_t ri = p->writes[k];
            const char *nm = (ri < rg->resource_count && rg->resources[ri].desc.debug_name)
                              ? rg->resources[ri].desc.debug_name : "?";
            snprintf(out->write_names[k], sizeof(out->write_names[k]), "%s", nm);
        }
    }
    g_fd.has_capture = true;
    g_fd.pending     = false;
}

/* ── Create / Destroy ─────────────────────────────────────────────── */

JceRenderGraph *jce_rg_create(void)
{
    JceRenderGraph *rg = (JceRenderGraph *)JCE_CALLOC(1, sizeof(*rg));
    if (!rg) return NULL;

    /* Reserve low views for engine use. Graph views start at 100. */
    /* No default: see jce_rg_set_base_view_id() in the header.  This was 100,
     * which lands inside the Scene View's point-shadow cube band (103..119)
     * and the Game View's core span -- silently overwriting whichever pass
     * owns those ids.  Nothing had ever called this API, so nothing had ever
     * hit it.  compile() now refuses until the caller chooses. */
    rg->base_view_id = JCE_RG_BASE_VIEW_UNSET;
    return rg;
}

void jce_rg_set_base_view_id(JceRenderGraph *rg, uint16_t base_view_id)
{
    if (!rg) return;
    rg->base_view_id = base_view_id;
    rg->compiled     = false;   /* view assignment is part of compile output */
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

    if (rg->base_view_id == JCE_RG_BASE_VIEW_UNSET) {
        LOG_ERROR(LOG_TAG,
                  "compile: no base view id.  bgfx view ids are one flat "
                  "0..255 space and jce_views.h has already claimed almost "
                  "all of it; there is no band this graph can safely assume. "
                  "Call jce_rg_set_base_view_id() with a range you know is "
                  "free in YOUR process.");
        JCE_PROFILE_ZONE_END;
        return false;
    }

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
            NULL, 0);
        r->transient_allocated = true;
    }

    /* Execute passes in compiled order. */
    for (uint16_t i = 0; i < rg->exec_count; i++) {
        uint16_t pi = rg->exec_order[i];
        PassRecord *p = &rg->passes[pi];
        if (p->culled || !p->fn) continue;

        p->fn((JceRGPass){ pi }, p->bgfx_view, p->userdata);
    }

    /* Frame debug capture (after pass execute so view IDs are stable). */
    if (g_fd.pending) {
        fd_capture_now(rg);
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

/* ── Self-test ────────────────────────────────────────────────────── */

/* jce_rg_add_pass() REJECTS a NULL execute callback, and the ordering tests
 * below do not execute anything -- so they need a callback that does nothing
 * rather than no callback.  Passing NULL is what made this self-test fail the
 * first time it was ever run (2026-08-31): all three add_pass calls returned
 * the invalid handle, pass_count stayed 0, and compile() then reported
 * success on an empty graph.
 *
 * NOTE for whoever owns this subsystem: add_pass rejects a NULL fn while
 * execute (jce_render_graph.c:424, `if (p->culled || !p->fn) continue;`)
 * tolerates one.  Those two cannot both be the intended contract.  Left as
 * found -- loosening a validation check in the renderer on a reading of the
 * code is not a self-test's call to make. */
static void rg_selftest_noop(JceRGPass pass, uint16_t view, void *userdata)
{
    (void)pass; (void)view; (void)userdata;
}

bool jce_rg_self_test(void)
{
    bool ok = true;

    /* ----- Test 1: linear DAG compiles in correct order ----- */
    JceRenderGraph *rg = jce_rg_create();
    if (!rg) return false;

    /* Any band is free here: this runs headless with no scene renderer.  It
     * is set explicitly because compile() now requires it -- see
     * jce_rg_set_base_view_id() in the header for why there is no default. */
    jce_rg_set_base_view_id(rg, 0u);

    JceRGResourceDesc d = { 256, 256, JCE_RG_FORMAT_RGBA8, "rt" };
    JceRGResource a = jce_rg_create_resource(rg, &d);
    JceRGResource b = jce_rg_create_resource(rg, &d);

    JceRGPass p0 = jce_rg_add_pass(rg, "shadow", rg_selftest_noop, NULL);
    JceRGPass p1 = jce_rg_add_pass(rg, "opaque", rg_selftest_noop, NULL);
    JceRGPass p2 = jce_rg_add_pass(rg, "post",   rg_selftest_noop, NULL);
    /* Checked, because not checking is why the NULL-fn bug above was silent:
     * add_pass returned the invalid handle three times, pass_write() then
     * bounds-rejected every edge, and compile() reported success on the empty
     * graph that was left. */
    if (p0.idx == UINT16_MAX || p1.idx == UINT16_MAX || p2.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "self-test 1: add_pass rejected a pass "
                  "(idx %u,%u,%u)", p0.idx, p1.idx, p2.idx);
        jce_rg_destroy(rg);
        return false;
    }

    jce_rg_pass_write(rg, p0, a);
    jce_rg_pass_read (rg, p1, a);
    jce_rg_pass_write(rg, p1, b);
    jce_rg_pass_read (rg, p2, b);

    if (!jce_rg_compile(rg)) {
        LOG_ERROR(LOG_TAG, "self-test 1: linear DAG failed to compile");
        ok = false;
    }
    if (rg->exec_count != 3) {
        LOG_ERROR(LOG_TAG, "self-test 1: exec_count=%d (want 3)",
                  rg->exec_count);
        ok = false;
    } else if (rg->exec_order[0] != p0.idx ||
               rg->exec_order[1] != p1.idx ||
               rg->exec_order[2] != p2.idx) {
        LOG_ERROR(LOG_TAG, "self-test 1: exec_order=%d,%d,%d "
                  "(want %d,%d,%d = shadow,opaque,post)",
                  rg->exec_order[0], rg->exec_order[1], rg->exec_order[2],
                  p0.idx, p1.idx, p2.idx);
        ok = false;
    }
    jce_rg_destroy(rg);

    /* ----- Test 2: cycle is detected ----- */
    rg = jce_rg_create();
    if (!rg) return false;

    jce_rg_set_base_view_id(rg, 0u);

    JceRGResource ra = jce_rg_create_resource(rg, &d);
    JceRGResource rb = jce_rg_create_resource(rg, &d);
    JceRGPass q0 = jce_rg_add_pass(rg, "loop_a", rg_selftest_noop, NULL);
    JceRGPass q1 = jce_rg_add_pass(rg, "loop_b", rg_selftest_noop, NULL);
    if (q0.idx == UINT16_MAX || q1.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "self-test 2: add_pass rejected a pass");
        jce_rg_destroy(rg);
        return false;
    }
    /* q0 writes a, reads b ; q1 writes b, reads a → cycle */
    jce_rg_pass_write(rg, q0, ra); jce_rg_pass_read(rg, q0, rb);
    jce_rg_pass_write(rg, q1, rb); jce_rg_pass_read(rg, q1, ra);

    if (jce_rg_compile(rg)) {
        /* Cycle MUST cause compile to fail. */
        LOG_ERROR(LOG_TAG, "self-test 2: a cyclic graph compiled; the "
                  "cycle check did not reject it");
        ok = false;
    }
    jce_rg_destroy(rg);

    /* ----- Test 3: compile refuses without an explicit view band ----- */
    rg = jce_rg_create();
    if (!rg) return false;
    {
        JceRGPass r0 = jce_rg_add_pass(rg, "unbased", rg_selftest_noop, NULL);
        if (r0.idx == UINT16_MAX) {
            LOG_ERROR(LOG_TAG, "self-test 3: add_pass rejected a pass");
            ok = false;
        } else if (jce_rg_compile(rg)) {
            /* Before 2026-08-31 this compiled against a hardcoded base of 100,
             * which overlaps the Scene View's point-cube band and the Game
             * View's core span. */
            LOG_ERROR(LOG_TAG, "self-test 3: compiled with no base view id; "
                      "the graph would have picked a band on its own");
            ok = false;
        }
    }
    jce_rg_destroy(rg);

    LOG_INFO(LOG_TAG, "self-test: %s", ok ? "PASS" : "FAIL");
    return ok;
}
