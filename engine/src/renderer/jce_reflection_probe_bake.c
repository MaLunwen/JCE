/*
 * jce_reflection_probe_bake.c  Probe capture queue.
 *
 * View matrices are right-handed look-at to the six cube directions.
 * Face order matches OpenGL / bgfx cubemap convention:
 *   0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z
 */

#include <jce/renderer/jce_reflection_probe_bake.h>

#include <math.h>
#include <string.h>

static JceReflectionProbeBake s_queue[JCE_REFLECTION_PROBE_QUEUE_MAX];

/* Right-handed look-at, +Y up unless the look direction is parallel.
 * Output is a column-major 4x4 in `m` (16 floats). */
static void look_at(const float eye[3], const float target[3],
                     const float up[3], float m[16])
{
    float f[3] = { target[0]-eye[0], target[1]-eye[1], target[2]-eye[2] };
    float fl = sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]);
    if (fl > 1e-7f) { f[0]/=fl; f[1]/=fl; f[2]/=fl; }
    float s[3] = {
        f[1]*up[2] - f[2]*up[1],
        f[2]*up[0] - f[0]*up[2],
        f[0]*up[1] - f[1]*up[0],
    };
    float sl = sqrtf(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]);
    if (sl > 1e-7f) { s[0]/=sl; s[1]/=sl; s[2]/=sl; }
    float u[3] = {
        s[1]*f[2] - s[2]*f[1],
        s[2]*f[0] - s[0]*f[2],
        s[0]*f[1] - s[1]*f[0],
    };
    m[ 0] = s[0]; m[ 1] = u[0]; m[ 2] = -f[0]; m[ 3] = 0;
    m[ 4] = s[1]; m[ 5] = u[1]; m[ 6] = -f[1]; m[ 7] = 0;
    m[ 8] = s[2]; m[ 9] = u[2]; m[10] = -f[2]; m[11] = 0;
    m[12] = -(s[0]*eye[0] + s[1]*eye[1] + s[2]*eye[2]);
    m[13] = -(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]);
    m[14] =  (f[0]*eye[0] + f[1]*eye[1] + f[2]*eye[2]);
    m[15] = 1;
}

static void perspective_90(float near_z, float far_z, float m[16])
{
    /* fov = 90deg → f = 1/tan(45) = 1, aspect = 1. */
    float nf = 1.0f / (near_z - far_z);
    memset(m, 0, 16 * sizeof(float));
    m[ 0] = 1.0f;
    m[ 5] = 1.0f;
    m[10] = (far_z + near_z) * nf;
    m[11] = -1.0f;
    m[14] = 2.0f * far_z * near_z * nf;
}

void jce_reflection_probe_bake_build_view_matrices(JceReflectionProbeBake *b)
{
    if (!b) return;
    const float *p = b->position;
    /* Targets in world space relative to position. */
    float tgt[6][3] = {
        { p[0]+1, p[1],   p[2]   }, { p[0]-1, p[1],   p[2]   },
        { p[0],   p[1]+1, p[2]   }, { p[0],   p[1]-1, p[2]   },
        { p[0],   p[1],   p[2]+1 }, { p[0],   p[1],   p[2]-1 },
    };
    /* +Y up for X/Z faces; +Z/-Z up for +Y/-Y faces to avoid degenerate. */
    float up[6][3] = {
        { 0, 1, 0 }, { 0, 1, 0 },
        { 0, 0, 1 }, { 0, 0,-1 },
        { 0, 1, 0 }, { 0, 1, 0 },
    };
    for (int i = 0; i < 6; ++i)
        look_at(p, tgt[i], up[i], b->view[i]);
    perspective_90(b->near_plane, b->far_plane, b->proj);
}

uint32_t jce_reflection_probe_bake_enqueue(uint32_t probe_id,
                                            const float position[3],
                                            float near_z, float far_z,
                                            uint16_t face_res)
{
    if (!position) return UINT32_MAX;
    for (uint32_t i = 0; i < JCE_REFLECTION_PROBE_QUEUE_MAX; ++i) {
        if (!s_queue[i].active) {
            JceReflectionProbeBake *b = &s_queue[i];
            memset(b, 0, sizeof(*b));
            b->probe_id = probe_id;
            b->position[0] = position[0];
            b->position[1] = position[1];
            b->position[2] = position[2];
            b->near_plane = near_z > 0 ? near_z : 0.1f;
            b->far_plane  = far_z  > 0 ? far_z  : 100.0f;
            b->face_resolution   = face_res ? face_res : 128;
            b->pending_faces_mask = 0x3F; /* all 6 */
            b->active = true;
            jce_reflection_probe_bake_build_view_matrices(b);
            return i;
        }
    }
    return UINT32_MAX;
}

bool jce_reflection_probe_bake_cancel(uint32_t probe_id)
{
    for (uint32_t i = 0; i < JCE_REFLECTION_PROBE_QUEUE_MAX; ++i) {
        if (s_queue[i].active && s_queue[i].probe_id == probe_id) {
            s_queue[i].active = false;
            return true;
        }
    }
    return false;
}

uint32_t jce_reflection_probe_bake_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < JCE_REFLECTION_PROBE_QUEUE_MAX; ++i)
        if (s_queue[i].active) n++;
    return n;
}

JceReflectionProbeBake *jce_reflection_probe_bake_at(uint32_t idx)
{
    uint32_t seen = 0;
    for (uint32_t i = 0; i < JCE_REFLECTION_PROBE_QUEUE_MAX; ++i) {
        if (!s_queue[i].active) continue;
        if (seen == idx) return &s_queue[i];
        seen++;
    }
    return NULL;
}

bool jce_reflection_probe_bake_mark_face_done(uint32_t probe_id, uint8_t face)
{
    if (face >= 6) return false;
    for (uint32_t i = 0; i < JCE_REFLECTION_PROBE_QUEUE_MAX; ++i) {
        if (s_queue[i].active && s_queue[i].probe_id == probe_id) {
            s_queue[i].pending_faces_mask &= (uint8_t)~(1u << face);
            return s_queue[i].pending_faces_mask != 0;
        }
    }
    return false;
}

void jce_reflection_probe_bake_drain_done(void)
{
    for (uint32_t i = 0; i < JCE_REFLECTION_PROBE_QUEUE_MAX; ++i) {
        if (s_queue[i].active && s_queue[i].pending_faces_mask == 0)
            s_queue[i].active = false;
    }
}
