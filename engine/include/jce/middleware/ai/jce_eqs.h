/*
 * jce_eqs.h  Environment Query System (EQS) core — à la Unreal EQS.
 *
 * A deterministic, headless spatial-reasoning primitive: GENERATE a set of
 * candidate points around a context, SCORE each candidate against a list of
 * weighted tests, FILTER out the ones that fail a hard constraint, then SORT
 * the survivors by score so the best location (cover, ranged-attack spot,
 * flank, patrol point, ...) can be picked.
 *
 *   generator  ->  N candidates (pure geometry)
 *   tests      ->  per-candidate score = sum(curve(raw) * weight)
 *   filter     ->  discard candidates outside a test's [filter_min,filter_max]
 *   run        ->  collect survivors, SORT by score descending
 *
 * ------------------------------------------------------------------------
 * CONTRACT — the core makes ZERO physics / navmesh / behavior-tree calls.
 * ------------------------------------------------------------------------
 * DISTANCE and DOT tests are pure math on the candidate vector.  Anything
 * that needs to touch the world (is this point on the navmesh? is there
 * line-of-sight from the querier? is this point reachable?) is expressed as
 * a CALLBACK test: the caller injects a function pointer (mirroring
 * jce_perception's los_fn pattern) that the core invokes per candidate.  The
 * core therefore stays portable, deterministic and unit-testable with no
 * jce_navmesh_recast / jce_physics / jce_bt dependency.  Real wiring of
 * navmesh-snap (jce_recast_snap_to_navmesh) and LoS (jce_physics_raycast_
 * filtered) lives in the runtime adapter, not here.
 *
 * Determinism: pure functions over caller-owned data; RING uses sinf/cosf
 * only.  Same query desc -> same candidate order, same scores, same sort.
 *
 * Thread-safety: a JceEqs handle owns one scratch buffer and is NOT
 * thread-safe; use one handle per thread (or serialize).  The math itself is
 * re-entrant.
 *
 * Layer: AI (Layer 3).  Pure CPU; one .c.
 */

#ifndef JCE_EQS_H
#define JCE_EQS_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Generators                                                          */
/* ================================================================== */

typedef enum {
    JCE_EQS_GEN_GRID = 0, /* grid_width x grid_height points in XZ */
    JCE_EQS_GEN_RING = 1  /* ring_point_count points on a circle in XZ */
} JceEqsGeneratorKind;

/*
 * Candidate layout (V1):
 *   GRID — grid_width * grid_height points in the XZ plane, spaced
 *          grid_spacing apart, centered on `center`.  Row-major: the
 *          point at (col i, row j) is
 *            center + ((i - (W-1)/2)*spacing, 0, (j - (H-1)/2)*spacing).
 *          Y is taken from `center` (the caller fixes terrain Y later).
 *   RING — ring_point_count points evenly distributed on a circle of
 *          radius ring_radius in the XZ plane around `center`.  Point k is
 *            center + (cos(theta)*radius, 0, sin(theta)*radius),
 *            theta = 2*pi*k / ring_point_count, starting at +X.
 */
typedef struct {
    JceEqsGeneratorKind kind;
    jce_vec3            center;

    /* GRID */
    uint32_t grid_width;   /* points along X (>=1) */
    uint32_t grid_height;  /* points along Z (>=1) */
    float    grid_spacing; /* metres between adjacent points */

    /* RING */
    float    ring_radius;      /* circle radius in metres */
    uint32_t ring_point_count; /* points on the circle (>=1) */
} JceEqsGeneratorDesc;

/* ================================================================== */
/* Tests                                                               */
/* ================================================================== */

typedef enum {
    JCE_EQS_TEST_DISTANCE = 0, /* raw = |candidate - param_vec3|       */
    JCE_EQS_TEST_DOT      = 1, /* raw = dot(dir(candidate), param_vec3) */
    JCE_EQS_TEST_CALLBACK = 2  /* raw / pass via injected callback     */
} JceEqsTestKind;

/*
 * Curve maps a test's raw value into a [0,1] score:
 *   LINEAR  — clamp(1 - raw/param_f1, 0, 1).  Requires param_f1 > 0.
 *             (raw=0 -> 1, raw>=param_f1 -> 0.)  "smaller raw is better".
 *   INVERSE — 1 / (1 + raw).  raw=0 -> 1, grows -> 0.  raw clamped at >=0.
 *   CLAMPED — remap raw from [param_f0, param_f1] to [0,1], clamped.
 *             Requires param_f1 > param_f0.  "larger raw is better".
 *   CUSTOM  — score_fn(raw); caller owns the mapping (no validation).
 */
typedef enum {
    JCE_EQS_CURVE_LINEAR  = 0,
    JCE_EQS_CURVE_INVERSE = 1,
    JCE_EQS_CURVE_CLAMPED = 2,
    JCE_EQS_CURVE_CUSTOM  = 3
} JceEqsCurveKind;

