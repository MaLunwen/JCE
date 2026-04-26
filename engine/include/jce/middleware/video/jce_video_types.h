/*
 * jce_video_types.h  Lightweight video type definitions.
 *
 * Provides only the JceVideo handle type and invalid constant.
 * Include this instead of jce_video.h when you only need the type.
 */

#ifndef JCE_VIDEO_TYPES_H
#define JCE_VIDEO_TYPES_H


#include <jce/os/core/jce_defs.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Opaque handle (0 = invalid). */
typedef uint32_t JceVideo;

#define JCE_VIDEO_INVALID 0

JCE_EXTERN_C_END

#endif /* JCE_VIDEO_TYPES_H */
