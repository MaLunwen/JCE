/*
 * jce_net_bytes.h — little-endian wire codec shared by the net middleware.
 *
 * PRIVATE engine-internal header (NOT under engine/include).  Every net
 * TU used to carry its own copy of these primitives; they are collected
 * here so one definition backs the whole layer.
 *
 * Three flavours, matching the three call shapes the layer actually uses:
 *
 *   1. Fixed-offset primitives — jce_net_put_* / jce_net_get_*.
 *      Raw pointer + caller-computed offset, NO bounds checking.  The
 *      caller owns the sizing contract (fixed-layout records).
 *
 *   2. Advancing cursor — jce_net_wr_* / jce_net_rd_*.
 *      Walk a `uint8_t *` / `const uint8_t *` cursor over a buffer the
 *      caller has already sized.  Still NO bounds checking.
 *
 *   3. Checked buffers — JceNetWBuf (growable writer) and JceNetRBuf
 *      (bounds-checked reader).  These carry an `ok` flag: once a write
 *      allocation or a read bound fails the flag latches false and every
 *      subsequent operation is a no-op, so call sites can batch a whole
 *      packet and test `ok` once at the end.
 *
 * All integers travel little-endian regardless of host endianness;
 * floats travel as their IEEE-754 bit pattern in a u32 (memcpy, never a
 * type-punning cast).
 *
 * Layer: middleware/net internal.  Depends only on the C library plus
 * the engine's internal allocation macros (growable writer only).
 */

#ifndef JCE_NET_BYTES_H
#define JCE_NET_BYTES_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "os/core/jce_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* 1. Fixed-offset primitives (no bounds checking)                     */
/* ================================================================== */

static inline void jce_net_put_u8(uint8_t *p, uint8_t v)
{
    p[0] = v;
}

static inline void jce_net_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)( v        & 0xFFu);
    p[1] = (uint8_t)((v >> 8)  & 0xFFu);
}

