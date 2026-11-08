/*
 * jce_water_field.c -- one authority per water body.  See jce_water_field.h.
 */

#include "jce_water_field_internal.h"
#include "jce_water_fft_internal.h"
#include "jce_water_fft_motion_internal.h"
#include <jce/middleware/scene/jce_water.h>
#include <jce/middleware/scene/jce_water_fft.h>
#include <jce/os/core/jce_filesystem.h>

#include "os/core/jce_memory.h"

#include <float.h>
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
    struct {
        JceWaterFft *fft;
        uint64_t field_revision;
        uint64_t source_revision;
    } caustic;
    JceWaterFftSpectrumDesc spectrum; /* version 0 selects legacy */
    JceWaterFieldMotion motion;
    bool              sample_combined;
    double            time;          /* real scene seconds, also used by history */
    double            phase;
    double            phase_anchor;
    double            scene_anchor;
    uint64_t          revision;
};

static const JceWaterFieldMotion default_motion = {1.0f, 1.0f, 0.0f};

static bool field_motion_valid(const JceWaterFieldMotion *motion)
{
    return isfinite(motion->phase_rate) && motion->phase_rate >= 0.0f &&
           isfinite(motion->horizontal_scale) && motion->horizontal_scale >= 0.0f &&
           isfinite(motion->max_horizontal_displacement) &&
           motion->max_horizontal_displacement >= 0.0f;
}

static bool field_phase_at(const JceWaterField *f, double time, double *out)
{
    if (!isfinite(time)) return false;
    double phase = f->phase_anchor;
    if (f->motion.phase_rate != 0.0f)
        phase += (time - f->scene_anchor) * (double)f->motion.phase_rate;
    if (!isfinite(phase) || phase < -(double)FLT_MAX || phase > (double)FLT_MAX)
        return false;
    *out = phase;
    return true;
}

static void field_evolve_fft(JceWaterField *f)
{
    if (!f->fft) return;
    /* Each bilinear sample is a convex combination of bounded grid vectors.
     * Giving each level half the budget also bounds their sum at one point. */
    const double cap = (double)f->motion.max_horizontal_displacement;
    const double budget = f->fft2 ? cap * 0.5 : cap;
    const float phase = (float)f->phase;
    jce_water_fft_evolve_motion(f->fft, phase, f->motion.horizontal_scale, budget);
    if (f->fft2)
        jce_water_fft_evolve_motion(f->fft2, phase, f->motion.horizontal_scale, budget);
}

static void field_update_clock(JceWaterField *f, double time, double phase,
                               const JceWaterFieldMotion *motion, bool evaluate)
{
    const bool surface_changed = f->fft
        ? phase != f->phase || motion->horizontal_scale != f->motion.horizontal_scale ||
          motion->max_horizontal_displacement != f->motion.max_horizontal_displacement
        : time != f->time;

    if (motion->phase_rate != f->motion.phase_rate) {
        /* The old rate brings us to this instant. Only future or re-seeked
         * times on this segment use the new rate. */
        f->phase_anchor = phase;
        f->scene_anchor = time;
    }
    f->motion = *motion;
    f->time = time;
    f->phase = phase;
    if (evaluate && surface_changed) {
        field_evolve_fft(f);
        f->revision++;
    }
}

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

static JceWaterFft *field_build_fft(const JceWaterFieldDesc *d,
                                   const JceWaterFftSpectrumDesc *spectrum)
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
    if (spectrum && spectrum->version) {
        JceWaterFftSpectrumDesc config = *spectrum;
        config.seed = d->fft_seed;
        config.wind_speed = wind;
        if (!jce_water_fft_configure_spectrum(fft, &config)) {
            jce_water_fft_destroy(fft);
            return NULL;
        }
    } else if (d->fft_fetch > 0.0f)
        (void)jce_water_fft_use_jonswap(fft, wind, d->fft_fetch, d->fft_swell,
                                        d->fft_seed);
    return fft;
}

