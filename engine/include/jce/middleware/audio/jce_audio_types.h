/*
 * jce_audio_types.h  Lightweight audio type definitions.
 *
 * Provides only the JceSound/JceVoice handle types and invalid constants.
 * Include this instead of jce_audio.h when you only need the types.
 *
 * NOTE: This header now forwards to jce_asset_types.h in OS layer.
 * The types have been moved down to avoid layering violations.
 */

#ifndef JCE_AUDIO_TYPES_H
#define JCE_AUDIO_TYPES_H

#include <jce/os/core/jce_asset_types.h>

#endif /* JCE_AUDIO_TYPES_H */
