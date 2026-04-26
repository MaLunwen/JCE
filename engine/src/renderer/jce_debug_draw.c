/*
 * jce_debug_draw.c  3D debug line drawing (transient buffer, PT_LINES).
 *
 * Accumulates line vertices, then flushes them in a single draw call
 * using BGFX_STATE_PT_LINES.
 */

#include <jce/renderer/jce_debug_draw.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>

#include <bgfx/c99/bgfx.h>
#include <string.h>
#include <math.h>

#define LOG_TAG "debug_draw"

/* ================================================================== */
/* Line vertex buffer                                                  */
/* ================================================================== */

typedef struct {
    float    x, y, z;
    uint32_t abgr;
} DebugVertex;

#define MAX_DEBUG_LINES 8192

static DebugVertex s_verts[MAX_DEBUG_LINES * 2];
static uint32_t    s_vert_count = 0;
static bool        s_layout_ready = false;
static bgfx_vertex_layout_t s_layout;

/* ================================================================== */
/* Internal: push a line                                               */
/* ================================================================== */

static void push_line(jce_vec3 from, jce_vec3 to, uint32_t abgr)
{
    if (s_vert_count + 2 > MAX_DEBUG_LINES * 2) return;

    DebugVertex *a = &s_verts[s_vert_count++];
    a->x = from.x; a->y = from.y; a->z = from.z; a->abgr = abgr;

    DebugVertex *b = &s_verts[s_vert_count++];
    b->x = to.x; b->y = to.y; b->z = to.z; b->abgr = abgr;
}

/* ================================================================== */
/* Public: add line                                                    */
/* ================================================================== */

void jce_debug_draw_line(jce_vec3 from, jce_vec3 to, uint32_t abgr)
{
    push_line(from, to, abgr);
}

/* ================================================================== */
/* Public: box                                                         */
/* ================================================================== */

void jce_debug_draw_box(jce_vec3 center, jce_vec3 half,
                         jce_quat rot, uint32_t abgr)
{
    /* 8 corners of the box in local space. */
    jce_vec3 local[8] = {
        { -half.x, -half.y, -half.z },
        {  half.x, -half.y, -half.z },
        {  half.x,  half.y, -half.z },
        { -half.x,  half.y, -half.z },
        { -half.x, -half.y,  half.z },
        {  half.x, -half.y,  half.z },
        {  half.x,  half.y,  half.z },
        { -half.x,  half.y,  half.z },
    };

    /* Transform corners by rotation + translation. */
    jce_vec3 corners[8];
    for (int i = 0; i < 8; i++) {
        corners[i] = jce_v3_add(jce_q_rotate(rot, local[i]), center);
    }

    /* 12 edges of a box. */
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7},
    };
    for (int i = 0; i < 12; i++) {
        push_line(corners[edges[i][0]], corners[edges[i][1]], abgr);
    }
}

/* ================================================================== */
/* Public: sphere                                                      */
/* ================================================================== */

#define CIRCLE_SEGMENTS 24

void jce_debug_draw_sphere(jce_vec3 center, float radius, uint32_t abgr)
{
    float step = (2.0f * JCE_PI) / (float)CIRCLE_SEGMENTS;

    /* XZ circle (horizontal). */
    for (int i = 0; i < CIRCLE_SEGMENTS; i++) {
        float a0 = step * (float)i;
        float a1 = step * (float)((i + 1) % CIRCLE_SEGMENTS);
        jce_vec3 p0 = jce_v3(center.x + cosf(a0)*radius, center.y,
                              center.z + sinf(a0)*radius);
        jce_vec3 p1 = jce_v3(center.x + cosf(a1)*radius, center.y,
                              center.z + sinf(a1)*radius);
        push_line(p0, p1, abgr);
    }

    /* XY circle (front). */
    for (int i = 0; i < CIRCLE_SEGMENTS; i++) {
        float a0 = step * (float)i;
        float a1 = step * (float)((i + 1) % CIRCLE_SEGMENTS);
        jce_vec3 p0 = jce_v3(center.x + cosf(a0)*radius,
                              center.y + sinf(a0)*radius, center.z);
        jce_vec3 p1 = jce_v3(center.x + cosf(a1)*radius,
                              center.y + sinf(a1)*radius, center.z);
        push_line(p0, p1, abgr);
    }

    /* YZ circle (side). */
    for (int i = 0; i < CIRCLE_SEGMENTS; i++) {
        float a0 = step * (float)i;
        float a1 = step * (float)((i + 1) % CIRCLE_SEGMENTS);
        jce_vec3 p0 = jce_v3(center.x,
                              center.y + cosf(a0)*radius,
                              center.z + sinf(a0)*radius);
        jce_vec3 p1 = jce_v3(center.x,
                              center.y + cosf(a1)*radius,
                              center.z + sinf(a1)*radius);
        push_line(p0, p1, abgr);
    }
}

/* ================================================================== */
/* Public: capsule                                                     */
/* ================================================================== */

