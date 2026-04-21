/*
 * jce_miniaudio_opus_backend.h
 *
 * Plugs Ogg-Opus decoding into miniaudio via the
 * ma_decoding_backend_vtable extension point, so ma_decoder /
 * ma_engine handle .opus files transparently alongside the built-in
 * dr_wav / dr_mp3 / dr_flac / stb_vorbis decoders.
 *
 * Royalty-free codec (RFC 6716 + RFC 7845).
 */

#ifndef JCE_MINIAUDIO_OPUS_BACKEND_H
#define JCE_MINIAUDIO_OPUS_BACKEND_H

#include <miniaudio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Singleton vtable to be wired into ma_decoder_config::ppCustomBackendVTables. */
extern const ma_decoding_backend_vtable g_jce_ma_opus_backend_vtable;

#ifdef __cplusplus
}
#endif

#endif /* JCE_MINIAUDIO_OPUS_BACKEND_H */
