/*
 * jce_anim_clip_events.h  JSON sidecar for AnimationClip events.
 *
 * Loads/saves event arrays compatible with `jce_anim_player_set_events`
 * (existing B2 callback infrastructure) from a `.clip.events.json`
 * file co-located with the clip.  Each event has a name + time +
 * optional float / int / string payload.
 *
 * Layer: animation (Layer 4) — public.
 */

#ifndef JCE_ANIM_CLIP_EVENTS_H
#define JCE_ANIM_CLIP_EVENTS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_ANIM_CLIP_EVENTS_MAX 64
#define JCE_ANIM_EVENT_NAME_LEN  48
#define JCE_ANIM_EVENT_STR_LEN   64

typedef struct {
    char     name[JCE_ANIM_EVENT_NAME_LEN];
    float    time_seconds;
    float    float_param;
    int32_t  int_param;
    char     string_param[JCE_ANIM_EVENT_STR_LEN];
} JceAnimClipEvent;

typedef struct {
    JceAnimClipEvent events[JCE_ANIM_CLIP_EVENTS_MAX];
    uint32_t         count;
} JceAnimClipEventList;

JCE_API void jce_anim_clip_events_init(JceAnimClipEventList *list);
JCE_API bool jce_anim_clip_events_add (JceAnimClipEventList *list,
                                          const JceAnimClipEvent *ev);

/* Sort events by ascending time.  Call after batch-adding before
 * persisting / firing. */
JCE_API void jce_anim_clip_events_sort(JceAnimClipEventList *list);

/* Return count of events in (t_prev, t_now]; optional `out_indices`
 * is filled with the matching indices (caller-supplied cap). */
JCE_API uint32_t jce_anim_clip_events_in_range(const JceAnimClipEventList *list,
                                                 float t_prev, float t_now,
                                                 uint32_t *out_indices,
                                                 uint32_t cap);

JCE_API bool jce_anim_clip_events_load_json(JceAnimClipEventList *list,
                                              const char *path);
JCE_API bool jce_anim_clip_events_save_json(const JceAnimClipEventList *list,
                                              const char *path);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_CLIP_EVENTS_H */
