/*
 * jce_project.h  Layer 6 — Project metadata.
 *
 * A "project" is a user-authored game built against the JCE SDK.  Its
 * source of truth on disk is `<project_root>/jce_project.json`, a
 * versioned schema the editor reads/writes to drive the build profile,
 * asset packer, and run manager.
 *
 * Schema v2 (current)
 *   {
 *     "schema":  2,
 *     "name":    "MyGame",
 *     "version": "0.1.0",
 *     "target":  "MyGame",                  // CMake target name
 *     "exe":     "MyGame.exe",              // built artifact
 *     "sdk":     "C:/JCE-Editor/sdk",       // optional override
 *     "platform":"win",                     // default target platform
 *     "assets":  ["assets", "shaders"],     // (v1) extra asset dirs
 *     "source_assets":  "assets",           // (v2) primary editor-author dir
 *     "cooked_assets":  "resources/_cooked",// (v2) cook output, fed to PAK
 *     "startup_scene":  "scenes/main.scene",// (v2) loaded at engine boot
 *     "variants":["debug","release","dist"]
 *   }
 *
 * Schema v1 manifests still load — missing v2 fields take sensible
 * defaults (source_assets falls back to assets[0] or "assets",
 * cooked_assets defaults to "resources/_cooked", startup_scene NULL).
 *
 * All fields are optional except `name`.  Unknown keys are preserved on
 * round-trip via the underlying JSON tree (future work; v1+v2 drop them).
 */

#ifndef JCE_PROJECT_H
#define JCE_PROJECT_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

#define JCE_PROJECT_FILENAME       "jce_project.json"
#define JCE_PROJECT_SCHEMA_VERSION 3

typedef struct JceProject {
	char  *project_root;              /* absolute, '/'-normalised, no trailing slash */
	char  *manifest_path;             /* "<project_root>/jce_project.json" */
	int    schema_version;

	char  *name;                      /* display + default target name */
	char  *version;                   /* "0.1.0" */
	char  *target_name;               /* CMake target (defaults to name) */
	char  *output_exe;                /* artifact filename (".exe" on Win) */
	char  *sdk_path;                  /* optional explicit SDK dir; may be NULL */
	char  *default_target_platform;   /* "win"|"linux"|"mac"|"android"|"ios"|"wasm" */

	char **asset_dirs;
	int    asset_dirs_count;

	char **build_variants;            /* subset of {"debug","release","dist"} */
	int    build_variants_count;

	/* ── v2 additions (always present on load; may be NULL when unset) ── */
	char  *source_assets;             /* primary editor-author dir, relative
	                                   * to project_root.  Default "assets". */
	char  *cooked_assets;             /* cook output dir, relative to
	                                   * project_root.  Default
	                                   * "resources/_cooked".  This is the
	                                   * tree the CMake build feeds to PAK. */
	char  *startup_scene;             /* path relative to source_assets,
	                                   * e.g. "scenes/main.scene".  When set,
	                                   * the engine loads it after init.
	                                   * NULL = boot empty (legacy behaviour). */

	/* ── v3 additions (NULL/0 on older manifests) ────────────────────────
	 *
	 * Each entry is a path to a `.jbundle` file relative to project_root
	 * (or absolute).  At boot the runtime mounts these in order; the
	 * startup_scene is then resolved via the VFS so it can come from any
	 * mounted bundle.  Bundle files are also copied next to the exe by
	 * the build pipeline. */
	char **bundles;
	int    bundles_count;
} JceProject;

/* Walk up from `start_dir` looking for jce_project.json.  When found,
 * writes the directory containing it into `out_root` (size >= cap) and
 * returns true.  Pure stat — does not parse. */
JCE_API bool JCE_CALL jce_project_find_root(const char *start_dir,
                                            char *out_root, size_t cap);

/* Load <project_root>/jce_project.json.  Returns NULL on any error. */
JCE_API JceProject *JCE_CALL jce_project_load(const char *project_root);

/* Allocate a fresh in-memory project skeleton with sensible defaults.
 * Caller mutates fields then calls jce_project_save(). */
