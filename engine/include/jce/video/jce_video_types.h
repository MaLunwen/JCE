/*
 * jce_video_types.h  Lightweight video type definitions.
 *
 * Provides only the JceVideo handle type and invalid constant.
 * Include this instead of jce_video.h when you only need the type.
 */

#ifndef JCE_VIDEO_TYPES_H
#define JCE_VIDEO_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle (0 = invalid). */
typedef uint32_t JceVideo;

#define JCE_VIDEO_INVALID 0

#ifdef __cplusplus
}
#endif

#endif /* JCE_VIDEO_TYPES_H */
