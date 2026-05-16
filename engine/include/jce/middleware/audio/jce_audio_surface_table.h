/*
 * jce_audio_surface_table.h  Footstep surface → clip lookup table.
 *
 * Maps a surface id (e.g. assigned per physics material or terrain
 * layer) to a weighted random pool of clip paths.  Callers compute
 * the surface id from a raycast / contact + look up the next clip
 * to play via jce_audio_surface_resolve.
 *
 * Layer: middleware/audio (Layer 4) — public.
 */

#ifndef JCE_AUDIO_SURFACE_TABLE_H
#define JCE_AUDIO_SURFACE_TABLE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SURFACE_CLIPS_PER_ENTRY 8
#define JCE_SURFACE_TABLE_MAX       64
#define JCE_SURFACE_CLIP_PATH_LEN   128

typedef struct {
    char     path[JCE_SURFACE_CLIP_PATH_LEN];
    float    weight;
    float    pitch_min;
    float    pitch_max;
    float    volume_min;
    float    volume_max;
} JceSurfaceClip;

typedef struct {
    uint32_t        surface_id;
    char            label[48];          /* human-readable */
    JceSurfaceClip  clips[JCE_SURFACE_CLIPS_PER_ENTRY];
    uint32_t        clip_count;
    bool            active;
} JceSurfaceEntry;

/* Lifecycle. */
JCE_API void jce_audio_surface_clear(void);
JCE_API bool jce_audio_surface_add(uint32_t surface_id,
                                     const char *label,
                                     const JceSurfaceClip *clips,
                                     uint32_t clip_count);
JCE_API const JceSurfaceEntry *jce_audio_surface_get(uint32_t surface_id);
JCE_API uint32_t jce_audio_surface_count(void);

/* Pick a clip from the surface's pool using weighted random.
 * `rng_state` is a caller-owned LCG state (lets game code seed
 * deterministically per footstep). */
JCE_API bool jce_audio_surface_resolve(uint32_t  surface_id,
                                         uint32_t *rng_state,
                                         JceSurfaceClip *out_clip,
                                         float    *out_pitch,
                                         float    *out_volume);

/* JSON I/O. */
JCE_API bool jce_audio_surface_load_json(const char *path);
JCE_API bool jce_audio_surface_save_json(const char *path);

JCE_EXTERN_C_END

#endif /* JCE_AUDIO_SURFACE_TABLE_H */
