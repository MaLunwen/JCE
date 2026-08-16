/*
 * jce_water_field.c -- one authority per water body.  See jce_water_field.h.
 */

#include <jce/middleware/scene/jce_water_field.h>
#include <jce/middleware/scene/jce_water.h>
#include <jce/middleware/scene/jce_water_fft.h>
#include <jce/os/core/jce_filesystem.h>

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define JCE_WATER_FIELD_MAX_WAVES 8
#define WATER_FIELD_SOLVE_ITERS   4     /* the production norm; see the header */

struct JceWaterField {
    JceWaterFieldDesc desc;                       /* `waves` pointer scrubbed  */
    JceWaterWave      waves[JCE_WATER_FIELD_MAX_WAVES];
    int               wave_count;

    JceWaterFft      *fft;                        /* FFT model only            */
    JceWaterFft      *fft2;                       /* second cascade, or NULL   */
    double            time;
    uint64_t          revision;
};

/* ── Desc handling ─────────────────────────────────────────────────────── */

static void field_copy_desc(JceWaterField *f, const JceWaterFieldDesc *d)
{
    f->desc = *d;
    f->desc.waves = NULL;      /* borrowed for the call only; never retained */

    int n = d->wave_count;
    if (n < 0) n = 0;
    if (n > JCE_WATER_FIELD_MAX_WAVES) n = JCE_WATER_FIELD_MAX_WAVES;
    if (n > 0 && d->waves)
        memcpy(f->waves, d->waves, (size_t)n * sizeof(JceWaterWave));
    else
        n = 0;
    f->wave_count = n;
    f->desc.wave_count = n;
}

/* Only the parameters the spectrum is BUILT from.  Everything else (base
 * height, extent, centre) can change without invalidating a single sample, and
 * rebuilding for those would throw away the ocean every time a designer nudged
 * a plane. */
static bool spectrum_differs(const JceWaterFieldDesc *a,
                             const JceWaterFieldDesc *b)
{
    return a->model            != b->model            ||
           a->fft_resolution   != b->fft_resolution   ||
           a->fft_patch_size   != b->fft_patch_size   ||
           a->fft_wind_speed   != b->fft_wind_speed   ||
           a->fft_wind_dir_x   != b->fft_wind_dir_x   ||
           a->fft_wind_dir_z   != b->fft_wind_dir_z   ||
           a->fft_amplitude    != b->fft_amplitude    ||
           a->fft_seed         != b->fft_seed         ||
           a->fft_fetch        != b->fft_fetch        ||
           a->fft_swell        != b->fft_swell        ||
           a->cascade_fraction != b->cascade_fraction;
}

static JceWaterFft *field_build_fft(const JceWaterFieldDesc *d)
{
    int   n     = d->fft_resolution > 0 ? d->fft_resolution : 64;
    float patch = d->fft_patch_size > 0.0f ? d->fft_patch_size : 64.0f;
    float amp   = d->fft_amplitude  > 0.0f ? d->fft_amplitude  : 8e-4f;
    float wind  = d->fft_wind_speed > 0.0f ? d->fft_wind_speed : 8.0f;

    JceWaterFft *fft = jce_water_fft_create(n, patch, wind,
                                            d->fft_wind_dir_x, d->fft_wind_dir_z,
                                            amp, d->fft_seed);
    if (!fft) return NULL;

    /* Opt-in only: Phillips has no fetch, so a positive fetch is the caller
     * explicitly asking for the modern spectrum and accepting that every height
     * value changes. */
    if (d->fft_fetch > 0.0f)
        (void)jce_water_fft_use_jonswap(fft, wind, d->fft_fetch, d->fft_swell,
                                        d->fft_seed);
    return fft;
}

/* Build the second cascade at a NON-COMMENSURATE period near the requested
 * fraction.  A different seed too: the same seed at a different scale still
 * produces visibly correlated crests, which defeats the point of summing. */
static JceWaterFft *field_build_fft2(const JceWaterFieldDesc *d)
{
    if (!(d->cascade_fraction > 0.0f)) return NULL;
    const float primary = d->fft_patch_size > 0.0f ? d->fft_patch_size : 64.0f;
    const float second  =
        jce_water_cascade_pick_secondary(primary, d->cascade_fraction);
    if (!(second > 0.0f)) return NULL;

    JceWaterFieldDesc sub = *d;
    sub.fft_patch_size = second;
    sub.fft_seed       = d->fft_seed ^ 0x5BF03635u;
    return field_build_fft(&sub);
}

