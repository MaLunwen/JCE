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
#include "shadergraph/jce_shadergraph_registry.h"
#include "shadergraph/jce_shadergraph_shaderc.h"
#include "ui/jce_editor_panels.h"
#include "scene/jce_editor_scene_render.h"

extern "C" {
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_scene_renderer.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace jce_mgp {

/* Evaluate a PBR Output socket for the .mat.json.
 *
 * An UNCONNECTED output socket takes jce_sg::output_socket_default -- the same
 * value the generated shader uses -- rather than eval_socket's neutral
 * (1,1,1,1).  Those were the two answers: the shader said metallic 0 and
 * roughness 0.5, the material document said 1 and 1. */
void eval_output_socket(int out_node_id, int sock_idx, float out_color[4],
                        char out_tex[256])
{
    Link l;
    if (!find_link_into(out_node_id, sock_idx, &l)) {
        jce_sg::output_socket_default(sock_idx, out_color);
        out_tex[0] = 0;
        return;
    }
    eval_socket(out_node_id, sock_idx, out_color, out_tex, 0);
}

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


/* <stem>.matgraph.json -> <stem>.mat.json, the panel's one convention for
 * where a graph's material lives.
 *
 * ONE copy, because the two callers disagreed and the disagreement destroyed
 * data: shader_compile_finalize derived the sibling correctly, while
 * compile_to_material wrote the material to the graph path itself and
 * overwrote the graph.  A path convention with two implementations is a
 * convention exactly until one of them is edited. */
void mat_json_for_graph(const char *graph_path, char *out, size_t cap)
{
    std::snprintf(out, cap, "%s", graph_path ? graph_path : "");
    jce_editor_path_strip_extension(out);           /* drop .json */
    char *dot = std::strrchr(out, '.');
    if (dot && std::strcmp(dot, ".matgraph") == 0) *dot = '\0';
    std::strncat(out, ".mat.json", cap - std::strlen(out) - 1);
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
        eval_output_socket(out->id, 0, c, tex);
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
        eval_output_socket(out->id, 1, c, tex);
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
        eval_output_socket(out->id, 2, c, tex);
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
        eval_output_socket(out->id, 3, c, tex);
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
    char mat_out[640];
    mat_json_for_graph(s_g.path, mat_out, sizeof(mat_out));
    if (jce_pbr_material_save_json(mat_out, &m, tex_paths)) {
        log_append(false, "saved -> %s", mat_out);
        jce_editor_console_log("material compiled: %s", mat_out);
    } else {
        log_append(true, "save failed: %s", mat_out);
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material compile failed: %s", mat_out);
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

/* The last .sc this panel wrote, for the Shader Inspector to prefill from.
 * A static rather than a member of the graph: it is a fact about the
 * SESSION ("what did you last generate"), not about the document, and it
 * must survive loading a different graph. */
static char s_last_generated_sc[512];

void jce_mgp_note_generated_sc(const char *path)
{
    std::snprintf(s_last_generated_sc, sizeof(s_last_generated_sc), "%s",
                  path ? path : "");
}

extern "C" const char *jce_panel_material_graph_last_generated_sc(void)
{
    return s_last_generated_sc;
}

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
        jce_mgp_note_generated_sc(r.out_path.c_str());
    } else {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "shader generation failed (%zu error(s))", r.errors.size());
    }
}

/* ── Phase D: compile & bind ─────────────────────────────────────────── */

namespace {

} /* anonymous namespace */

/* ──────────────────────────────────────────────────────────────────
 * Async "Compile & Bind".
 *
 * The two shaderc.exe invocations are the slow part (subprocess spawn +
 * drain + poll, up to 30 s each). They run as structured background work;
 * the fast codegen + path resolution stays on the UI thread, and the GPU
 * program create + .bin/.mat.json persist run in the owner-thread completion
 * callback (GPU resource create is render-thread-only).
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
};

JceAsyncTask *g_sc_task = nullptr;

/* WORKER thread: run shaderc for vs then fs.  Only touches the job (and
 * read-only renderer backend inside compile_sc) — no UI / GPU state. */
JceAsyncRunResult shader_compile_worker(JceAsyncContext *ctx, void *arg)
{
    ShaderCompileJob *j = (ShaderCompileJob *)arg;
    if (jce_async_context_cancel_requested(ctx))
        return JCE_ASYNC_RUN_CANCELLED;

    j->vs_r = jce_sg::compile_sc(j->vs_path, j->var_path, j->include_dir,
                                 jce_sg::ShaderKind::Vertex);
    jce_async_context_set_progress(ctx, 0.5f);

    if (jce_async_context_cancel_requested(ctx))
        return JCE_ASYNC_RUN_CANCELLED;

    if (j->vs_r.ok) {
        j->fs_r = jce_sg::compile_sc(j->fs_sc_path, j->var_path,
                                     j->include_dir,
                                     jce_sg::ShaderKind::Fragment);
    }

    if (jce_async_context_cancel_requested(ctx))
        return JCE_ASYNC_RUN_CANCELLED;
    jce_async_context_set_progress(ctx, 1.0f);
    return JCE_ASYNC_RUN_SUCCESS;
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

    /* AND ONE BLOB PER BACKEND, beside the bare pair written above.
     *
     * A bgfx blob is bytecode for exactly one backend.  The bare files are
     * compiled for whatever backend the EDITOR is running, so on their own
     * they render here and link-fail everywhere else -- half of the reason a
     * Shader Graph material did not survive the cook (the other half was that
     * the packer never collected them at all).  The runtime prefers
     * `<stem>_<suffix>.bin` and falls back to the bare name, so this is
     * additive: a material authored before this keeps working.
     *
     * A profile that cannot express this graph FAILS LOUDLY here rather than
     * silently shipping a shader that reverts to stock PBR on that backend --
     * which is the whole defect being fixed. */
    {
        int ntargets = 0;
        const jce_sg::GraphTarget *tg = jce_sg::graph_targets(&ntargets);
        int okc = 0;
        for (int ti = 0; ti < ntargets; ++ti) {
            jce_sg::ShadercResult tv = jce_sg::compile_sc(
                j->vs_path, j->var_path, j->include_dir,
                jce_sg::ShaderKind::Vertex, tg[ti].backend);
            jce_sg::ShadercResult tf = jce_sg::compile_sc(
                j->fs_sc_path, j->var_path, j->include_dir,
                jce_sg::ShaderKind::Fragment, tg[ti].backend);
            if (!tv.ok || !tf.ok) {
                log_append(true, "backend %s: not compiled (%s)",
                           tg[ti].suffix,
                           tv.ok ? tf.error.c_str() : tv.error.c_str());
                continue;
            }
            char vb[700], fb[700];
            std::snprintf(vb, sizeof(vb), "%s/vs_%s_%s.bin",
                          j->dir.c_str(), j->base.c_str(), tg[ti].suffix);
            std::snprintf(fb, sizeof(fb), "%s/fs_%s_%s.bin",
                          j->dir.c_str(), j->base.c_str(), tg[ti].suffix);
            if (jce_fs_host_write_all(vb, tv.blob.data(), (uint64_t)tv.blob.size()) &&
                jce_fs_host_write_all(fb, tf.blob.data(), (uint64_t)tf.blob.size()))
                ++okc;
        }
        log_append(false, "persisted %d/%d backend variants", okc, ntargets);
    }

    /* Derive sibling .mat.json: <stem>.matgraph.json -> <stem>.mat.json. */
    char mat_json[640];
    mat_json_for_graph(j->graph_path.c_str(), mat_json, sizeof(mat_json));

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
        /* AND MAKE THE SCENE AGREE WITH THE PANEL.
         *
         * Hot-swapping the program above updates the graph panel's preview
         * sphere.  Entities already placed in the scene hold their own
         * resolved program index, taken at load, and the scene renderer caches
         * the .mat.json -> program association by path and never evicts it --
         * so the editor showed the new shader in one window and the old one in
         * the other, with nothing to say which was current.
         *
         * Forgetting the cache entry costs one re-read of the .mat.json on the
         * next draw; re-applying the material walks the entities whose
         * material_path matches and is what the material viewer's Save already
         * does. */
        if (JceSceneRenderer *ssr = jce_editor_get_scene_renderer())
            jce_scene_renderer_invalidate_custom_program(ssr, mat_json);
        jce_editor_inspector_reload_material(mat_json);
    } else {
        log_append(true, "failed to write graph ref into %s", mat_json);
    }
}

