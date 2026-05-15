/*
 * jce_mem_profiler_gpu.c  GPU memory bucket accounting.
 */

#include <jce/os/core/jce_mem_profiler_gpu.h>

#include <string.h>

static JceGpuMemBucket s_buckets[JCE_GPU_MEM__COUNT];
static uint64_t        s_grand_current;
static uint64_t        s_grand_peak;

static const char *s_names[JCE_GPU_MEM__COUNT] = {
    "Vertex Buffers",
    "Index Buffers",
    "Textures",
    "Render Targets",
    "Shaders",
    "Uniform Buffers",
    "Compute Buffers",
};

void jce_gpu_mem_record_alloc(JceGpuMemKind k, uint64_t bytes)
{
    if (k >= JCE_GPU_MEM__COUNT) return;
    JceGpuMemBucket *b = &s_buckets[k];
    b->current_bytes   += bytes;
    b->total_allocated += bytes;
    b->alloc_count++;
    if (b->current_bytes > b->peak_bytes) b->peak_bytes = b->current_bytes;
    s_grand_current += bytes;
    if (s_grand_current > s_grand_peak) s_grand_peak = s_grand_current;
}

void jce_gpu_mem_record_free(JceGpuMemKind k, uint64_t bytes)
{
    if (k >= JCE_GPU_MEM__COUNT) return;
    JceGpuMemBucket *b = &s_buckets[k];
    if (bytes > b->current_bytes) bytes = b->current_bytes;
    b->current_bytes -= bytes;
    b->free_count++;
    if (bytes > s_grand_current) bytes = s_grand_current;
    s_grand_current -= bytes;
}

const JceGpuMemBucket *jce_gpu_mem_bucket(JceGpuMemKind k)
{
    if (k >= JCE_GPU_MEM__COUNT) return NULL;
    return &s_buckets[k];
}

uint64_t jce_gpu_mem_total_current(void) { return s_grand_current; }
uint64_t jce_gpu_mem_total_peak   (void) { return s_grand_peak;    }

void jce_gpu_mem_reset_peaks(void)
{
    for (int i = 0; i < JCE_GPU_MEM__COUNT; ++i)
        s_buckets[i].peak_bytes = s_buckets[i].current_bytes;
    s_grand_peak = s_grand_current;
}

const char *jce_gpu_mem_kind_name(JceGpuMemKind k)
{
    if (k >= JCE_GPU_MEM__COUNT) return "?";
    return s_names[k];
}
