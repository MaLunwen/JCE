/*
 * jce_rand.h -- Deterministic pseudo-random number generator (PCG32).
 *
 * The engine previously had no shared public RNG, so subsystems (foliage,
 * particles, camera shake, weapons, spawners) each hand-rolled an xorshift.
 * This is the canonical replacement: a small, fast, well-distributed,
 * explicitly-seeded generator (O'Neill's PCG32).  Every instance is a plain
 * value type, so callers own their stream and results are fully reproducible
 * across runs and platforms — essential for deterministic gameplay, replays,
 * and unit tests.
 *
 * Layer: OS / Core (Layer 1) — dependency-free.
 */

#ifndef JCE_RAND_H
#define JCE_RAND_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* PCG32 state.  Initialise with jce_rng_seed before use (a zeroed struct is
 * valid but always yields the same default stream). */
typedef struct {
    uint64_t state;
    uint64_t inc;     /* stream selector; always odd internally */
} JceRng;

/* Seed the generator.  `seed` chooses the start point; `seq` selects an
 * independent stream (two RNGs with the same seed but different seq never
 * overlap). */
JCE_API void jce_rng_seed(JceRng *r, uint64_t seed, uint64_t seq);

/* Uniform 32-bit / 64-bit integers. */
JCE_API uint32_t jce_rng_u32(JceRng *r);
JCE_API uint64_t jce_rng_u64(JceRng *r);

/* Uniform float in [0,1) (24-bit mantissa) and double in [0,1) (53-bit). */
JCE_API float  jce_rng_f32(JceRng *r);
JCE_API double jce_rng_f64(JceRng *r);

/* Uniform float in [lo, hi). */
JCE_API float jce_rng_range_f(JceRng *r, float lo, float hi);

/* Uniform integer in [lo, hi] INCLUSIVE (unbiased rejection sampling).
 * Returns lo if hi <= lo. */
JCE_API int jce_rng_range_i(JceRng *r, int lo, int hi);

/* True with probability p (clamped to [0,1]). */
JCE_API bool jce_rng_chance(JceRng *r, float p);

JCE_EXTERN_C_END

#endif /* JCE_RAND_H */