void shader_compile_complete(JceAsyncTask *task, void *arg)
{
    ShaderCompileJob *j = (ShaderCompileJob *)arg;
    JceAsyncState state = jce_async_task_state(task);

    if (state == JCE_ASYNC_STATE_SUCCEEDED) {
        shader_compile_finalize(j);
    } else if (state == JCE_ASYNC_STATE_CANCELLED) {
        log_append(true, "shader compilation cancelled");
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                                     "material graph: shader compile cancelled");
    } else {
        const char *error = jce_async_task_error_message(task);
        log_append(true, "shader compilation task failed%s%s",
                   error && error[0] ? ": " : "",
                   error && error[0] ? error : "");
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                                     "material graph: shader compile task failed");
    }

    g_sc_task = nullptr;
    jce_async_task_release(task);
    delete j;
}

} /* anonymous namespace */

bool shader_compile_running(void) { return g_sc_task != nullptr; }

/* Kept for panel compatibility. Default-executor completions are pumped by
 * the engine before panel rendering. */
void shader_compile_poll(void)
{
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

    /* Both search orders live in jce_shadergraph_shaderc.cpp -- one copy,
     * which is also the one the Shader Inspector reads.  The char-buffer
     * wrappers that used to sit here were a second definition of the same
     * two names and the dedup audit said so. */
    /* 3. hand the two shaderc invocations to structured background work. */
    ShaderCompileJob *j = new ShaderCompileJob();
    j->vs_path     = jce_sg::resolve_vs_pbr_path();
    j->var_path    = jce_sg::resolve_varying_def_path();
    j->include_dir = include_dir;
    j->fs_sc_path  = cg.out_path;
    j->cg_out_path = cg.out_path;
    j->dir         = dir;
    j->base        = base;
    j->graph_path  = s_g.path;
    jce_mgp_note_generated_sc(cg.out_path.c_str());

    log_append(false, "compiling shaders in background…");
    jce_editor_console_log("material graph: compiling shaders in background…");

    JceAsyncTaskDesc desc;
    jce_async_task_desc_init(&desc);
    desc.work = shader_compile_worker;
    desc.complete = shader_compile_complete;
    desc.user_data = j;
    desc.debug_name = "editor.material_graph.compile";
    desc.priority = JCE_ASYNC_PRIORITY_BACKGROUND;

    g_sc_task = jce_async_submit(jce_async_default_executor(), &desc);
    if (!g_sc_task) {
        delete j;
        log_append(true, "could not queue shader compilation");
        jce_editor_console_log_level(
            JCE_CONSOLE_ERROR,
            "material graph: background queue rejected shader compile");
    }
}

} /* namespace jce_mgp */
