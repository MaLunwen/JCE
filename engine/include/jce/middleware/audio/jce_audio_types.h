/*
 * jce_audio_types.h  Lightweight audio type definitions.
 *
 * Provides only the JceSound/JceVoice handle types and invalid constants.
 * Include this instead of jce_audio.h when you only need the types.
 */

#ifndef JCE_AUDIO_TYPES_H
#define JCE_AUDIO_TYPES_H


#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Opaque handles (0 = invalid). */
typedef uint32_t JceSound;
typedef uint32_t JceVoice;

#define JCE_SOUND_INVALID 0
#define JCE_VOICE_INVALID 0

JCE_EXTERN_C_END

#endif /* JCE_AUDIO_TYPES_H */
