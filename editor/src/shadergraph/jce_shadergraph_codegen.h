/*
 * jce_shadergraph_codegen.h — Graph -> BGFX .sc source emission.
 *
 * Takes a JceShaderGraph, runs topological sort + type checking, walks
 * the graph in dependency order and emits GLSL that assigns the five
 * material output locals (mat_base_color / mat_normal_ts / mat_metallic
 * / mat_roughness / mat_emissive) consumed by the codegen template
 * (engine/shaders/graph/fs_graph_template.sc).
 *
 * The substitution region in the template is delimited by:
 *   /JCE_BEGIN_MATERIAL/   ...replaced text...   /JCE_END_MATERIAL/
 *
 * No bgfx / ImGui dependency; safe to use from headless tests.
 */

#pragma once

#include "shadergraph/jce_shadergraph_types.h"

#include <string>
#include <vector>

namespace jce_sg {

struct CodegenDiag {
    int         node_id;   /* -1 if not node-scoped */
    std::string message;
};

struct CodegenResult {
    bool        ok = false;       /* true iff source was generated */
    std::string source;           /* full .sc text (template + body substitution) */
    std::string out_path;         /* path written, empty if out_dir was null */
    std::vector<CodegenDiag> errors;
    std::vector<CodegenDiag> warnings;
};

/* Generate a fragment shader from `g`.
 *
 *   template_path : path to fs_graph_template.sc (must exist).
 *   out_basename  : basename without `fs_` prefix or `.sc` extension.
 *                   The written file is `<out_dir>/fs_<basename>.sc`.
 *   out_dir       : output directory; if null/empty, source is only
 *                   returned in `result.source` (not written to disk).
 */
CodegenResult codegen(const Graph &g,
                      const char *template_path,
                      const char *out_basename,
                      const char *out_dir);

} /* namespace jce_sg */
