/*
 * jce_light_cluster.c  Clustered light culling — CPU implementation.
 */

#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_light_cluster.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "light_cluster"

struct JceLightCluster {
    JceLightClusterDesc desc;

    /* Camera state. */
    jce_mat4 inv_view;
    jce_mat4 proj;
    float    near_plane;
    float    far_plane;
    float    log_far_over_near;   /* precomputed log(far/near) */
    float    inv_log_far_over_near;
    bool     camera_valid;

    /* Per-cell storage (heap; reallocated on desc change). */
    uint32_t *cell_counts;
    uint32_t *cell_indices;

    /* Last result snapshot. */
    JceLightClusterResult result;
};

static uint32_t cell_total(const JceLightClusterDesc *d)
{
    return d->cells_x * d->cells_y * d->slices_z;
}

JceLightCluster *jce_light_cluster_create(const JceLightClusterDesc *desc)
{
    if (!desc || !desc->cells_x || !desc->cells_y || !desc->slices_z
        || !desc->max_per_cell) {
        return NULL;
    }

    JceLightCluster *lc = (JceLightCluster *)JCE_CALLOC(1, sizeof(*lc));
    if (!lc) return NULL;

    lc->desc = *desc;

    uint32_t cells = cell_total(desc);
    lc->cell_counts  = (uint32_t *)JCE_CALLOC(cells, sizeof(uint32_t));
    lc->cell_indices = (uint32_t *)JCE_CALLOC((size_t)cells * desc->max_per_cell,
                                              sizeof(uint32_t));
    if (!lc->cell_counts || !lc->cell_indices) {
        JCE_FREE(lc->cell_counts);
        JCE_FREE(lc->cell_indices);
        JCE_FREE(lc);
        return NULL;
    }

    return lc;
}

void jce_light_cluster_destroy(JceLightCluster *lc)
{
    if (!lc) return;
    JCE_FREE(lc->cell_counts);
    JCE_FREE(lc->cell_indices);
    JCE_FREE(lc);
}

void jce_light_cluster_set_camera(JceLightCluster *lc,
                                   const jce_mat4 *inv_view,
                                   const jce_mat4 *proj,
                                   float near_plane,
                                   float far_plane)
{
    if (!lc || !inv_view || !proj) return;
    if (!(near_plane > 0.0f) || !(far_plane > near_plane)) return;

    lc->inv_view             = *inv_view;
    lc->proj                 = *proj;
    lc->near_plane           = near_plane;
    lc->far_plane            = far_plane;
    lc->log_far_over_near    = logf(far_plane / near_plane);
    lc->inv_log_far_over_near =
        lc->log_far_over_near > 0.0f ? 1.0f / lc->log_far_over_near : 0.0f;
    lc->camera_valid         = true;
}

uint32_t jce_light_cluster_slice_for_view_z(const JceLightCluster *lc, float view_z)
{
    if (!lc || !lc->camera_valid) return 0;
    if (view_z <= lc->near_plane) return 0;
    if (view_z >= lc->far_plane) return lc->desc.slices_z - 1;

    float t = logf(view_z / lc->near_plane) * lc->inv_log_far_over_near;
    int s = (int)(t * (float)lc->desc.slices_z);
    if (s < 0) s = 0;
    if (s >= (int)lc->desc.slices_z) s = (int)lc->desc.slices_z - 1;
    return (uint32_t)s;
}

/* Inverse of slice_for_view_z: returns the view-space Z at the given
 * slice boundary [0, slices_z]. */
static float view_z_for_slice(const JceLightCluster *lc, uint32_t slice)
{
    float t = (float)slice / (float)lc->desc.slices_z;
    return lc->near_plane * expf(lc->log_far_over_near * t);
}

/* Project a world-space sphere center into view space. */
static void world_to_view(const jce_mat4 *inv_view,
                          jce_vec3 ws, jce_vec3 *out_vs)
{
    jce_mat4 view = jce_m4_inverse(inv_view);

    const float *m = JCE_M4_PTR(view);
    out_vs->x = m[0]*ws.x + m[4]*ws.y + m[8]*ws.z  + m[12];
    out_vs->y = m[1]*ws.x + m[5]*ws.y + m[9]*ws.z  + m[13];
    out_vs->z = m[2]*ws.x + m[6]*ws.y + m[10]*ws.z + m[14];
}

