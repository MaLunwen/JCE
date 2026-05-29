/*
 * jce_toolchain.c  Build-toolchain discovery.
 *
 * Detection strategy per kind:
 *   cmake / ninja / gcc / clang  PATH search (+ optional `--version`)
 *   msvc                         vswhere.exe in standard location
 *   ndk                          ANDROID_NDK_HOME / ANDROID_NDK_ROOT /
 *                                ANDROID_HOME\ndk\* / %LOCALAPPDATA%
 *   emsdk                        EMSDK env var
 *   xcode                        xcode-select -p (macOS only)
 *
 * Subprocess use is bounded: at most one short `--version` invocation
 * per detected tool, with a 2-second timeout.  Detection is intended
 * to run at editor startup and after the user edits the Toolchains
 * panel; both are cold paths so a few hundred ms is acceptable.
 *
 * No globals besides the toolchain table itself.  Not thread-safe.
 */

#include <jce/os/core/jce_toolchain.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_log.h>

#include <SDL3/SDL.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "toolchain"

#if defined(_WIN32)
    #define TC_PATH_SEP        ';'
    #define TC_EXE_SUFFIX      ".exe"
    #define TC_NATIVE_DIR_SEP  '\\'
#else
    #define TC_PATH_SEP        ':'
    #define TC_EXE_SUFFIX      ""
    #define TC_NATIVE_DIR_SEP  '/'
#endif

/* ------------------------------------------------------------------ */
/*  Tables                                                            */
/* ------------------------------------------------------------------ */

static const char *const k_kind_names[JCE_TOOLCHAIN_COUNT] = {
    "cmake", "ninja", "msvc", "gcc", "clang", "ndk", "emsdk", "xcode"
};

static JceToolchain s_state[JCE_TOOLCHAIN_COUNT];
static char         s_override_path[JCE_TOOLCHAIN_COUNT][JCE_TOOLCHAIN_PATH_MAX];
static bool         s_has_override[JCE_TOOLCHAIN_COUNT];
static bool         s_initialised;

const char *jce_toolchain_kind_name(JceToolchainKind k)
{
    if ((int)k < 0 || (int)k >= JCE_TOOLCHAIN_COUNT) return "?";
    return k_kind_names[k];
}

