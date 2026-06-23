/* jce_mod_loader.h
 *
 * Game-side modding / external-PAK loader (FEATURE 9.5).
 *
 * A *mod* is a JCE Archive (JPAK; *.jpak or *.jmod) dropped into a mods
 * directory next to the game.  Each mod may override or add asset reads on
 * top of the game's base content — and ONLY asset reads: a mod cannot run
 * code (no DLLs, no scripts loaded by this module).  The loader is the
 * runtime side of the layered-archive mount stack (spec §11.2): it scans a
 * directory, reads an optional per-mod manifest (id / name / version /
 * load-order / enabled), and mounts the enabled mods as override layers in a
 * deterministic load order so a higher-priority enabled mod's copy of an
 * asset wins over a lower-priority mod's copy, which in turn wins over the
 * base content.
 *
 * Sandbox / threat model (read before extending):
 *   - Mods override ASSET READS via jce_mod_loader_mount(), nothing else.
 *     Path lookups go through jce_archive_normalize_path(), which strips
 *     drive letters, collapses "." / ".." and roots every virtual path —
 *     so a mod entry named "../../system/x" can never escape the virtual
 *     namespace into the host filesystem.
 *   - No code execution.  This module never dlopen()s, never evaluates a
 *     script, and never honors a manifest field that would.  A mod is data.
 *   - Discovery is explicit: jce_mod_loader_scan() must be called by the
 *     app; nothing here auto-runs from engine boot.  After scanning, the app
 *     decides which mods to enable and when to mount, so a shipped game can
 *     gate modding behind a setting.
 *   - Encrypted base content stays readable through the mount because each
 *     archive keeps its own decryption key; mods are normally plain.
 *
 * Typical use:
 *     JceModLoader *ml = jce_mod_loader_create();
 *     jce_mod_loader_scan(ml, "mods");          // discover *.jpak / *.jmod
 *     jce_mod_loader_set_base(ml, base_archive); // optional lowest layer
 *     // (optionally toggle / reorder mods from a saved config here)
 *     jce_mod_loader_mount(ml);                  // build the override stack
 *     const JceArchiveMount *m = jce_mod_loader_mount_handle(ml);
 *     jce_archive_mount_read(m, "textures/player.png", buf, sizeof buf);
 *     ...
 *     jce_mod_loader_destroy(ml);               // unmounts + closes mods
 */
#ifndef JCE_MOD_LOADER_H
#define JCE_MOD_LOADER_H

#include <jce/os/core/jce_defs.h>
#include <jce/resource/jce_archive.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Opaque mod-loader handle. */
typedef struct JceModLoader JceModLoader;

/* Public, read-only view of one discovered mod.  The string pointers stay
 * valid until the owning mod is removed or the loader is destroyed; do not
 * free them.  `id` is never NULL (it falls back to the file's base name).  */
typedef struct JceModInfo {
    const char *id;          /* stable identifier (manifest id or file stem) */
    const char *name;        /* human-readable name (== id when unspecified) */
    const char *version;     /* version string ("" when unspecified)         */
    const char *file;        /* absolute path to the .jpak/.jmod on disk     */
    int32_t     load_order;  /* lower = lower priority; ties break by id      */
    bool        enabled;     /* mounted as an override layer when true        */
    bool        mounted;     /* currently part of the live mount stack        */
} JceModInfo;

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/* Create an empty loader, or NULL on allocation failure. */
JCE_API JceModLoader *JCE_CALL jce_mod_loader_create(void);

/* Destroy the loader: unmounts the stack, closes every mod archive it
 * opened, and frees all bookkeeping.  The base archive set via
 * jce_mod_loader_set_base() is borrowed and NOT closed.  NULL is a no-op. */
JCE_API void JCE_CALL jce_mod_loader_destroy(JceModLoader *ml);

/* ── Discovery ───────────────────────────────────────────────────────── */

/* Scan `dir` (a host directory path) for *.jpak / *.jmod files, opening each
 * and reading its optional manifest.  Newly found mods start enabled (a
 * manifest may set "enabled": false to start disabled).  Already-known mods
 * (same file path) are left untouched, so re-scanning is idempotent and does
 * not clobber the app's enable/order edits.  A missing directory is not an
 * error (returns 0 found).  Returns the number of NEW mods discovered, or -1
 * on a hard error (NULL loader).  Does not mount anything. */
JCE_API int JCE_CALL jce_mod_loader_scan(JceModLoader *ml, const char *dir);

/* Number of discovered mods (enabled or not). */
JCE_API size_t JCE_CALL jce_mod_loader_count(const JceModLoader *ml);

/* Fill *out with the i-th discovered mod's info (sorted by effective load
 * order, then id, matching the mount order).  Returns true on success. */
JCE_API bool JCE_CALL jce_mod_loader_get(const JceModLoader *ml, size_t index,
                                         JceModInfo *out);

/* Find a discovered mod by id; returns its index or -1 when absent. */
JCE_API int JCE_CALL jce_mod_loader_find(const JceModLoader *ml, const char *id);

/* ── Enable / disable & load order ───────────────────────────────────── */

/* Enable or disable the mod with `id`.  Disabled mods are not mounted (their
 * assets never win).  Returns true if the mod exists.  Takes effect on the
 * next jce_mod_loader_mount(); call mount() afterwards to apply live. */
JCE_API bool JCE_CALL jce_mod_loader_set_enabled(JceModLoader *ml,
                                                 const char *id, bool enabled);

/* Query enabled state; false if the mod is unknown. */
JCE_API bool JCE_CALL jce_mod_loader_is_enabled(const JceModLoader *ml,
                                                const char *id);

/* Set a mod's load order.  Higher order = higher priority (its assets win
 * over lower-order enabled mods).  Returns true if the mod exists.  Applies
 * on the next mount(). */
JCE_API bool JCE_CALL jce_mod_loader_set_load_order(JceModLoader *ml,
                                                    const char *id, int32_t order);

/* Query a mod's load order; 0 if the mod is unknown. */
JCE_API int32_t JCE_CALL jce_mod_loader_get_load_order(const JceModLoader *ml,
                                                       const char *id);

/* ── Base content + mounting ─────────────────────────────────────────── */

/* Set the base archive that sits BELOW every mod in the override stack (its
 * assets are the fallback when no enabled mod provides a path).  Borrowed —
 * the caller keeps ownership and must keep it open for the loader's lifetime.
 * NULL clears it.  Applies on the next mount(). */
JCE_API void JCE_CALL jce_mod_loader_set_base(JceModLoader *ml, JceArchive *base);

/* (Re)build the override mount stack from the current base + enabled mods, in
 * ascending effective load order (base lowest, highest-order enabled mod on
 * top).  Safe to call repeatedly after toggling/reordering.  Returns the
 * number of layers mounted (base counts as one when set), or -1 on error. */
JCE_API int JCE_CALL jce_mod_loader_mount(JceModLoader *ml);

/* The live mount stack built by the last jce_mod_loader_mount(); read assets
 * through it with jce_archive_mount_find() / jce_archive_mount_read().  NULL
 * until the first successful mount().  Owned by the loader. */
JCE_API const JceArchiveMount *JCE_CALL jce_mod_loader_mount_handle(const JceModLoader *ml);

/* Number of mods currently in the live mount stack (excludes the base and any
 * disabled mod). */
JCE_API size_t JCE_CALL jce_mod_loader_mounted_count(const JceModLoader *ml);

JCE_EXTERN_C_END

#endif /* JCE_MOD_LOADER_H */