JCE_API JceProject *JCE_CALL jce_project_new(const char *project_root,
                                             const char *name);

/* Serialise `p` back to its manifest_path. */
JCE_API bool JCE_CALL jce_project_save(const JceProject *p);

JCE_API void JCE_CALL jce_project_free(JceProject *p);

/* Mutate a single string-typed field on `p`, releasing the previous
 * value with the engine allocator and copying `value` (NULL or "" stores
 * NULL).  `field` must be one of:
 *   "name", "version", "target_name", "output_exe", "sdk_path",
 *   "default_target_platform", "source_assets", "cooked_assets",
 *   "startup_scene".
 * Returns false on unknown field or allocation failure. */
JCE_API bool JCE_CALL jce_project_set_field(JceProject *p,
                                            const char *field,
                                            const char *value);

/* ── Bundle list (schema v3) ────────────────────────────────────────── */

/* Append `path` to p->bundles (deep-copied).  No duplicate check. */
JCE_API bool JCE_CALL jce_project_bundle_add   (JceProject *p, const char *path);

/* Remove the entry at `index`.  Returns false on out-of-range. */
JCE_API bool JCE_CALL jce_project_bundle_remove(JceProject *p, int index);

/* Replace the entire bundle list (deep-copied).  Pass count=0/paths=NULL
 * to clear. */
JCE_API bool JCE_CALL jce_project_bundle_set_all(JceProject *p,
                                                 const char *const *paths,
                                                 int count);

/* Returns true iff `dir` looks like the JCE engine source tree (used by
 * the editor to auto-enable engine-developer mode).  Detection: the file
 * `<dir>/engine/include/jce/api.h` exists. */
JCE_API bool JCE_CALL jce_project_is_engine_workspace(const char *dir);

/* ── Project templates ────────────────────────────────────────────────
 *
 * Phase 2: write a turn-key minimal project on disk that an end user can
 * immediately build against the SDK.  Each template lays down:
 *
 *   <project_dir>/jce_project.json
 *   <project_dir>/CMakeLists.txt
 *   <project_dir>/src/main.c
 *   <project_dir>/assets/.gitkeep
 *
 * `project_dir` must NOT already contain a jce_project.json (refuses to
 * overwrite).  Missing intermediate directories are created.  On failure
 * a short diagnostic is written to `err_out` (when non-NULL).
 */
typedef enum {
	JCE_PROJECT_TEMPLATE_EMPTY    = 0,
	JCE_PROJECT_TEMPLATE_BASIC_2D = 1,
	JCE_PROJECT_TEMPLATE_BASIC_3D = 2,
} JceProjectTemplate;

JCE_API bool JCE_CALL jce_project_create_from_template(
		const char *project_dir,
		const char *name,
		JceProjectTemplate tpl,
		char *err_out, size_t err_cap);

/* ── Maintenance: refresh / eject src/main.c ─────────────────────────
 *
 * Default JCE projects ship a one-line src/main.c that includes
 * `<jce/application/jce_default_main.inc.h>` so engine upgrades reach
 * old projects automatically on next Build.  These two helpers wrap
 * the two common authoring intents around that file:
 *
 *   jce_project_reset_main_c  — overwrite project_dir/src/main.c with
 *       the current empty-template main.c.  Use this when an old
 *       project pre-dates the SDK-header pattern, or when the user
 *       has hand-edited src/main.c and wants to start over.
 *
 *   jce_project_eject_main_c  — replace the thin shim with an inlined
 *       copy of jce_default_main.inc.h's body.  From that moment on
 *       the project owns main.c and stops tracking engine upgrades —
 *       by design, so the user can deeply customise boot flow.
 *
 * Both return false on I/O or template-lookup failure.  When non-NULL
 * the diagnostic buffer receives a short reason string. */
JCE_API bool JCE_CALL jce_project_reset_main_c (const char *project_dir,
                                                const char *name,
                                                char *err_out, size_t err_cap);

JCE_API bool JCE_CALL jce_project_eject_main_c (const char *project_dir,
                                                const char *name,
                                                char *err_out, size_t err_cap);

JCE_EXTERN_C_END

#endif /* JCE_PROJECT_H */
