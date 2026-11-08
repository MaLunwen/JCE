#include "jce_water_shore_step.h"
#include <math.h>
#include <stddef.h>

/* Audusse et al. hydrostatic reconstruction, with a Rusanov face flux. */
static const double gravity = 9.81;
static const double dry_epsilon = 1e-7;

typedef struct ShoreFlux {
    double depth, x, z;
} ShoreFlux;

typedef struct ShoreFace {
    ShoreFlux left, right;
} ShoreFace;

static double clamp_unit(double x)
{
    return fmin(1.0, fmax(0.0, x));
}

static ShoreFace shore_face(JceWaterShoreCell a, JceWaterShoreCell b,
                            double bed_a, double bed_b, bool along_z)
{
    const double raised_bed = fmax(bed_a, bed_b);
    const double ha = fmax(0.0, a.depth + bed_a - raised_bed);
    const double hb = fmax(0.0, b.depth + bed_b - raised_bed);
    const double ia = a.depth > dry_epsilon ? 1.0 / a.depth : 0.0;
    const double ib = b.depth > dry_epsilon ? 1.0 / b.depth : 0.0;
    const double ua = a.momentum_x * ia, va = a.momentum_z * ia;
    const double ub = b.momentum_x * ib, vb = b.momentum_z * ib;
    const double un_a = along_z ? va : ua;
    const double un_b = along_z ? vb : ub;
    const double wave_speed = fmax(fabs(un_a) + sqrt(gravity * ha),
                                   fabs(un_b) + sqrt(gravity * hb));
    const double qa = ha * ua, ra = ha * va;
    const double qb = hb * ub, rb = hb * vb;
    const double pressure = 0.5 * gravity * (ha * ha + hb * hb);
    const ShoreFlux common = {
        0.5 * (un_a * ha + un_b * hb - wave_speed * (hb - ha)),
        0.5 * (un_a * qa + un_b * qb - wave_speed * (qb - qa) +
               (along_z ? 0.0 : pressure)),
        0.5 * (un_a * ra + un_b * rb - wave_speed * (rb - ra) +
               (along_z ? pressure : 0.0)),
    };
    ShoreFace face = {common, common};
    const double ca = 0.5 * gravity * ((double)a.depth * a.depth - ha * ha);
    const double cb = 0.5 * gravity * ((double)b.depth * b.depth - hb * hb);
    if (along_z) {
        face.left.z += ca;
        face.right.z += cb;
    } else {
        face.left.x += ca;
        face.right.x += cb;
    }
    return face;
}

static bool in_range(double v, double lo, double hi)
{
    return v >= lo && v <= hi;
}

static bool parameters_valid(const JceWaterShoreGrid *grid,
                             const JceWaterShoreControls *p)
{
    return grid && p && grid->bed && grid->side >= 4 && grid->side <= 256 &&
        in_range(grid->cell_size, 0.05, 1000.0) &&
        in_range(p->implicit_drag, 0.0, 100.0) &&
        in_range(p->exponential_drag, 0.0, 100.0) &&
        in_range(p->dry_drag, 0.0, 100.0) &&
        in_range(p->wet_depth, 0.001, 10.0) &&
        in_range(p->max_speed, 0.1, 100.0) &&
        in_range(p->sponge_width, 0.0, 0.5) &&
        in_range(p->sponge_rate, 0.0, 100.0) &&
        (p->sponge_width == 0.0f || grid->boundary_target);
}

static bool cell_speed(JceWaterShoreCell c, double *max_speed)
{
    if (!in_range(c.depth, 0.0, 10000.0) ||
        !isfinite(c.momentum_x) || !isfinite(c.momentum_z)) return false;
    if (c.depth <= dry_epsilon) return true;
    const double speed = fmax(fabs(c.momentum_x), fabs(c.momentum_z)) / c.depth
                       + sqrt(gravity * c.depth);
    *max_speed = fmax(*max_speed, speed);
    return true;
}

static JceWaterShoreCell ghost_cell(JceWaterShoreCell c,
    const JceWaterShoreCell *target, bool along_z)
{
    if (target) return *target;
    if (along_z) c.momentum_z = -c.momentum_z;
    else c.momentum_x = -c.momentum_x;
    return c;
}

