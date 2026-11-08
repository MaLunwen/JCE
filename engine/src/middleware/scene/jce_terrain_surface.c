/* World-space heightfield projection shared by scene consumers. */
#include "jce_terrain_surface_internal.h"

#include <math.h>
#include <string.h>

static jce_vec3 terrain_surface_transform_point(const jce_mat4 *m, jce_vec3 p)
{
    const jce_vec4 v = jce_m4_mul_v4(m, jce_v4(p.x, p.y, p.z, 1.0f));
    return jce_v3(v.x, v.y, v.z);
}

static jce_vec3 terrain_surface_transform_vector(const jce_mat4 *m, jce_vec3 v)
{
    const jce_vec4 r = jce_m4_mul_v4(m, jce_v4(v.x, v.y, v.z, 0.0f));
    return jce_v3(r.x, r.y, r.z);
}

static void terrain_surface_expand_bounds(JceTerrainSurface *s, jce_vec3 p,
                                     bool first)
{
    if (first) {
        s->world_min = p;
        s->world_max = p;
        return;
    }
    if (p.x < s->world_min.x) s->world_min.x = p.x;
    if (p.y < s->world_min.y) s->world_min.y = p.y;
    if (p.z < s->world_min.z) s->world_min.z = p.z;
    if (p.x > s->world_max.x) s->world_max.x = p.x;
    if (p.y > s->world_max.y) s->world_max.y = p.y;
    if (p.z > s->world_max.z) s->world_max.z = p.z;
}

bool jce_terrain_surface_init(JceTerrainSurface *out, JceTerrain *terrain,
                            const jce_mat4 *world)
{
    if (!out || !terrain || !world) return false;

    const jce_vec3 scale = jce_m4_extract_scale(world);
    if (scale.x <= 1e-6f || scale.y <= 1e-6f || scale.z <= 1e-6f)
        return false;

    memset(out, 0, sizeof *out);
    out->terrain = terrain;
    out->world = *world;
    out->inverse = jce_m4_inverse(world);
    out->normal_matrix = jce_m4_transpose(&out->inverse);
    out->local_min_y = fminf(0.0f, jce_terrain_max_height(terrain));
    out->local_max_y = fmaxf(0.0f, jce_terrain_max_height(terrain));

    const float sx = jce_terrain_world_size_x(terrain);
    const float sz = jce_terrain_world_size_z(terrain);
    bool first = true;
    for (int iy = 0; iy < 2; ++iy)
        for (int iz = 0; iz < 2; ++iz)
            for (int ix = 0; ix < 2; ++ix) {
                const jce_vec3 local = jce_v3(ix ? sx : 0.0f,
                    iy ? out->local_max_y : out->local_min_y,
                    iz ? sz : 0.0f);
                terrain_surface_expand_bounds(out,
                    terrain_surface_transform_point(world, local), first);
                first = false;
            }

    const float eps = 1e-5f;
    out->upright = fabsf(world->col[1].x) <= eps * scale.y &&
                   fabsf(world->col[1].z) <= eps * scale.y &&
                   fabsf(world->col[0].y) <= eps * scale.x &&
                   fabsf(world->col[2].y) <= eps * scale.z;
    return true;
}

static bool terrain_surface_local_normal(const JceTerrainSurface *s, float x,
                                    float z, jce_vec3 *out_normal)
{
    const JceTerrain *terrain = s->terrain;
    const int width = jce_terrain_width(terrain);
    const int height = jce_terrain_height(terrain);
    const float size_x = jce_terrain_world_size_x(terrain);
    const float size_z = jce_terrain_world_size_z(terrain);
    if (width < 2 || height < 2 || size_x <= 0.0f || size_z <= 0.0f)
        return false;

    const float ex = size_x / (float)(width - 1);
    const float ez = size_z / (float)(height - 1);
    const float xm = fmaxf(0.0f, x - ex);
    const float xp = fminf(size_x, x + ex);
    const float zm = fmaxf(0.0f, z - ez);
    const float zp = fminf(size_z, z + ez);
    const float dx_den = xp - xm;
    const float dz_den = zp - zm;
    if (dx_den <= 1e-8f || dz_den <= 1e-8f) return false;

    const float dx = (jce_terrain_sample_height(terrain, xp, z) -
                      jce_terrain_sample_height(terrain, xm, z)) / dx_den;
    const float dz = (jce_terrain_sample_height(terrain, x, zp) -
                      jce_terrain_sample_height(terrain, x, zm)) / dz_den;
    *out_normal = jce_v3_normalize(jce_v3(-dx, 1.0f, -dz));
    return jce_v3_len(*out_normal) > 0.0f;
}