JceWaterField *JCE_CALL jce_water_field_create(const JceWaterFieldDesc *desc)
{
    if (!desc) return NULL;

    JceWaterField *f = (JceWaterField *)JCE_CALLOC(1, sizeof(JceWaterField));
    if (!f) return NULL;

    field_copy_desc(f, desc);

    if (f->desc.model == JCE_WATER_FIELD_FFT) {
        f->fft = field_build_fft(&f->desc);
        if (!f->fft) { JCE_FREE(f); return NULL; }
        /* The field's sampling contract promises a normal and a jacobian, so
         * it must ASK for the fields that supply them -- otherwise every query
         * silently returns the flat-water defaults. */
        (void)jce_water_fft_enable_slopes(f->fft);
        (void)jce_water_fft_enable_foam(f->fft);
        f->fft2 = field_build_fft2(&f->desc);
        if (f->fft2) {
            (void)jce_water_fft_enable_slopes(f->fft2);
            (void)jce_water_fft_enable_foam(f->fft2);
        }
    }

    /* Evaluate at t=0 so the field is queryable the instant it exists.  A
     * "created but not yet evolved" state would be a second way for a consumer
     * to read zeros that look like a flat sea. */
    f->time     = 0.0;
    f->revision = 1u;
    if (f->fft)  jce_water_fft_evolve(f->fft,  0.0f);
    if (f->fft2) jce_water_fft_evolve(f->fft2, 0.0f);
    return f;
}

void JCE_CALL jce_water_field_destroy(JceWaterField *f)
{
    if (!f) return;
    jce_water_fft_destroy(f->fft);
    jce_water_fft_destroy(f->fft2);
    JCE_FREE(f);
}

bool JCE_CALL jce_water_field_sync(JceWaterField *f, const JceWaterFieldDesc *d)
{
    if (!f || !d) return false;

    const bool rebuild = spectrum_differs(&f->desc, d);

    /* JCE_DBG_WATER_LOG=<path>: every sync, who asked and for what.
     *
     * This field is documented as ONE authority per water body, and two
     * writers acquire it -- the renderer and the runtime's buoyancy -- with
     * descs that were never compared against each other. */
    {
        /* jce_fs_host_* rather than fopen/fprintf: this file builds for the
         * wasm and console targets too, where C stdio is not the host
         * filesystem.  jce_fs_host_append also retires the resident FILE* and
         * the fflush -- a line is durable when the call returns, which is what
         * a log read after the crash it was added to explain has to be. */
        static char s_log_path[512];
        static int  s_tried;
        if (!s_tried) {
            const char *e = getenv("JCE_DBG_WATER_LOG");
            s_tried = 1;
            if (e && e[0] && e[0] != '0') {
                size_t n = strlen(e);
                if (n < sizeof(s_log_path)) {
                    memcpy(s_log_path, e, n + 1);
                    /* Truncate once, matching the "wb" the fopen form opened
                     * with: one run's log must not read as a continuation of
                     * the previous run's. */
                    (void)jce_fs_host_write_all(s_log_path, "", 0);
                }
            }
        }
        if (s_log_path[0]) {
            char line[256];
            int  n = snprintf(line, sizeof(line),
                    "SYNC rebuild=%d model=%d base=%.3f cx=%.2f cz=%.2f "
                    "wind=%.4f rev=%llu\n",
                    rebuild ? 1 : 0, (int)d->model, (double)d->base_height,
                    (double)d->center_x, (double)d->center_z,
                    (double)d->fft_wind_speed,
                    (unsigned long long)f->revision);
            if (n > 0 && (size_t)n < sizeof(line))
                (void)jce_fs_host_append(s_log_path, line, (uint64_t)n);
        }
    }
    if (!rebuild) {
        field_copy_desc(f, d);   /* cheap parameters only; surface unaffected */
        return true;
    }

    JceWaterFft *next = NULL;
    if (d->model == JCE_WATER_FIELD_FFT) {
        next = field_build_fft(d);
        /* Keep the previous, still-valid field rather than publishing a
         * half-updated one. */
        if (!next) return false;
        (void)jce_water_fft_enable_slopes(next);
        (void)jce_water_fft_enable_foam(next);
    }
    JceWaterFft *next2 = NULL;
    if (d->model == JCE_WATER_FIELD_FFT) {
        next2 = field_build_fft2(d);
        if (next2) {
            (void)jce_water_fft_enable_slopes(next2);
            (void)jce_water_fft_enable_foam(next2);
        }
    }

    jce_water_fft_destroy(f->fft);
    jce_water_fft_destroy(f->fft2);
    f->fft  = next;
    f->fft2 = next2;
    field_copy_desc(f, d);
    if (f->fft)  jce_water_fft_evolve(f->fft,  (float)f->time);
    if (f->fft2) jce_water_fft_evolve(f->fft2, (float)f->time);
    f->revision++;
    return true;
}

