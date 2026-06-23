/*
 * jce_material_graph_eval.cpp — CPU-side recursive PBR evaluator that
 * walks the graph from the Output node and writes a .mat.json via
 * jce_pbr_material_save_json.
 *
 * Phase B extraction: same algorithm as the legacy single-file panel,
 * only the namespace and includes changed.
 */
#include "panels/material_graph/jce_material_graph_state.h"

#include "shadergraph/jce_shadergraph_codegen.h"
#include "shadergraph/jce_shadergraph_shaderc.h"
#include "ui/jce_editor_panels.h"

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_thread.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_renderer.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace jce_mgp {

void eval_socket(int node_id, int sock_idx, float out_color[4],
                 char out_tex[256], int depth)
{
    out_color[0] = out_color[1] = out_color[2] = out_color[3] = 1.0f;
    out_tex[0] = 0;
    if (depth > 16) return;
    Link l;
    if (!find_link_into(node_id, sock_idx, &l)) return;
    Node *n = find_node(l.from_node);
    if (!n) return;
    switch (n->type) {
        case NT_COLOR:
            for (int i = 0; i < 4; ++i) out_color[i] = n->color[i];
            return;
        case NT_FLOAT:
            out_color[0] = n->scalar; out_color[1] = out_color[2] = 0;
            out_color[3] = 1.0f;
            return;
        case NT_TEXTURE:
            std::snprintf(out_tex, 256, "%s", n->text);
            return;
        case NT_MUL_C: {
            float a[4], b[4]; char ta[256], tb[256];
            eval_socket(n->id, 0, a, ta, depth + 1);
            eval_socket(n->id, 1, b, tb, depth + 1);
            for (int i = 0; i < 4; ++i) out_color[i] = a[i] * b[i];
            if (ta[0]) std::snprintf(out_tex, 256, "%s", ta);
            else if (tb[0]) std::snprintf(out_tex, 256, "%s", tb);
            return;
        }
        case NT_MUL_F: {
            float a[4], b[4]; char ta[256], tb[256];
            eval_socket(n->id, 0, a, ta, depth + 1);
            eval_socket(n->id, 1, b, tb, depth + 1);
            out_color[0] = a[0] * b[0];
            return;
        }
        case NT_ADD_F: {
            float a[4], b[4]; char ta[256], tb[256];
            eval_socket(n->id, 0, a, ta, depth + 1);
            eval_socket(n->id, 1, b, tb, depth + 1);
            out_color[0] = a[0] + b[0];
            return;
        }
        case NT_NORMAL_MAP:
            std::snprintf(out_tex, 256, "%s", n->text);
            out_color[0] = 0.5f; out_color[1] = 0.5f;
            out_color[2] = 1.0f; out_color[3] = 1.0f;
            return;
        case NT_UV:
            out_color[0] = n->scalar;
            return;
        case NT_SUB_F: {
            float a[4], b[4]; char ta[256], tb[256];
            eval_socket(n->id, 0, a, ta, depth + 1);
            eval_socket(n->id, 1, b, tb, depth + 1);
            out_color[0] = a[0] - b[0];
            return;
        }
        case NT_LERP_C: {
            float a[4], b[4], t[4]; char ta[256], tb[256], tt[256];
            eval_socket(n->id, 0, a, ta, depth + 1);
            eval_socket(n->id, 1, b, tb, depth + 1);
            eval_socket(n->id, 2, t, tt, depth + 1);
            float k = t[0]; if (k < 0) k = 0; if (k > 1) k = 1;
            for (int i = 0; i < 4; ++i) out_color[i] = a[i] * (1.0f - k) + b[i] * k;
            return;
        }
        case NT_FRESNEL: {
            float a[4]; char ta[256];
            eval_socket(n->id, 0, a, ta, depth + 1);
            float bias = a[0]; if (bias < 0) bias = 0; if (bias > 1) bias = 1;
            out_color[0] = 1.0f - bias;
            return;
        }
        case NT_OUTPUT:
            return;
    }
}

void compile_to_material(void)
{
    log_clear();
    s_prev.valid = false;
    Node *out = nullptr;
    for (auto &n : s_g.nodes) if (n.type == NT_OUTPUT) { out = &n; break; }
    if (!out) {
        log_append(true, "missing PBR Output node");
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material graph: missing PBR Output node");
        return;
    }
    if (s_g.path[0] == 0) {
        log_append(true, "no .matgraph.json file path set");
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material graph: set a file path before compiling");
        return;
    }

    JcePbrMaterial m = jce_pbr_material_default();
    char tex_paths[5][256] = {};

    /* BaseColor (slot 0). */
    {
        float c[4]; char tex[256];
        eval_socket(out->id, 0, c, tex, 0);
        m.base_color_factor[0] = c[0];
        m.base_color_factor[1] = c[1];
        m.base_color_factor[2] = c[2];
        m.base_color_factor[3] = c[3];
        if (tex[0]) std::snprintf(tex_paths[0], 256, "%s", tex);
        for (int i = 0; i < 4; ++i) s_prev.base_color[i] = c[i];
        std::snprintf(s_prev.base_tex, sizeof(s_prev.base_tex), "%s", tex);
        log_append(false, "BaseColor: rgba=(%.2f,%.2f,%.2f,%.2f) tex=\"%s\"",
                   c[0], c[1], c[2], c[3], tex_paths[0][0] ? tex_paths[0] : "(none)");
    }
    /* Metallic (slot 1). */
    {
        float c[4]; char tex[256];
        eval_socket(out->id, 1, c, tex, 0);
        m.metallic_factor = c[0];
        if (tex[0]) std::snprintf(tex_paths[1], 256, "%s", tex);
        s_prev.metallic = c[0];
        std::snprintf(s_prev.mr_tex, sizeof(s_prev.mr_tex), "%s", tex);
        log_append(false, "Metallic: %.3f tex=\"%s\"", c[0],
                   tex_paths[1][0] ? tex_paths[1] : "(none)");
    }
    /* Roughness (slot 2). */
    {
        float c[4]; char tex[256];
        eval_socket(out->id, 2, c, tex, 0);
        m.roughness_factor = c[0];
        if (tex[0] && !tex_paths[1][0])
            std::snprintf(tex_paths[1], 256, "%s", tex);
        s_prev.roughness = c[0];
        log_append(false, "Roughness: %.3f tex=\"%s\"", c[0],
                   tex[0] ? tex : "(none)");
    }
    /* Emissive (slot 3). */
    {
        float c[4]; char tex[256];
        eval_socket(out->id, 3, c, tex, 0);
        m.emissive_factor[0] = c[0];
        m.emissive_factor[1] = c[1];
        m.emissive_factor[2] = c[2];
        if (tex[0]) std::snprintf(tex_paths[4], 256, "%s", tex);
        s_prev.emissive[0] = c[0];
        s_prev.emissive[1] = c[1];
        s_prev.emissive[2] = c[2];
        std::snprintf(s_prev.emis_tex, sizeof(s_prev.emis_tex), "%s", tex);
        log_append(false, "Emissive: rgb=(%.2f,%.2f,%.2f) tex=\"%s\"",
                   c[0], c[1], c[2], tex_paths[4][0] ? tex_paths[4] : "(none)");
    }

    s_prev.valid = true;
    if (jce_pbr_material_save_json(s_g.path, &m, tex_paths)) {
        log_append(false, "saved -> %s", s_g.path);
        jce_editor_console_log("material compiled: %s", s_g.path);
    } else {
        log_append(true, "save failed: %s", s_g.path);
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material compile failed: %s", s_g.path);
    }
}

/* ── Phase C: graph -> BGFX .sc source code ──────────────────────────── */

namespace {

/* Resolve the graph template path:
 *   1. $JCE_SHADER_TEMPLATE = absolute path to fs_graph_template.sc
 *   2. $JCE_SHADER_DEV_DIR/shaders/graph/fs_graph_template.sc
 *   3. ./engine/shaders/graph/fs_graph_template.sc  (in-tree dev fallback)
 */
void resolve_template_path(char *out, size_t cap)
{
    const char *p = std::getenv("JCE_SHADER_TEMPLATE");
    if (p && p[0]) { std::snprintf(out, cap, "%s", p); return; }
    const char *dev = std::getenv("JCE_SHADER_DEV_DIR");
    if (dev && dev[0]) {
        std::snprintf(out, cap, "%s/shaders/graph/fs_graph_template.sc", dev);
        return;
    }
    std::snprintf(out, cap, "engine/shaders/graph/fs_graph_template.sc");
}

/* Derive output basename + dir from the graph's .matgraph.json path:
 *   <dir>/foo.matgraph.json -> dir=<dir>, basename="foo"
 * Returns false if `path` has no usable basename. */
bool split_output_paths(const char *path, char *out_dir, size_t dir_cap,
                        char *out_base, size_t base_cap)
{
    if (!path || !path[0]) return false;
    const char *file = jce_editor_path_basename_view(path);
    std::snprintf(out_base, base_cap, "%s", file);
    /* strip trailing .matgraph.json (or any double extension). */
    jce_editor_path_strip_extension(out_base);
    char *dot2 = std::strrchr(out_base, '.');
    if (dot2 && std::strcmp(dot2, ".matgraph") == 0) *dot2 = '\0';

    std::snprintf(out_dir, dir_cap, "%s", path);
    jce_editor_path_trim_to_parent(out_dir);
    if (!out_dir[0]) out_dir[0] = '.', out_dir[1] = '\0';

    return out_base[0] != '\0';
}

} /* anonymous namespace */

void generate_shader(void)
{
    log_clear();
    if (!s_g.path[0]) {
        log_append(true, "Set graph file path first (Save / Load).");
        return;
    }

    char tpl[1024];
    resolve_template_path(tpl, sizeof(tpl));

    char dir[512], base[128];
    if (!split_output_paths(s_g.path, dir, sizeof(dir), base, sizeof(base))) {
        log_append(true, "Invalid graph path: %s", s_g.path);
        return;
    }

    jce_sg::CodegenResult r = jce_sg::codegen(s_g, tpl, base, dir);

    for (const jce_sg::CodegenDiag &w : r.warnings)
        log_append(false, "warn: %s", w.message.c_str());
    for (const jce_sg::CodegenDiag &e : r.errors)
        log_append(true, "error: %s", e.message.c_str());

    if (r.ok) {
        log_append(false, "shader -> %s", r.out_path.c_str());
        jce_editor_console_log("shader generated: %s", r.out_path.c_str());
    } else {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "shader generation failed (%zu error(s))", r.errors.size());
    }
}

