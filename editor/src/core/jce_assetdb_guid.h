/*
 * jce_assetdb_guid.h  Stable 128-bit asset identifier + dependency
 * tracking.
 *
 * Editor-side equivalent of Unity's GUID system.  Every imported
 * asset gets a `.meta` sidecar holding a 128-bit GUID; runtime code
 * looks up assets through the GUID rather than the volatile project
 * path.  Cross-asset references (a Material referring to a Texture)
 * record the GUID, so renames don't break links.
 *
 * The dependency tracker is the reverse-edge graph: given GUID X,
 * what other GUIDs reference it?  Used by:
 *   - Hot reload: when X changes, dirty everyone who depends on X
 *   - Move / rename: rewrite forward references in dependants
 *   - "Find References" right-click action
 *
 * Storage is in-memory only (no disk persistence at this layer); the
 * editor flushes the dependency snapshot at shutdown if desired.
 */

#ifndef JCE_ASSETDB_GUID_H
#define JCE_ASSETDB_GUID_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JCE_GUID_STR_LEN 33   /* 32 hex chars + NUL */

typedef struct {
    uint64_t lo;
    uint64_t hi;
} JceGuid;

/* Generate a new pseudo-random GUID.  Uses an internal counter +
 * mixing function so calls within one editor session are unique.
 * Cryptographic strength is not required — only uniqueness. */
JceGuid jce_guid_new(void);

/* All-zero GUID acts as the "invalid / unset" sentinel. */
bool    jce_guid_is_zero (JceGuid g);
bool    jce_guid_equal   (JceGuid a, JceGuid b);

/* Hex string round-trip (32 lowercase hex digits, no dashes). */
void    jce_guid_to_string  (JceGuid g, char out[JCE_GUID_STR_LEN]);
bool    jce_guid_from_string(const char *s, JceGuid *out);

/* ── Registry ────────────────────────────────────────────────── */

/* Register an asset path → GUID mapping.  If `path` already has a
 * GUID, the existing one is returned and `guid` is ignored.  Returns
 * the canonical GUID for the path. */
JceGuid jce_assetdb_register(const char *path, JceGuid guid);

/* Lookup by path (returns zero GUID when unknown). */
JceGuid jce_assetdb_guid_for_path(const char *path);

/* Reverse lookup — fills `out_path` with the path bound to `guid`
 * and returns true.  Returns false if the GUID isn't known. */
bool    jce_assetdb_path_for_guid(JceGuid guid, char *out_path, size_t cap);

/* Drop a path/GUID entry (e.g. when asset is deleted).  Also clears
 * any incoming/outgoing dependency edges. */
bool    jce_assetdb_forget(const char *path);

/* ── Dependency edges ────────────────────────────────────────── */

/* Record that `from` references `to`.  Idempotent. */
void    jce_assetdb_add_dependency   (JceGuid from, JceGuid to);
void    jce_assetdb_remove_dependency(JceGuid from, JceGuid to);

/* Clear every outgoing dependency from `from` (used when re-importing
 * an asset — caller then re-adds them). */
void    jce_assetdb_clear_dependencies(JceGuid from);

/* Fill `out` with GUIDs that reference `to`.  Returns the count
 * actually written.  Use a small cap (16–64); few assets have many
 * dependants in practice. */
uint32_t jce_assetdb_dependants(JceGuid to, JceGuid *out, uint32_t cap);

/* Fill `out` with GUIDs that `from` references.  Symmetric helper. */
uint32_t jce_assetdb_dependencies(JceGuid from, JceGuid *out, uint32_t cap);

/* Counts (for editor stats / debug). */
uint32_t jce_assetdb_entry_count(void);
uint32_t jce_assetdb_edge_count (void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ASSETDB_GUID_H */
