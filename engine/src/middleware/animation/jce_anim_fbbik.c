/*
 * jce_anim_fbbik.c — Full-Body IK: FABRIK on a joint tree (sub-base averaging).
 *
 * Per iteration:
 *   backward (leaves->root): each effector node is set to its (weighted) target;
 *     every other node takes the AVERAGE of the positions its children pull it
 *     to (so a shared spine/pelvis reconciles all the limbs that hang off it);
 *   forward  (root->leaves): the root is re-anchored to its input position and
 *     each child is placed exactly one bone-length from its finalized parent,
 *     toward that node's backward position — which makes the pass length-exact.
 *
 * Pure + deterministic; see the header for the contract.
 */

#include <jce/middleware/animation/jce_anim_fbbik.h>
#include <jce/middleware/animation/jce_skeleton.h>

#include <math.h>
#include <string.h>

static jce_vec3 v_add(jce_vec3 a, jce_vec3 b)
{ jce_vec3 r = { a.x + b.x, a.y + b.y, a.z + b.z }; return r; }

static jce_vec3 v_sub(jce_vec3 a, jce_vec3 b)
{ jce_vec3 r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }

static jce_vec3 v_scale(jce_vec3 a, float s)
{ jce_vec3 r = { a.x * s, a.y * s, a.z * s }; return r; }

static float v_len(jce_vec3 a)
{ return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }

/* Unit direction from `from` to `to`; degenerate -> +X so a coincident pair
 * still produces a finite, length-correct placement. */
static jce_vec3 v_dir(jce_vec3 to, jce_vec3 from)
{
    jce_vec3 d = v_sub(to, from);
    float    l = v_len(d);
    if (l < 1e-9f) { jce_vec3 ax = { 1.0f, 0.0f, 0.0f }; return ax; }
    return v_scale(d, 1.0f / l);
}

void JCE_CALL jce_anim_fbbik_compute_lengths(JceFbbikBody *b)
{
    if (!b) return;
    for (int i = 0; i < b->node_count; ++i) {
        if (b->parents[i] < 0) b->lengths[i] = 0.0f;
        else b->lengths[i] = v_len(v_sub(b->positions[i],
                                         b->positions[b->parents[i]]));
    }
}

int JCE_CALL
jce_anim_fbbik_solve(JceFbbikBody           *b,
                     const JceFbbikEffector *effs,
                     int                     neff,
                     int                     max_iter,
                     float                   tol)
{
    if (!b || b->node_count <= 0 || b->node_count > JCE_FBBIK_MAX_NODES)
        return 0;
    if (max_iter < 1) max_iter = 1;
    if (neff < 0) neff = 0;
    if (neff > JCE_FBBIK_MAX_EFFECTORS) neff = JCE_FBBIK_MAX_EFFECTORS;

    const int N = b->node_count;
    const jce_vec3 root_orig = b->positions[0];

    /* Per-node effector seed (the blended target the node is pulled to). */
    jce_vec3 eff_target[JCE_FBBIK_MAX_NODES];
    int      is_eff[JCE_FBBIK_MAX_NODES];
    memset(is_eff, 0, sizeof(int) * (size_t)N);
    for (int e = 0; e < neff; ++e) {
        int n = effs[e].node;
        if (n < 0 || n >= N) continue;
        float w = effs[e].weight;
        if (w < 0.0f) w = 0.0f;
        if (w > 1.0f) w = 1.0f;
        eff_target[n] = jce_v3_lerp(b->positions[n], effs[e].target, w);
        is_eff[n] = 1;
    }

    jce_vec3 bw[JCE_FBBIK_MAX_NODES];
    jce_vec3 accum[JCE_FBBIK_MAX_NODES];
    int      accn[JCE_FBBIK_MAX_NODES];
    int      ran = 0;

    for (int it = 0; it < max_iter; ++it) {
        ++ran;

        /* ── backward: leaves -> root (parents have lower index) ── */
        memset(accum, 0, sizeof(jce_vec3) * (size_t)N);
        memset(accn,  0, sizeof(int)      * (size_t)N);
        for (int i = N - 1; i >= 0; --i) {
            jce_vec3 bpos;
            if (is_eff[i])          bpos = eff_target[i];
            else if (accn[i] > 0)   bpos = v_scale(accum[i], 1.0f / (float)accn[i]);
            else                    bpos = b->positions[i];  /* untouched leaf/branch */
            bw[i] = bpos;

            if (i != 0) {
                int p = b->parents[i];
                /* parent wants to sit lengths[i] from this child, in the
                 * direction of the parent's CURRENT position. */
                jce_vec3 dir     = v_dir(b->positions[p], bpos);
                jce_vec3 contrib = v_add(bpos, v_scale(dir, b->lengths[i]));
                accum[p] = v_add(accum[p], contrib);
                accn[p] += 1;
            }
        }

        /* ── forward: root -> leaves (length-exact placement) ── */
        b->positions[0] = root_orig;
        for (int i = 1; i < N; ++i) {
            int p = b->parents[i];
            jce_vec3 dir = v_dir(bw[i], b->positions[p]);
            b->positions[i] = v_add(b->positions[p], v_scale(dir, b->lengths[i]));
        }

        /* ── convergence against the (blended) effector targets ── */
        float worst = 0.0f;
        for (int i = 0; i < N; ++i) {
            if (!is_eff[i]) continue;
            float d = v_len(v_sub(b->positions[i], eff_target[i]));
            if (d > worst) worst = d;
        }
        if (worst <= tol) break;
    }

    return ran;
}

