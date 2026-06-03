/*
 * jce_pick_id.h  RGBA8 object-ID picking protocol.
 */

#ifndef JCE_PICK_ID_H
#define JCE_PICK_ID_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Key 0 is reserved for "no hit"; valid keys are 1..0x00FFFFFF. */
JCE_API bool     jce_scene_pick_encode_rgba(uint64_t key,
                                            uint8_t out_rgba[4]);
JCE_API uint64_t jce_scene_pick_decode_rgba(const uint8_t rgba[4]);

JCE_EXTERN_C_END

#endif /* JCE_PICK_ID_H */