static bool terrain_surface_ray_interval(const JceTerrainSurface *s, jce_vec3 o,
                                    jce_vec3 d, float *out_near,
                                    float *out_far)
{
    const jce_vec3 mn = jce_v3(0.0f, s->local_min_y,
                                0.0f);
    const jce_vec3 mx = jce_v3(jce_terrain_world_size_x(s->terrain),
                                s->local_max_y,
                                jce_terrain_world_size_z(s->terrain));
    float tmin = 0.0f;
    float tmax = 1e30f;
    const float ov[3] = { o.x, o.y, o.z };
    const float dv[3] = { d.x, d.y, d.z };
    const float av[3] = { mn.x, mn.y, mn.z };
    const float bv[3] = { mx.x, mx.y, mx.z };
    for (int axis = 0; axis < 3; ++axis) {
        if (fabsf(dv[axis]) <= 1e-8f) {
            if (ov[axis] < av[axis] || ov[axis] > bv[axis]) return false;
            continue;
        }
        float a = (av[axis] - ov[axis]) / dv[axis];
        float b = (bv[axis] - ov[axis]) / dv[axis];
        if (a > b) { const float tmp = a; a = b; b = tmp; }
        if (a > tmin) tmin = a;
        if (b < tmax) tmax = b;
        if (tmin > tmax) return false;
    }
    *out_near = tmin;
    *out_far = tmax;
    return tmax >= 0.0f;
}

static float terrain_surface_signed_height(const JceTerrainSurface *s, jce_vec3 p)
{
    return p.y - jce_terrain_sample_height(s->terrain, p.x, p.z);
}

static bool terrain_surface_sample_tilted(const JceTerrainSurface *s, float world_x,
                                     float world_z, jce_vec3 *out_local)
{
    const float pad = fmaxf(1.0f, s->world_max.y - s->world_min.y) * 0.01f;
    const jce_vec3 world_o = jce_v3(world_x, s->world_max.y + pad, world_z);
    const jce_vec3 local_o = terrain_surface_transform_point(&s->inverse, world_o);
    jce_vec3 local_d = terrain_surface_transform_vector(&s->inverse,
                                                    jce_v3(0.0f, -1.0f, 0.0f));
    local_d = jce_v3_normalize(local_d);
    if (jce_v3_len(local_d) <= 0.0f) return false;

    float t0, t1;
    if (!terrain_surface_ray_interval(s, local_o, local_d, &t0, &t1)) return false;
    if (t0 < 0.0f) t0 = 0.0f;
    if (t1 < t0) return false;

    const float cell_x = jce_terrain_world_size_x(s->terrain) /
                         (float)(jce_terrain_width(s->terrain) - 1);
    const float cell_z = jce_terrain_world_size_z(s->terrain) /
                         (float)(jce_terrain_height(s->terrain) - 1);
    float step = 0.5f * fminf(cell_x, cell_z);
    if (step < 0.01f) step = 0.01f;
    int steps = (int)ceilf((t1 - t0) / step);
    if (steps < 1) steps = 1;
    if (steps > 4096) steps = 4096;

    float prev_t = t0;
    jce_vec3 prev_p = jce_v3_add(local_o, jce_v3_scale(local_d, prev_t));
    float prev_f = terrain_surface_signed_height(s, prev_p);
    for (int i = 1; i <= steps; ++i) {
        const float t = t0 + (t1 - t0) * (float)i / (float)steps;
        const jce_vec3 p = jce_v3_add(local_o, jce_v3_scale(local_d, t));
        const float f = terrain_surface_signed_height(s, p);
        if (prev_f >= 0.0f && f <= 0.0f) {
            float a = prev_t;
            float b = t;
            for (int it = 0; it < 14; ++it) {
                const float mid = 0.5f * (a + b);
                const jce_vec3 mp = jce_v3_add(local_o,
                                               jce_v3_scale(local_d, mid));
                if (terrain_surface_signed_height(s, mp) > 0.0f) a = mid;
                else b = mid;
            }
            *out_local = jce_v3_add(local_o,
                                    jce_v3_scale(local_d, 0.5f * (a + b)));
            return true;
        }
        prev_t = t;
        prev_f = f;
    }
    return false;
}

