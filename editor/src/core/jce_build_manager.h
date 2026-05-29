/*
 * jce_build_manager.h  CMake-driven build orchestration.
 *
 * Spawns `cmake --preset <p>` (configure) and `cmake --build --preset <p>`
 * (compile) child processes, drains their stdout/stderr to the editor
 * console with a "[build]" prefix, and exposes a small status struct so
 * the Build Settings dialog can render a progress / log strip.
 *
 * One build at a time.  Re-entrancy returns false from start_*().  The
 * dialog UI is responsible for calling jce_build_manager_poll() each
 * frame so completion / log lines are picked up.
 *
 * Threading: all entry points are expected from the editor main thread.
 *
 * Layer: Editor (depends on jce_process from os/core).
 */

#ifndef JCE_BUILD_MANAGER_H
#define JCE_BUILD_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JCE_BUILD_IDLE      = 0,
    JCE_BUILD_RUNNING   = 1, /* configure or compile in flight */
    JCE_BUILD_SUCCEEDED = 2,
    JCE_BUILD_FAILED    = 3
} JceBuildState;

typedef enum {
    JCE_BUILD_STAGE_NONE          = 0,
    JCE_BUILD_STAGE_CONFIGURE     = 1,
    JCE_BUILD_STAGE_COMPILE       = 2,
    JCE_BUILD_STAGE_CONAN_INSTALL = 3
} JceBuildStage;

typedef struct {
    JceBuildState state;
    JceBuildStage stage;
    int           exit_code;
    /* The preset name in flight (or last finished). */
    char          preset[128];
    /* Last error text — populated when state == JCE_BUILD_FAILED. */
    char          last_error[256];
} JceBuildStatus;

void jce_build_manager_init(void);
void jce_build_manager_shutdown(void);

/* Drain pipes, detect exit.  Call once per editor frame. */
void jce_build_manager_poll(void);

/* Run `cmake --build --preset <preset> --target PackGameAssets` —
 * asset-only incremental repack used by save/launch hooks.
 *
 * NOTE: in this repository `cmake --preset` triggers a "Duplicate
 * preset" failure (CMakeUserPresets.json includes 6 conan-generated
 * preset files that all define `conan-release`), so this function
 * is currently a stub that returns false.  Auto-repack-on-save is
 * effectively disabled until a script-based path is wired up.  The
 * call sites still compile and gracefully degrade. */
bool jce_build_manager_repack_game_assets(const char *preset);

/* Request graceful stop (SIGINT / Ctrl+C semantics).  Falls back to
 * force-kill after ~5 s if the child does not exit. */
void jce_build_manager_request_stop(void);

bool jce_build_manager_is_running(void);
void jce_build_manager_get_status(JceBuildStatus *out);

/* Set the default working directory used by all spawned tool
 * processes (run_script + tool probes).  Pass NULL or "" to clear
 * and fall back to the editor's own cwd.  Safe to call from the main
 * thread.  The Build Profiles panel calls this whenever the user
 * picks a new project root. */
void jce_build_manager_set_default_working_dir(const char *dir);

/* ---------------------------------------------------------------- *
 * Script delegation (preferred entry point for the "▶ Build" button) *
 * ---------------------------------------------------------------- *
 *
 * Instead of reimplementing the conan→cmake→ninja choreography in C++
 * (which already exists, tested, in scripts/build-*.{bat,sh}), the
 * editor spawns the platform's authoritative build script and streams
 * its stdout/stderr to the Console.  This deliberately keeps a SINGLE
 * source of truth for build logic — the scripts.
 *
 * Implementation note: on Windows the script is launched via
 * `cmd.exe /c <script_path> <args>` (avoids SDL_CreateProcess +
 * working_directory + bare-name PATH-search quirks).  On POSIX the
 * script is launched via `/bin/bash <script_path> <args>` (does not
 * require the script to be chmod +x).
 *
 * `script_path` may be relative to `working_dir` (e.g.
 * "scripts/build-desktop.bat") or absolute. */
typedef struct {
    const char *label;          /* shown in status line, e.g. "windows-x64-release" */
    const char *script_path;    /* relative or absolute */
    const char *script_args;    /* may be NULL / "" */
    const char *working_dir;    /* project root; NULL = inherit default cwd */
} JceBuildScriptConfig;

/* Returns the platform-default desktop build script (relative path from
 * the project root).  Returns NULL on unsupported platforms. */
const char *jce_build_manager_default_desktop_script(void);

/* Returns the universal "build-project" script (scripts/build-project.bat
 * on Windows, .sh on POSIX) used in Project mode — when the open root is
 * a user project (has jce_project.json) rather than the engine source
 * tree.  Path is relative to the editor install root. */
const char *jce_build_manager_default_project_script(void);

/* Spawn the script via the platform shell, stream output to console.
 * Returns false if a build is already running or the shell spawn
 * fails.  Status flips to RUNNING (stage = COMPILE for UI purposes)
 * and then SUCCEEDED / FAILED based on exit code. */
bool jce_build_manager_run_script(const JceBuildScriptConfig *cfg);

/* ---------------------------------------------------------------- *
 * Tool availability probe                                            *
 * ---------------------------------------------------------------- */

typedef struct {
    bool cmake_ok;
    bool conan_ok;
    bool ninja_ok;
    char cmake_version[64];   /* first line of `--version`, may be empty */
    char conan_version[64];
    char ninja_version[64];
} JceBuildToolStatus;

/* Probe PATH for cmake/conan/ninja by spawning `<tool> --version`
 * with a short blocking timeout.  Safe to call from the UI thread;
 * each tool gets ~2 s wall-clock budget.  Never spawns anything if a
 * build is already running (returns the previously cached result). */
void jce_build_manager_check_tools(JceBuildToolStatus *out);

#ifdef __cplusplus
}
#endif

#endif /* JCE_BUILD_MANAGER_H */
