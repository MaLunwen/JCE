/*
 * jce_mem_profile.h  Tag-categorised memory accounting (P3-A.5).
 *
 * Lightweight, opt-in instrumentation: subsystems voluntarily call
 * jce_mem_profile_record_alloc / _record_free around their large
 * allocations so the editor's Memory Profiler panel can show where
 * bytes actually go (textures vs meshes vs net buffers etc.).
 *
 * Intentionally NOT wired into the global mimalloc hook — that would
 * tax every tiny allocation and double-count what we already see via
 * Tracy / mimalloc's process counters. Instead we instrument a curated
 * set of high-traffic call sites.
 *
 * Backed by a 64-entry table guarded by a single internal mutex. The
 * record APIs are safe to call from any thread; reads (get_stats /
 * get_total) take a brief lock and snapshot.
 *
 * Layer: L2 OS / Core. No engine deps.
 */

#ifndef JCE_MEM_PROFILE_H
#define JCE_MEM_PROFILE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_MEM_TAG_COUNT_MAX 64

typedef enum JceMemTag {
    JCE_MEM_TAG_GENERIC = 0,
    JCE_MEM_TAG_RESOURCE_TEXTURES,
    JCE_MEM_TAG_RESOURCE_MESHES,
    JCE_MEM_TAG_RESOURCE_AUDIO,
    JCE_MEM_TAG_RESOURCE_OTHER,
    JCE_MEM_TAG_RENDERER,
    JCE_MEM_TAG_PHYSICS,
    JCE_MEM_TAG_SCENE_ECS,
    JCE_MEM_TAG_SCRIPTING,
    JCE_MEM_TAG_NETWORK,
    JCE_MEM_TAG_UI,
    JCE_MEM_TAG_AUDIO,
    JCE_MEM_TAG_EDITOR,
    JCE_MEM_TAG_TEMP,
    /* User tags start here. */
    JCE_MEM_TAG_USER_BASE = 32,
    JCE_MEM_TAG_COUNT__SENTINEL = JCE_MEM_TAG_COUNT_MAX
} JceMemTag;

typedef struct JceMemTagStats {
    uint64_t current_bytes;
    uint64_t peak_bytes;
    uint64_t total_bytes_allocated;     /* cumulative */
    uint64_t total_bytes_freed;
    uint32_t live_alloc_count;
    uint32_t total_alloc_count;         /* cumulative */
} JceMemTagStats;

/* Override / set the human-readable name of a tag. Default names cover
 * the built-in enum values; call this to label user tags. The string
 * must outlive the program (typically a string literal). */
JCE_API void JCE_CALL jce_mem_profile_register_tag_name(JceMemTag tag,
                                                        const char *name);

/* Returns the registered or default tag name, or "?" if out of range. */
JCE_API const char *JCE_CALL jce_mem_profile_tag_name(JceMemTag tag);

/* Cheap counter bumps. Safe from any thread. */
JCE_API void JCE_CALL jce_mem_profile_record_alloc(JceMemTag tag, size_t bytes);
JCE_API void JCE_CALL jce_mem_profile_record_free (JceMemTag tag, size_t bytes);

/* Snapshot a tag's stats into *out. Returns false if tag out of range
 * or out is NULL. */
JCE_API bool JCE_CALL jce_mem_profile_get_stats(JceMemTag tag,
                                                JceMemTagStats *out);

/* Reset every tag's peak_bytes counter to its current_bytes value.
 * Useful for "watch from here" workflows in the editor. */
JCE_API void JCE_CALL jce_mem_profile_reset_peaks(void);

/* Totals across every tag (sums current_bytes / peak_bytes). Either
 * out pointer may be NULL. */
JCE_API void JCE_CALL jce_mem_profile_get_total(uint64_t *current,
                                                uint64_t *peak);

/* Process-level memory snapshot, sourced from mimalloc's mi_process_info
 * (no platform macros, no PSAPI). Always fills *out; returns false only
 * if out is NULL. */
typedef struct JceProcessMemStats {
    uint64_t process_rss_bytes;
    uint64_t process_committed_bytes;
    uint64_t mimalloc_reserved_bytes;
    uint64_t mimalloc_committed_bytes;
} JceProcessMemStats;

JCE_API bool JCE_CALL jce_mem_profile_get_process_stats(JceProcessMemStats *out);

JCE_EXTERN_C_END

#endif /* JCE_MEM_PROFILE_H */
