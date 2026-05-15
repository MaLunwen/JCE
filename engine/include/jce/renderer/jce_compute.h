/*
 * jce_compute.h  Compute shader descriptor abstraction.
 *
 * Data-layer wrapper for dispatching a compute kernel: binding table
 * + workgroup dimensions + push constants.  The actual bgfx::dispatch
 * call lives in a downstream module (B17 wire-up); this module owns
 * the descriptor model so game code + editor can author dispatches
 * without depending on bgfx headers.
 *
 * Mirrors Unity ComputeShader.SetBuffer / SetTexture / SetFloat at
 * the data layer.  Up to 8 buffer bindings + 8 texture bindings + 16
 * float constants per dispatch — enough for everyday compute work
 * (particle update, blur, occlusion query reduction).
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_COMPUTE_H
#define JCE_COMPUTE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_COMPUTE_MAX_BUFFERS  8
#define JCE_COMPUTE_MAX_TEXTURES 8
#define JCE_COMPUTE_MAX_CONSTS   16
#define JCE_COMPUTE_NAME_LEN     32

typedef enum {
    JCE_COMPUTE_ACCESS_READ       = 0,
    JCE_COMPUTE_ACCESS_WRITE      = 1,
    JCE_COMPUTE_ACCESS_READ_WRITE = 2,
} JceComputeAccess;

typedef struct {
    char             name[JCE_COMPUTE_NAME_LEN];
    /* Opaque handle interpreted by the binding layer (typically a
     * bgfx storage buffer index).  Use 0xFFFFFFFFu for unbound. */
    uint32_t         handle;
    uint32_t         stride;          /* bytes per element */
    uint32_t         element_count;
    JceComputeAccess access;
    bool             active;
} JceComputeBufferBinding;

typedef struct {
    char             name[JCE_COMPUTE_NAME_LEN];
    uint32_t         handle;          /* bgfx texture handle */
    uint8_t          mip;
    JceComputeAccess access;
    bool             active;
} JceComputeTextureBinding;

typedef struct {
    char  name[JCE_COMPUTE_NAME_LEN];
    float value[4];                   /* up to vec4 */
    bool  active;
} JceComputeConstant;

typedef struct {
    char     shader_name[JCE_COMPUTE_NAME_LEN];  /* e.g. "cs_particle_update" */
    uint32_t workgroup_x;
    uint32_t workgroup_y;
    uint32_t workgroup_z;
    JceComputeBufferBinding  buffers [JCE_COMPUTE_MAX_BUFFERS];
    JceComputeTextureBinding textures[JCE_COMPUTE_MAX_TEXTURES];
    JceComputeConstant       consts  [JCE_COMPUTE_MAX_CONSTS];
} JceComputeDispatch;

/* Zero-initialise a dispatch struct with workgroup (1,1,1). */
JCE_API void jce_compute_dispatch_init(JceComputeDispatch *d,
                                        const char *shader_name);

/* Slot-based setters.  Returns the slot index on success or -1 on
 * full / invalid descriptor.  Idempotent on duplicate names — calls
 * replace the existing binding. */
JCE_API int  jce_compute_set_buffer (JceComputeDispatch *d,
                                      const char *name,
                                      uint32_t handle,
                                      uint32_t stride,
                                      uint32_t element_count,
                                      JceComputeAccess access);
JCE_API int  jce_compute_set_texture(JceComputeDispatch *d,
                                      const char *name,
                                      uint32_t handle,
                                      uint8_t mip,
                                      JceComputeAccess access);
JCE_API int  jce_compute_set_float  (JceComputeDispatch *d,
                                      const char *name, float v);
JCE_API int  jce_compute_set_vec4   (JceComputeDispatch *d,
                                      const char *name,
                                      float x, float y, float z, float w);

JCE_API void jce_compute_set_workgroup(JceComputeDispatch *d,
                                        uint32_t x, uint32_t y, uint32_t z);

/* Count active bindings for stats / inspector display. */
JCE_API uint32_t jce_compute_active_buffer_count (const JceComputeDispatch *d);
JCE_API uint32_t jce_compute_active_texture_count(const JceComputeDispatch *d);
JCE_API uint32_t jce_compute_active_const_count  (const JceComputeDispatch *d);

JCE_EXTERN_C_END

#endif /* JCE_COMPUTE_H */
