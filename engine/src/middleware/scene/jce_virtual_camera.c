/*
 * jce_virtual_camera.c  Cinemachine-style vcam manager (Sprint 4 #17).
 */

#include <jce/middleware/scene/jce_virtual_camera.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define JCE_VCAM_MAX 64

typedef struct {
    JceVirtualCamera cam;
    int              used;
} JceVcamSlot;

struct JceVcamManager {
    JceVcamSlot slots[JCE_VCAM_MAX];
    int         active_handle;     /* current winner */
    int         prev_handle;       /* fading-out winner */
    float       blend_duration;
    float       blend_t;           /* 0..blend_duration */
    JceVcamOutput cur;             /* for damping continuity */
    int         have_cur;
};

JCE_API JceVcamManager *JCE_CALL
jce_vcam_manager_create(void)
{
    JceVcamManager *m = (JceVcamManager *)JCE_CALLOC(1, sizeof *m);
    if (!m) return NULL;
    m->active_handle  = -1;
    m->prev_handle    = -1;
    m->blend_duration = 0.5f;
    return m;
}

JCE_API void JCE_CALL
jce_vcam_manager_destroy(JceVcamManager *m) { JCE_FREE(m); }

JCE_API int JCE_CALL
jce_vcam_add(JceVcamManager *m, const JceVirtualCamera *cam)
{
    if (!m || !cam) return -1;
    for (int i = 0; i < JCE_VCAM_MAX; ++i) {
        if (!m->slots[i].used) {
            m->slots[i].used = 1;
            m->slots[i].cam  = *cam;
            return i;
        }
    }
    return -1;
}

JCE_API void JCE_CALL
jce_vcam_update(JceVcamManager *m, int h, const JceVirtualCamera *cam)
{
    if (!m || !cam || h < 0 || h >= JCE_VCAM_MAX || !m->slots[h].used) return;
    m->slots[h].cam = *cam;
}

JCE_API void JCE_CALL
jce_vcam_remove(JceVcamManager *m, int h)
{
    if (!m || h < 0 || h >= JCE_VCAM_MAX) return;
    m->slots[h].used = 0;
    if (m->active_handle == h) m->active_handle = -1;
    if (m->prev_handle   == h) m->prev_handle   = -1;
}

JCE_API int JCE_CALL
jce_vcam_count(const JceVcamManager *m)
{
    if (!m) return 0;
    int n = 0;
    for (int i = 0; i < JCE_VCAM_MAX; ++i) if (m->slots[i].used) ++n;
    return n;
}

JCE_API void JCE_CALL
jce_vcam_set_blend_duration(JceVcamManager *m, float s)
{ if (m) m->blend_duration = s < 0 ? 0 : s; }

JCE_API const JceVirtualCamera *JCE_CALL
jce_vcam_get(const JceVcamManager *m, int h)
{
    if (!m || h < 0 || h >= JCE_VCAM_MAX || !m->slots[h].used) return NULL;
    return &m->slots[h].cam;
}

JCE_API int JCE_CALL
jce_vcam_active_handle(const JceVcamManager *m)
{ return m ? m->active_handle : -1; }

/* ────────── helpers ────────── */

static int pick_winner(const JceVcamManager *m)
{
    int best = -1; int32_t bp = 0;
    for (int i = 0; i < JCE_VCAM_MAX; ++i) {
        if (!m->slots[i].used) continue;
        if (!m->slots[i].cam.active) continue;
        if (best < 0 || m->slots[i].cam.priority > bp) {
            best = i; bp = m->slots[i].cam.priority;
        }
    }
    return best;
}

static void desired_from(const JceVirtualCamera *c, JceVcamOutput *o)
{
    if (c->mode == JCE_VCAM_TRACK_FOLLOW || c->mode == JCE_VCAM_TRACK_FOLLOW_LOOK) {
        o->position[0] = c->follow_pos[0] + c->offset[0];
        o->position[1] = c->follow_pos[1] + c->offset[1];
        o->position[2] = c->follow_pos[2] + c->offset[2];
    } else {
        o->position[0] = c->position[0];
        o->position[1] = c->position[1];
        o->position[2] = c->position[2];
    }
    if (c->mode == JCE_VCAM_TRACK_LOOK_AT || c->mode == JCE_VCAM_TRACK_FOLLOW_LOOK) {
        o->target[0] = c->look_at_pos[0];
        o->target[1] = c->look_at_pos[1];
        o->target[2] = c->look_at_pos[2];
    } else {
        o->target[0] = c->target[0];
        o->target[1] = c->target[1];
        o->target[2] = c->target[2];
    }
    o->fov_deg = c->fov_deg;
}

static void blend(const JceVcamOutput *a, const JceVcamOutput *b, float t, JceVcamOutput *o)
{
    if (t < 0) t = 0; else if (t > 1) t = 1;
    for (int i = 0; i < 3; ++i) {
        o->position[i] = a->position[i] + (b->position[i] - a->position[i]) * t;
        o->target  [i] = a->target  [i] + (b->target  [i] - a->target  [i]) * t;
    }
    o->fov_deg = a->fov_deg + (b->fov_deg - a->fov_deg) * t;
}

JCE_API void JCE_CALL
jce_vcam_evaluate(JceVcamManager *m, float dt, JceVcamOutput *out)
{
    if (!m || !out) return;

    int winner = pick_winner(m);
    if (winner != m->active_handle) {
        m->prev_handle   = m->active_handle;
        m->active_handle = winner;
        m->blend_t       = 0;
    }

    JceVcamOutput target_out;
    memset(&target_out, 0, sizeof target_out);
    target_out.fov_deg = 60.0f;

    if (winner >= 0) {
        JceVcamOutput a;
        desired_from(&m->slots[winner].cam, &a);

        if (m->prev_handle >= 0 && m->blend_duration > 1e-4f &&
            m->blend_t < m->blend_duration && m->slots[m->prev_handle].used) {
            JceVcamOutput p;
            desired_from(&m->slots[m->prev_handle].cam, &p);
            float t = m->blend_t / m->blend_duration;
            blend(&p, &a, t, &target_out);
            m->blend_t += dt;
            if (m->blend_t >= m->blend_duration) m->prev_handle = -1;
        } else {
            target_out = a;
            m->prev_handle = -1;
        }

        /* Per-vcam damping (exponential smoothing). */
        float d = m->slots[winner].cam.damping;
        if (d > 1e-4f && m->have_cur) {
            float k = 1.0f - expf(-dt / d);
            blend(&m->cur, &target_out, k, &target_out);
        }
    }

    m->cur      = target_out;
    m->have_cur = 1;
    *out        = target_out;
}