/* ── Clock ─────────────────────────────────────────────────────────────── */

void JCE_CALL jce_water_field_set_time(JceWaterField *f, double t)
{
    if (!f) return;
    if (t == f->time) return;      /* idempotent: a second setter costs nothing */
    f->time = t;
    /* BOTH cascades on the one clock.  A second cascade with its own clock
     * would be the two-clock defect again, one level down. */
    if (f->fft)  jce_water_fft_evolve(f->fft,  (float)t);
    if (f->fft2) jce_water_fft_evolve(f->fft2, (float)t);
    f->revision++;
}

double   JCE_CALL jce_water_field_time(const JceWaterField *f) { return f ? f->time : 0.0; }
uint64_t JCE_CALL jce_water_field_revision(const JceWaterField *f) { return f ? f->revision : 0u; }

const JceWaterFft *JCE_CALL jce_water_field_fft(const JceWaterField *f)
{
    return f ? f->fft : NULL;
}

const JceWaterFft *JCE_CALL jce_water_field_fft2(const JceWaterField *f)
{
    return f ? f->fft2 : NULL;
}

JceWaterFieldModel JCE_CALL jce_water_field_model(const JceWaterField *f)
{
    return f ? f->desc.model : JCE_WATER_FIELD_GERSTNER;
}

float JCE_CALL jce_water_field_base_height(const JceWaterField *f)
{
    return f ? f->desc.base_height : 0.0f;
}

/* ── Sampling ──────────────────────────────────────────────────────────── */

static bool field_contains(const JceWaterField *f, float x, float z)
{
    /* A non-positive extent means "unbounded" -- an ocean authored without a
     * plane size should not silently refuse every query. */
    if (f->desc.size_x > 0.0f) {
        const float hx = 0.5f * f->desc.size_x;
        if (x < f->desc.center_x - hx || x > f->desc.center_x + hx) return false;
    }
    if (f->desc.size_z > 0.0f) {
        const float hz = 0.5f * f->desc.size_z;
        if (z < f->desc.center_z - hz || z > f->desc.center_z + hz) return false;
    }
    return true;
}

/* Band-limit: drop waves shorter than half the caller's feature size.  Applied
 * to the Gerstner set by omission; the FFT is band-limited by its own grid, so
 * for it this collapses to "use the cascade you have". */
static int field_band_limit(const JceWaterField *f, float min_spatial_length,
                            JceWaterWave *out)
{
    int n = 0;
    for (int i = 0; i < f->wave_count; i++) {
        if (min_spatial_length > 0.0f &&
            f->waves[i].wavelength < 0.5f * min_spatial_length)
            continue;
        out[n++] = f->waves[i];
    }
    return n;
}

