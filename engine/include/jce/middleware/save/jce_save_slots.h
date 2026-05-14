/*
 * jce_save_slots.h  Multi-slot save game manager.
 *
 * Sits on top of jce_snapshot.h (binary section writer) and provides:
 *   - Numbered save slots 0..N-1 under <user>/saves/slot_<N>.jsnp
 *   - Per-slot metadata (display name, timestamp, playtime, thumbnail
 *     hint) stored in a small sidecar JSON
 *   - List / read-header / load / save / delete operations
 *   - Versioning + magic so loaders can reject older incompatible saves
 *
 * Mirrors Unity-style save manager patterns (AssetBundle / scriptable
 * save profile) at the data layer.  Storage location is the host
 * user-writable directory — same root the screenshot system uses.
 *
 * Layer: middleware / save (Layer 4) — public.
 */

#ifndef JCE_SAVE_SLOTS_H
#define JCE_SAVE_SLOTS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SAVE_SLOT_MAX       16
#define JCE_SAVE_NAME_MAX       64
#define JCE_SAVE_THUMB_PATH_MAX 256

typedef struct {
    int       slot_index;
    char      display_name[JCE_SAVE_NAME_MAX];
    uint64_t  timestamp_unix;     /* save time, seconds since epoch */
    float     playtime_seconds;   /* total in-game time at save */
    char      thumbnail_path[JCE_SAVE_THUMB_PATH_MAX];
    /* Format version of the underlying .jsnp blob.  Loader rejects
     * unknown versions to avoid mis-interpreting payload. */
    uint32_t  format_version;
    /* Size in bytes of the underlying .jsnp file (0 = unknown). */
    uint64_t  blob_size_bytes;
    bool      exists;             /* false → slot is empty */
} JceSaveSlotInfo;

/* Configure the root directory containing slot files + their .meta
 * sidecars.  Call once at startup; defaults to "saves" relative to
 * the host CWD when not configured.  Returns true if the directory
 * was created (or already existed). */
JCE_API bool jce_save_slots_set_root(const char *root_dir);

/* Read the metadata for every slot 0..JCE_SAVE_SLOT_MAX-1 into the
 * caller-supplied array.  Empty slots get `exists=false`.  Returns
 * the array length actually written (always JCE_SAVE_SLOT_MAX with
 * the current implementation). */
JCE_API uint32_t jce_save_slots_list(JceSaveSlotInfo *out,
                                     uint32_t         max);

/* Read metadata for a single slot.  Returns false if the slot file
 * doesn't exist or the metadata is malformed. */
JCE_API bool jce_save_slots_get_info(int slot, JceSaveSlotInfo *out);

/* Write `blob` of `size` bytes to slot `slot` and update the meta
 * sidecar with display_name + current timestamp + supplied playtime.
 * `thumbnail_path` may be empty.  Returns true on success.  Caller
 * is responsible for the actual serialization of game state into the
 * blob (typically via jce_snapshot_writer_*). */
JCE_API bool jce_save_slots_write(int           slot,
                                   const void   *blob,
                                   uint64_t      size,
                                   const char   *display_name,
                                   float         playtime_seconds,
                                   const char   *thumbnail_path,
                                   uint32_t      format_version);

/* Read slot `slot` into a freshly-allocated buffer.  Caller frees
 * with `free()`.  Writes byte count to `*out_size` and the metadata
 * (if `out_info` non-NULL).  Returns NULL on failure. */
JCE_API void *jce_save_slots_read(int               slot,
                                   uint64_t         *out_size,
                                   JceSaveSlotInfo  *out_info);

/* Delete a slot (file + sidecar).  Returns true if removed (or
 * already gone). */
JCE_API bool jce_save_slots_delete(int slot);

/* Find the slot with the newest timestamp_unix.  Returns -1 if no
 * slots exist. */
JCE_API int  jce_save_slots_most_recent(void);

JCE_EXTERN_C_END

#endif /* JCE_SAVE_SLOTS_H */