/* ── Phase D: compile & bind ─────────────────────────────────────────── */

namespace {

/* Resolve vs_pbr.sc — the vertex shader our graph-generated fragments
 * link against.  Search order mirrors resolve_template_path():
 *   1. $JCE_SHADER_VS_PBR (explicit override)
 *   2. $JCE_SHADER_DEV_DIR/shaders/pbr/vs_pbr.sc
 *   3. ./engine/shaders/pbr/vs_pbr.sc
 */
void resolve_vs_pbr_path(char *out, size_t cap)
{
    const char *p = std::getenv("JCE_SHADER_VS_PBR");
    if (p && p[0]) { std::snprintf(out, cap, "%s", p); return; }
    const char *dev = std::getenv("JCE_SHADER_DEV_DIR");
    if (dev && dev[0]) {
        std::snprintf(out, cap, "%s/shaders/pbr/vs_pbr.sc", dev);
        return;
    }
    std::snprintf(out, cap, "engine/shaders/pbr/vs_pbr.sc");
}

/* The PBR varying.def.sc all graph-generated FS share with vs_pbr. */
void resolve_varying_def_path(char *out, size_t cap)
{
    const char *p = std::getenv("JCE_SHADER_VARYING_DEF");
    if (p && p[0]) { std::snprintf(out, cap, "%s", p); return; }
    const char *dev = std::getenv("JCE_SHADER_DEV_DIR");
    if (dev && dev[0]) {
        std::snprintf(out, cap, "%s/shaders/pbr/varying_pbr.def.sc", dev);
        return;
    }
    std::snprintf(out, cap, "engine/shaders/pbr/varying_pbr.def.sc");
}

} /* anonymous namespace */

