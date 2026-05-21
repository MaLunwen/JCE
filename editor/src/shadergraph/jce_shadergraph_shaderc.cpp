/*
 * jce_shadergraph_shaderc.cpp — see header for contract.
 *
 * Implementation notes:
 *   - Uses jce_process (SDL3-backed) to spawn shaderc.exe.  We never
 *     touch CreateProcess / popen / system() — keeps Windows / macOS /
 *     Linux behaviour identical and unit-testable.
 *   - Output .bin is written to a unique temp path under
 *     `<cwd>/.cache/shadergraph/`, then slurped back into memory and
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

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jce_sg {

namespace {

constexpr size_t kStderrBufCap = 8192;
constexpr int    kTimeoutMs    = 30000;

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
    case JCE_BACKEND_OPENGLES: return { "android", "300_es" };
    case JCE_BACKEND_METAL:    return { "osx",     "metal" };
    default:                   return { "windows", "s_5_0" };
    }
}

std::string make_temp_bin_path()
{
    /* Editor UI thread only — no atomics needed. */
    static uint32_t s_counter   = 0;
    static bool     s_dir_ready = false;

    if (!s_dir_ready) {
        jce_fs_host_create_directory(".cache/shadergraph");
        s_dir_ready = true;
    }

    uint32_t n  = s_counter++;
    uint64_t ms = jce_time_ticks_ms();

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  ".cache/shadergraph/sg_%llu_%u.bin",
                  (unsigned long long)ms, n);
    return std::string(buf);
}

void quote_if_needed(std::string &out, const std::string &s)
{
    bool has_space = s.find(' ') != std::string::npos;
    if (has_space) out.push_back('"');
    out.append(s);
    if (has_space) out.push_back('"');
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

std::string resolve_shader_include_dir()
{
    std::string p = env_or_empty("JCE_SHADERC_INCLUDE_DIR");
    if (!p.empty()) return p;
    return env_or_empty("BGFX_SHADER_INCLUDE_PATH");
}

ShadercResult compile_sc(const std::string &sc_path,
                         const std::string &varying_def_path,
                         const std::string &include_dir,
                         ShaderKind         kind)
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

    JceRendererBackend backend = jce_renderer_get_backend(nullptr);
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
    if (!include_dir.empty()) {
        args += " -i ";
        quote_if_needed(args, include_dir);
    }
    /* O0 keeps editor iteration fast — release shipping shaders go
       through the build-time CMake path with full optimisation. */
    args += " -O 0";

    r.shaderc_cmd = shaderc + " " + args;

    JceProcessConfig cfg{};
    cfg.executable_path  = shaderc.c_str();
    cfg.working_directory = nullptr;
    cfg.arguments        = args.c_str();
    cfg.capture_stdout   = false;
    cfg.capture_stderr   = true;

    JceProcess *p = jce_process_spawn(&cfg);
    if (!p) {
        r.error = "failed to spawn shaderc: " + shaderc;
        return r;
    }

    /* Drain stderr while polling exit, with a hard timeout. */
    std::string stderr_buf;
    int      exit_code = -1;
    uint64_t t0        = jce_time_ticks_ms();
    char     chunk[1024];
    for (;;) {
        size_t n = jce_process_read_stderr(p, chunk, sizeof(chunk));
        if (n > 0) stderr_buf.append(chunk, n);

        if (jce_process_poll_exit(p, &exit_code)) break;

        uint64_t elapsed = jce_time_ticks_ms() - t0;
        if ((int64_t)elapsed > kTimeoutMs) {
            jce_process_force_kill(p);
            jce_process_destroy(p);
            r.error = "shaderc timed out after 30s";
            std::remove(bin_path.c_str());
            return r;
        }
        jce_thread_sleep_ms(5);
    }

    /* One final drain in case shaderc wrote to stderr just before exit. */
    for (;;) {
        size_t n = jce_process_read_stderr(p, chunk, sizeof(chunk));
        if (n == 0) break;
        stderr_buf.append(chunk, n);
        if (stderr_buf.size() > kStderrBufCap) break;
    }
    jce_process_destroy(p);

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

} /* namespace jce_sg */
