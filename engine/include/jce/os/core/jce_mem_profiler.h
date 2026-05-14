/*
 * jce_mem_profiler.h  Lightweight tagged memory accounting.
 *
 * Game subsystems push (tag, delta_bytes) events when they allocate
 * or free; this module aggregates totals per tag and exposes a
 * snapshot accessor.  The editor profiler panel reads the snapshot
 * each frame to render a per-category memory bar chart.
 *
 * Tagging is opt-in: existing allocations through jce_alloc.h aren't
 * automatically tracked here.  Subsystems that care explicitly call
 * jce_mem_profiler_record_alloc / record_free with their own tag.
 *
 * Mirrors Unity's Memory Profiler module breakdown — without any
 * tooling-pipe coupling, so it works in shipped builds too.
 *
 * Thread safety: snapshot reads are atomic; pushes are single-thread.
 * If you need MT pushes, queue them and drain on the main thread.
 *
 * Layer: os/core (Layer 1) — public.
 */

#ifndef JCE_MEM_PROFILER_H
#define JCE_MEM_PROFILER_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_MEM_TAG_MAX     32       /* max distinct tags */
#define JCE_MEM_TAG_NAME_LEN 32

/* Record an allocation against a tag.  Negative `delta_bytes` is
 * treated as a free.  Calling with an unknown tag registers it. */
JCE_API void jce_mem_profiler_record(const char *tag,
                                     int64_t     delta_bytes);

JCE_API void jce_mem_profiler_record_alloc(const char *tag, uint64_t bytes);
JCE_API void jce_mem_profiler_record_free (const char *tag, uint64_t bytes);

/* Snapshot of one tag's accumulated state. */
typedef struct {
    char     tag[JCE_MEM_TAG_NAME_LEN];
    uint64_t current_bytes;
    uint64_t peak_bytes;
    uint64_t total_allocated; /* lifetime alloc */
    uint64_t total_freed;
    uint32_t alloc_count;
    uint32_t free_count;
} JceMemTagSnapshot;

/* Write up to `max` tag snapshots to `out` and return the count
 * actually written. */
JCE_API uint32_t jce_mem_profiler_snapshot(JceMemTagSnapshot *out,
                                            uint32_t max);

/* Grand-total memory currently tracked across all tags. */
JCE_API uint64_t jce_mem_profiler_total_current(void);
JCE_API uint64_t jce_mem_profiler_total_peak(void);

/* Reset peak markers (current/total counters are NOT reset — they
 * reflect ongoing allocations). */
JCE_API void     jce_mem_profiler_reset_peaks(void);

JCE_EXTERN_C_END

#endif /* JCE_MEM_PROFILER_H */