/* ──────────────────────────────────────────────────────────────────
 * Async "Compile & Bind".
 *
 * The two shaderc.exe invocations are the slow part (subprocess spawn +
 * drain + poll, up to 30 s each).  They run on a background worker; the
 * fast codegen + path resolution stays on the UI thread, and the GPU
 * program create + .bin/.mat.json persist run back on the main thread in
 * shader_compile_poll() (GPU resource create is render-thread-only).
 * ────────────────────────────────────────────────────────────────── */
namespace {

struct ShaderCompileJob {
    /* inputs (main thread) */
    std::string vs_path, var_path, include_dir, fs_sc_path;
    /* persist context, snapshotted at launch (s_g.path may change) */
    std::string dir, base, graph_path, cg_out_path;
    /* outputs (worker) */
    jce_sg::ShadercResult vs_r;
    jce_sg::ShadercResult fs_r;
    JceAtomicI32         *done = nullptr;   /* 0 running, 1 finished */
};

JceThread        *g_sc_worker = nullptr;
ShaderCompileJob *g_sc_job    = nullptr;

/* WORKER thread: run shaderc for vs then fs.  Only touches the job (and
 * read-only renderer backend inside compile_sc) — no UI / GPU state. */
void shader_compile_worker(void *arg)
{
    ShaderCompileJob *j = (ShaderCompileJob *)arg;
    j->vs_r = jce_sg::compile_sc(j->vs_path, j->var_path, j->include_dir,
                                 jce_sg::ShaderKind::Vertex);
    if (j->vs_r.ok)
        j->fs_r = jce_sg::compile_sc(j->fs_sc_path, j->var_path,
                                     j->include_dir,
                                     jce_sg::ShaderKind::Fragment);
    jce_atomic_i32_store(j->done, 1);
}

/* MAIN thread: consume a finished compile — link the program, swap it in,
 * persist the blobs + .mat.json.  Early-returns on any failure (callers
 * free the job afterwards). */
void shader_compile_finalize(ShaderCompileJob *j)
{
    if (!j->vs_r.ok) {
        log_append(true, "vs compile failed:\n%s", j->vs_r.error.c_str());
        jce_editor_console_log_level(JCE_CONSOLE_ERROR, "shaderc vs failed");
        return;
    }
    if (!j->fs_r.ok) {
        log_append(true, "fs compile failed:\n%s", j->fs_r.error.c_str());
        jce_editor_console_log_level(JCE_CONSOLE_ERROR, "shaderc fs failed");
        return;
    }

    /* Link into a bgfx program (render-thread-only — that's why this runs
     * here and not on the worker). */
    JceShaderHandle prog = jce_renderer_create_program_from_blobs(
        j->vs_r.blob.data(), j->vs_r.blob.size(),
        j->fs_r.blob.data(), j->fs_r.blob.size());
    if (!jce_shader_valid(prog)) {
        log_append(true, "bgfx_create_program failed (vs %zu B, fs %zu B)",
                   j->vs_r.blob.size(), j->fs_r.blob.size());
        return;
    }

    /* Swap in.  Destroy the previous program *after* installing the new
     * one — bgfx defers actual destruction to end-of-frame so any
     * in-flight draw remains valid. */
    JceShaderHandle old = s_prev.custom_program;
    s_prev.custom_program = prog;
    if (jce_shader_valid(old))
        jce_renderer_destroy_program(old);

    log_append(false, "compile & bind OK -> %s (program idx=%u)",
               j->cg_out_path.c_str(), (unsigned)prog.idx);
    jce_editor_console_log("material graph: compile & bind OK (%s)",
                           j->cg_out_path.c_str());

    /* Persist the compiled blobs next to the graph and record the Shader
     * Graph reference in the sibling .mat.json so the engine can
     * re-create this program at material load (no shaderc at runtime). */
    char vs_bin[640], fs_bin[640];
    std::snprintf(vs_bin, sizeof(vs_bin), "%s/vs_%s.bin",
                  j->dir.c_str(), j->base.c_str());
    std::snprintf(fs_bin, sizeof(fs_bin), "%s/fs_%s.bin",
                  j->dir.c_str(), j->base.c_str());
    bool wrote_vs = jce_fs_host_write_all(vs_bin, j->vs_r.blob.data(),
                                          (uint64_t)j->vs_r.blob.size());
    bool wrote_fs = jce_fs_host_write_all(fs_bin, j->fs_r.blob.data(),
                                          (uint64_t)j->fs_r.blob.size());
    if (!wrote_vs || !wrote_fs) {
        log_append(true, "could not persist compiled .bin blobs "
                         "(custom shader will not survive reload)");
        return;
    }

    /* Derive sibling .mat.json: <stem>.matgraph.json -> <stem>.mat.json. */
    char mat_json[640];
    std::snprintf(mat_json, sizeof(mat_json), "%s", j->graph_path.c_str());
    jce_editor_path_strip_extension(mat_json);      /* drop .json */
    char *dot2 = std::strrchr(mat_json, '.');
    if (dot2 && std::strcmp(dot2, ".matgraph") == 0) *dot2 = '\0';
    std::strncat(mat_json, ".mat.json",
                 sizeof(mat_json) - std::strlen(mat_json) - 1);

    /* Store paths relative to the material dir (bare filenames here, since
     * the graph, blobs and .mat.json share one directory). */
    char vs_rel[160], fs_rel[160], graph_rel[160];
    std::snprintf(vs_rel, sizeof(vs_rel), "vs_%s.bin", j->base.c_str());
    std::snprintf(fs_rel, sizeof(fs_rel), "fs_%s.bin", j->base.c_str());
    std::snprintf(graph_rel, sizeof(graph_rel), "%s",
                  jce_editor_path_basename_view(j->graph_path.c_str()));

    if (jce_pbr_material_set_graph_shader(mat_json, graph_rel, vs_rel, fs_rel)) {
        log_append(false, "graph shader persisted -> %s", mat_json);
        jce_editor_console_log("material graph: graph shader saved to %s",
                               mat_json);
    } else {
        log_append(true, "failed to write graph ref into %s", mat_json);
    }
}

} /* anonymous namespace */