bool JCE_CALL jce_water_field_sample(const JceWaterField *f,
                                     float world_x, float world_z,
                                     float min_spatial_length,
                                     JceWaterSample *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!f) return false;
    if (!field_contains(f, world_x, world_z)) return false;

    const float t = (float)f->time;
    const float base = f->desc.base_height;

    out->position = jce_v3(world_x, base, world_z);
    out->normal   = jce_v3(0.0f, 1.0f, 0.0f);
    out->jacobian = 1.0f;

    if (f->desc.model == JCE_WATER_FIELD_FFT && f->fft) {
        /* ONE inverse solve.  This used to run five -- one for the height and
         * four more to central-difference a normal and a Jacobian out of it --
         * which was both five times the cost and less accurate than the
         * spectral quantities the FFT can now produce directly.  Worse, a
         * finite difference across a crest straddles the peak and flattens it,
         * so the approximation failed exactly where a breaking wave is. */
        JceWaterFftSample s;
        jce_water_fft_sample_surface(f->fft, world_x, world_z,
                                     WATER_FIELD_SOLVE_ITERS, &s);
        /* Sum the cascades.  The second is sampled at the SAME world XZ --
         * it is a different tiling of one ocean, not a different place -- and
         * its displacement adds, which is why the combined surface repeats
         * only where both cascades repeat at once. */
        float h = s.height, sx = s.slope_x, sz = s.slope_z, jac = s.jacobian;
        if (f->fft2) {
            JceWaterFftSample s2;
            jce_water_fft_sample_surface(f->fft2, world_x, world_z,
                                         WATER_FIELD_SOLVE_ITERS, &s2);
            h  += s2.height;
            sx += s2.slope_x;
            sz += s2.slope_z;
            /* Folds compound: a cell folded by either cascade is folded. */
            jac = (jac < s2.jacobian) ? jac : s2.jacobian;
        }
        out->position.y = base + h;

        /* Exact gradient (eq. 37) -> exact normal. */
        const float nx = -sx, ny = 1.0f, nz = -sz;
        const float inv = 1.0f / sqrtf(nx * nx + ny * ny + nz * nz);
        out->normal = jce_v3(nx * inv, ny * inv, nz * inv);

        /* Fold determinant of the horizontal map; < 0 is a breaking crest. */
        out->jacobian = jac;
    } else {
        JceWaterWave band[JCE_WATER_FIELD_MAX_WAVES];
        const int n = field_band_limit(f, min_spatial_length, band);
        out->position.y = jce_water_sample_height_displaced(
            band, n, base, world_x, world_z, t, WATER_FIELD_SOLVE_ITERS);

        float nrm[3];
        jce_water_sample_normal(band, n, world_x, world_z, t, nrm);
        out->normal = jce_v3(nrm[0], nrm[1], nrm[2]);

        /* Gerstner folds exactly when sum(Q*k*A) exceeds 1 -- the same
         * condition that makes the inverse solve stop converging. */
        float qka = 0.0f;
        for (int i = 0; i < n; i++) {
            if (band[i].wavelength <= 0.0f) continue;
            const float k = 6.28318530717958647692f / band[i].wavelength;
            qka += band[i].steepness * k * band[i].amplitude;
        }
        out->jacobian = 1.0f - qka;
    }

    return true;
}

bool JCE_CALL jce_water_field_submersion(const JceWaterField *field,
                                         float world_x, float world_y,
                                         float world_z,
                                         float min_spatial_length,
                                         float *out_depth)
{
    if (!out_depth) return false;

    JceWaterSample s;
    if (!jce_water_field_sample(field, world_x, world_z,
                                min_spatial_length, &s))
        return false;   /* not over this body -- leave *out_depth alone */

    /* s.position.y is the DISPLACED surface at this XZ, which is the whole
     * reason the sample carries a position instead of a height. */
    *out_depth = s.position.y - world_y;
    return true;
}

float JCE_CALL jce_water_field_surface_y(const JceWaterField *f,
                                         float world_x, float world_z,
                                         float min_spatial_length)
{
    JceWaterSample s;
    if (!jce_water_field_sample(f, world_x, world_z, min_spatial_length, &s))
        return f ? f->desc.base_height : 0.0f;
    return s.position.y;
}

/* ── The set ───────────────────────────────────────────────────────────── */

#define WATER_FIELD_SET_MAX 16     /* matches the renderer's water_cache */

struct JceWaterFieldSet {
    struct {
        bool           used;
        bool           touched;    /* acquired since the last sweep */
        uint64_t       key;
        JceWaterField *field;
    } slot[WATER_FIELD_SET_MAX];
    const void *driver;            /* holds the clock claim; see the header */
    double      time;
};

JceWaterFieldSet *JCE_CALL jce_water_field_set_create(void)
{
    return (JceWaterFieldSet *)JCE_CALLOC(1, sizeof(JceWaterFieldSet));
}

void JCE_CALL jce_water_field_set_destroy(JceWaterFieldSet *set)
{
    if (!set) return;
    for (int i = 0; i < WATER_FIELD_SET_MAX; i++)
        jce_water_field_destroy(set->slot[i].field);
    JCE_FREE(set);
}

JceWaterField *JCE_CALL jce_water_field_set_find(const JceWaterFieldSet *set,
                                                 uint64_t key)
{
    if (!set) return NULL;
    for (int i = 0; i < WATER_FIELD_SET_MAX; i++)
        if (set->slot[i].used && set->slot[i].key == key)
            return set->slot[i].field;
    return NULL;
}