void jce_debug_draw_capsule(jce_vec3 center, float radius,
                             float half_height, jce_quat rot,
                             uint32_t abgr)
{
    /* Draw a capsule as: two hemispheres + connecting lines. */
    float step = (2.0f * JCE_PI) / (float)CIRCLE_SEGMENTS;

    jce_vec3 top_center = jce_v3_add(center,
        jce_q_rotate(rot, jce_v3(0, half_height, 0)));
    jce_vec3 bot_center = jce_v3_add(center,
        jce_q_rotate(rot, jce_v3(0, -half_height, 0)));

    /* Top and bottom circles. */
    for (int i = 0; i < CIRCLE_SEGMENTS; i++) {
        float a0 = step * (float)i;
        float a1 = step * (float)((i + 1) % CIRCLE_SEGMENTS);

        jce_vec3 d0 = jce_q_rotate(rot,
            jce_v3(cosf(a0)*radius, 0, sinf(a0)*radius));
        jce_vec3 d1 = jce_q_rotate(rot,
            jce_v3(cosf(a1)*radius, 0, sinf(a1)*radius));

        push_line(jce_v3_add(top_center, d0),
                  jce_v3_add(top_center, d1), abgr);
        push_line(jce_v3_add(bot_center, d0),
                  jce_v3_add(bot_center, d1), abgr);
    }

    /* 4 vertical lines connecting top and bottom. */
    for (int i = 0; i < 4; i++) {
        float a = step * (float)(i * (CIRCLE_SEGMENTS / 4));
        jce_vec3 d = jce_q_rotate(rot,
            jce_v3(cosf(a)*radius, 0, sinf(a)*radius));
        push_line(jce_v3_add(top_center, d),
                  jce_v3_add(bot_center, d), abgr);
    }

    /* Top hemisphere arcs (XY and YZ planes). */
    for (int i = 0; i < CIRCLE_SEGMENTS / 2; i++) {
        float a0 = step * (float)i;
        float a1 = step * (float)(i + 1);
        jce_vec3 d0 = jce_q_rotate(rot,
            jce_v3(cosf(a0)*radius, sinf(a0)*radius, 0));
        jce_vec3 d1 = jce_q_rotate(rot,
            jce_v3(cosf(a1)*radius, sinf(a1)*radius, 0));
        push_line(jce_v3_add(top_center, d0),
                  jce_v3_add(top_center, d1), abgr);

        d0 = jce_q_rotate(rot,
            jce_v3(0, sinf(a0)*radius, cosf(a0)*radius));
        d1 = jce_q_rotate(rot,
            jce_v3(0, sinf(a1)*radius, cosf(a1)*radius));
        push_line(jce_v3_add(top_center, d0),
                  jce_v3_add(top_center, d1), abgr);
    }

    /* Bottom hemisphere arcs (inverted Y). */
    for (int i = 0; i < CIRCLE_SEGMENTS / 2; i++) {
        float a0 = step * (float)i;
        float a1 = step * (float)(i + 1);
        jce_vec3 d0 = jce_q_rotate(rot,
            jce_v3(cosf(a0)*radius, -sinf(a0)*radius, 0));
        jce_vec3 d1 = jce_q_rotate(rot,
            jce_v3(cosf(a1)*radius, -sinf(a1)*radius, 0));
        push_line(jce_v3_add(bot_center, d0),
                  jce_v3_add(bot_center, d1), abgr);

        d0 = jce_q_rotate(rot,
            jce_v3(0, -sinf(a0)*radius, cosf(a0)*radius));
        d1 = jce_q_rotate(rot,
            jce_v3(0, -sinf(a1)*radius, cosf(a1)*radius));
        push_line(jce_v3_add(bot_center, d0),
                  jce_v3_add(bot_center, d1), abgr);
    }
}

/* ================================================================== */
/* Clear                                                               */
/* ================================================================== */

void jce_debug_draw_clear(void)
{
    s_vert_count = 0;
}

/* ================================================================== */
/* Flush                                                               */
/* ================================================================== */

void jce_debug_draw_flush(uint16_t view_id, const JceRenderer *renderer)
{
    if (s_vert_count == 0) return;

    /* Ensure layout is initialized. */
    if (!s_layout_ready) {
        bgfx_vertex_layout_begin(&s_layout, bgfx_get_renderer_type());
        bgfx_vertex_layout_add(&s_layout, BGFX_ATTRIB_POSITION, 3,
                               BGFX_ATTRIB_TYPE_FLOAT, false, false);
        bgfx_vertex_layout_add(&s_layout, BGFX_ATTRIB_COLOR0, 4,
                               BGFX_ATTRIB_TYPE_UINT8, true, false);
        bgfx_vertex_layout_end(&s_layout);
        s_layout_ready = true;
    }

    /* Allocate transient vertex buffer. */
    bgfx_transient_vertex_buffer_t tvb;
    if (bgfx_get_avail_transient_vertex_buffer(s_vert_count, &s_layout) < s_vert_count)
        goto done;
    bgfx_alloc_transient_vertex_buffer(&tvb, s_vert_count, &s_layout);

    memcpy(tvb.data, s_verts, s_vert_count * sizeof(DebugVertex));

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, s_vert_count);

    /* Identity transform. */
    {
        jce_mat4 identity;
        memset(&identity, 0, sizeof(identity));
        identity.raw[0][0] = 1.0f;
        identity.raw[1][1] = 1.0f;
        identity.raw[2][2] = 1.0f;
        identity.raw[3][3] = 1.0f;
        bgfx_set_transform(identity.raw[0], 1);
    }

    /* Render state: lines, depth test, no blending. */
    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_WRITE_A
                   | BGFX_STATE_WRITE_Z
                   | BGFX_STATE_DEPTH_TEST_LESS
                   | BGFX_STATE_PT_LINES
                   | BGFX_STATE_MSAA;
    bgfx_set_state(state, 0);

    /* Use the color-only shader program. */
    JceShaderHandle sh = jce_renderer_get_program_color(renderer);
    bgfx_program_handle_t prog;
    prog.idx = sh.idx;
    if (BGFX_HANDLE_IS_VALID(prog))
        bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);

done:
    s_vert_count = 0;
}