/* ── Skeleton binding (inc 2) ──────────────────────────────────────────── */

int JCE_CALL
jce_anim_fbbik_solve_pose(const JceSkeleton          *skel,
                          const jce_mat4             *local_transforms,
                          const jce_mat4             *root_world,
                          const JceFbbikSkelEffector *effs,
                          int                         neff,
                          jce_vec3                   *out_world_positions,
                          int                         max_iter,
                          float                       tol)
{
    if (!skel || !out_world_positions) return 0;

    uint32_t n = jce_skeleton_joint_count(skel);
    if (n == 0u || n > JCE_FBBIK_MAX_NODES) return 0;

    const jce_mat4 *pose = local_transforms ? local_transforms
                                            : jce_skeleton_rest_pose(skel);
    if (!pose) return 0;

    /* FK: world transform per joint (parent precedes child by skeleton order). */
    static jce_mat4 g[JCE_FBBIK_MAX_NODES];   /* large; module-private scratch */
    JceFbbikBody body;
    body.node_count = (int)n;

    for (uint32_t i = 0; i < n; ++i) {
        int parent = jce_skeleton_joint_parent(skel, i);
        jce_mat4 lm = pose[i];
        if (parent < 0)
            g[i] = root_world ? jce_m4_multiply(root_world, &lm) : lm;
        else
            g[i] = jce_m4_multiply(&g[parent], &lm);

        body.positions[i].x = g[i].col[3].x;
        body.positions[i].y = g[i].col[3].y;
        body.positions[i].z = g[i].col[3].z;
        body.parents[i]     = parent;   /* skeleton parent ( -1 root ) */
    }
    jce_anim_fbbik_compute_lengths(&body);

    /* Map skeleton-joint effectors onto body nodes (same indexing). */
    JceFbbikEffector be[JCE_FBBIK_MAX_EFFECTORS];
    int bn = 0;
    if (neff > JCE_FBBIK_MAX_EFFECTORS) neff = JCE_FBBIK_MAX_EFFECTORS;
    for (int e = 0; e < neff; ++e) {
        if (effs[e].joint < 0 || (uint32_t)effs[e].joint >= n) continue;
        be[bn].node   = effs[e].joint;
        be[bn].target = effs[e].target;
        be[bn].weight = effs[e].weight;
        ++bn;
    }

    int ran = jce_anim_fbbik_solve(&body, be, bn, max_iter, tol);

    for (uint32_t i = 0; i < n; ++i)
        out_world_positions[i] = body.positions[i];

    return ran;
}

/* ── Position -> rotation write-back ───────────────────────────────────── */

/* Rotation mat4 (column-major) aligning unit vector `a` onto unit vector `b`
 * (Rodrigues).  Parallel -> identity; antiparallel -> 180 deg about any
 * perpendicular axis. */