JceWaterField *JCE_CALL jce_water_field_set_acquire(JceWaterFieldSet *set,
                                                    uint64_t key,
                                                    const JceWaterFieldDesc *desc)
{
    if (!set || !desc) return NULL;

    int free_slot = -1;
    for (int i = 0; i < WATER_FIELD_SET_MAX; i++) {
        if (set->slot[i].used) {
            if (set->slot[i].key != key) continue;
            set->slot[i].touched = true;
            if (!jce_water_field_sync(set->slot[i].field, desc)) return NULL;
            jce_water_field_set_time(set->slot[i].field, set->time);
            return set->slot[i].field;
        }
        if (free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;

    JceWaterField *f = jce_water_field_create(desc);
    if (!f) return NULL;
    jce_water_field_set_time(f, set->time);   /* born on the shared clock */

    set->slot[free_slot].used    = true;
    set->slot[free_slot].touched = true;
    set->slot[free_slot].key     = key;
    set->slot[free_slot].field   = f;
    return f;
}

bool JCE_CALL jce_water_field_set_advance(JceWaterFieldSet *set,
                                          const void *driver, double dt)
{
    if (!set || !driver) return false;
    if (!set->driver) set->driver = driver;   /* first caller claims it */
    if (set->driver != driver) return false;  /* everyone else reads, not writes */

    set->time += dt;
    for (int i = 0; i < WATER_FIELD_SET_MAX; i++)
        if (set->slot[i].used)
            jce_water_field_set_time(set->slot[i].field, set->time);
    return true;
}

void JCE_CALL jce_water_field_set_release(JceWaterFieldSet *set,
                                          const void *driver)
{
    if (set && set->driver == driver) set->driver = NULL;
}

double JCE_CALL jce_water_field_set_get_time(const JceWaterFieldSet *set)
{
    return set ? set->time : 0.0;
}

void JCE_CALL jce_water_field_set_sweep(JceWaterFieldSet *set)
{
    if (!set) return;
    for (int i = 0; i < WATER_FIELD_SET_MAX; i++) {
        if (!set->slot[i].used) continue;
        if (set->slot[i].touched) { set->slot[i].touched = false; continue; }
        jce_water_field_destroy(set->slot[i].field);
        memset(&set->slot[i], 0, sizeof(set->slot[i]));
    }
}

/* ── Cascades ──────────────────────────────────────────────────────────
 * See jce_water_field.h. */

float JCE_CALL jce_water_cascade_repeat_distance(float period_a,
                                                 float period_b,
                                                 float max_search)
{
    if (!(period_a > 0.0f) || !(period_b > 0.0f)) return 0.0f;
    if (!(max_search > 0.0f)) max_search = 100000.0f;

    /* Walk multiples of the LARGER period and ask how close each lands to a
     * multiple of the smaller.  Walking the larger keeps the loop short: the
     * answer is always a multiple of both, so it is a multiple of the larger.
     *
     * The tolerance is absolute (0.5 m) rather than relative on purpose: what
     * the eye notices is a seam displaced by less than about half a metre,
     * regardless of how far away that seam is. */
    const float big   = (period_a > period_b) ? period_a : period_b;
    const float small = (period_a > period_b) ? period_b : period_a;
    const float tol   = 0.5f;

    for (float d = big; d <= max_search; d += big) {
        const float k = d / small;
        const float frac = k - floorf(k);
        const float off = (frac < 0.5f ? frac : 1.0f - frac) * small;
        if (off <= tol) return d;
    }
    return max_search;   /* no realignment within the search: good enough */
}

float JCE_CALL jce_water_cascade_pick_secondary(float primary, float fraction)
{
    if (!(primary > 0.0f)) return 0.0f;
    if (!(fraction > 0.0f)) fraction = 0.41f;
    if (fraction > 0.95f)   fraction = 0.95f;

    const float target = primary * fraction;
    /* Search a band around the target and keep whichever candidate pushes the
     * realignment furthest away.
     *
     * The SEARCH is what matters, and it is not optional: measured over 30
     * primary/fraction pairs it materially beat simply taking primary*fraction
     * in 28 of them, and the cases where it matters most are the round
     * fractions a person picks by hand -- at 0.5 or 0.25 the raw target
     * divides the primary exactly, the pair realigns at the primary itself,
     * and the second cascade buys nothing whatsoever.
     *
     * The 0.37 m step is NOT load-bearing: a whole-metre sweep finds equally
     * good candidates, because the band is wide enough that some candidate is
     * always non-commensurate.  It is kept only because it is what was
     * measured; do not read significance into the constant. */
    float best = target;
    float best_d = -1.0f;
    for (int i = -40; i <= 40; ++i) {
        const float cand = target + (float)i * 0.37f;
        if (cand < 4.0f || cand >= primary) continue;
        const float d = jce_water_cascade_repeat_distance(primary, cand, 20000.0f);
        if (d > best_d) { best_d = d; best = cand; }
    }
    return best;
}
