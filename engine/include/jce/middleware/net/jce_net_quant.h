/*
 * jce_net_quant.h — P4-D.3 wire-format quantization helpers.
 *
 * Deterministic bit-twiddling primitives used by transform snapshot
 * encoding to compress per-entry payloads:
 *
 *   - fp16: IEEE-754 half precision packing / unpacking for positions
 *     and other bounded-range floats.
 *
 *   - Smallest-three quaternion: drop the largest |component|,
 *     store a 2-bit index plus three 10-bit signed mantissas in a
 *     single 32-bit word.  The dropped component is reconstructed
 *     on the receiver via sqrt(1 - x^2 - y^2 - z^2).  Sign is
 *     normalised so the dropped component is always >= 0 (using
 *     the q == -q identity of unit quaternions).
 *
 * All helpers are pure / stateless / endianness-independent at the
 * jce_math level; the raw 16/32-bit words must still be written to
 * the wire by the caller using a little-endian writer.
 */

#ifndef JCE_NET_QUANT_H
#define JCE_NET_QUANT_H

#include <jce/os/core/jce_math.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------- scalar fp16 ------- */

JCE_API uint16_t jce_quant_f32_to_f16(float v);
JCE_API float    jce_quant_f16_to_f32(uint16_t v);

/* ------- vec3 in fp16 (6 bytes) ------- */

JCE_API void     jce_quant_pack_vec3_f16  (jce_vec3 v, uint16_t out[3]);
JCE_API jce_vec3 jce_quant_unpack_vec3_f16(const uint16_t in[3]);

/* ------- smallest-three quaternion (4 bytes / 32 bits) ------- */

JCE_API uint32_t jce_quant_pack_quat_st3  (jce_quat q);
JCE_API jce_quat jce_quant_unpack_quat_st3(uint32_t packed);

#ifdef __cplusplus
}
#endif
#endif /* JCE_NET_QUANT_H */
