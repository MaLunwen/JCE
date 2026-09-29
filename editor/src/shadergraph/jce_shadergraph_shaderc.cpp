/*
 * jce_shadergraph_shaderc.cpp — see header for contract.
 *
 * Implementation notes:
 *   - Uses jce_process (SDL3-backed) to spawn shaderc.exe.  We never
 *     touch CreateProcess / popen / system() — keeps Windows / macOS /
 *     Linux behaviour identical and unit-testable.
 *   - Output .bin is written to a unique temp path under the per-user
 *     cache dir `~/.jce/cache/shadergraph/`, then slurped back into memory and
 *     the temp file is deleted.  bgfx_copy() in the engine helper
 *     duplicates the bytes again, so the editor may free its vector
 *     immediately after handing the blob to the renderer.
 *   - We deliberately do *not* link to bgfx from the editor side; the
 *     blob is opaque bytes until the engine helper turns it into a
 *     program.
 */
#include "shadergraph/jce_shadergraph_shaderc.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>

#include "core/jce_editor_config.h"   /* jce_editor_dotjce_path (~/.jce) */

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jce_sg {

namespace {

constexpr size_t kStderrBufCap = 8192;
/* 30 s was calibrated for a graph shader that was ~100 lines and carried its
 * own two-term lighting.  A graph shader is now the engine's PBR fragment
 * body -- shadows, IBL, fog, clustered lights -- because that is what makes a
 * graph material match the scene it sits in, and fxc at -O 3 needs real time
 * for it.  MEASURED on this tree, one invocation each, no editor running:
 * dx11 17.3 s, glsl 3.3 s, essl 3.3 s, spv 0.6 s, metal 0.5 s.  Under the
 * editor (a renderer on the same machine) dx11 crossed 30 s and the compile
 * was reported as a failure.
 *
 * Raised to 180 s: about ten times the measured worst case, which is the
 * headroom a budget needs to be a runaway guard rather than a race.  This is
 * not "the number was too small" -- the thing being compiled changed size by
 * an order of magnitude, and the old number stopped describing it. */
constexpr int    kTimeoutMs    = 180000;

bool file_exists(const char *path)
{
    if (!path || !*path) return false;
    return jce_fs_host_exists_file(path);
}

std::string env_or_empty(const char *name)
{
    const char *v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string();
}

/* Backend → (--platform, --profile) — mirror tools/compile_shaders.cmake
 * lines 99-108.  vertex/fragment use the same platform+profile. */
struct ProfileMap {
    const char *platform;
    const char *profile;
};

ProfileMap profile_for_backend(JceRendererBackend b)
{
    switch (b) {
    case JCE_BACKEND_D3D11:    return { "windows", "s_5_0" };
    case JCE_BACKEND_D3D12:    return { "windows", "s_5_0" };
    case JCE_BACKEND_VULKAN:   return { "linux",   "spirv" };
    case JCE_BACKEND_OPENGL:   return { "linux",   "120"   };
        /* 120, not a newer number: bgfx rewrites the `#version` line
         * at load time, and this shaderc cannot emit 330 or 400 at all
         * (glsl-optimizer handles <=400 and stops at 1.50).  The GL
         * floor is the conan tier -- see check_gfx_api_tiers.py. */
    case JCE_BACKEND_OPENGLES: return { "android", "300_es" };
    case JCE_BACKEND_METAL:    return { "osx",     "metal" };
    default:                   return { "windows", "s_5_0" };
    }
}

std::string make_temp_bin_path()
{
    /* Editor UI thread only — no atomics needed. */
    static uint32_t s_counter = 0;
    static char     s_dir[1024] = {0};

    if (!s_dir[0]) {
        /* Per-user cache dir (~/.jce/cache/shadergraph), deterministic and
         * independent of CWD (a Finder/`open` double-click runs with
         * CWD=$HOME, which scattered the cache around). */
        jce_editor_dotjce_path("cache/shadergraph", s_dir, sizeof(s_dir));
        jce_fs_host_create_directory(s_dir);
    }

    uint32_t n  = s_counter++;
    uint64_t ms = jce_time_ticks_ms();

    char buf[1100];
    std::snprintf(buf, sizeof(buf),
                  "%s/sg_%llu_%u.bin",
                  s_dir, (unsigned long long)ms, n);
    return std::string(buf);
}

void quote_if_needed(std::string &out, const std::string &s)
{
    bool has_space = s.find(' ') != std::string::npos;
    if (has_space) out.push_back('"');
    out.append(s);
    if (has_space) out.push_back('"');
}

/* Spawn shaderc, drain stderr, wait with a hard timeout.
 *
 * Returns false only when the process could not be started or had to be
 * killed -- a non-zero EXIT is a successful run of a failing compile, and
 * the caller decides what that means.  stderr comes back either way,
 * because shaderc says why it refused there. */
bool run_shaderc(const std::string &exe, const std::string &args,
                 std::string *out_stderr, int *out_exit)
{
    JceProcessConfig cfg{};
    cfg.executable_path   = exe.c_str();
    cfg.working_directory = nullptr;
    cfg.arguments         = args.c_str();
    cfg.capture_stdout    = false;
    cfg.capture_stderr    = true;

    JceProcess *p = jce_process_spawn(&cfg);
    if (!p) return false;

    std::string buf;
    int      exit_code = -1;
    uint64_t t0        = jce_time_ticks_ms();
    char     chunk[1024];
    for (;;) {
        size_t n = jce_process_read_stderr(p, chunk, sizeof(chunk));
        if (n > 0) buf.append(chunk, n);

        if (jce_process_poll_exit(p, &exit_code)) break;

        uint64_t elapsed = jce_time_ticks_ms() - t0;
        if ((int64_t)elapsed > kTimeoutMs) {
            jce_process_force_kill(p);
            jce_process_destroy(p);
            if (out_stderr) *out_stderr = "shaderc timed out (see kTimeoutMs)";
            return false;
        }
        jce_thread_sleep_ms(5);
    }

    /* One final drain in case shaderc wrote to stderr just before exit. */
    for (;;) {
        size_t n = jce_process_read_stderr(p, chunk, sizeof(chunk));
        if (n == 0) break;
        buf.append(chunk, n);
        if (buf.size() > kStderrBufCap) break;
    }
    jce_process_destroy(p);

    if (out_stderr) *out_stderr = buf;
    if (out_exit)   *out_exit   = exit_code;
    return true;
}

} /* anonymous namespace */

std::string resolve_shaderc_path()
{
    std::string p = env_or_empty("JCE_SHADERC_EXECUTABLE");
    if (!p.empty() && file_exists(p.c_str())) return p;

    p = env_or_empty("BGFX_SHADERC");
    if (!p.empty() && file_exists(p.c_str())) return p;

    /* Convenience fallback — sit next to the editor binary.  Probe
     * both with and without an .exe suffix so we stay free of
     * platform macros (constraint #2). */
    if (file_exists("./shaderc.exe")) return "./shaderc.exe";
    if (file_exists("./shaderc"))     return "./shaderc";

    return std::string();
}

std::string resolve_vs_pbr_path()
{
    std::string p = env_or_empty("JCE_SHADER_VS_PBR");
    if (!p.empty()) return p;
    std::string dev = env_or_empty("JCE_SHADER_DEV_DIR");
    if (!dev.empty()) return dev + "/shaders/pbr/vs_pbr.sc";
    return "engine/shaders/pbr/vs_pbr.sc";
}

std::string resolve_varying_def_path()
{
    std::string p = env_or_empty("JCE_SHADER_VARYING_DEF");
    if (!p.empty()) return p;
    std::string dev = env_or_empty("JCE_SHADER_DEV_DIR");
    if (!dev.empty()) return dev + "/shaders/pbr/varying_pbr.def.sc";
    return "engine/shaders/pbr/varying_pbr.def.sc";
}

std::string resolve_shader_include_dir()
{
    std::string p = env_or_empty("JCE_SHADERC_INCLUDE_DIR");
    if (!p.empty()) return p;
    return env_or_empty("BGFX_SHADER_INCLUDE_PATH");
}

/* The persisted set.  essl1 is deliberately absent: it is the ES 2.0 profile
 * the engine's own build restricts to a hand-vetted basic subset (no
 * derivatives, no MRT, no instancing), and a graph shader is exactly the kind
 * that would not compile for it.  A missing variant is a link failure on that
 * backend, which is honest; a silently wrong one would not be. */
static const GraphTarget kGraphTargets[] = {
    { (int)JCE_BACKEND_D3D11,    "dx11" },
    { (int)JCE_BACKEND_VULKAN,   "spv"  },
    { (int)JCE_BACKEND_OPENGL,   "glsl" },
    { (int)JCE_BACKEND_OPENGLES, "essl" },
    { (int)JCE_BACKEND_METAL,    "mtl"  },
};

const GraphTarget *graph_targets(int *out_count)
{
    if (out_count) *out_count = (int)(sizeof kGraphTargets / sizeof kGraphTargets[0]);
    return kGraphTargets;
}

ShadercResult compile_sc(const std::string &sc_path,
                         const std::string &varying_def_path,
                         const std::string &include_dir,
                         ShaderKind         kind,
                         int                want_backend,
                         const CompileOpts &opts)
{
    ShadercResult r;

    std::string shaderc = resolve_shaderc_path();
    if (shaderc.empty()) {
        r.error = "shaderc executable not found. Set JCE_SHADERC_EXECUTABLE "
                  "or BGFX_SHADERC to the path of shaderc(.exe).";
        return r;
    }
    if (!file_exists(sc_path.c_str())) {
        r.error = "shader source not found: " + sc_path;
        return r;
    }

    /* -1 = whatever the editor is running on; anything else is an explicit
     * target, which is what the persisting caller passes. */
    JceRendererBackend backend = (want_backend < 0)
        ? jce_renderer_get_backend(nullptr)
        : (JceRendererBackend)want_backend;
    ProfileMap pm = profile_for_backend(backend);

    std::string bin_path = make_temp_bin_path();

    /* Build argument string honoured by jce_process_spawn (quote-aware
     * tokenisation, no shell expansion). */
    std::string args;
    args += "-f ";   quote_if_needed(args, sc_path);
    args += " -o ";  quote_if_needed(args, bin_path);
    args += " --type ";
    args += (kind == ShaderKind::Vertex) ? "vertex" : "fragment";
    args += " --platform ";
    args += pm.platform;
    args += " -p ";
    args += pm.profile;
    if (!varying_def_path.empty()) {
        args += " --varyingdef ";
        quote_if_needed(args, varying_def_path);
    }
    /* The caller's own dirs FIRST: a stale copy of the same file name in a
     * shared include dir must not win over the source being compiled. */
    if (!opts.extra_include.empty()) {
        args += " -i ";
        quote_if_needed(args, opts.extra_include);
    }
    if (!include_dir.empty()) {
        args += " -i ";
        quote_if_needed(args, include_dir);
    }
    /* -O 0 keeps editor iteration fast.
     *
     * The line here used to say "release shipping shaders go through the
     * build-time CMake path with full optimisation".  Measured, that is not
     * what the build does: tools/compile_shaders.cmake passes NO -O at all,
     * and for HLSL that is a different output again -- fs_pbr is 416168 bytes
     * with no flag and 203160 with any level from 0 to 3.  Two things follow.
     * The engine's own D3D shaders are not compiled the way that comment
     * claimed, and the per-backend blobs THIS function persists for a graph
     * material do ship, at -O 0, which is a real cost and is now recorded as
     * its own parity row rather than hidden behind a sentence.
     *
     * The flags are a parameter so a caller that must reproduce the engine
     * build can, byte for byte, instead of approximating it. */
    if (opts.pass_opt_flag) {
        char ob[16];
        std::snprintf(ob, sizeof(ob), " -O %d", opts.opt_level);
        args += ob;
    }
    if (!opts.defines.empty()) {
        args += " --define ";
        quote_if_needed(args, opts.defines);
    }

    r.shaderc_cmd = shaderc + " " + args;

    int         exit_code  = -1;
    std::string stderr_buf;
    if (!run_shaderc(shaderc, args, &stderr_buf, &exit_code)) {
        r.error = stderr_buf.empty()
                    ? ("failed to spawn shaderc: " + shaderc)
                    : stderr_buf;
        std::remove(bin_path.c_str());
        return r;
    }

    if (exit_code != 0) {
        r.error = stderr_buf.empty()
                    ? std::string("shaderc exited with code ")
                          + std::to_string(exit_code)
                    : stderr_buf;
        std::remove(bin_path.c_str());
        return r;
    }

    /* Slurp the .bin into memory, then delete the temp file. */
    uint64_t bin_size = 0;
    void    *bin_data = jce_fs_host_read_all(bin_path.c_str(), &bin_size);
    std::remove(bin_path.c_str());
    if (!bin_data || bin_size == 0) {
        if (bin_data) jce_fs_buffer_free(bin_data);
        r.error = "shaderc reported success but output blob is empty: "
                + bin_path;
        return r;
    }

    r.blob.assign((const uint8_t *)bin_data,
                  (const uint8_t *)bin_data + (size_t)bin_size);
    jce_fs_buffer_free(bin_data);
    r.ok = true;
    /* Non-fatal warnings still propagate to UI through `error` if
     * future callers want to surface them; ok=true tells them blob is
     * valid.  For now we ignore stderr on success. */
    return r;
}

/* -- inspection ----------------------------------------------------- */

ShaderInspectResult inspect_sc(const std::string &sc_path,
                               const std::string &varying_def_path,
                               const std::string &include_dir,
                               ShaderKind         kind,
                               int                want_backend)
{
    ShaderInspectResult r;

    std::string shaderc = resolve_shaderc_path();
    if (shaderc.empty()) {
        r.error = "shaderc executable not found. Set JCE_SHADERC_EXECUTABLE "
                  "or BGFX_SHADERC to the path of shaderc(.exe).";
        return r;
    }
    if (!file_exists(sc_path.c_str())) {
        r.error = "shader source not found: " + sc_path;
        return r;
    }

    JceRendererBackend backend = (want_backend < 0)
        ? jce_renderer_get_backend(nullptr)
        : (JceRendererBackend)want_backend;
    ProfileMap pm = profile_for_backend(backend);

    /* --disasm is a DirectX-only option in bgfx's shaderc: on any other
     * profile it is accepted and produces nothing.  Deciding here, from the
     * profile, is what lets the panel say "not available on this backend"
     * instead of showing an empty box that reads like a failure. */
    r.disasm_supported = (pm.profile[0] == 's' && pm.profile[1] == '_');

    /* Common head of both invocations. */
    std::string common;
    common += "-f ";  quote_if_needed(common, sc_path);
    common += " --type ";
    common += (kind == ShaderKind::Vertex) ? "vertex" : "fragment";
    common += " --platform ";
    common += pm.platform;
    common += " -p ";
    common += pm.profile;
    if (!varying_def_path.empty()) {
        common += " --varyingdef ";
        quote_if_needed(common, varying_def_path);
    }
    if (!include_dir.empty()) {
        common += " -i ";
        quote_if_needed(common, include_dir);
    }

    /* 1. THE COMPILE.  -O 3 and not the editor's usual -O 0: the whole
     *    question this panel answers is what the shipped shader costs, and
     *    an unoptimised listing answers a question nobody asked. */
    std::string bin_path = make_temp_bin_path();
    std::string args = common;
    args += " -o ";  quote_if_needed(args, bin_path);
    args += " -O 3";
    if (r.disasm_supported) args += " --disasm";

    r.shaderc_cmd = shaderc + " " + args;

    int         exit_code = -1;
    std::string err;
    if (!run_shaderc(shaderc, args, &err, &exit_code)) {
        r.error = err.empty() ? ("failed to spawn shaderc: " + shaderc) : err;
        std::remove(bin_path.c_str());
        return r;
    }
    if (exit_code != 0) {
        r.error = err.empty()
                    ? std::string("shaderc exited with code ")
                          + std::to_string(exit_code)
                    : err;
        std::remove(bin_path.c_str());
        return r;
    }

    uint64_t bin_size = 0;
    void    *bin_data = jce_fs_host_read_all(bin_path.c_str(), &bin_size);
    std::remove(bin_path.c_str());
    if (bin_data && bin_size > 0) {
        r.blob.assign((const uint8_t *)bin_data,
                      (const uint8_t *)bin_data + (size_t)bin_size);
        r.ok = true;
    } else {
        r.error = "shaderc reported success but wrote no blob";
    }
    if (bin_data) jce_fs_buffer_free(bin_data);

    /* shaderc writes the listing beside the output, as <out>.disasm -- NOT
     * to stdout, which is where a reasonable person looks first.  Verified
     * on this toolchain rather than assumed. */
    if (r.disasm_supported) {
        std::string dis_path = bin_path + ".disasm";
        uint64_t    dsz      = 0;
        void       *ddata    = jce_fs_host_read_all(dis_path.c_str(), &dsz);
        std::remove(dis_path.c_str());
        if (ddata && dsz > 0)
            r.disasm.assign((const char *)ddata, (size_t)dsz);
        if (ddata) jce_fs_buffer_free(ddata);
    }

    /* 2. THE PREPROCESSED SOURCE.  A separate run because --preprocess
     *    replaces the compile rather than accompanying it.  Its failure is
     *    reported but does not sink the result: the uniform table is the
     *    more valuable half and it is already in hand. */
    {
        std::string pp_path = make_temp_bin_path();
        std::string pargs   = common;
        pargs += " -o ";  quote_if_needed(pargs, pp_path);
        pargs += " --preprocess";

        int pexit = -1;
        std::string perr;
        if (run_shaderc(shaderc, pargs, &perr, &pexit) && pexit == 0) {
            uint64_t psz  = 0;
            void    *pdat = jce_fs_host_read_all(pp_path.c_str(), &psz);
            if (pdat && psz > 0)
                r.preprocessed.assign((const char *)pdat, (size_t)psz);
            if (pdat) jce_fs_buffer_free(pdat);
        } else if (!perr.empty()) {
            if (!r.error.empty()) r.error += "\n";
            r.error += "--preprocess: " + perr;
        }
        std::remove(pp_path.c_str());
    }

    return r;
}

} /* namespace jce_sg */
