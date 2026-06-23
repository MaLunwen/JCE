/*
 * jce_snapshot.h  Generic save/load snapshot framework.
 *
 * Versioned binary serialization with a registry of typed sections.
 * Subsystems register a stable string ID + version + write/read pair;
 * jce_snapshot_save() walks all registered providers and writes a
 * single composite file, jce_snapshot_load() routes sections back to
 * their provider by ID with version negotiation.
 *
 * Generic — knows nothing about ECS, physics, scripts, etc.  Any
 * subsystem can plug in.  Same registry powers save files, network
 * snapshots, and editor undo (with a memory-buffer sink).
 *
 * File format (little-endian, packed):
 *   Header:
 *     char     magic[4]   = 'J','S','N','P'
 *     uint32_t format_ver = 1
 *     uint32_t section_count
 *     uint32_t flags        // reserved
 *   Per-section:
 *     uint32_t id_len; char id[id_len];
 *     uint32_t version
 *     uint32_t payload_size
 *     byte     payload[payload_size]
 *   Footer:
 *     uint32_t crc32        // crc of all preceding bytes (excluding footer)
 *
 * Thread-safety: a JceSnapshot context is single-threaded.  Provider
 * registration must complete before any save/load call.  Save/load
 * itself is reentrant against *different* contexts; share state
 * across threads only via your provider read/write callbacks.
 *
 * Example:
 *   JceSnapshot *snap = jce_snapshot_create();
 *   jce_snapshot_register(snap, "transforms", 1, write_xforms, read_xforms, ud);
 *   jce_snapshot_register(snap, "inventory",  2, write_inv,    read_inv,    ud);
 *   jce_snapshot_save(snap, "save01.jsnp");
 *   ...
 *   jce_snapshot_load(snap, "save01.jsnp");   // unknown sections are skipped
 *   jce_snapshot_destroy(snap);
 *
 * Layer: middleware/save (Layer 3 — optional, no other deps but jce_core).
 */
#ifndef JCE_SNAPSHOT_H
#define JCE_SNAPSHOT_H

#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Stream — opaque writer/reader passed to provider callbacks.         */
/* Backed by an in-memory growable buffer.                              */
/* ================================================================== */
typedef struct JceSnapshotStream JceSnapshotStream;

/* Primitive write helpers.  Return false on out-of-memory (writer). */
JCE_API bool jce_snap_write_bytes (JceSnapshotStream *s, const void *data, size_t n);
JCE_API bool jce_snap_write_u8    (JceSnapshotStream *s, uint8_t  v);
JCE_API bool jce_snap_write_u32   (JceSnapshotStream *s, uint32_t v);
JCE_API bool jce_snap_write_u64   (JceSnapshotStream *s, uint64_t v);
JCE_API bool jce_snap_write_f32   (JceSnapshotStream *s, float    v);
JCE_API bool jce_snap_write_string(JceSnapshotStream *s, const char *str);

/* Primitive read helpers.  Return false on truncated input. */
JCE_API bool jce_snap_read_bytes (JceSnapshotStream *s, void *out, size_t n);
JCE_API bool jce_snap_read_u8    (JceSnapshotStream *s, uint8_t  *out);
JCE_API bool jce_snap_read_u32   (JceSnapshotStream *s, uint32_t *out);
JCE_API bool jce_snap_read_u64   (JceSnapshotStream *s, uint64_t *out);
JCE_API bool jce_snap_read_f32   (JceSnapshotStream *s, float    *out);
/* String reader allocates with JCE_MALLOC; caller must JCE_FREE. */
JCE_API bool jce_snap_read_string(JceSnapshotStream *s, char **out_str);

/* Bytes remaining in the section payload (read mode only). */
JCE_API size_t jce_snap_remaining(const JceSnapshotStream *s);

/* ================================================================== */
/* Provider registry                                                   */
/* ================================================================== */

typedef bool (*JceSnapshotWriteFn)(JceSnapshotStream *s, void *user);
typedef bool (*JceSnapshotReadFn)(JceSnapshotStream *s,
                                  uint32_t loaded_version,
                                  void    *user);

typedef struct JceSnapshotRegistry JceSnapshotRegistry;

JCE_API JceSnapshotRegistry *jce_snapshot_registry_create(void);
JCE_API void                 jce_snapshot_registry_destroy(JceSnapshotRegistry *r);

JCE_API void jce_snapshot_register(JceSnapshotRegistry *r,
                                   const char         *id,
                                   uint32_t            current_version,
                                   JceSnapshotWriteFn  write_fn,
                                   JceSnapshotReadFn   read_fn,
                                   void               *user);

JCE_API void jce_snapshot_unregister(JceSnapshotRegistry *r, const char *id);

/* Return the `user` pointer registered for section `id`, or NULL when no
 * such section is registered.  Lets a provider find and reuse a context it
 * heap-allocated on a prior registration (e.g. to re-point it at a reloaded
 * scene without leaking the old one).  NULL `user` registrations are
 * indistinguishable from absent ones. */
JCE_API void *jce_snapshot_get_user(JceSnapshotRegistry *r, const char *id);

/* ================================================================== */
/* Save / load                                                         */
/* ================================================================== */

JCE_API bool jce_snapshot_save_to_buffer(JceSnapshotRegistry *r,
                                         void   **out_buf,
                                         size_t  *out_size);

JCE_API bool jce_snapshot_save_to_file(JceSnapshotRegistry *r,
                                       const char *path);

JCE_API bool jce_snapshot_load_from_buffer(JceSnapshotRegistry *r,
                                           const void *buf, size_t size);

JCE_API bool jce_snapshot_load_from_file(JceSnapshotRegistry *r,
                                         const char *path);

/* ================================================================== */
/* Inspection                                                          */
/* ================================================================== */
typedef struct {
    uint32_t format_version;
    uint32_t section_count;
    uint32_t flags;
} JceSnapshotHeaderInfo;

JCE_API bool jce_snapshot_peek_header(const void *buf, size_t size,
                                      JceSnapshotHeaderInfo *out);

JCE_EXTERN_C_END
#endif /* JCE_SNAPSHOT_H */