static inline void jce_net_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)( v        & 0xFFu);
    p[1] = (uint8_t)((v >> 8)  & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint8_t jce_net_get_u8(const uint8_t *p)
{
    return p[0];
}

static inline uint16_t jce_net_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t jce_net_get_u32(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Floats travel as their IEEE-754 bit pattern in a u32, byte-ordered LE.
 * memcpy avoids the strict-aliasing UB of a float*->uint32_t* cast. */
static inline void jce_net_put_f32(uint8_t *p, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    jce_net_put_u32(p, bits);
}

static inline float jce_net_get_f32(const uint8_t *p)
{
    uint32_t bits = jce_net_get_u32(p);
    float v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

/* ================================================================== */
/* 2. Advancing cursor over a caller-sized buffer (no bounds checking)  */
/* ================================================================== */

static inline void jce_net_wr_u8(uint8_t **p, uint8_t v)
{
    jce_net_put_u8(*p, v);
    *p += 1;
}

static inline void jce_net_wr_u16(uint8_t **p, uint16_t v)
{
    jce_net_put_u16(*p, v);
    *p += 2;
}

static inline void jce_net_wr_u32(uint8_t **p, uint32_t v)
{
    jce_net_put_u32(*p, v);
    *p += 4;
}

static inline uint8_t jce_net_rd_u8(const uint8_t **p)
{
    uint8_t v = jce_net_get_u8(*p);
    *p += 1;
    return v;
}

static inline uint16_t jce_net_rd_u16(const uint8_t **p)
{
    uint16_t v = jce_net_get_u16(*p);
    *p += 2;
    return v;
}

static inline uint32_t jce_net_rd_u32(const uint8_t **p)
{
    uint32_t v = jce_net_get_u32(*p);
    *p += 4;
    return v;
}

/* ================================================================== */
/* 3a. Growable little-endian writer                                   */
/* ================================================================== */

/* `buf` is heap memory owned by the caller — free it with JCE_FREE once
 * the packet has been handed to the transport.  `ok` latches false on the
 * first allocation failure; every later write is then a no-op. */
typedef struct JceNetWBuf {
    uint8_t *buf;
    uint32_t size;
    uint32_t cap;
    bool     ok;
} JceNetWBuf;

/* Initialiser for a fresh writer: `JceNetWBuf w = JCE_NET_WBUF_INIT;` */
#define JCE_NET_WBUF_INIT { NULL, 0u, 0u, true }

/* Grow so that `need` more bytes fit.  Doubling from a 256 B floor.
 * Returns false (and latches ok=false) when the allocation fails. */
static inline bool jce_net_wbuf_reserve(JceNetWBuf *w, uint32_t need)
{
    if (!w->ok) return false;
    if (w->size + need <= w->cap) return true;
    uint32_t nc = w->cap ? w->cap : 256u;
    while (nc < w->size + need) nc *= 2u;
    uint8_t *nb = (uint8_t *)JCE_REALLOC(w->buf, nc);
    if (!nb) { w->ok = false; return false; }
    w->buf = nb;
    w->cap = nc;
    return true;
}

static inline void jce_net_w_bytes(JceNetWBuf *w, const void *p, uint32_t n)
{
    if (!jce_net_wbuf_reserve(w, n)) return;
    memcpy(w->buf + w->size, p, n);
    w->size += n;
}

static inline void jce_net_w_u8(JceNetWBuf *w, uint8_t v)
{
    jce_net_w_bytes(w, &v, 1u);
}

static inline void jce_net_w_u16(JceNetWBuf *w, uint16_t v)
{
    uint8_t b[2];
    jce_net_put_u16(b, v);
    jce_net_w_bytes(w, b, 2u);
}

static inline void jce_net_w_u32(JceNetWBuf *w, uint32_t v)
{
    uint8_t b[4];
    jce_net_put_u32(b, v);
    jce_net_w_bytes(w, b, 4u);
}

static inline void jce_net_w_f32(JceNetWBuf *w, float v)
{
    uint8_t b[4];
    jce_net_put_f32(b, v);
    jce_net_w_bytes(w, b, 4u);
}

/* ================================================================== */
/* 3b. Bounds-checked little-endian reader                             */
/* ================================================================== */

/* `buf` is borrowed (typically the received datagram).  `ok` latches
 * false on the first short read. */
typedef struct JceNetRBuf {
    const uint8_t *buf;
    uint32_t       size;
    uint32_t       cursor;
    bool           ok;
} JceNetRBuf;

/* Initialiser over a received packet:
 *   JceNetRBuf r = JCE_NET_RBUF_INIT(data, size); */
#define JCE_NET_RBUF_INIT(p, n) { (const uint8_t *)(p), (n), 0u, true }

/* Overflow-safe bound: a live reader guarantees cursor <= size, so
 * `n > size - cursor` cannot wrap (unlike `cursor + n > size`, which is
 * 32-bit modular and fails open when n is near UINT32_MAX). */
static inline bool jce_net_r_bytes(JceNetRBuf *r, void *dst, uint32_t n)
{
    if (!r->ok || r->cursor > r->size || n > r->size - r->cursor) {
        r->ok = false;
        return false;
    }
    memcpy(dst, r->buf + r->cursor, n);
    r->cursor += n;
    return true;
}

static inline bool jce_net_r_u8(JceNetRBuf *r, uint8_t *out)
{
    return jce_net_r_bytes(r, out, 1u);
}

static inline bool jce_net_r_u16(JceNetRBuf *r, uint16_t *out)
{
    uint8_t b[2];
    if (!jce_net_r_bytes(r, b, 2u)) return false;
    *out = jce_net_get_u16(b);
    return true;
}

static inline bool jce_net_r_u32(JceNetRBuf *r, uint32_t *out)
{
    uint8_t b[4];
    if (!jce_net_r_bytes(r, b, 4u)) return false;
    *out = jce_net_get_u32(b);
    return true;
}

static inline bool jce_net_r_f32(JceNetRBuf *r, float *out)
{
    uint8_t b[4];
    if (!jce_net_r_bytes(r, b, 4u)) return false;
    *out = jce_net_get_f32(b);
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_NET_BYTES_H */
