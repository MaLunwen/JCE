/*
 * jce_asset_types.h  Core asset type definitions (OS layer).
 *
 * Provides only the basic asset handle types (audio, etc.) at the OS level.
 * These are fundamental data types needed by resource layer without
 * pulling in middleware dependencies.
 *
 * Layer: OS/core
 */

#ifndef JCE_ASSET_TYPES_H
#define JCE_ASSET_TYPES_H


#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Opaque audio handles (0 = invalid). */
typedef uint32_t JceSound;
typedef uint32_t JceVoice;

#define JCE_SOUND_INVALID 0
#define JCE_VOICE_INVALID 0

JCE_EXTERN_C_END

#endif /* JCE_ASSET_TYPES_H */