/* Build the second cascade at a NON-COMMENSURATE period near the requested
 * fraction.  A different seed too: the same seed at a different scale still
 * produces visibly correlated crests, which defeats the point of summing. */
static JceWaterFft *field_build_fft2(const JceWaterFieldDesc *d,
                                    const JceWaterFftSpectrumDesc *spectrum)
{
    if (!(d->cascade_fraction > 0.0f)) return NULL;
    const float primary = d->fft_patch_size > 0.0f ? d->fft_patch_size : 64.0f;
    const float second  =
        jce_water_cascade_pick_secondary(primary, d->cascade_fraction);
    if (!(second > 0.0f)) return NULL;

    JceWaterFieldDesc sub = *d;
    sub.fft_patch_size = second;
    sub.fft_seed       = d->fft_seed ^ 0x5BF03635u;
    return field_build_fft(&sub, spectrum);
}

static JceWaterField *field_create_configured(const JceWaterFieldDesc *desc,
    const JceWaterFftSpectrumDesc *spectrum, const JceWaterField *clock_source,
    const JceWaterFieldMotion *motion, double time)
{
    if (!desc) return NULL;

    JceWaterField *f = (JceWaterField *)JCE_CALLOC(1, sizeof(JceWaterField));
    if (!f) return NULL;

    field_copy_desc(f, desc);
    if (spectrum) f->spectrum = *spectrum;
    f->motion = clock_source ? clock_source->motion : *motion;
    if (clock_source) {
        f->sample_combined = clock_source->sample_combined;
        f->caustic.source_revision = clock_source->caustic.source_revision;
        f->phase_anchor = clock_source->phase_anchor;
        f->scene_anchor = clock_source->scene_anchor;
    }
    double phase;
    if (!field_phase_at(f, time, &phase)) { JCE_FREE(f); return NULL; }
    field_update_clock(f, time, phase, motion, false);

    if (f->desc.model == JCE_WATER_FIELD_FFT) {
        f->fft = field_build_fft(&f->desc, &f->spectrum);
        if (!f->fft) { JCE_FREE(f); return NULL; }
        /* The field's sampling contract promises a normal and a jacobian, so
         * it must ASK for the fields that supply them -- otherwise every query
         * silently returns the flat-water defaults. */
        if (!jce_water_fft_enable_slopes(f->fft) ||
            !jce_water_fft_enable_foam(f->fft)) {
            jce_water_field_destroy(f);
            return NULL;
        }
        f->fft2 = field_build_fft2(&f->desc, &f->spectrum);
        if ((f->desc.cascade_fraction > 0.0f && !f->fft2) ||
            (f->fft2 && (!jce_water_fft_enable_slopes(f->fft2) ||
                        !jce_water_fft_enable_foam(f->fft2)))) {
            jce_water_field_destroy(f);
            return NULL;
        }
    }

    /* The new field is queryable immediately at the owner's current phase. */
    f->revision = 1u;
    field_evolve_fft(f);
    return f;
}

JceWaterField *JCE_CALL jce_water_field_create(const JceWaterFieldDesc *desc)
{
    return field_create_configured(desc, NULL, NULL, &default_motion, 0.0);
}

void JCE_CALL jce_water_field_destroy(JceWaterField *f)
{
    if (!f) return;
    jce_water_fft_destroy(f->fft);
    jce_water_fft_destroy(f->fft2);
    jce_water_fft_destroy(f->caustic.fft);
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

    JceWaterField *next = field_create_configured(d, &f->spectrum, f,
                                                   &f->motion, f->time);
    if (!next) return false;
    next->revision = f->revision + 1u;
    JceWaterField retired = *f;
    *f = *next;
    *next = retired;
    jce_water_field_destroy(next);
    return true;
}

/* ── Clock ─────────────────────────────────────────────────────────────── */

void JCE_CALL jce_water_field_set_time(JceWaterField *f, double t)
{
    if (!f || t == f->time) return;
    double phase;
    if (field_phase_at(f, t, &phase))
        field_update_clock(f, t, phase, &f->motion, true);
}

