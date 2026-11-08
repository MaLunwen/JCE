/*
 * jce_run_manager_internal.h — the launcher's PURE path resolution.
 *
 * resolve_executable() in jce_run_manager.cpp does two different jobs: it
 * gathers editor state (the saved config, the open project) and then does
 * pure string/path work over what it gathered.  Only the first half needs
 * build_manager, assetdb, the panels and a real filesystem, so the second half
 * lives here and in jce_run_manager_resolve.cpp.
 *
 * Why the split exists: the resolution order is the editor's "▶ Run" launch
 * path, it changed materially on 2026-08-27 (the shipped default executable
 * path became empty, so the project-derived stage had to start running), and
 * NOTHING tested it -- jce_run_manager.cpp cannot be compiled into a test
 * without stubbing four subsystems.  A pure function with an injected
 * existence predicate can be.
 */

#ifndef JCE_RUN_MANAGER_INTERNAL_H
#define JCE_RUN_MANAGER_INTERNAL_H

#include <string>
#include <vector>

/* "Does something exist at this path?"  Injected so a test can hand in a set
 * of names instead of a disk.  The production predicate answers true for a
 * file OR a directory, matching what the launcher accepts. */
typedef bool (*JceRunPathExists)(const std::string &path, void *user);

struct JceRunResolveInputs {
    std::string              configured;         /* may be empty */
    std::string              exe_suffix;         /* ".exe" on Windows, else "" */
    std::string              build_output_path;  /* Project Settings > Build */
    std::string              target_name;        /* config target, else project */
    std::string              artifact_name;      /* actual manifest/output filename */
    std::vector<std::string> project_artifacts;  /* current project, shared layout */
    std::vector<std::string> output_dirs;        /* well-known preset out dirs */
    std::vector<std::string> parents;            /* ".", "..", "../..", ... */
};

/*
 * Resolution order (first hit wins):
 *   1. `configured` as-is, then with exe_suffix appended
 *   2. relative `configured` under each of `parents`
 *   3. <build_output_path>/<artifact_name>, or target_name for legacy inputs
 *   4. Current-project artifact candidates supplied by the shared build owner
 *   5. <parent>/<output_dir>/<artifact_name> for legacy workspace inputs
 * Returns `configured` unchanged when nothing matched, so the caller can print
 * it as a diagnostic.  An EMPTY `configured` skips stages 1-2 rather than
 * short-circuiting the whole function: since the shipped default is empty,
 * stages 3-5 are exactly what has to run. Empty artifact/target names skip
 * guessed-name searches; explicit current-project candidates remain separate.
 */
std::string jce_run_manager_resolve_path(const JceRunResolveInputs &in,
                                         JceRunPathExists exists, void *user);

#endif /* JCE_RUN_MANAGER_INTERNAL_H */