static double sponge_weight(int x, int z, int side, double width)
{
    if (width <= 0.0) return 0.0;
    const double edge = fmin(fmin(x + 0.5, z + 0.5),
                             fmin(side - x - 0.5, side - z - 0.5));
    const double t = clamp_unit(1.0 - edge / (side * width));
    return t * t * (3.0 - 2.0 * t);
}

bool jce_water_shore_substep(const JceWaterShoreGrid *grid,
    const JceWaterShoreControls *p, const JceWaterShoreCell *current,
    JceWaterShoreCell *next, double frame_dt, double *consumed_dt)
{
    if (!consumed_dt) return false;
    *consumed_dt = 0.0;
    if (!parameters_valid(grid, p) || !current || !next || current == next ||
        !isfinite(frame_dt) || frame_dt <= 0.0) return false;
    const int side = grid->side;
    const int count = side * side;
    const JceWaterShoreCell *boundary = grid->boundary_target;
    double speed = 0.0;
    for (int i = 0; i < count; ++i) {
        if (!in_range(grid->bed[i], -10000.0, 10000.0) ||
            !cell_speed(current[i], &speed) ||
            (boundary && !cell_speed(boundary[i], &speed))) return false;
    }
    /* Conservative unsplit 2D bound, with headroom at a wet/dry front. */
    const double dt = fmin(frame_dt, 0.2 * grid->cell_size / fmax(speed, dry_epsilon));
    const double dt_dx = dt / grid->cell_size;
    const double drag = 1.0 / (1.0 + p->implicit_drag * dt);
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            const int i = z * side + x;
            const JceWaterShoreCell c = current[i];
            const JceWaterShoreCell *target = boundary ? &boundary[i] : NULL;
            const JceWaterShoreCell gx = ghost_cell(c, target, false);
            const JceWaterShoreCell gz = ghost_cell(c, target, true);
            const int east = x < side - 1 ? i + 1 : i;
            const int west = x > 0 ? i - 1 : i;
            const int north = z < side - 1 ? i + side : i;
            const int south = z > 0 ? i - side : i;
            const double bed = grid->bed[i];
            const ShoreFlux e = shore_face(c, east != i ? current[east] : gx,
                                           bed, grid->bed[east], false).left;
            const ShoreFlux w = shore_face(west != i ? current[west] : gx, c,
                                           grid->bed[west], bed, false).right;
            const ShoreFlux n = shore_face(c, north != i ? current[north] : gz,
                                           bed, grid->bed[north], true).left;
            const ShoreFlux s = shore_face(south != i ? current[south] : gz, c,
                                           grid->bed[south], bed, true).right;
            double h = c.depth - dt_dx * (e.depth - w.depth + n.depth - s.depth);
            double q = c.momentum_x - dt_dx * (e.x - w.x + n.x - s.x);
            double r = c.momentum_z - dt_dx * (e.z - w.z + n.z - s.z);
            if (h < -dry_epsilon || !isfinite(h) || !isfinite(q) || !isfinite(r))
                return false;
            h = fmax(0.0, h);
            const double wet = clamp_unit(h / p->wet_depth);
            const double friction = drag * exp(-dt * (p->exponential_drag + p->dry_drag * (1.0 - wet)));
            q *= friction;
            r *= friction;
            if (target) {
                const double t = -expm1(-dt * p->sponge_rate *
                    sponge_weight(x, z, side, p->sponge_width));
                h += t * (target->depth - h);
                q += t * (target->momentum_x - q);
                r += t * (target->momentum_z - r);
            }
            const double momentum = hypot(q, r);
            const double limit = h * p->max_speed;
            if (h <= dry_epsilon) {
                q = 0.0;
                r = 0.0;
            } else if (momentum > limit) {
                q *= limit / momentum;
                r *= limit / momentum;
            }
            next[i] = (JceWaterShoreCell){(float)h, (float)q, (float)r};
        }
    }
    *consumed_dt = dt;
    return true;
}
