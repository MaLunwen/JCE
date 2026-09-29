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

/* Where the PBR vertex shader and the varying.def.sc every graph-generated
 * fragment links against live.  Search order, for both:
 *   1. $JCE_SHADER_VS_PBR / $JCE_SHADER_VARYING_DEF (explicit override)
 *   2. $JCE_SHADER_DEV_DIR/shaders/pbr/...
 *   3. the in-tree engine/shaders/pbr/... , relative to the cwd
 * Always returns a path -- the last candidate is unconditional, so the
 * caller reports "not found" against a name a person can go look for. */
std::string resolve_vs_pbr_path();
std::string resolve_varying_def_path();

/* Compile a .sc shader source to a backend-specific .bin blob.
 *
 *   sc_path          absolute or cwd-relative path to a .sc file.
 *   varying_def_path path to the matching varying.def.sc.
 *   include_dir      bgfx shader include directory (see
 *                    resolve_shader_include_dir()); pass "" to skip.
 *   kind             Vertex or Fragment.
 *
 * Blocks the caller (synchronous spawn + wait), but with a hard
 * timeout of 30 s.  Intended for editor UI button clicks only.
 *
 *   backend          which backend to compile FOR.  -1 (the default) means
 *                    the one the editor is running on.  A caller that
 *                    PERSISTS the blob should pass each backend in turn
 *                    instead: a bgfx blob is bytecode for exactly one of
 *                    them, so a file written for the editor's own backend
 *                    links on the machine that authored it and nowhere else.
 *                    That is half of why a Shader Graph material did not
 *                    survive the cook. */
/* How to invoke shaderc, when the defaults are not what the caller needs.
 *
 * THE DEFAULTS ARE THE GRAPH'S, and they are deliberate: -O 3, because the
 * question a graph shader raises is what the shipped shader costs.  The
 * ENGINE's own shaders are built by tools/compile_shaders.cmake with a
 * different pair of decisions -- no -O at all, and BGFX_CONFIG_MAX_BONES=128
 * -- and that difference is not cosmetic: fs_pbr compiles to 416168 bytes the
 * build's way and 203160 with -O, so a "reload" that used the graph's flags
 * would REPLACE every engine shader with a differently-compiled one and call
 * it a reload.
 *
 * So the flags are a parameter rather than a constant.  Two hard-coded lists
 * of compiler flags is the same defect as two lists of profiles: they agree
 * until one of them is edited. */
struct CompileOpts {
    /* Pass `-O <opt_level>` at all.  FALSE reproduces the engine build, which
     * passes no -O -- and that is not the same as -O 0.  Measured on fs_pbr,
     * s_5_0: no flag gives 416168 bytes and every level from -O 0 to -O 3
     * gives 203160, so the flag's presence changes what shaderc emits
     * independently of the level. */
    bool        pass_opt_flag = true;
    int         opt_level     = 0;   /* today's graph behaviour: -O 0 */
    /* Extra --define argument in NAME=VALUE form; empty means none. */
    std::string defines;
    /* An extra -i, searched BEFORE include_dir.
     *
     * Include resolution is part of what a compile IS, not a detail of it.
     * The engine build passes -i for its own shader directories; a sweep that
     * passed only the bgfx ABI dir resolved fs_pbr_body.sh to the COPY the SDK
     * installs rather than to the source being edited, and produced the
     * pre-edit binary while reporting a successful recompile. */
    std::string extra_include;
};

ShadercResult compile_sc(const std::string &sc_path,
                         const std::string &varying_def_path,
                         const std::string &include_dir,
                         ShaderKind         kind,
                         int                backend = -1,
                         const CompileOpts &opts = CompileOpts{});

/* Everything a person can be told about one compiled shader.
 *
 * THREE THINGS, and they come from three different places, which is why
 * this is one struct and not three calls:
 *   preprocessed  shaderc --preprocess: the .sc after includes and defines,
 *                 which is the source the compiler actually saw.  This is
 *                 the half that answers "is my #define reaching the file".
 *   blob          the compiled .bin.  Feed it to jce_shader_reflect() for
 *                 the uniform table, the attributes and the code section --
 *                 the editor does not parse the format itself, the engine
 *                 owns that.
 *   disasm        shaderc --disasm, DirectX only.  Empty on every other
 *                 backend, and `disasm_supported` says whether the emptiness
 *                 means "none produced" or "not available here".
 *
 * A stage that fails leaves its field empty and appends to `error`; the
 * others still run.  A missing disassembly must not cost you the uniform
 * table. */
struct ShaderInspectResult {
    bool                 ok = false;          /* the .bin compiled          */
    std::vector<uint8_t> blob;
    std::string          preprocessed;
    std::string          disasm;
    bool                 disasm_supported = false;
    std::string          error;
    std::string          shaderc_cmd;
};

/* Compile `sc_path` for `backend` and gather everything above.
 * Same blocking contract as compile_sc: editor button clicks only. */
ShaderInspectResult inspect_sc(const std::string &sc_path,
                               const std::string &varying_def_path,
                               const std::string &include_dir,
                               ShaderKind         kind,
                               int                backend);

/* The backends a persisted graph blob is compiled for, and the file suffix
 * each one is stored under -- the same suffixes the engine's own shaders use
 * (`fs_pbr_dx11.bin`, `_glsl`, `_spv`), because the runtime picks between
 * them with the same jce_shaders_backend_suffix(). */
struct GraphTarget { int backend; const char *suffix; };
const GraphTarget *graph_targets(int *out_count);

} /* namespace jce_sg */
