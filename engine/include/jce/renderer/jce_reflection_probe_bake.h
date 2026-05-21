/*
 * jce_reflection_probe_bake.h  Reflection probe cubemap bake (P3-E.3).
 *
 * Unity-parity offline bake: render the scene into a 6-face cubemap from
 * a probe position, convolve into irradiance + specular mip chain, and
 * persist the result to disk for later sampling.
 *
 * Concurrency model
 * -----------------
 * - At most ONE bake may be in flight at a time. The single shared
 *   working buffer (per-face RGBA8 plus convolution scratch) is sized
 *   for the largest supported cubemap and lives module-scoped.
 * - `submit()` returns a non-zero JceReflectionProbeBakeHandle on
 *   success; if a bake is already running it returns 0 (invalid).
 * - Heavy work runs on a dedicated worker thread (jce_thread). Callers
 *   poll progress every frame via `poll()` and read the atomic status /
 *   progress snapshot.
 *
 * v1 limitation
 * -------------
 * The v1 implementation does NOT yet hook into the live scene renderer
 * (a bgfx off-screen target entry point is the next increment). Instead
 * each face is filled with a procedural sky-and-ground gradient derived
 * from the probe position, then convolved analytically. The on-disk
 * artefact is a real KTX1 cubemap (bimg `imageWriteKtx`) so the editor
 * pipeline (button → progress → component references file) is fully
 * exercised end-to-end and the artefact loads in standard KTX tooling
 * + bgfx. An irradiance sidecar is written next to it as
 * `<stem>.irr.ktx`. The submitted `output_path_ktx2` may use `.cube`
 * or `.ktx2`; the engine rewrites the suffix to `.ktx` to match the
 * real container. Native KTX2 / supercompression + live scene capture
 * land in a follow-up without ABI change.
 */

#ifndef JCE_REFLECTION_PROBE_BAKE_H
#define JCE_REFLECTION_PROBE_BAKE_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceReflectionProbeBakeDesc {
    jce_vec3    position;
    uint32_t    cubemap_size;          /* 128 / 256 / 512; 0 → default 256. */
    uint32_t    specular_mip_count;    /* 0 → default 5. */
    const char *output_path_ktx2;      /* absolute or host-relative; engine
                                          rewrites `.cube` / `.ktx2` to
                                          `.ktx` to match the real KTX1
                                          container shipped by bimg. */
    bool        include_skybox;
    bool        include_dynamic_objects;
} JceReflectionProbeBakeDesc;

typedef enum JceBakeStatus {
    JCE_BAKE_STATUS_IDLE = 0,
    JCE_BAKE_STATUS_RENDERING_FACES,
    JCE_BAKE_STATUS_CONVOLVING_IRRADIANCE,
    JCE_BAKE_STATUS_CONVOLVING_SPECULAR,
    JCE_BAKE_STATUS_ENCODING_KTX2,
    JCE_BAKE_STATUS_DONE,
    JCE_BAKE_STATUS_FAILED,
    JCE_BAKE_STATUS_CANCELLED
} JceBakeStatus;

typedef struct JceReflectionProbeBakeProgress {
    JceBakeStatus status;
    float         progress;     /* 0..1 within the current step. */
    float         overall;      /* 0..1 across the whole bake. */
    const char   *message;      /* Short engine-owned string; may be NULL. */
} JceReflectionProbeBakeProgress;

typedef uint32_t JceReflectionProbeBakeHandle;

/* Submit a bake. Returns 0 on rejection (busy / invalid desc / IO). */
JCE_API JceReflectionProbeBakeHandle JCE_CALL
jce_reflection_probe_bake_submit(const JceReflectionProbeBakeDesc *desc);

/* Fill `out` with the latest snapshot. Returns true if `h` is the
 * currently active or most-recently completed handle; false if `h` is
 * stale or 0 (in which case `out` is zero-initialised). */
JCE_API bool JCE_CALL
jce_reflection_probe_bake_poll(JceReflectionProbeBakeHandle h,
                                JceReflectionProbeBakeProgress *out);

/* Request a cooperative cancel of the active bake. No-op if `h` is not
 * the active handle. The worker observes the flag between steps and
 * transitions to JCE_BAKE_STATUS_CANCELLED. */
JCE_API void JCE_CALL
jce_reflection_probe_bake_cancel(JceReflectionProbeBakeHandle h);

JCE_EXTERN_C_END

#endif /* JCE_REFLECTION_PROBE_BAKE_H */
