/*
 * jce_miniaudio_opus_backend.h  (engine-internal)
 *
 * Plugs Ogg-Opus decoding into miniaudio via the
 * ma_decoding_backend_vtable extension point, so ma_decoder /
 * ma_engine handle .opus files transparently alongside the built-in
 * dr_wav / dr_mp3 / dr_flac / stb_vorbis decoders.
 *
 * NOTE: This header intentionally exposes the miniaudio ABI and is
 * therefore NOT part of JCE's public API.  Application code must
 * never include this file directly — use the audio facade in
 * <jce/middleware/audio/jce_audio.h> instead.
 *
 * Royalty-free codec (RFC 6716 + RFC 7845).
 */

#ifndef JCE_MINIAUDIO_OPUS_BACKEND_H
#define JCE_MINIAUDIO_OPUS_BACKEND_H


#include <jce/os/core/jce_defs.h>

#include <miniaudio.h>

JCE_EXTERN_C_BEGIN

/* Singleton vtable to be wired into ma_decoder_config::ppCustomBackendVTables. */
extern const ma_decoding_backend_vtable g_jce_ma_opus_backend_vtable;

JCE_EXTERN_C_END

#endif /* JCE_MINIAUDIO_OPUS_BACKEND_H */