bool jce_light_cluster_build(JceLightCluster *lc,
                              const JceLightProxy *lights,
                              uint32_t count)
{
    if (!lc) return false;
    if (count > 0 && !lights) return false;
    if (!lc->camera_valid) return false;

    uint32_t cells = cell_total(&lc->desc);
    memset(lc->cell_counts, 0, cells * sizeof(uint32_t));

    uint32_t total_assignments = 0;
    uint32_t overflow_cells    = 0;

    /* Project FOV from the projection matrix.  For a perspective
     * proj[1][1] = 1 / tan(fovy/2). */
    const float *p = JCE_M4_PTR(lc->proj);
    float tan_half_y = (p[5] != 0.0f) ? 1.0f / p[5] : 1.0f;
    float tan_half_x = (p[0] != 0.0f) ? 1.0f / p[0] : tan_half_y;

    /* Capture which cells we already added a given light to so we don't
     * double-insert when a sphere straddles cell boundaries. */
    for (uint32_t li = 0; li < count && li < lc->desc.max_lights; ++li) {
        jce_vec3 vs;
        world_to_view(&lc->inv_view, lights[li].position_ws, &vs);
        float r = lights[li].radius;

        /* Z range. */
        float z_min = vs.z - r;
        float z_max = vs.z + r;
        if (z_max <= lc->near_plane || z_min >= lc->far_plane) continue;
        if (z_min < lc->near_plane) z_min = lc->near_plane;
        if (z_max > lc->far_plane)  z_max = lc->far_plane;

        uint32_t s_lo = jce_light_cluster_slice_for_view_z(lc, z_min);
        uint32_t s_hi = jce_light_cluster_slice_for_view_z(lc, z_max);

        /* XY tile range — conservative bbox in NDC. */
        float zref = vs.z > lc->near_plane ? vs.z : lc->near_plane;
        float half_w = tan_half_x * zref;
        float half_h = tan_half_y * zref;

        float x_min_ndc = (vs.x - r) / half_w;
        float x_max_ndc = (vs.x + r) / half_w;
        float y_min_ndc = (vs.y - r) / half_h;
        float y_max_ndc = (vs.y + r) / half_h;

        if (x_max_ndc < -1.0f || x_min_ndc > 1.0f) continue;
        if (y_max_ndc < -1.0f || y_min_ndc > 1.0f) continue;

        if (x_min_ndc < -1.0f) x_min_ndc = -1.0f;
        if (x_max_ndc >  1.0f) x_max_ndc =  1.0f;
        if (y_min_ndc < -1.0f) y_min_ndc = -1.0f;
        if (y_max_ndc >  1.0f) y_max_ndc =  1.0f;

        int tx_lo = (int)floorf((x_min_ndc * 0.5f + 0.5f) * (float)lc->desc.cells_x);
        int tx_hi = (int)floorf((x_max_ndc * 0.5f + 0.5f) * (float)lc->desc.cells_x);
        int ty_lo = (int)floorf((y_min_ndc * 0.5f + 0.5f) * (float)lc->desc.cells_y);
        int ty_hi = (int)floorf((y_max_ndc * 0.5f + 0.5f) * (float)lc->desc.cells_y);

        if (tx_lo < 0) tx_lo = 0;
        if (ty_lo < 0) ty_lo = 0;
        if (tx_hi >= (int)lc->desc.cells_x) tx_hi = (int)lc->desc.cells_x - 1;
        if (ty_hi >= (int)lc->desc.cells_y) ty_hi = (int)lc->desc.cells_y - 1;

        for (uint32_t s = s_lo; s <= s_hi; ++s) {
            for (int ty = ty_lo; ty <= ty_hi; ++ty) {
                for (int tx = tx_lo; tx <= tx_hi; ++tx) {
                    uint32_t cell = (s * lc->desc.cells_y + (uint32_t)ty)
                                    * lc->desc.cells_x + (uint32_t)tx;
                    uint32_t *cnt = &lc->cell_counts[cell];
                    if (*cnt >= lc->desc.max_per_cell) {
                        overflow_cells++;
                        continue;
                    }
                    lc->cell_indices[cell * lc->desc.max_per_cell + *cnt] = li;
                    (*cnt)++;
                    total_assignments++;
                }
            }
        }
    }

    lc->result.cell_counts        = lc->cell_counts;
    lc->result.cell_indices       = lc->cell_indices;
    lc->result.cells_x            = lc->desc.cells_x;
    lc->result.cells_y            = lc->desc.cells_y;
    lc->result.slices_z           = lc->desc.slices_z;
    lc->result.max_per_cell       = lc->desc.max_per_cell;
    lc->result.total_lights_in    = count;
    lc->result.total_assignments  = total_assignments;
    lc->result.overflow_cells     = overflow_cells;

    /* Silence unused-helper warning if optimizer removes it. */
    (void)view_z_for_slice;
    return true;
}

JceLightClusterResult jce_light_cluster_get_result(const JceLightCluster *lc)
{
    if (!lc) {
        JceLightClusterResult empty = {0};
        return empty;
    }
    return lc->result;
}