bool jce_toolchain_kind_from_name(const char *name, JceToolchainKind *out_kind)
{
    if (!name || !out_kind) return false;
    for (int i = 0; i < JCE_TOOLCHAIN_COUNT; ++i) {
        if (SDL_strcasecmp(name, k_kind_names[i]) == 0) {
            *out_kind = (JceToolchainKind)i;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Generic helpers                                                   */
/* ------------------------------------------------------------------ */

static void str_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    SDL_strlcpy(dst, src, cap);
}

static const char *tc_getenv(const char *name)
{
    const char *v = SDL_getenv(name);
    if (v && v[0]) return v;
    return NULL;
}

/* Search $PATH for `exe_base` (with platform-appropriate suffix).
 * On success writes the parent directory (i.e. the install root) into
 * `out` and returns true. */
static bool tc_find_on_path(const char *exe_base,
                            char *out, size_t out_cap)
{
    if (!exe_base || !out || out_cap == 0) return false;
    const char *path = tc_getenv("PATH");
    if (!path) return false;

    char exe_name[256];
    SDL_snprintf(exe_name, sizeof exe_name, "%s%s", exe_base, TC_EXE_SUFFIX);

    char dir[JCE_TOOLCHAIN_PATH_MAX];
    char cand[JCE_TOOLCHAIN_PATH_MAX];
    const char *p = path;
    while (*p) {
        const char *sep = strchr(p, TC_PATH_SEP);
        size_t n = sep ? (size_t)(sep - p) : strlen(p);
        if (n > 0 && n < sizeof dir) {
            memcpy(dir, p, n);
            dir[n] = 0;
            if (jce_path_join(cand, sizeof cand, dir, exe_name)
                && jce_fs_host_exists_file(cand)) {
                str_copy(out, out_cap, dir);
                return true;
            }
        }
        if (!sep) break;
        p = sep + 1;
    }
    return false;
}

/* Run `exe arg` with stdout captured, wait for completion, and return
 * the first non-empty line in `out` (truncated to cap).  Returns false
 * on spawn failure.  Empty arg means "no args".  `timeout_ms` is a
 * cooperative cap — children that take longer get force-killed. */
static bool tc_capture_first_line(const char *exe, const char *arg,
                                  int timeout_ms,
                                  char *out, size_t out_cap)
{
    if (!exe || !out || out_cap == 0) return false;
    out[0] = 0;

    JceProcessConfig cfg = {0};
    cfg.executable_path  = exe;
    cfg.arguments        = (arg && arg[0]) ? arg : NULL;
    cfg.capture_stdout   = true;
    cfg.capture_stderr   = false;

    JceProcess *p = jce_process_spawn(&cfg);
    if (!p) return false;

    char buf[1024];
    size_t total   = 0;
    const Uint64 t0 = SDL_GetTicks();
    const Uint64 deadline = t0 + (timeout_ms > 0 ? (Uint64)timeout_ms : 2000);
    int exit_code = 0;
    bool exited   = false;
    while (!exited) {
        if (total < sizeof buf - 1) {
            size_t got = jce_process_read_stdout(p, buf + total,
                                                 sizeof buf - 1 - total);
            total += got;
        }
        if (jce_process_poll_exit(p, &exit_code)) {
            exited = true;
            break;
        }
        if (SDL_GetTicks() >= deadline) {
            jce_process_force_kill(p);
            break;
        }
        SDL_Delay(5);
    }
    /* Drain any tail bytes left in the stdout pipe after exit. */
    if (exited && total < sizeof buf - 1) {
        for (;;) {
            size_t got = jce_process_read_stdout(p, buf + total,
                                                 sizeof buf - 1 - total);
            if (got == 0) break;
            total += got;
            if (total >= sizeof buf - 1) break;
        }
    }
    jce_process_destroy(p);
    buf[total] = 0;
    (void)exit_code;

    if (total == 0) return false;
    /* First non-empty line. */
    char *line = buf;
    while (*line) {
        char *nl = strpbrk(line, "\r\n");
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        if (llen > 0) {
            if (llen >= out_cap) llen = out_cap - 1;
            memcpy(out, line, llen);
            out[llen] = 0;
            return true;
        }
        if (!nl) break;
        line = nl + 1;
    }
    return true;
}

/* Extract a "1.2.3"-style version token from an arbitrary line.  Picks
 * the longest run of digits+dots.  Writes "" on failure. */
static void tc_extract_version(const char *line, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return;
    out[0] = 0;
    if (!line) return;

    const char *best_s = NULL;
    size_t      best_n = 0;
    const char *p = line;
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            const char *s = p;
            while (*p && (isdigit((unsigned char)*p) || *p == '.')) ++p;
            size_t n = (size_t)(p - s);
            if (n > best_n) { best_s = s; best_n = n; }
        } else {
            ++p;
        }
    }
    if (best_s && best_n > 0) {
        if (best_n >= out_cap) best_n = out_cap - 1;
        memcpy(out, best_s, best_n);
        out[best_n] = 0;
    }
}

/* Read the first ~1KB of a small text file. */
static bool tc_read_small_file(const char *path, char *out, size_t out_cap)
{
    if (!path || !out || out_cap == 0) return false;
    out[0] = 0;
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return false;
    size_t n = SDL_ReadIO(io, out, out_cap - 1);
    SDL_CloseIO(io);
    if (n == 0) return false;
    out[n] = 0;
    return true;
}

