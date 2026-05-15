/*
 * jce_mem_profiler_gpu.h  GPU memory accounting buckets.
 *
 * Complements jce_mem_profiler (CPU tags) with GPU-side categories:
 *   - Vertex buffers
 *   - Index buffers
 *   - Textures
 *   - Render targets
 *   - Shaders / pipeline state
 *   - Uniform / constant buffers
 *   - Compute buffers
 *
 * Each bucket carries (current_bytes, peak_bytes).  Renderer hooks
 * call record_alloc/free with the appropriate kind whenever a GPU
 * resource is created/destroyed.  The editor's Memory Profiler
 * panel reads the snapshot for its GPU pane.
 *
 * Layer: os/core (Layer 1) — public.
 */

#ifndef JCE_MEM_PROFILER_GPU_H
#define JCE_MEM_PROFILER_GPU_H

#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_GPU_MEM_VERTEX     = 0,
    JCE_GPU_MEM_INDEX      = 1,
    JCE_GPU_MEM_TEXTURE    = 2,
    JCE_GPU_MEM_RENDER_TGT = 3,
    JCE_GPU_MEM_SHADER     = 4,
    JCE_GPU_MEM_UNIFORM    = 5,
    JCE_GPU_MEM_COMPUTE    = 6,
    JCE_GPU_MEM__COUNT     = 7,
} JceGpuMemKind;

typedef struct {
    uint64_t current_bytes;
    uint64_t peak_bytes;
    uint64_t total_allocated;
    uint32_t alloc_count;
    uint32_t free_count;
} JceGpuMemBucket;

JCE_API void     jce_gpu_mem_record_alloc(JceGpuMemKind k, uint64_t bytes);
JCE_API void     jce_gpu_mem_record_free (JceGpuMemKind k, uint64_t bytes);

JCE_API const JceGpuMemBucket *jce_gpu_mem_bucket(JceGpuMemKind k);
JCE_API uint64_t jce_gpu_mem_total_current(void);
JCE_API uint64_t jce_gpu_mem_total_peak   (void);
JCE_API void     jce_gpu_mem_reset_peaks  (void);

JCE_API const char *jce_gpu_mem_kind_name(JceGpuMemKind k);

JCE_EXTERN_C_END

#endif /* JCE_MEM_PROFILER_GPU_H */
