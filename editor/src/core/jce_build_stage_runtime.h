/*
 * jce_build_stage_runtime.h — stage the scripting runtime beside a packaged exe.
 *
 * Split out of jce_build_manager.cpp, which the staging work pushed over
 * AGENTS.md §11's 3000-line cap.  The seam was already there: this is a
 * self-contained job with paths in and a count out, and it shares no state
 * with the build state machine it used to sit inside.
 */
#ifndef JCE_BUILD_STAGE_RUNTIME_H
#define JCE_BUILD_STAGE_RUNTIME_H

#include <string>

/* Copy the scripting runtime payload that the build placed beside
 * `built_exe` into `out_dir`.
 *
 * A game that enables scripting links backends whose runtimes it does not
 * itself contain: the C ABI shared library, the JVM bridge, CPython,
 * nethost plus the managed bridge and the runtimeconfig hostfxr finds BY
 * NAME beside it, and directories the registration shim resolves beside the
 * executable.  jce_script_stage_runtime() (CMake) puts all of that next to
 * the BUILT exe, so a package that carries only the exe cannot start: the
 * loader fails before main() with "nethost.dll: cannot open shared object
 * file", exit 127, one line of output.  Measured on a six-language game,
 * against a package the build had reported as staged.
 *
 * What travels is decided by SHAPE, never by a list of languages -- the
 * packager must not learn which backends exist, or the next backend added
 * ships broken and silent:
 *
 *   - shared libraries, and the *.runtimeconfig.json that sits beside one;
 *   - directories named jce_script_*, the prefix the registration shim
 *     (jce_script_register_linked.c.in) already looks for beside the exe.
 *
 * Everything else beside the built exe is deliberately left behind: the
 * cooked `resources` tree is staged -- or withheld -- by the
 * encryption-aware path in the build manager, and .config/.jce are runtime
 * state a fresh install must not inherit.
 *
 * Returns the number of entries copied, or -1 when a copy failed; on -1
 * `error` (when non-null) is filled with a message naming the source dir.
 * A project with no scripting stages nothing and returns 0. */
int jce_build_stage_runtime_payload(const std::string &built_exe,
                                    const std::string &out_dir,
                                    std::string *error);

#endif /* JCE_BUILD_STAGE_RUNTIME_H */