double   JCE_CALL jce_water_field_time(const JceWaterField *f) { return f ? f->time : 0.0; }
double jce_water_field_phase(const JceWaterField *f) { return f ? f->phase : 0.0; }
uint64_t JCE_CALL jce_water_field_revision(const JceWaterField *f) { return f ? f->revision : 0u; }

const JceWaterFft *JCE_CALL jce_water_field_fft(const JceWaterField *f)
{
    return f ? f->fft : NULL;
}

const JceWaterFft *JCE_CALL jce_water_field_fft2(const JceWaterField *f)
{
    return f ? f->fft2 : NULL;
}

const JceWaterFft *jce_water_field_caustic_fft(JceWaterField *f,
    float period_fraction, uint64_t *source_revision)
{
    if (source_revision) *source_revision = 0;
    if (!f) return NULL;
    if (period_fraction == 0.0f) {
        jce_water_fft_destroy(f->caustic.fft);
        f->caustic.fft = NULL;
        f->caustic.field_revision = 0;
        return NULL;
    }
    if (!f->fft || !f->fft2 || !isfinite(period_fraction) ||
        period_fraction <= 0.0f || period_fraction >= 1.0f)
        return NULL;
    const float period = jce_water_fft_patch_size(f->fft) * period_fraction;
    if (!(isfinite(period) && period >= 0.01f && period < jce_water_fft_patch_size(f->fft2)))
        return NULL;

    const bool rebuild = !f->caustic.fft || period != jce_water_fft_patch_size(f->caustic.fft);
    if (rebuild || f->caustic.field_revision != f->revision) {
        if (f->caustic.source_revision == UINT64_MAX) return NULL;
        JceWaterFft *next = f->caustic.fft;
        if (rebuild) {
            JceWaterFieldDesc desc = f->desc;
            desc.fft_patch_size = period;
            /* The fine band projects into a fixed-size tile, so do not let
             * a high-resolution geometry FFT inflate its CPU or upload cost. */
            if (desc.fft_resolution > 64) desc.fft_resolution = 64;
            desc.fft_seed ^= 0xC2B2AE35u;
            next = field_build_fft(&desc, &f->spectrum);
            if (!next) return NULL;
        }

        /* This source uses the existing phase, including live rate edits and
         * rewinds. It never participates in displacement of the surface, so
         * its independent limit leaves both geometric budgets untouched. */
        jce_water_fft_evolve_motion(next, (float)f->phase, f->motion.horizontal_scale,
                                   (double)f->motion.max_horizontal_displacement);
        if (rebuild) {
            jce_water_fft_destroy(f->caustic.fft);
            f->caustic.fft = next;
        }
        f->caustic.field_revision = f->revision;
        ++f->caustic.source_revision;
    }
    if (source_revision) *source_revision = f->caustic.source_revision;
    return f->caustic.fft;
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

static bool field_sample(const JceWaterField *f,
                         float world_x, float world_z,
                         float min_spatial_length, bool require_containment,
                         JceWaterSample *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!f) return false;
    if (require_containment && !field_contains(f, world_x, world_z)) return false;

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
        if (f->fft2 && f->sample_combined) {
            jce_water_fft_sample_surface_pair(f->fft, f->fft2, world_x, world_z,
                                              WATER_FIELD_SOLVE_ITERS, &s);
        } else {
            jce_water_fft_sample_surface(f->fft, world_x, world_z,
                                         WATER_FIELD_SOLVE_ITERS, &s);
            /* Public legacy callers retain their independent cascade solves. */
            if (f->fft2) {
                JceWaterFftSample second;
                jce_water_fft_sample_surface(f->fft2, world_x, world_z,
                                             WATER_FIELD_SOLVE_ITERS, &second);
                s.height += second.height;
                s.slope_x += second.slope_x;
                s.slope_z += second.slope_z;
                s.jacobian = (s.jacobian < second.jacobian) ? s.jacobian : second.jacobian;
            }
        }
        out->position.y = base + s.height;

        /* Exact gradient (eq. 37) -> exact normal. */
        const float nx = -s.slope_x, ny = 1.0f, nz = -s.slope_z;
        const float inv = 1.0f / sqrtf(nx * nx + ny * ny + nz * nz);
        out->normal = jce_v3(nx * inv, ny * inv, nz * inv);

        /* Fold determinant of the horizontal map; < 0 is a breaking crest. */
        out->jacobian = s.jacobian;
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

bool JCE_CALL jce_water_field_sample(const JceWaterField *f,
                                     float world_x, float world_z,
                                     float min_spatial_length,
                                     JceWaterSample *out)
{
    return field_sample(f, world_x, world_z, min_spatial_length, true, out);
}

bool jce_water_field_sample_render(const JceWaterField *f,
                                  float world_x, float world_z,
                                  JceWaterSample *out)
{
    if (!f || !f->fft || !isfinite(world_x) || !isfinite(world_z)) return false;
    return field_sample(f, world_x, world_z, 0.0f, false, out);
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
        JceWaterFoamState *foam;
        JceWaterShoreState *shore;
        JceWaterShoreBinding shore_binding;
    } slot[WATER_FIELD_SET_MAX];
    const void *driver;            /* holds the clock claim; see the header */
    double      time;
    uint64_t    foam_identity;
    uint64_t    shore_identity;
};