/*
 * CALLBACK value/filter function.  Receives a candidate position and the
 * test's `user` pointer.  Mirrors jce_perception's los_fn injection style so
 * navmesh / LoS / reachability live OUTSIDE the core.
 *
 * Interpretation depends on whether the test is a filter:
 *   - filter test  (has_filter_min || has_filter_max): the bool return is the
 *                   PASS flag (true keeps the candidate, false discards it);
 *                   raw is treated as 1.0 for kept candidates' scoring curve.
 *   - scoring test (no filter): supply `value_fn` (JceEqsValueFn below); it
 *                   returns the raw value fed to the curve.  `test_fn`'s bool
 *                   return is ignored for scoring tests.
 *
 * In short: `test_fn` is the boolean filter form; `value_fn` is the
 * value-producing scoring form; `score_fn` is only the CUSTOM curve mapping.
 */
typedef bool  (*JceEqsTestFn)   (jce_vec3 candidate, void *user);
typedef float (*JceEqsValueFn)  (jce_vec3 candidate, void *user);
typedef float (*JceEqsScoringFn)(float raw); /* CUSTOM curve: raw -> [0,1] */

typedef struct {
    JceEqsTestKind  kind;
    JceEqsCurveKind curve;
    float           weight;     /* score contribution multiplier */

    jce_vec3 param_vec3;        /* DISTANCE target / DOT direction (need not
                                 * be normalized for DOT) */
    jce_vec3 dot_origin;        /* DOT: dir = normalize(candidate - dot_origin) */
    float    param_f0;          /* CLAMPED min raw */
    float    param_f1;          /* LINEAR max_dist / CLAMPED max raw */

    /* CALLBACK plumbing. */
    JceEqsTestFn  test_fn;      /* boolean filter form */
    JceEqsValueFn value_fn;     /* value-producing form (raw for the curve) */
    JceEqsScoringFn score_fn;   /* CUSTOM curve only */
    void         *user;         /* passed to test_fn / value_fn */

    /* Hard filter: if set, a candidate whose raw is outside the inclusive
     * range is discarded entirely (its whole-query score is voided). */
    bool  has_filter_min;
    bool  has_filter_max;
    float filter_min;
    float filter_max;
} JceEqsTestDesc;

/* ================================================================== */
/* Query / run                                                         */
/* ================================================================== */

/* A scored candidate written to the caller's output buffer. */
typedef struct {
    jce_vec3 position;
    float    score;
} JceEqsScoredPoint;

typedef struct {
    JceEqsGeneratorDesc   generator;
    const JceEqsTestDesc *tests;       /* may be NULL when test_count == 0 */
    uint32_t              test_count;

    JceEqsScoredPoint    *out_candidates; /* caller-owned output buffer */
    uint32_t              out_capacity;   /* number of slots in out_candidates */
} JceEqsQueryDesc;

typedef struct JceEqs JceEqs;

/* Allocate an EQS handle with scratch for up to `max_candidates` generated
 * points.  Returns NULL on allocation failure or max_candidates == 0. */
JCE_API JceEqs *JCE_CALL jce_eqs_create(uint32_t max_candidates);
JCE_API void    JCE_CALL jce_eqs_destroy(JceEqs *eqs);

/* The generator's candidate count for a desc (0 on a malformed desc, e.g. a
 * zero grid/ring dimension).  Lets callers size out_capacity / max_candidates. */
JCE_API uint32_t JCE_CALL jce_eqs_candidate_count(const JceEqsGeneratorDesc *gen);

/*
 * Validate a query before running it.  Returns false (so callers can fail
 * loudly) when any test's curve parameters would divide by zero or are
 * otherwise ill-formed:
 *   LINEAR  requires param_f1 > 0
 *   CLAMPED requires param_f1 > param_f0
 *   CUSTOM  requires score_fn != NULL
 *   CALLBACK requires test_fn (filter form) or value_fn (value form)
 * Also rejects a NULL desc or a malformed generator.
 */
JCE_API bool JCE_CALL jce_eqs_validate_query(const JceEqsQueryDesc *query);

/*
 * Run the query.  For each generated candidate, every test computes its raw
 * value; if a test is a hard filter and the raw is outside its
 * [filter_min,filter_max] the candidate is DISCARDED, otherwise the candidate
 * accumulates curve(raw)*weight.  Surviving candidates are SORTED by score
 * descending and written to query->out_candidates (up to out_capacity).
 *
 * Returns the number of surviving candidates written (<= out_capacity), or 0
 * on a NULL/invalid query (run internally calls jce_eqs_validate_query) or
 * when the generated count exceeds the handle's max_candidates.
 */
JCE_API uint32_t JCE_CALL jce_eqs_run(JceEqs *eqs, const JceEqsQueryDesc *query);

/*
 * Convenience: run the query and return the single highest-scoring position.
 * Returns true and writes *out_best on success; returns false (leaving
 * *out_best untouched) when no candidate survives or the query is invalid.
 * out_best may be NULL only if out_score is wanted; both may be NULL to just
 * test for any survivor.
 */
JCE_API bool JCE_CALL jce_eqs_pick_best(JceEqs *eqs,
                                        const JceEqsQueryDesc *query,
                                        jce_vec3 *out_best,
                                        float *out_score);

JCE_EXTERN_C_END

#endif /* JCE_EQS_H */
