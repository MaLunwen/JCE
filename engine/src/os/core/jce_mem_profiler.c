/*
 * jce_mem_profiler.c  Tagged memory accounting.
 *
 * Storage: small fixed-size array of tag records (linear lookup).
 * No external dependencies — totals are local atomic-ish reads.
 */

#include <jce/os/core/jce_mem_profiler.h>

#include <string.h>

typedef struct {
    char     name[JCE_MEM_TAG_NAME_LEN];
    uint64_t current_bytes;
    uint64_t peak_bytes;
    uint64_t total_allocated;
    uint64_t total_freed;
    uint32_t alloc_count;
    uint32_t free_count;
    bool     used;
} Tag;

static Tag      s_tags[JCE_MEM_TAG_MAX];
static uint32_t s_tag_count = 0;
static uint64_t s_grand_total = 0;
static uint64_t s_grand_peak  = 0;

static int find_or_add(const char *name)
{
    if (!name || !name[0]) return -1;
    for (uint32_t i = 0; i < s_tag_count; ++i)
        if (s_tags[i].used && strncmp(s_tags[i].name, name,
                                       JCE_MEM_TAG_NAME_LEN) == 0) return (int)i;
    if (s_tag_count >= JCE_MEM_TAG_MAX) return -1;
    Tag *t = &s_tags[s_tag_count];
    memset(t, 0, sizeof(*t));
    strncpy(t->name, name, JCE_MEM_TAG_NAME_LEN - 1);
    t->used = true;
    return (int)s_tag_count++;
}

void jce_mem_profiler_record(const char *tag, int64_t delta)
{
    int idx = find_or_add(tag);
    if (idx < 0) return;
    Tag *t = &s_tags[idx];
    if (delta >= 0) {
        uint64_t d = (uint64_t)delta;
        t->current_bytes   += d;
        t->total_allocated += d;
        t->alloc_count++;
        s_grand_total += d;
    } else {
        uint64_t d = (uint64_t)(-delta);
        if (d > t->current_bytes) d = t->current_bytes;
        t->current_bytes -= d;
        t->total_freed   += d;
        t->free_count++;
        if (d > s_grand_total) d = s_grand_total;
        s_grand_total -= d;
    }
    if (t->current_bytes > t->peak_bytes) t->peak_bytes = t->current_bytes;
    if (s_grand_total > s_grand_peak)     s_grand_peak  = s_grand_total;
}

void jce_mem_profiler_record_alloc(const char *tag, uint64_t bytes)
{
    if (bytes > (uint64_t)INT64_MAX) bytes = (uint64_t)INT64_MAX;
    jce_mem_profiler_record(tag, (int64_t)bytes);
}

void jce_mem_profiler_record_free(const char *tag, uint64_t bytes)
{
    if (bytes > (uint64_t)INT64_MAX) bytes = (uint64_t)INT64_MAX;
    jce_mem_profiler_record(tag, -(int64_t)bytes);
}

uint32_t jce_mem_profiler_snapshot(JceMemTagSnapshot *out, uint32_t max)
{
    if (!out || max == 0) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_tag_count && n < max; ++i) {
        if (!s_tags[i].used) continue;
        strncpy(out[n].tag, s_tags[i].name, JCE_MEM_TAG_NAME_LEN - 1);
        out[n].tag[JCE_MEM_TAG_NAME_LEN - 1] = '\0';
        out[n].current_bytes   = s_tags[i].current_bytes;
        out[n].peak_bytes      = s_tags[i].peak_bytes;
        out[n].total_allocated = s_tags[i].total_allocated;
        out[n].total_freed     = s_tags[i].total_freed;
        out[n].alloc_count     = s_tags[i].alloc_count;
        out[n].free_count      = s_tags[i].free_count;
        n++;
    }
    return n;
}

uint64_t jce_mem_profiler_total_current(void) { return s_grand_total; }
uint64_t jce_mem_profiler_total_peak(void)    { return s_grand_peak; }

void jce_mem_profiler_reset_peaks(void)
{
    for (uint32_t i = 0; i < s_tag_count; ++i)
        if (s_tags[i].used) s_tags[i].peak_bytes = s_tags[i].current_bytes;
    s_grand_peak = s_grand_total;
}