bool shader_compile_running(void) { return g_sc_worker != nullptr; }

/* MAIN thread, per-frame: pick up a finished background compile. */
void shader_compile_poll(void)
{
    ShaderCompileJob *j = g_sc_job;
    if (!j) return;
    if (jce_atomic_i32_load(j->done) == 0) return;   /* still running */

    if (g_sc_worker) { jce_thread_join(g_sc_worker); g_sc_worker = nullptr; }

    shader_compile_finalize(j);

    jce_atomic_i32_destroy(j->done);
    delete j;
    g_sc_job = nullptr;
}

void compile_and_bind(void)
{
    if (shader_compile_running()) {
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "material graph: a shader compile is already running");
        return;
    }

    log_clear();
    if (!s_g.path[0]) {
        log_append(true, "Set graph file path first (Save / Load).");
        return;
    }

    /* 1. codegen — produce the .sc on disk and grab its path. */
    char tpl[1024];
    resolve_template_path(tpl, sizeof(tpl));

    char dir[512], base[128];
    if (!split_output_paths(s_g.path, dir, sizeof(dir), base, sizeof(base))) {
        log_append(true, "Invalid graph path: %s", s_g.path);
        return;
    }

    jce_sg::CodegenResult cg = jce_sg::codegen(s_g, tpl, base, dir);
    for (const jce_sg::CodegenDiag &w : cg.warnings)
        log_append(false, "warn: %s", w.message.c_str());
    for (const jce_sg::CodegenDiag &e : cg.errors)
        log_append(true, "error: %s", e.message.c_str());
    if (!cg.ok) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "compile & bind aborted: codegen failed");
        return;
    }

    /* 2. resolve shaderc-side paths + sanity-check the include dir. */
    std::string include_dir = jce_sg::resolve_shader_include_dir();
    if (include_dir.empty()) {
        log_append(true,
            "Shader include directory not set. Define "
            "JCE_SHADERC_INCLUDE_DIR (or BGFX_SHADER_INCLUDE_PATH) to "
            "the bgfx shader include path (the directory containing "
            "bgfx_shader.sh).");
        return;
    }

    char vs_path[1024], var_path[1024];
    resolve_vs_pbr_path(vs_path, sizeof(vs_path));
    resolve_varying_def_path(var_path, sizeof(var_path));

    /* 3. hand the two shaderc invocations to a worker thread. */
    ShaderCompileJob *j = new ShaderCompileJob();
    j->vs_path     = vs_path;
    j->var_path    = var_path;
    j->include_dir = include_dir;
    j->fs_sc_path  = cg.out_path;
    j->cg_out_path = cg.out_path;
    j->dir         = dir;
    j->base        = base;
    j->graph_path  = s_g.path;
    j->done        = jce_atomic_i32_create(0);
    g_sc_job = j;

    log_append(false, "compiling shaders in background…");
    jce_editor_console_log("material graph: compiling shaders in background…");

    g_sc_worker = jce_thread_create(shader_compile_worker, j, "jce_sg_compile");
    if (!g_sc_worker) {
        /* No worker thread available: run inline then finalise now. */
        shader_compile_worker(j);
        shader_compile_poll();
    }
}

} /* namespace jce_mgp */
