/*
 * jce_shadergraph_shaderc.h — Runtime shaderc invocation for the
 *                             editor's Material Graph "Compile & Bind".
 *
 * Spawns the bgfx `shaderc` executable via the engine's jce_process
 * wrapper (SDL3-backed) to turn a .sc source file into a backend-
 * specific .bin blob that can be fed into bgfx_create_shader.
 *
 * Backend (dx11 / spirv / glsl / metal / essl) is auto-selected from
 * the live bgfx renderer type — the editor compiles for whatever
 * backend the runtime is using right now.
 *
 * No bgfx dependency in this header; consumers receive raw bytes and
 * may pass them to jce_renderer_create_program_from_blobs.
 *
 * Layer: editor (depends on engine os/core + std::string/vector).
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jce_sg {

enum class ShaderKind {
    Vertex,
    Fragment,
};

struct ShadercResult {
    bool                 ok = false;
    std::vector<uint8_t> blob;          /* compiled .bin bytes (bgfx format) */
    std::string          error;         /* shaderc stderr on failure, or our own error */
    std::string          shaderc_cmd;   /* command line invoked (for the CompileLog) */
};

/* Resolve the shaderc executable path.  Search order:
 *   1. $JCE_SHADERC_EXECUTABLE
 *   2. $BGFX_SHADERC
 *   3. ./shaderc(.exe) next to the editor binary (cwd)
 * Returns empty string if none of the candidates exist on disk. */
std::string resolve_shaderc_path();

/* Resolve the bgfx shader include directory.  Search order:
 *   1. $JCE_SHADERC_INCLUDE_DIR
 *   2. $BGFX_SHADER_INCLUDE_PATH
 * Returns empty string if neither env var is set. */
std::string resolve_shader_include_dir();

/* Compile a .sc shader source to a backend-specific .bin blob.
 *
 *   sc_path          absolute or cwd-relative path to a .sc file.
 *   varying_def_path path to the matching varying.def.sc.
 *   include_dir      bgfx shader include directory (see
 *                    resolve_shader_include_dir()); pass "" to skip.
 *   kind             Vertex or Fragment.
 *
 * Blocks the caller (synchronous spawn + wait), but with a hard
 * timeout of 30 s.  Intended for editor UI button clicks only. */
ShadercResult compile_sc(const std::string &sc_path,
                         const std::string &varying_def_path,
                         const std::string &include_dir,
                         ShaderKind         kind);

} /* namespace jce_sg */
