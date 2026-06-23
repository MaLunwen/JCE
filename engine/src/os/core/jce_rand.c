/*
 * jce_rand.c -- PCG32 deterministic RNG (see jce_rand.h).
 *
 * PCG32 (Melissa O'Neill, pcg-random.org): a 64-bit LCG state run through an
 * xorshift+rotate output permutation.  Small, fast, and statistically far
 * better than a bare xorshift, while staying trivially reproducible.
 */

#include <jce/os/core/jce_rand.h>

#define PCG_MUL 6364136223846793005ULL

void jce_rng_seed(JceRng *r, uint64_t seed, uint64_t seq)
{
    if (!r) return;
    r->state = 0u;
    r->inc   = (seq << 1u) | 1u;        /* must be odd */
    (void)jce_rng_u32(r);
    r->state += seed;
    (void)jce_rng_u32(r);
}

uint32_t jce_rng_u32(JceRng *r)
{
    if (!r) return 0u;
    uint64_t old = r->state;
    r->state = old * PCG_MUL + (r->inc | 1u);
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    /* rotate right by `rot`; the (-rot) & 31 form avoids UB at rot == 0. */
    return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
}

uint64_t jce_rng_u64(JceRng *r)
{
    uint64_t hi = (uint64_t)jce_rng_u32(r);
    uint64_t lo = (uint64_t)jce_rng_u32(r);
    return (hi << 32) | lo;
}

float jce_rng_f32(JceRng *r)
{
    /* top 24 bits → [0,1) with a clean float mantissa. */
    return (float)(jce_rng_u32(r) >> 8) * (1.0f / 16777216.0f);
}

double jce_rng_f64(JceRng *r)
{
    /* 53 bits → [0,1). */
    uint64_t v = jce_rng_u64(r) >> 11;
    return (double)v * (1.0 / 9007199254740992.0);
}

float jce_rng_range_f(JceRng *r, float lo, float hi)
{
    return lo + (hi - lo) * jce_rng_f32(r);
}

int jce_rng_range_i(JceRng *r, int lo, int hi)
{
    if (hi <= lo) return lo;
    /* Unbiased: reject the top partial bucket. */
    uint32_t range = (uint32_t)(hi - lo) + 1u;     /* inclusive span */
    uint32_t limit = (uint32_t)(0u - range) % range; /* = 2^32 % range */
    uint32_t x;
    do { x = jce_rng_u32(r); } while (x < limit);
    return lo + (int)(x % range);
}

bool jce_rng_chance(JceRng *r, float p)
{
    if (p <= 0.0f) return false;
    if (p >= 1.0f) return true;
    return jce_rng_f32(r) < p;
}