static jce_mat4 fbbik_rot_from_to(jce_vec3 a, jce_vec3 b)
{
    jce_mat4 m;
    memset(&m, 0, sizeof m);
    m.col[0].x = 1.0f; m.col[1].y = 1.0f; m.col[2].z = 1.0f; m.col[3].w = 1.0f;

    jce_vec3 v = { a.y * b.z - a.z * b.y,
                   a.z * b.x - a.x * b.z,
                   a.x * b.y - a.y * b.x };
    float s = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    float c = a.x * b.x + a.y * b.y + a.z * b.z;

    if (s < 1e-6f) {
        if (c >= 0.0f) return m;                 /* parallel -> identity */
        /* antiparallel: 180 deg about a perpendicular axis. */
        jce_vec3 perp = (fabsf(a.x) < 0.9f) ? (jce_vec3){1,0,0} : (jce_vec3){0,1,0};
        v.x = a.y * perp.z - a.z * perp.y;
        v.y = a.z * perp.x - a.x * perp.z;
        v.z = a.x * perp.y - a.y * perp.x;
        s = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        if (s < 1e-6f) return m;
        c = -1.0f;
    }

    float kx = v.x / s, ky = v.y / s, kz = v.z / s;  /* unit axis */
    float ct = c, st = s;                            /* cos/sin theta */
    float omc = 1.0f - ct;

    /* Rodrigues R[row][col], placed column-major (m.col[c].<row>). */
    m.col[0].x = ct + kx*kx*omc;     m.col[0].y = ky*kx*omc + kz*st;  m.col[0].z = kz*kx*omc - ky*st;
    m.col[1].x = kx*ky*omc - kz*st;  m.col[1].y = ct + ky*ky*omc;     m.col[1].z = kz*ky*omc + kx*st;
    m.col[2].x = kx*kz*omc + ky*st;  m.col[2].y = ky*kz*omc - kx*st;  m.col[2].z = ct + kz*kz*omc;
    return m;
}

void JCE_CALL
jce_anim_fbbik_write_back(const JceSkeleton *skel,
                          const jce_mat4    *old_globals,
                          const jce_vec3    *solved_positions,
                          jce_mat4          *out_globals)
{
    if (!skel || !old_globals || !solved_positions || !out_globals) return;
    uint32_t n = jce_skeleton_joint_count(skel);

    for (uint32_t j = 0; j < n; ++j) {
        /* Primary child = first joint whose parent is j. */
        int child = -1;
        for (uint32_t k = j + 1; k < n; ++k) {
            if (jce_skeleton_joint_parent(skel, k) == (int)j) { child = (int)k; break; }
        }

        jce_mat4 ng;
        if (child >= 0) {
            jce_vec3 oj = { old_globals[j].col[3].x, old_globals[j].col[3].y, old_globals[j].col[3].z };
            jce_vec3 oc = { old_globals[child].col[3].x, old_globals[child].col[3].y, old_globals[child].col[3].z };
            jce_vec3 old_dir = jce_v3_sub(oc, oj);
            jce_vec3 new_dir = jce_v3_sub(solved_positions[child], solved_positions[j]);
            float ol = jce_v3_len(old_dir), nl = jce_v3_len(new_dir);
            if (ol > 1e-6f && nl > 1e-6f) {
                old_dir = jce_v3_scale(old_dir, 1.0f / ol);
                new_dir = jce_v3_scale(new_dir, 1.0f / nl);
                jce_mat4 delta = fbbik_rot_from_to(old_dir, new_dir);
                ng = jce_m4_multiply(&delta, &old_globals[j]);  /* world-space delta */
            } else {
                ng = old_globals[j];
            }
        } else {
            ng = old_globals[j];   /* leaf: keep orientation */
        }

        /* Force the joint to its solved position (rotation/scale preserved). */
        ng.col[3].x = solved_positions[j].x;
        ng.col[3].y = solved_positions[j].y;
        ng.col[3].z = solved_positions[j].z;
        ng.col[3].w = 1.0f;
        out_globals[j] = ng;
    }
}
