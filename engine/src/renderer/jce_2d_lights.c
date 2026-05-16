/*
 * jce_2d_lights.c  2D light gather buffer.
 *
 * Single-threaded; caller fills the buffer once per frame from
 * scene component data, then gathers/queries for each batch.
 */

#include <jce/renderer/jce_2d_lights.h>

#include <string.h>

#define POOL_MAX 256

static JceLight2DInstance s_pool[POOL_MAX];
static uint32_t            s_count;

void jce_2d_lights_clear(void) { s_count = 0; }

bool jce_2d_lights_push(const JceLight2DInstance *inst)
{
    if (!inst || s_count >= POOL_MAX) return false;
    s_pool[s_count++] = *inst;
    return true;
}

uint32_t jce_2d_lights_count(void) { return s_count; }

const JceLight2DInstance *jce_2d_lights_at(uint32_t idx)
{
    return idx < s_count ? &s_pool[idx] : NULL;
}

static bool aabb_disc_overlap(JceLight2DAabb view,
                                float cx, float cy, float r)
{
    float dx = cx;
    if (cx < view.min_x) dx = view.min_x;
    else if (cx > view.max_x) dx = view.max_x;
    float dy = cy;
    if (cy < view.min_y) dy = view.min_y;
    else if (cy > view.max_y) dy = view.max_y;
    float ddx = cx - dx, ddy = cy - dy;
    return (ddx*ddx + ddy*ddy) <= r * r;
}

uint32_t jce_2d_lights_gather(JceLight2DAabb view, uint32_t mask,
                                JceLight2DInstance *out, uint32_t cap)
{
    if (!out || cap == 0) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_count && n < cap; ++i) {
        const JceLight2DInstance *L = &s_pool[i];
        if (mask && !(L->target_layer_mask & mask)) continue;
        if (L->kind == JCE_2D_LIGHT_GLOBAL ||
            aabb_disc_overlap(view, L->position[0], L->position[1],
                               L->outer_radius))
            out[n++] = *L;
    }
    /* Insertion sort by outer_radius desc — small N. */
    for (uint32_t i = 1; i < n; ++i) {
        JceLight2DInstance tmp = out[i];
        uint32_t j = i;
        while (j > 0 && out[j-1].outer_radius < tmp.outer_radius) {
            out[j] = out[j-1];
            j--;
        }
        out[j] = tmp;
    }
    return n;
}
