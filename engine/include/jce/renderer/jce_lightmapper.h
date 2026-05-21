/*
 * jce_lightmapper.h  Engine-side GI / direct-light baker (Sprint 4 #15).
 *
 * Exposes a single batch-bake entry point that takes a flat list of
 * occluder/receiver primitives and a flat list of lights, and writes
 * an RGBA8 lightmap of (width × height) representing the irradiance at
 * the receiver plane.  The current implementation handles directional
 * + point lights with shadowing against axis-aligned-box and sphere
 * occluders; spotlights are clipped to a cone.  No SH / probe support
 * yet; that is planned for Sprint 5.
 *
 * The baker is intentionally CPU-only and self-contained (no Embree
 * dependency) so that the editor's lightmap panel can drive it inline.
 * For full progressive baking the editor uses jce_thread_create to
 * background a worker.
 */

#ifndef JCE_LIGHTMAPPER_H
#define JCE_LIGHTMAPPER_H

#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_LM_OCC_BOX    = 0,   /* center + extent */
    JCE_LM_OCC_SPHERE = 1,   /* center + radius */
} JceLightmapOccluderKind;

typedef struct {
    JceLightmapOccluderKind kind;
    float center[3];
    float extent_or_radius[3]; /* box: half-extents on x,y,z; sphere: r in [0] */
} JceLightmapOccluder;

typedef enum {
    JCE_LM_LIGHT_DIRECTIONAL = 0,
    JCE_LM_LIGHT_POINT       = 1,
    JCE_LM_LIGHT_SPOT        = 2,
} JceLightmapLightKind;

typedef struct {
    JceLightmapLightKind kind;
    float position[3];      /* point/spot */
    float direction[3];     /* directional/spot, normalized */
    float color[3];         /* linear RGB */
    float intensity;        /* multiplier */
    float range;            /* point/spot, 0 = infinite */
    float cone_cos;         /* spot: cosine of half-angle */
} JceLightmapLight;

typedef struct {
    /* Receiver plane in world space (e.g. terrain "floor"). */
    float origin[3];        /* corner */
    float u_axis[3];        /* width  basis */
    float v_axis[3];        /* height basis */
    float normal[3];        /* surface normal (for cosine term) */
    int   width, height;    /* texel resolution */
    int   shadow_samples;   /* per-light occluder samples (1 for hard shadow) */
    float ambient[3];       /* additive ambient floor */
} JceLightmapBakeDesc;

/* Bake direct lighting; out_rgba must be width*height*4 bytes.
   Returns 0 on success, negative on failure. */
JCE_API int JCE_CALL
jce_lightmapper_bake_direct(const JceLightmapBakeDesc      *desc,
                            const JceLightmapOccluder *occ, int occ_count,
                            const JceLightmapLight    *lights, int light_count,
                            uint8_t                  *out_rgba);

/*
 * Bake SH9 (order-2 real spherical harmonics) for an array of probe positions.
 *
 * For each probe, hemisphere samples are drawn (Monte-Carlo with cosine
 * weighting) against the provided occluder list; surviving rays accumulate
 * per-light irradiance projected onto the 9 SH basis functions.
 *
 *   positions    — probe positions in world space, shape [probe_count][3]
 *   probe_count  — number of probes
 *   occ, occ_count   — scene occluder list (reuse from lightmap bake)
 *   lights, light_count — scene light list
 *   sample_count — hemisphere samples per probe (recommended: 256–2048)
 *   out_sh9      — output array [probe_count][9][3] (9 coeffs × RGB)
 *
 * Returns 0 on success, negative on failure (NULL pointer / zero count).
 */
JCE_API int JCE_CALL
jce_lightmapper_bake_sh9(const float           (*positions)[3],
                         int                    probe_count,
                         const JceLightmapOccluder *occ,  int occ_count,
                         const JceLightmapLight    *lights, int light_count,
                         int                    sample_count,
                         float                (*out_sh9)[9][3]);

JCE_EXTERN_C_END

#endif /* JCE_LIGHTMAPPER_H */