/* Find "key = value" in a Java-properties-style buffer. */
static bool tc_props_get(const char *buf, const char *key,
                         char *out, size_t out_cap)
{
    if (!buf || !key || !out || out_cap == 0) return false;
    out[0] = 0;
    size_t klen = strlen(key);
    const char *line = buf;
    while (*line) {
        const char *nl = strpbrk(line, "\r\n");
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        const char *p = line;
        while (p < line + llen && isspace((unsigned char)*p)) ++p;
        if ((size_t)((line + llen) - p) > klen
            && strncmp(p, key, klen) == 0) {
            p += klen;
            while (p < line + llen && isspace((unsigned char)*p)) ++p;
            if (p < line + llen && *p == '=') {
                ++p;
                while (p < line + llen && isspace((unsigned char)*p)) ++p;
                size_t vlen = (size_t)((line + llen) - p);
                if (vlen >= out_cap) vlen = out_cap - 1;
                memcpy(out, p, vlen);
                out[vlen] = 0;
                /* trim trailing whitespace */
                while (vlen > 0 && isspace((unsigned char)out[vlen - 1])) {
                    out[--vlen] = 0;
                }
                return vlen > 0;
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Per-kind probes                                                   */
/* ------------------------------------------------------------------ */

/* Try `<dir>/<exe_base><exe_suffix>`; on hit, populate out->path with
 * `dir` and capture the version line.  Returns true on hit. */
static bool try_install_dir(JceToolchain *out, const char *dir,
                            const char *exe_base)
{
    if (!dir || !dir[0] || !exe_base) return false;
    char exe[JCE_TOOLCHAIN_PATH_MAX];
    SDL_snprintf(exe, sizeof exe, "%s%c%s%s",
                 dir, TC_NATIVE_DIR_SEP, exe_base, TC_EXE_SUFFIX);
    if (!jce_fs_host_exists_file(exe)) return false;

    str_copy(out->path, sizeof out->path, dir);
    char first[256];
    if (tc_capture_first_line(exe, "--version", 2000, first, sizeof first)) {
        tc_extract_version(first, out->version, sizeof out->version);
    }
    out->present = true;
    return true;
}

/* Per-kind list of common install directories to fall back on when the
 * tool is not on $PATH.  Env-var refs of the form "$NAME/sub" are
 * expanded; the dir is skipped if the env var is unset/empty. */
static const char *const k_cmake_dirs[] = {
#if defined(_WIN32)
    "$ProgramFiles\\CMake\\bin",
    "$ProgramFiles(x86)\\CMake\\bin",
    "$LOCALAPPDATA\\Programs\\CMake\\bin",
    "$CMAKE_ROOT\\bin",
#else
    "/usr/local/bin",
    "/usr/bin",
    "/opt/cmake/bin",
    "/snap/bin",
#endif
    NULL,
};

static const char *const k_ninja_dirs[] = {
#if defined(_WIN32)
    "$ProgramFiles\\Ninja",
    "$LOCALAPPDATA\\Programs\\Ninja",
#else
    "/usr/local/bin",
    "/usr/bin",
#endif
    NULL,
};

static const char *const k_clang_dirs[] = {
#if defined(_WIN32)
    "$ProgramFiles\\LLVM\\bin",
    "$ProgramFiles(x86)\\LLVM\\bin",
    "C:\\msys64\\mingw64\\bin",
    "C:\\msys64\\clang64\\bin",
#else
    "/usr/local/bin",
    "/usr/bin",
    "/opt/llvm/bin",
    "/opt/homebrew/opt/llvm/bin",
#endif
    NULL,
};

static const char *const k_gcc_dirs[] = {
#if defined(_WIN32)
    "C:\\msys64\\mingw64\\bin",
    "C:\\msys64\\ucrt64\\bin",
    "$MINGW_HOME\\bin",
    "C:\\MinGW\\bin",
#else
    "/usr/local/bin",
    "/usr/bin",
#endif
    NULL,
};

/* Expand $NAME / $NAME(x86) prefix into the env value.  Writes the
 * substituted path into `out`.  Returns false if the env var is
 * required but unset/empty. */
static bool expand_env_dir(const char *pattern, char *out, size_t cap)
{
    if (!pattern || !pattern[0]) return false;
    if (pattern[0] != '$') {
        str_copy(out, cap, pattern);
        return true;
    }
    const char *p = pattern + 1;
    char name[64];
    size_t n = 0;
    /* env-var name may contain ( ) (e.g. "ProgramFiles(x86)"). */
    while (*p && *p != '\\' && *p != '/' && n + 1 < sizeof name) {
        name[n++] = *p++;
    }
    name[n] = 0;
    const char *val = tc_getenv(name);
    if (!val || !val[0]) return false;
    SDL_snprintf(out, cap, "%s%s", val, p);
    return true;
}

static void probe_with_fallbacks(JceToolchain *out, const char *exe_base,
                                 const char *const *dirs)
{
    /* 1. $PATH first — most common case. */
    char dir[JCE_TOOLCHAIN_PATH_MAX];
    if (tc_find_on_path(exe_base, dir, sizeof dir)) {
        if (try_install_dir(out, dir, exe_base)) return;
    }
    /* 2. Well-known install locations. */
    if (!dirs) return;
    for (int i = 0; dirs[i]; ++i) {
        if (!expand_env_dir(dirs[i], dir, sizeof dir)) continue;
        if (try_install_dir(out, dir, exe_base)) return;
    }
}

static void probe_cmake(JceToolchain *out) { probe_with_fallbacks(out, "cmake", k_cmake_dirs); }
static void probe_ninja(JceToolchain *out) { probe_with_fallbacks(out, "ninja", k_ninja_dirs); }
static void probe_gcc  (JceToolchain *out) { probe_with_fallbacks(out, "gcc",   k_gcc_dirs);   }
static void probe_clang(JceToolchain *out) { probe_with_fallbacks(out, "clang", k_clang_dirs); }

static void probe_msvc(JceToolchain *out)
{
#if defined(_WIN32)
    /* Standard installer path for vswhere — shipped with every VS
     * 2017+ install, even Build Tools. */
    const char *pf86 = tc_getenv("ProgramFiles(x86)");
    if (!pf86) pf86 = "C:\\Program Files (x86)";

    char vswhere[JCE_TOOLCHAIN_PATH_MAX];
    SDL_snprintf(vswhere, sizeof vswhere,
                 "%s\\Microsoft Visual Studio\\Installer\\vswhere.exe", pf86);
    if (!jce_fs_host_exists_file(vswhere)) return;

    char line[JCE_TOOLCHAIN_PATH_MAX];
    if (!tc_capture_first_line(
            vswhere,
            "-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath",
            3000, line, sizeof line)) {
        return;
    }
    if (!line[0]) return;

    str_copy(out->path, sizeof out->path, line);

    /* Read tools version too — best effort. */
    char ver_line[128];
    if (tc_capture_first_line(
            vswhere,
            "-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property catalog_productDisplayVersion",
            3000, ver_line, sizeof ver_line)) {
        tc_extract_version(ver_line, out->version, sizeof out->version);
    }
    out->present = true;
#else
    (void)out;
#endif
}

/* Try a single NDK root candidate.  Returns true and populates `out`
 * if the candidate looks like a real NDK install. */
static bool try_ndk_root(JceToolchain *out, const char *root)
{
    if (!root || !root[0]) return false;
    char marker[JCE_TOOLCHAIN_PATH_MAX];
    /* source.properties is present in every NDK r10e+. */
    SDL_snprintf(marker, sizeof marker, "%s%csource.properties",
                 root, TC_NATIVE_DIR_SEP);
    if (!jce_fs_host_exists_file(marker)) return false;

    str_copy(out->path, sizeof out->path, root);

    char buf[2048];
    if (tc_read_small_file(marker, buf, sizeof buf)) {
        tc_props_get(buf, "Pkg.Revision", out->version, sizeof out->version);
    }
    out->present = true;
    return true;
}

static void probe_ndk(JceToolchain *out)
{
    const char *env_keys[] = {
        "ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "NDK_HOME", "NDK_ROOT"
    };
    for (size_t i = 0; i < sizeof env_keys / sizeof env_keys[0]; ++i) {
        const char *v = tc_getenv(env_keys[i]);
        if (v && try_ndk_root(out, v)) return;
    }

    /* ANDROID_HOME/ndk/<version> — pick the highest-versioned dir. */
    const char *sdk = tc_getenv("ANDROID_HOME");
    if (!sdk) sdk = tc_getenv("ANDROID_SDK_ROOT");
    if (sdk) {
        char ndk_dir[JCE_TOOLCHAIN_PATH_MAX];
        SDL_snprintf(ndk_dir, sizeof ndk_dir, "%s%cndk",
                     sdk, TC_NATIVE_DIR_SEP);
        if (jce_fs_host_exists_dir(ndk_dir)) {
            char best_name[128] = {0};
            char best_path[JCE_TOOLCHAIN_PATH_MAX] = {0};
            char **list = SDL_GlobDirectory(ndk_dir, NULL,
                                            SDL_GLOB_CASEINSENSITIVE, NULL);
            if (list) {
                for (int i = 0; list[i]; ++i) {
                    /* Lexicographic max works fine for "26.2.x" etc. */
                    if (strcmp(list[i], best_name) > 0) {
                        SDL_strlcpy(best_name, list[i], sizeof best_name);
                        SDL_snprintf(best_path, sizeof best_path, "%s%c%s",
                                     ndk_dir, TC_NATIVE_DIR_SEP, list[i]);
                    }
                }
                SDL_free(list);
            }
            if (best_path[0] && try_ndk_root(out, best_path)) return;
        }
    }
}

static void probe_emsdk(JceToolchain *out)
{
    const char *root = tc_getenv("EMSDK");
    if (!root) return;
    char marker[JCE_TOOLCHAIN_PATH_MAX];
    SDL_snprintf(marker, sizeof marker, "%s%cemsdk%s",
                 root, TC_NATIVE_DIR_SEP, TC_EXE_SUFFIX[0] ? ".bat" : "");
    /* Fall back to the python entry point. */
    if (!jce_fs_host_exists_file(marker)) {
        SDL_snprintf(marker, sizeof marker, "%s%cemsdk.py",
                     root, TC_NATIVE_DIR_SEP);
        if (!jce_fs_host_exists_file(marker)) return;
    }
    str_copy(out->path, sizeof out->path, root);

    /* Version: prefer EMSDK_PYTHON path's neighbouring version file,
     * else fall back to the .emscripten user config which records the
     * active sdk. */
    char rel[JCE_TOOLCHAIN_PATH_MAX];
    SDL_snprintf(rel, sizeof rel, "%s%cemscripten-releases-tags.json",
                 root, TC_NATIVE_DIR_SEP);
    char buf[2048];
    if (tc_read_small_file(rel, buf, sizeof buf)) {
        const char *latest = strstr(buf, "\"latest\"");
        if (latest) {
            const char *q1 = strchr(latest + 8, '"');
            if (q1) {
                const char *q2 = strchr(q1 + 1, '"');
                if (q2 && q2 > q1 + 1) {
                    size_t n = (size_t)(q2 - q1 - 1);
                    if (n >= sizeof out->version) n = sizeof out->version - 1;
                    memcpy(out->version, q1 + 1, n);
                    out->version[n] = 0;
                }
            }
        }
    }
    out->present = true;
}

static void probe_xcode(JceToolchain *out)
{
#if defined(__APPLE__)
    char line[JCE_TOOLCHAIN_PATH_MAX];
    if (!tc_capture_first_line("/usr/bin/xcode-select", "-p",
                               2000, line, sizeof line)) {
        return;
    }
    if (!line[0] || !jce_fs_host_exists_dir(line)) return;
    str_copy(out->path, sizeof out->path, line);

    char ver[128];
    if (tc_capture_first_line("/usr/bin/xcodebuild", "-version",
                              2000, ver, sizeof ver)) {
        tc_extract_version(ver, out->version, sizeof out->version);
    }
    out->present = true;
#else
    (void)out;
#endif
}

/* ------------------------------------------------------------------ */
/*  Override probing                                                  */
/* ------------------------------------------------------------------ */

static void probe_override(JceToolchainKind kind, const char *path,
                           JceToolchain *out)
{
    /* The override is interpreted as the install root.  We accept it
     * as "present" if either:
     *   - the path is a directory and contains a kind-specific marker
     *   - or the path itself is an existing file (user pointed straight
     *     at cmake.exe or similar — we then store the parent dir). */
    if (!path || !path[0]) return;

    char root[JCE_TOOLCHAIN_PATH_MAX];
    str_copy(root, sizeof root, path);

    if (jce_fs_host_exists_file(root) && !jce_fs_host_exists_dir(root)) {
        char parent[JCE_TOOLCHAIN_PATH_MAX];
        if (jce_path_parent(parent, sizeof parent, root)) {
            str_copy(root, sizeof root, parent);
        }
    }
    if (!jce_fs_host_exists_dir(root)) return;

    out->present = true;
    out->from_override = true;
    str_copy(out->path, sizeof out->path, root);

    /* Best-effort version when we know how to derive it. */
    switch (kind) {
    case JCE_TOOLCHAIN_NDK: {
        char marker[JCE_TOOLCHAIN_PATH_MAX];
        SDL_snprintf(marker, sizeof marker, "%s%csource.properties",
                     root, TC_NATIVE_DIR_SEP);
        char buf[2048];
        if (tc_read_small_file(marker, buf, sizeof buf)) {
            tc_props_get(buf, "Pkg.Revision",
                         out->version, sizeof out->version);
        }
        break;
    }
    default:
        out->version[0] = 0;
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

typedef void (*ProbeFn)(JceToolchain *);

static const ProbeFn k_probes[JCE_TOOLCHAIN_COUNT] = {
    probe_cmake, probe_ninja, probe_msvc,  probe_gcc,
    probe_clang, probe_ndk,   probe_emsdk, probe_xcode
};

void jce_toolchain_refresh(void)
{
    for (int i = 0; i < JCE_TOOLCHAIN_COUNT; ++i) {
        JceToolchain *t = &s_state[i];
        memset(t, 0, sizeof *t);
        t->kind = (JceToolchainKind)i;

        if (s_has_override[i]) {
            probe_override((JceToolchainKind)i, s_override_path[i], t);
        }
        if (!t->present && k_probes[i]) {
            k_probes[i](t);
        }
    }

    /* Cross-toolchain fallback: when MSVC is found but cmake/ninja
     * weren't, try the copies VS bundles under Common7.  This is the
     * common state when the editor is launched from a normal shell
     * (vcvars64 not sourced) on a developer box. */
    JceToolchain *msvc  = &s_state[JCE_TOOLCHAIN_MSVC];
    JceToolchain *cmk   = &s_state[JCE_TOOLCHAIN_CMAKE];
    JceToolchain *nin   = &s_state[JCE_TOOLCHAIN_NINJA];
    if (msvc->present && msvc->path[0]) {
        if (!cmk->present) {
            char d[JCE_TOOLCHAIN_PATH_MAX];
            SDL_snprintf(d, sizeof d,
                "%s\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin",
                msvc->path);
            try_install_dir(cmk, d, "cmake");
        }
        if (!nin->present) {
            char d[JCE_TOOLCHAIN_PATH_MAX];
            SDL_snprintf(d, sizeof d,
                "%s\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\Ninja",
                msvc->path);
            try_install_dir(nin, d, "ninja");
        }
    }

    s_initialised = true;
    LOG_INFO(LOG_TAG, "toolchain refresh complete");
}

const JceToolchain *jce_toolchain_get(JceToolchainKind kind)
{
    if ((int)kind < 0 || (int)kind >= JCE_TOOLCHAIN_COUNT) return NULL;
    if (!s_initialised) jce_toolchain_refresh();
    return &s_state[kind];
}

void jce_toolchain_set_override(JceToolchainKind kind, const char *path)
{
    if ((int)kind < 0 || (int)kind >= JCE_TOOLCHAIN_COUNT) return;
    if (path && path[0]) {
        SDL_strlcpy(s_override_path[kind], path, sizeof s_override_path[kind]);
        s_has_override[kind] = true;
    } else {
        s_override_path[kind][0] = 0;
        s_has_override[kind] = false;
    }
    jce_toolchain_refresh();
}