JceWaterFieldSet *JCE_CALL jce_water_field_set_create(void)
{
    return (JceWaterFieldSet *)JCE_CALLOC(1, sizeof(JceWaterFieldSet));
}

void JCE_CALL jce_water_field_set_destroy(JceWaterFieldSet *set)
{
    if (!set) return;
    for (int i = 0; i < WATER_FIELD_SET_MAX; i++) {
        jce_water_foam_destroy(set->slot[i].foam);
        jce_water_shore_state_destroy(set->slot[i].shore);
        jce_water_field_destroy(set->slot[i].field);
    }
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

JceWaterField *JCE_CALL jce_water_field_set_acquire_spectrum(
    JceWaterFieldSet *set, uint64_t key, const JceWaterFieldDesc *desc,
    const JceWaterFftSpectrumDesc *spectrum)
{
    return jce_water_field_set_acquire_motion(set, key, desc, spectrum, NULL);
}

JceWaterField *jce_water_field_set_acquire_motion(
    JceWaterFieldSet *set, uint64_t key, const JceWaterFieldDesc *desc,
    const JceWaterFftSpectrumDesc *spectrum, const JceWaterFieldMotion *motion)
{
    if (!set || !desc) return NULL;

    int slot = -1;
    JceWaterField *old = NULL;
    for (int i = 0; i < WATER_FIELD_SET_MAX; ++i) {
        if (set->slot[i].used) {
            if (set->slot[i].key != key) continue;
            /* Rejected edits keep the existing body alive through sweep. */
            set->slot[i].touched = true;
            slot = i;
            old = set->slot[i].field;
            break;
        }
        if (slot < 0) slot = i;
    }
    if (slot < 0 || (motion && !field_motion_valid(motion))) return NULL;

    JceWaterFftSpectrumDesc config = {0};
    if (spectrum) {
        if (spectrum->struct_size < sizeof(config) ||
            spectrum->version != JCE_WATER_FFT_SPECTRUM_VERSION)
            return NULL;
        config = *spectrum;
        config.struct_size = sizeof(config);
        /* Body identity and sustained wind belong to the existing desc. */
        config.seed = desc->fft_seed;
        config.wind_speed = desc->fft_wind_speed > 0.0f ? desc->fft_wind_speed : 8.0f;
        if (!jce_water_fft_spectrum_valid(&config)) return NULL;
    }
    if (!motion && (!old ? !spectrum : memcmp(&old->spectrum, &config, sizeof(config)) == 0))
        return jce_water_field_set_acquire(set, key, desc);

    const JceWaterFieldMotion *desired = motion ? motion : old ? &old->motion : &default_motion;
    if (old && !spectrum_differs(&old->desc, desc) &&
        memcmp(&old->spectrum, &config, sizeof(config)) == 0) {
        double phase;
        if (!field_phase_at(old, set->time, &phase)) return NULL;
        if (!jce_water_field_sync(old, desc)) return NULL;
        const uint64_t revision = old->revision;
        field_update_clock(old, set->time, phase, desired, true);
        if (motion && !old->sample_combined) {
            old->sample_combined = true;
            if (old->fft2 && old->revision == revision) old->revision++;
        }
        return old;
    }

    /* Build a candidate at the current phase before changing the live slot. */
    JceWaterField *next = field_create_configured(desc, &config, old, desired, set->time);
    if (!next) return NULL;
    if (motion) next->sample_combined = true;
    if (old) {
        /* Existing consumers borrow this field pointer. Commit in place. */
        next->revision = old->revision + 1u;
        JceWaterField retired = *old;
        *old = *next;
        *next = retired;
        jce_water_field_destroy(next);
        return old;
    }
    set->slot[slot].used = true;
    set->slot[slot].touched = true;
    set->slot[slot].key = key;
    set->slot[slot].field = next;
    return next;
}

bool jce_water_field_set_configure_foam(JceWaterFieldSet *set, uint64_t key,
    const JceWaterFoamComponent *config)
{
    if (!set) return false;
    for (int i = 0; i < WATER_FIELD_SET_MAX; ++i) {
        if (!set->slot[i].used || set->slot[i].key != key) continue;
        if (config && (!set->slot[i].field->spectrum.version ||
                       !jce_water_foam_valid(config))) return false;
        uint64_t identity = set->slot[i].foam ? set->slot[i].foam->identity : 0;
        if (config && !jce_water_foam_matches(set->slot[i].foam, set->slot[i].field)) {
            identity = ++set->foam_identity;
            if (!identity) identity = ++set->foam_identity;
        }
        return jce_water_foam_configure(&set->slot[i].foam,
            set->slot[i].field, config, identity);
    }
    return config == NULL;
}

const JceWaterFoamState *jce_water_field_set_foam(const JceWaterFieldSet *set, uint64_t key)
{
    if (!set) return NULL;
    for (int i = 0; i < WATER_FIELD_SET_MAX; ++i) {
        if (!set->slot[i].used || set->slot[i].key != key) continue;
        const JceWaterFoamState *foam = set->slot[i].foam;
        return jce_water_foam_matches(foam, set->slot[i].field) &&
            foam->time == jce_water_field_time(set->slot[i].field) ? foam : NULL;
    }
    return NULL;
}

static int shore_slot(const JceWaterFieldSet *set, uint64_t key)
{
    if (!set) return -1;
    for (int i = 0; i < WATER_FIELD_SET_MAX; ++i)
        if (set->slot[i].used && set->slot[i].key == key) return i;
    return -1;
}

bool jce_water_field_set_shore_matches(const JceWaterFieldSet *set, uint64_t key,
    const JceWaterShoreBinding *binding)
{
    const int i = shore_slot(set, key);
    if (i < 0 || !set->slot[i].shore || !binding) return false;
    const JceWaterShoreBinding *old = &set->slot[i].shore_binding;
    return old->terrain == binding->terrain && old->terrain_revision == binding->terrain_revision &&
        old->still_y == binding->still_y && old->min_x == binding->min_x &&
        old->min_z == binding->min_z && old->extent == binding->extent &&
        old->drag == binding->drag && old->absorption == binding->absorption &&
        memcmp(old->terrain_world.raw, binding->terrain_world.raw, sizeof old->terrain_world.raw) == 0;
}

void jce_water_field_set_configure_shore(JceWaterFieldSet *set, uint64_t key,
    const JceWaterShoreBinding *binding, const JceWaterShoreStateDesc *desc, const float *bed)
{
    const int i = shore_slot(set, key);
    if (i < 0) return;
    JceWaterShoreState *next = NULL;
    if (binding && desc && bed) {
        int other = 0, cells = 0;
        for (int j = 0; j < WATER_FIELD_SET_MAX; ++j) {
            if (j == i || !set->slot[j].shore) continue;
            JceWaterShoreSnapshot snapshot;
            if (!jce_water_shore_state_snapshot(set->slot[j].shore, &snapshot)) continue;
            ++other;
            cells += snapshot.side * snapshot.side;
        }
        /* Preserve the previous four 32x32 regions' total solver-cell budget.
         * One explicit 64x64 region consumes it; never silently coarsen a bed. */
        if (other < 4 && desc->side >= 4 && desc->side <= JCE_WATER_SHORE_MAX_SIDE &&
            cells + desc->side * desc->side <= JCE_WATER_SHORE_CELL_BUDGET) {
            uint64_t identity = ++set->shore_identity;
            if (!identity) identity = ++set->shore_identity;
            next = jce_water_shore_state_create(desc, bed, set->time, identity);
        }
    }
    jce_water_shore_state_destroy(set->slot[i].shore);
    set->slot[i].shore = next;
    if (next) set->slot[i].shore_binding = *binding;
}

const JceWaterShoreState *jce_water_field_set_shore(const JceWaterFieldSet *set, uint64_t key)
{
    const int i = shore_slot(set, key);
    if (i < 0) return NULL;
    const JceWaterShoreState *shore = set->slot[i].shore;
    return shore && set->slot[i].field->fft &&
        jce_water_shore_state_time(shore) == set->time ? shore : NULL;
}

static void advance_shore(JceWaterShoreState *shore, const JceWaterField *field, double time)
{
    if (!shore || !field->fft || jce_water_shore_state_time(shore) == time) return;
    JceWaterShoreSnapshot grid;
    if (!jce_water_shore_state_snapshot(shore, &grid) ||
        grid.side < 4 || grid.side > JCE_WATER_SHORE_MAX_SIDE)
        return;
    const float xmin = grid.min_x + 0.5f * grid.cell_size;
    const float zmin = grid.min_z + 0.5f * grid.cell_size;
    const float xmax = xmin + (grid.side - 1) * grid.cell_size;
    const float zmax = zmin + (grid.side - 1) * grid.cell_size;
    float edge[4 * JCE_WATER_SHORE_MAX_SIDE];
    for (int side = 0; side < 4; ++side) {
        for (int i = 0; i < grid.side; ++i) {
            const float x = side == 0 ? xmin : side == 1 ? xmax : xmin + i * grid.cell_size;
            const float z = side == 2 ? zmin : side == 3 ? zmax : zmin + i * grid.cell_size;
            JceWaterSample sample;
            if (!jce_water_field_sample_render(field, x, z, &sample)) return;
            edge[side * grid.side + i] = sample.position.y - field->desc.base_height;
        }
    }
    (void)jce_water_shore_state_evolve(shore, time, edge);
}

bool JCE_CALL jce_water_field_set_advance(JceWaterFieldSet *set,
                                          const void *driver, double dt)
{
    if (!set || !driver || !isfinite(dt) || (set->driver && set->driver != driver))
        return false;
    const double time = set->time + dt;
    if (!isfinite(time)) return false;
    /* A rejected time cannot partially advance one body or its histories. */
    for (int i = 0; i < WATER_FIELD_SET_MAX; ++i) {
        double phase;
        if (set->slot[i].used && !field_phase_at(set->slot[i].field, time, &phase))
            return false;
    }
    set->driver = driver;  /* the first successful caller claims the clock */
    set->time = time;
    for (int i = 0; i < WATER_FIELD_SET_MAX; i++)
        if (set->slot[i].used) {
            jce_water_field_set_time(set->slot[i].field, set->time);
            if (set->slot[i].foam)
                (void)jce_water_foam_advance(set->slot[i].foam, set->slot[i].field);
            if (set->slot[i].shore)
                advance_shore(set->slot[i].shore, set->slot[i].field, set->time);
        }
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
        jce_water_foam_destroy(set->slot[i].foam);
        jce_water_shore_state_destroy(set->slot[i].shore);
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
