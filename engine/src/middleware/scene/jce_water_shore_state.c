#include "jce_water_shore_state.h"
#include "jce_water_shore_atlas.h"

#include <math.h>
#include "os/core/jce_memory.h"
#include <string.h>

enum { MAX_SIDE = 128, MAX_SUBSTEPS = 64, LAYERS = 3 };
static const double max_delta = 0.125;

struct JceWaterShoreState {
    JceWaterShoreStateDesc desc;
    float *bed;
    JceWaterShoreCell *flow[LAYERS];
    float *density[LAYERS];
    JceWaterShoreCell *target;
    JceShoreDistance *shore;
    float previous_edge[4 * MAX_SIDE];
    int published;
    bool has_shore;
    double time;
    uint64_t identity, revision;
};

void jce_water_shore_state_destroy(JceWaterShoreState *s)
{
    if (!s) return;
    for (int i = 0; i < LAYERS; ++i) {
        JCE_FREE(s->flow[i]);
        JCE_FREE(s->density[i]);
    }
    JCE_FREE(s->target);
    JCE_FREE(s->shore);
    JCE_FREE(s->bed);
    JCE_FREE(s);
}

static bool publish_distance(JceWaterShoreState *s, size_t count)
{
    int32_t *a = JCE_MALLOC(count * sizeof(*a));
    int32_t *b = JCE_MALLOC(count * sizeof(*b));
    uint8_t *wet = JCE_MALLOC(count * sizeof(*wet));
    bool ok = false;
    if (a && b && wet) {
        for (size_t i = 0; i < count; ++i) wet[i] = s->bed[i] < 0.0f;
        ok = jce_water_shore_jfa(s->desc.side, s->desc.cell_size, wet,
                                  a, b, s->shore, &s->has_shore);
    }
    JCE_FREE(wet);
    JCE_FREE(a);
    JCE_FREE(b);
    return ok;
}

static void reset(JceWaterShoreState *s, double time, const float *edge)
{
    const size_t count = (size_t)s->desc.side * s->desc.side;
    JceWaterShoreCell *flow = s->flow[s->published];
    for (size_t i = 0; i < count; ++i)
        flow[i] = (JceWaterShoreCell){fmaxf(-s->bed[i], 0.0f), 0.0f, 0.0f};
    memset(s->density[s->published], 0, count * sizeof(float));
    if (edge) memcpy(s->previous_edge, edge, (size_t)s->desc.side * 4 * sizeof(float));
    else memset(s->previous_edge, 0, sizeof(s->previous_edge));
    s->time = time;
    ++s->revision;
}

JceWaterShoreState *jce_water_shore_state_create(const JceWaterShoreStateDesc *desc,
    const float *bed, double time, uint64_t identity)
{
    if (!desc || !bed || !isfinite(time) || desc->side < 4 || desc->side > MAX_SIDE ||
        !(desc->cell_size >= 0.25f && desc->cell_size <= 100.0f) ||
        !isfinite(desc->min_x) || !isfinite(desc->min_z) || !isfinite(desc->still_y)) return NULL;
    const size_t count = (size_t)desc->side * desc->side;
    for (size_t i = 0; i < count; ++i)
        if (!(bed[i] >= -64.0f && bed[i] <= 64.0f)) return NULL;
    JceWaterShoreState *s = JCE_CALLOC(1, sizeof(*s));
    if (!s) return NULL;
    s->desc = *desc;
    s->identity = identity;
    s->bed = JCE_MALLOC(count * sizeof(*s->bed));
    s->target = JCE_MALLOC(count * sizeof(*s->target));
    s->shore = JCE_MALLOC(count * sizeof(*s->shore));
    if (!s->bed || !s->target || !s->shore) goto fail;
    for (int i = 0; i < LAYERS; ++i) {
        s->flow[i] = JCE_MALLOC(count * sizeof(*s->flow[i]));
        s->density[i] = JCE_MALLOC(count * sizeof(*s->density[i]));
        if (!s->flow[i] || !s->density[i]) goto fail;
    }
    memcpy(s->bed, bed, count * sizeof(*bed));
    if (!publish_distance(s, count)) goto fail;
    reset(s, time, NULL);
    /* Validate both existing steppers before publishing this owner. */
    double elapsed;
    const JceWaterShoreGrid grid = {desc->side, desc->cell_size, s->bed, s->target};
    if (!jce_water_shore_boundary(desc->side, s->bed, desc->water.sponge_width,
            desc->max_incoming_height, desc->max_incoming_speed, NULL, s->target) ||
        !jce_water_shore_substep(&grid, &desc->water, s->flow[0], s->flow[1], 0.001, &elapsed) ||
        !jce_water_shore_foam_step(desc->side, desc->cell_size, elapsed, &desc->foam,
            s->shore, s->has_shore, s->flow[1], s->density[0], s->density[1])) goto fail;
    return s;
fail:
    jce_water_shore_state_destroy(s);
    return NULL;
}

