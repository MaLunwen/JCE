/*
 * api_audio.h  Audio system.
 *
 * Sound playback, music streaming, spatial audio.
 * Built on SoLoud.
 */

#ifndef JCE_API_AUDIO_H
#define JCE_API_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/audio/jce_audio_file.h>
#include <jce/os/core/jce_asset_types.h>

/* Completed 2026-08-31.  These headers export JCE_API symbols and were
 * reachable from NO umbrella, so §4's promise -- `#include <jce/api.h>`
 * gives you the whole engine -- did not hold for them.  Several are named
 * in §4's own table by capability.
 */
#include <jce/middleware/audio/jce_audio_duck_pump.h>
#include <jce/middleware/audio/jce_audio_ecs.h>
#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/middleware/audio/jce_audio_mixer_config.h>
#include <jce/middleware/audio/jce_audio_occlusion.h>
#include <jce/middleware/audio/jce_m4a_decode.h>
#include <jce/middleware/audio/jce_music.h>
#include <jce/middleware/audio/jce_reverb_zones.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_AUDIO_H */
