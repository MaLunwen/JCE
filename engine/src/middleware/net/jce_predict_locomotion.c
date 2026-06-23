/*
 * jce_predict_locomotion.c — pure deterministic kinematic locomotion step.
 *
 * See jce_predict_locomotion.h for the rationale.  This TU is intentionally
 * dependency-free beyond <math.h> and its own header: NO ECS, NO physics, NO
 * net transport, NO globals, NO rand.  Every output is a closed-form function
 * of (prev_state, input, params), which is exactly what the prediction core's
 * rollback/replay requires to reproduce a timeline across peers.
 */

#include <jce/middleware/net/jce_predict_locomotion.h>

#include <math.h>

/* ================================================================== */
/* Defaults                                                            */
/* ================================================================== */

void jce_predict_loco_params_default(JcePredictLocoParams *p)
{
    if (!p) return;
    p->dt          = 1.0f / 60.0f;
    p->move_speed  = 5.0f;
    p->sprint_mult = 1.6f;
    p->gravity     = 20.0f;
    p->jump_speed  = 7.0f;
    p->ground_y    = 0.0f;
}

/* ================================================================== */
/* Step                                                                */
/* ================================================================== */

int jce_predict_locomotion_step(const void *prev_state,
                                const void *input,
                                void       *out_state,
                                void       *user)
{
    const JcePredictState *prev = (const JcePredictState *)prev_state;
    const JcePredictInput *in   = (const JcePredictInput *)input;
    JcePredictState       *out  = (JcePredictState *)out_state;

    if (!prev || !out) return 1;   /* treated as a step failure by the core */

    JcePredictLocoParams params;
    if (user) {
        params = *(const JcePredictLocoParams *)user;
    } else {
        jce_predict_loco_params_default(&params);
    }

    float dt = params.dt;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;

    /* Start from the previous state, then overwrite the integrated fields so
     * out_state is fully defined even when `input` is NULL (idle tick). */
    *out = *prev;

    float walk_x = in ? in->walk_x : 0.0f;
    float walk_z = in ? in->walk_z : 0.0f;
    float speed_mult = in ? in->speed_mult : 1.0f;
    if (speed_mult <= 0.0f) speed_mult = 1.0f;
    bool jump   = in ? (in->jump   != 0u) : false;
    bool sprint = in ? (in->sprint != 0u) : false;

    /* ── Horizontal velocity from the (normalized) move direction ──────── */
    float len = sqrtf(walk_x * walk_x + walk_z * walk_z);
    float speed = params.move_speed * (sprint ? params.sprint_mult : 1.0f)
                  * speed_mult;
    float vx = 0.0f, vz = 0.0f;
    if (len > 1e-6f) {
        float inv = 1.0f / len;
        vx = walk_x * inv * speed;
        vz = walk_z * inv * speed;
        /* Face the move direction.  atan2(x, z) gives the yaw about +Y where
         * +Z is forward (0) and +X is right (+90deg), matching the engine's
         * left-handed scene convention used by the character driver. */
        out->yaw = atan2f(walk_x, walk_z);
    }
    /* else: not moving -> keep prev yaw (already copied above). */

    out->vel[0] = vx;
    out->vel[2] = vz;
    out->pos[0] = prev->pos[0] + vx * dt;
    out->pos[2] = prev->pos[2] + vz * dt;

    /* ── Vertical: gravity + grounded-jump + ground clamp ──────────────── */
    const float eps = 1e-4f;
    bool on_ground = (prev->pos[1] <= params.ground_y + eps);

    float vy = prev->vel[1] - params.gravity * dt;
    if (jump && on_ground) {
        vy = params.jump_speed;
        on_ground = false;
    }

    float py = prev->pos[1] + vy * dt;
    if (py < params.ground_y) {
        py = params.ground_y;
        vy = 0.0f;
    }

    out->pos[1] = py;
    out->vel[1] = vy;

    return 0;
}

/* ================================================================== */
/* Tolerant compare                                                    */
/* ================================================================== */

bool jce_predict_loco_compare(const void *a, const void *b, void *user)
{
    const JcePredictState *sa = (const JcePredictState *)a;
    const JcePredictState *sb = (const JcePredictState *)b;
    if (!sa || !sb) return false;   /* unknown -> NOT equal -> force correct */

    float eps = user ? *(const float *)user : JCE_PREDICT_LOCO_POS_EPSILON;
    if (eps < 0.0f) eps = JCE_PREDICT_LOCO_POS_EPSILON;

    float dx = sa->pos[0] - sb->pos[0];
    float dy = sa->pos[1] - sb->pos[1];
    float dz = sa->pos[2] - sb->pos[2];
    float d2 = dx * dx + dy * dy + dz * dz;

    return d2 <= (eps * eps);
}