static bool terrain_surface_project(const JceTerrainSurface *s,
    float world_x, float world_z, jce_vec3 *out_local)
{
    const float eps = 1e-4f;
    if (world_x < s->world_min.x - eps || world_x > s->world_max.x + eps ||
        world_z < s->world_min.z - eps || world_z > s->world_max.z + eps)
        return false;

    jce_vec3 local;
    if (s->upright) {
        local = terrain_surface_transform_point(&s->inverse,
            jce_v3(world_x, 0.0f, world_z));
        const float sx = jce_terrain_world_size_x(s->terrain);
        const float sz = jce_terrain_world_size_z(s->terrain);
        if (local.x < -eps || local.x > sx + eps ||
            local.z < -eps || local.z > sz + eps)
            return false;
        local.x = fminf(sx, fmaxf(0.0f, local.x));
        local.z = fminf(sz, fmaxf(0.0f, local.z));
        local.y = jce_terrain_sample_height(s->terrain, local.x, local.z);
    } else if (!terrain_surface_sample_tilted(s, world_x, world_z, &local)) {
        return false;
    }

    *out_local = local;
    return true;
}

bool jce_terrain_surface_sample(const JceTerrainSurface *s, float world_x,
    float world_z, bool want_normal, float *out_world_y, jce_vec3 *out_normal)
{
    jce_vec3 local;
    if (!terrain_surface_project(s, world_x, world_z, &local)) return false;
    *out_world_y = terrain_surface_transform_point(&s->world, local).y;
    if (!want_normal) return true;

    jce_vec3 local_normal;
    if (!terrain_surface_local_normal(s, local.x, local.z, &local_normal))
        return false;
    *out_normal = jce_v3_normalize(terrain_surface_transform_vector(
        &s->normal_matrix, local_normal));
    return jce_v3_len(*out_normal) > 0.0f;
}

bool jce_terrain_surface_sample_bed(JceTerrain *terrain, const jce_mat4 *world,
    unsigned side, float cell_size, float min_x, float min_z, float still_y,
    float *bed)
{
    if (!terrain || !world || !bed || side < 4u || side > 128u ||
        !(cell_size >= 0.25f && cell_size <= 100.0f) ||
        !isfinite(min_x) || !isfinite(min_z) || !isfinite(still_y))
        return false;

    /* A failed tile read reports height zero through the legacy sampler.
     * Only a resident height grid can certify nearshore bed coverage. */
    if (!jce_terrain_heights(terrain)) return false;
    const int width = jce_terrain_width(terrain);
    const int height = jce_terrain_height(terrain);
    const float sx = jce_terrain_world_size_x(terrain);
    const float sz = jce_terrain_world_size_z(terrain);
    if (width < 2 || height < 2 || !(sx > 0.0f) || !(sz > 0.0f) ||
        !isfinite(sx) || !isfinite(sz))
        return false;

    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!isfinite(world->raw[c][r])) return false;

    /* The legacy matrix inverse returns identity for singular inputs.
     * Reject those inputs here rather than publishing a bed in the wrong space. */
    const jce_vec3 x = jce_v3(world->col[0].x, world->col[0].y, world->col[0].z);
    const jce_vec3 y = jce_v3(world->col[1].x, world->col[1].y, world->col[1].z);
    const jce_vec3 z = jce_v3(world->col[2].x, world->col[2].y, world->col[2].z);
    const float det = jce_v3_dot(x, jce_v3_cross(y, z));
    if (!isfinite(det) || fabsf(det) < 1e-8f) return false;

    JceTerrainSurface surface;
    if (!jce_terrain_surface_init(&surface, terrain, world)) return false;

    for (unsigned row = 0; row < side; ++row) {
        const float wz = min_z + ((float)row + 0.5f) * cell_size;
        for (unsigned col = 0; col < side; ++col) {
            const float wx = min_x + ((float)col + 0.5f) * cell_size;
            jce_vec3 local;
            if (!terrain_surface_project(&surface, wx, wz, &local) ||
                !isfinite(local.x) || !isfinite(local.z))
                return false;
            const float fx = fminf(fmaxf(local.x / sx, 0.0f), 1.0f);
            const float fz = fminf(fmaxf(local.z / sz, 0.0f), 1.0f);
            int cx = (int)(fx * (float)(width - 1));
            int cz = (int)(fz * (float)(height - 1));
            if (cx == width - 1) --cx;
            if (cz == height - 1) --cz;
            if (jce_terrain_cell_is_hole(terrain, cx, cz)) return false;

            const float elevation =
                terrain_surface_transform_point(world, local).y - still_y;
            if (!(elevation >= -64.0f && elevation <= 64.0f)) return false;
            bed[row * side + col] = elevation;
        }
    }
    return true;
}
