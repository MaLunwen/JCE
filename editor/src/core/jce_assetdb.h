/*
 * jce_assetdb.h  Project-wide asset database with reverse references.
 *
 * Scans a project root, builds a path -> kind map and a reverse map of
 * asset_path -> [files referencing it].  Consumed by the inspector
 * ASSET_REF field, the asset browser, and validation tools.
 *
 * The database is rebuilt on demand (jce_assetdb_rescan); call this
 * after large file changes.  Initial path uses the asset browser's
 * project_root.
 *
 * NOT the asset path index.  editor/src/scene/jce_asset_path_index.*
 * indexes the same tree and the split is deliberate (REF-019): that one
 * maps a FUZZY BASENAME -> absolute path to repair stale references, is
 * built on a worker and swapped in whole, and publishes a generation
 * counter.  This one maps an exact normalized ABSOLUTE PATH -> asset KIND
 * (plus the project-relative path) and enumerates in insertion order for
 * the browser / picker; it is SYNCHRONOUS on purpose so set_root() is
 * queryable the moment it returns, and this translation unit is kept
 * dependency-free (jce_core only) so it unit-tests standalone.  Different
 * key, different value, different lifecycle — do not fold them together.
 */

#ifndef JCE_ASSETDB_H
#define JCE_ASSETDB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>   /* the header is extern "C"; a C caller needs `bool` */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JCE_ASSET_KIND_UNKNOWN = 0,
    JCE_ASSET_KIND_TEXTURE,
    JCE_ASSET_KIND_MODEL,
    JCE_ASSET_KIND_AUDIO,
    JCE_ASSET_KIND_MATERIAL,
    JCE_ASSET_KIND_SCENE,
    JCE_ASSET_KIND_SHADER,
    JCE_ASSET_KIND_SCRIPT,
    JCE_ASSET_KIND_PARTICLE,
    JCE_ASSET_KIND_DATA,
} JceAssetKind;

const char *jce_assetdb_kind_label(JceAssetKind kind);

/* Initialize / rescan against project_root.  NULL or empty resets. */
void jce_assetdb_set_root(const char *project_root);
void jce_assetdb_rescan(void);

/* Returns the current project root (absolute path) or "" if unset. */
const char *jce_assetdb_get_root(void);

/* Flat enumeration. */
int            jce_assetdb_count(void);
const char    *jce_assetdb_path_at(int idx);
const char    *jce_assetdb_rel_at(int idx);
JceAssetKind   jce_assetdb_kind_at(int idx);

/* Lookups (path is project-relative or absolute, both accepted). */
JceAssetKind   jce_assetdb_get_kind(const char *path);

/* The language a Script component's `scriptPath` resolves to, or NULL.
 *
 * KIND IS NOT ATTACHABILITY, AND THIS IS THE DIFFERENCE.  classify() files
 * .c/.cpp/.h/.hpp/.js/.ts under JCE_ASSET_KIND_SCRIPT on purpose — the asset
 * browser should show project code as code and not as "unknown".  But the
 * Script component's picker filters on that same kind, so for as long as the
 * kind was the only question asked, every header and translation unit in the
 * project was offered as an attachable script.  MEASURED on Elemental
 * Serenity: 20 entries under kind=script, of which 5 resolve.  The cruellest
 * of the other 15 is src/es_flower_sway.cpp — the project's actual C++
 * script SOURCE, which cannot be attached by path and never will be: a cpp
 * scriptPath names the CLASS, spelled with the backend's own ".jcecpp"
 * extension (jce_script_vm_cpp.h), and the translation unit that compiles it
 * is a build input.  Picking the file that looks like the right answer
 * produced a silently dead component.
 *
 * So: ask kind when you want to know what a file IS, and ask this when you
 * want to know whether a Script component can use it.  Both the picker's
 * filter and the inspector's "does any language claim this?" go through
 * here, which is what stops the offered set and the accepted set drifting
 * apart again.
 *
 * Answers from the engine's shipped script-extension catalog, so it reads
 * the same in an editor built with no scripting backend at all: whether THIS
 * executable linked a VM for the language is a separate question, asked of
 * the VM registry by the one caller that needs it. */
const char    *jce_assetdb_script_language(const char *path);

/* The asset picker's whole per-entry filter decision, minus the search text.
 *
 * This lives here rather than inside the ImGui loop for one reason: an
 * ImGui loop cannot be unit-tested, so a rule written inline is a rule
 * nothing checks — and the rule it replaced ("kind matches") was wrong for
 * scripts for as long as it existed.  jce_dialog_asset_picker calls this and
 * adds only jce_panel_filter_match_ci(), so what the test drives below IS
 * the decision the dialog makes.
 *
 * `requested_kind` is 0 for "any".  `entry_kind` is the DB's stored kind
 * (never reclassified from the path — a scan may have known better).
 *
 * Held by "asset picker filter decision" in
 * tests/editor/test_jce_editor_assetdb.cpp, which fails if the script rule
 * is dropped, inverted, or applied to the wrong kind. */
bool           jce_assetdb_picker_accepts(int requested_kind,
                                          JceAssetKind entry_kind,
                                          const char *path);

/* Reverse references: which files reference this asset?
 * Writes up to max_out paths into out_paths (caller-owned char[][512]).
 * Returns total count (may exceed max_out). */
int jce_assetdb_find_references(const char *asset_path,
                                char (*out_paths)[512], int max_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* JCE_ASSETDB_H */