bool jce_water_shore_state_evolve(JceWaterShoreState *s, double time,
    const float *edge)
{
    if (!s || !isfinite(time)) return false;
    const JceWaterShoreStateDesc *d = &s->desc;
    const int edge_count = 4 * d->side;
    if (edge) {
        for (int i = 0; i < edge_count; ++i)
            if (!isfinite(edge[i])) return false;
    }
    const double dt = time - s->time;
    if (dt == 0.0) return true;
    if (dt < 0.0 || dt > max_delta) {
        reset(s, time, edge);
        return true;
    }
    const int scratch_a = (s->published + 1) % LAYERS;
    const int scratch_b = (s->published + 2) % LAYERS;
    int current = s->published;
    int next = scratch_a;
    double remaining = dt;
    const JceWaterShoreGrid grid = {d->side, d->cell_size, s->bed, s->target};
    float current_edge[4 * MAX_SIDE];
    for (int step = 0; step < MAX_SUBSTEPS && remaining > 1e-12; ++step) {
        const double alpha = 1.0 - remaining / dt;
        for (int i = 0; i < edge_count; ++i) {
            const float endpoint = edge ? edge[i] : 0.0f;
            const double prior = s->previous_edge[i];
            current_edge[i] = (float)(prior + alpha * ((double)endpoint - prior));
        }
        double used;
        if (!jce_water_shore_boundary(d->side, s->bed, d->water.sponge_width,
                d->max_incoming_height, d->max_incoming_speed, current_edge, s->target) ||
            !jce_water_shore_substep(&grid, &d->water, s->flow[current], s->flow[next],
                remaining, &used) ||
            !jce_water_shore_foam_step(d->side, d->cell_size, used, &d->foam,
                s->shore, s->has_shore, s->flow[next], s->density[current], s->density[next]))
            return false;
        remaining -= used;
        current = next;
        next = next == scratch_a ? scratch_b : scratch_a;
    }
    if (remaining > 1e-12) return false;
    if (edge) memcpy(s->previous_edge, edge, (size_t)edge_count * sizeof(float));
    else memset(s->previous_edge, 0, sizeof(s->previous_edge));
    s->published = current;
    s->time = time;
    ++s->revision;
    return true;
}

bool jce_water_shore_state_sample(const JceWaterShoreState *s, float wx, float wz,
    JceWaterShoreStateSample *out)
{
    if (!s || !out || !isfinite(wx) || !isfinite(wz)) return false;
    const JceWaterShoreStateDesc *d = &s->desc;
    const float u = (wx - d->min_x) / d->cell_size;
    const float v = (wz - d->min_z) / d->cell_size;
    if (!(u >= 0 && u <= d->side && v >= 0 && v <= d->side)) return false;
    const float cx = fminf((float)d->side - 1, fmaxf(0.0f, u - 0.5f));
    const float cz = fminf((float)d->side - 1, fmaxf(0.0f, v - 0.5f));
    const int x = (int)cx, z = (int)cz;
    const int right = x + (x + 1 < d->side), upper = z + (z + 1 < d->side);
    const float fx = cx - x, fz = cz - z;
    const float weights[4] = {(1-fx)*(1-fz), fx*(1-fz), (1-fx)*fz, fx*fz};
    const int indices[4] = {z*d->side+x, z*d->side+right, upper*d->side+x, upper*d->side+right};
    const float edge = fminf(fminf(u, v), fminf(d->side-u, d->side-v));
    const float blend = fminf(1.0f, edge / fmaxf(1.0f, d->side * d->water.sponge_width));
    JceWaterShoreStateSample sample = {d->still_y, 0.0f, 0.0f, blend * blend * (3.0f - 2.0f * blend)};
    for (int k = 0; k < 4; ++k) {
        const int i = indices[k];
        const float h = s->flow[s->published][i].depth;
        sample.surface_y += weights[k] * jce_water_shore_render_offset(
            d->side, s->flow[s->published], s->bed, i);
        sample.depth += weights[k] * h;
        sample.foam += weights[k] * s->density[s->published][i];
    }
    *out = sample;
    return true;
}

double jce_water_shore_state_time(const JceWaterShoreState *s) { return s ? s->time : 0.0; }
uint64_t jce_water_shore_state_revision(const JceWaterShoreState *s) { return s ? s->revision : 0; }

bool jce_water_shore_state_snapshot(const JceWaterShoreState *s, JceWaterShoreSnapshot *out)
{
    if (!s || !out) return false;
    *out = (JceWaterShoreSnapshot){
        s->desc.side, s->desc.cell_size, s->desc.min_x, s->desc.min_z,
        s->flow[s->published], s->bed, s->density[s->published], s->identity, s->revision
    };
    return true;
}
