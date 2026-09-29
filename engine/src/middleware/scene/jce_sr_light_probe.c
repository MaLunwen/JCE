/*
 * jce_sr_light_probe.c  See jce_sr_light_probe.h.
 */

#include "jce_sr_light_probe.h"

#include "os/core/jce_memory.h"      /* JCE_MALLOC / JCE_REALLOC / JCE_FREE */
#include <jce/os/core/jce_log.h>

#include <string.h>

#define LOG_TAG "jce_sr_light_probe"

/* A scene's whole baked probe population.  4096 is ~64 authored groups at the
 * JCE_LIGHT_PROBE_MAX of 64 each; past that the sample cost (a linear scan per
 * draw anchor) stops being free, and a scene that large wants a spatial index
 * rather than a bigger array. */
#define SR_PROBE_SET_MAX 4096

typedef struct {
    jce_vec3 pos;
    float    sh9[9][3];
} SrProbe;

struct JceSrProbeSet {
    SrProbe *p;
    int      count;
    int      cap;
    bool     warned_full;   /* the overflow is logged ONCE, not per frame */
};

JceSrProbeSet *sr_probe_set_create(void)
{
    /* JCE_NEW, not a hand-rolled malloc+memset: the tree has a macro for
     * exactly this and find_similar_code.py caught the hand-rolled version as
     * a duplicate of jce_save_migration_registry_create() on its first run. */
    return JCE_NEW(JceSrProbeSet);
}

void sr_probe_set_destroy(JceSrProbeSet *s)
{
    if (!s) return;
    if (s->p) JCE_FREE(s->p);
    JCE_FREE(s);
}

void sr_probe_set_clear(JceSrProbeSet *s)
{
    if (s) s->count = 0;   /* keeps the allocation; the population is stable */
}

int sr_probe_set_count(const JceSrProbeSet *s)
{
    return s ? s->count : 0;
}

bool sr_probe_set_add(JceSrProbeSet *s, jce_vec3 pos, const float sh9[9][3])
{
    if (!s || !sh9) return false;
    if (s->count >= SR_PROBE_SET_MAX) {
        if (!s->warned_full) {
            s->warned_full = true;
            LOG_WARN(LOG_TAG, "baked light probes capped at %d; the rest of "
                              "this scene's probes are ignored",
                     SR_PROBE_SET_MAX);
        }
        return false;
    }
    if (s->count == s->cap) {
        int ncap = s->cap ? s->cap * 2 : 64;
        if (ncap > SR_PROBE_SET_MAX) ncap = SR_PROBE_SET_MAX;
        SrProbe *np = (SrProbe *)JCE_REALLOC(s->p, (size_t)ncap * sizeof(*np));
        if (!np) return false;
        s->p   = np;
        s->cap = ncap;
    }
    s->p[s->count].pos = pos;
    memcpy(s->p[s->count].sh9, sh9, sizeof(s->p[0].sh9));
    s->count++;
    return true;
}

float sr_probe_set_sample(const JceSrProbeSet *s, jce_vec3 p, float out[9][3])
{
    if (!s || !out || s->count <= 0) return 0.0f;

    /* Partial selection of the k nearest.  An insertion sort over k entries
     * beats a full sort by a mile at this size and, more to the point, is the
     * whole reason this stays a linear scan instead of needing an index. */
    int   best_i[SR_PROBE_SAMPLE_K];
    float best_d2[SR_PROBE_SAMPLE_K];
    int   n = 0;
    for (int i = 0; i < s->count; i++) {
        const jce_vec3 d = jce_v3_sub(s->p[i].pos, p);
        const float    d2 = jce_v3_dot(d, d);
        if (n < SR_PROBE_SAMPLE_K) {
            int j = n++;
            while (j > 0 && best_d2[j - 1] > d2) {
                best_d2[j] = best_d2[j - 1];
                best_i[j]  = best_i[j - 1];
                j--;
            }
            best_d2[j] = d2;
            best_i[j]  = i;
        } else if (d2 < best_d2[SR_PROBE_SAMPLE_K - 1]) {
            int j = SR_PROBE_SAMPLE_K - 1;
            while (j > 0 && best_d2[j - 1] > d2) {
                best_d2[j] = best_d2[j - 1];
                best_i[j]  = best_i[j - 1];
                j--;
            }
            best_d2[j] = d2;
            best_i[j]  = i;
        }
    }

    /* w = 1/(d^2 + eps).  The epsilon is what makes "standing exactly on a
     * probe" finite instead of an infinity that turns the whole blend into
     * NaN -- and it is small enough (1e-6 m^2 = a millimetre squared) that at
     * any real distance it changes nothing. */
    float w[SR_PROBE_SAMPLE_K], wsum = 0.0f;
    for (int k = 0; k < n; k++) {
        w[k] = 1.0f / (best_d2[k] + 1e-6f);
        wsum += w[k];
    }
    if (!(wsum > 0.0f)) return 0.0f;   /* also catches NaN */

    const float inv = 1.0f / wsum;
    memset(out, 0, 9 * 3 * sizeof(float));
    for (int k = 0; k < n; k++) {
        const float ww = w[k] * inv;
        const float (*src)[3] = s->p[best_i[k]].sh9;
        for (int c = 0; c < 9; c++) {
            out[c][0] += src[c][0] * ww;
            out[c][1] += src[c][1] * ww;
            out[c][2] += src[c][2] * ww;
        }
    }
    return 1.0f;
}
