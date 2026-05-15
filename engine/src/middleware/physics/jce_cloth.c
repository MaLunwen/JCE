/*
 * jce_cloth.c  Cloth descriptor — data layer only.
 *
 * Bullet integration lives elsewhere; this module owns particle
 * arrays + derived constraints.  The triangle-edge helper does a
 * canonical-pair set (smaller index first) to dedupe shared edges.
 */

#include <jce/middleware/physics/jce_cloth.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

void jce_cloth_desc_init(JceClothDesc *d)
{
    if (!d) return;
    memset(d, 0, sizeof(*d));
    d->damping           = 0.02f;
    d->stretching_stiff  = 0.7f;
    d->bending_stiff     = 0.2f;
    d->gravity_mul       = 1.0f;
}

void jce_cloth_desc_dispose(JceClothDesc *d)
{
    if (!d) return;
    free(d->positions);
    free(d->inv_mass);
    free(d->distance_constraints);
    free(d->bending_constraints);
    memset(d, 0, sizeof(*d));
}

bool jce_cloth_set_particle_count(JceClothDesc *d, uint32_t count)
{
    if (!d) return false;
    float *p = (float *)realloc(d->positions, count * 3 * sizeof(float));
    if (!p && count > 0) return false;
    d->positions = p;
    d->particle_count = count;
    if (d->inv_mass) {
        float *q = (float *)realloc(d->inv_mass, count * sizeof(float));
        if (!q && count > 0) return false;
        d->inv_mass = q;
    }
    return true;
}

bool jce_cloth_set_positions(JceClothDesc *d, const float *xyz)
{
    if (!d || !d->positions || !xyz) return false;
    memcpy(d->positions, xyz, d->particle_count * 3 * sizeof(float));
    return true;
}

bool jce_cloth_set_inv_mass(JceClothDesc *d, const float *m)
{
    if (!d) return false;
    if (!d->inv_mass) {
        d->inv_mass = (float *)malloc(d->particle_count * sizeof(float));
        if (!d->inv_mass) return false;
    }
    memcpy(d->inv_mass, m, d->particle_count * sizeof(float));
    return true;
}

bool jce_cloth_set_distance_constraints(JceClothDesc *d,
                                          const JceClothConstraint *c,
                                          uint32_t count)
{
    if (!d) return false;
    JceClothConstraint *p = (JceClothConstraint *)realloc(
        d->distance_constraints, count * sizeof(JceClothConstraint));
    if (!p && count > 0) return false;
    d->distance_constraints = p;
    if (count > 0) memcpy(p, c, count * sizeof(JceClothConstraint));
    d->distance_count = count;
    return true;
}

bool jce_cloth_set_bending_constraints(JceClothDesc *d,
                                         const JceClothConstraint *c,
                                         uint32_t count)
{
    if (!d) return false;
    JceClothConstraint *p = (JceClothConstraint *)realloc(
        d->bending_constraints, count * sizeof(JceClothConstraint));
    if (!p && count > 0) return false;
    d->bending_constraints = p;
    if (count > 0) memcpy(p, c, count * sizeof(JceClothConstraint));
    d->bending_count = count;
    return true;
}

static uint64_t edge_key(uint32_t a, uint32_t b)
{
    if (a > b) { uint32_t t = a; a = b; b = t; }
    return ((uint64_t)a << 32) | (uint64_t)b;
}

bool jce_cloth_build_distance_from_triangles(JceClothDesc *d,
                                               const uint32_t *indices,
                                               uint32_t triangle_count)
{
    if (!d || !indices || !d->positions) return false;

    /* Worst-case edge count = 3 * triangle_count.  Allocate, then
     * dedupe by sorting + unique.  For very large meshes a hash-set
     * is preferable; bounded triangle counts make a sort fine. */
    uint32_t cap = triangle_count * 3;
    uint64_t *keys = (uint64_t *)malloc(cap * sizeof(uint64_t));
    if (!keys) return false;
    uint32_t  *a_arr = (uint32_t *)malloc(cap * sizeof(uint32_t));
    uint32_t  *b_arr = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!a_arr || !b_arr) {
        free(keys); free(a_arr); free(b_arr);
        return false;
    }
    for (uint32_t i = 0; i < triangle_count; ++i) {
        uint32_t i0 = indices[i * 3 + 0];
        uint32_t i1 = indices[i * 3 + 1];
        uint32_t i2 = indices[i * 3 + 2];
        keys[i * 3 + 0] = edge_key(i0, i1); a_arr[i * 3 + 0] = i0; b_arr[i * 3 + 0] = i1;
        keys[i * 3 + 1] = edge_key(i1, i2); a_arr[i * 3 + 1] = i1; b_arr[i * 3 + 1] = i2;
        keys[i * 3 + 2] = edge_key(i2, i0); a_arr[i * 3 + 2] = i2; b_arr[i * 3 + 2] = i0;
    }
    /* Insertion sort — adequate for thousands of edges; one-shot. */
    for (uint32_t i = 1; i < cap; ++i) {
        uint64_t k = keys[i];
        uint32_t aa = a_arr[i], bb = b_arr[i];
        uint32_t j = i;
        while (j > 0 && keys[j - 1] > k) {
            keys[j]  = keys[j - 1];
            a_arr[j] = a_arr[j - 1];
            b_arr[j] = b_arr[j - 1];
            j--;
        }
        keys[j] = k; a_arr[j] = aa; b_arr[j] = bb;
    }
    /* Dedupe + emit. */
    JceClothConstraint *out = (JceClothConstraint *)malloc(
        cap * sizeof(JceClothConstraint));
    if (!out) {
        free(keys); free(a_arr); free(b_arr);
        return false;
    }
    uint32_t emitted = 0;
    uint64_t prev = (uint64_t)-1;
    for (uint32_t i = 0; i < cap; ++i) {
        if (keys[i] == prev) continue;
        prev = keys[i];
        uint32_t aa = a_arr[i], bb = b_arr[i];
        if (aa >= d->particle_count || bb >= d->particle_count) continue;
        const float *pa = &d->positions[aa * 3];
        const float *pb = &d->positions[bb * 3];
        float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
        out[emitted].a = aa;
        out[emitted].b = bb;
        out[emitted].rest_length = sqrtf(dx*dx + dy*dy + dz*dz);
        emitted++;
    }
    free(keys); free(a_arr); free(b_arr);
    free(d->distance_constraints);
    d->distance_constraints = out;
    d->distance_count       = emitted;
    return true;
}
