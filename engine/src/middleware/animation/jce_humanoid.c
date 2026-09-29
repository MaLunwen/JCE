/*
 * jce_humanoid.c  Humanoid bone mapping and rotation retargeting.
 * See jce_humanoid.h for what this does and what it deliberately does not.
 */

#include <jce/middleware/animation/jce_humanoid.h>

#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <ctype.h>
#include <math.h>   /* acosf / fabsf, for the IK law of cosines */
#include <stdio.h>
#include <string.h>

#define LOG_TAG "humanoid"

/* Unit quaternions: the inverse IS the conjugate, and this file only ever
 * handles unit ones (rest rotations from glTF, animation samples that the
 * sampler normalises).  Spelled out rather than assumed. */
static jce_quat q_inv(jce_quat q)
{
    jce_quat r;
    r.x = -q.x; r.y = -q.y; r.z = -q.z; r.w = q.w;
    return r;
}

static const char *const BONE_NAMES[JCE_HB_COUNT] = {
    "Hips", "Spine", "Chest", "UpperChest", "Neck", "Head",
    "LeftShoulder", "LeftUpperArm", "LeftLowerArm", "LeftHand",
    "RightShoulder", "RightUpperArm", "RightLowerArm", "RightHand",
    "LeftUpperLeg", "LeftLowerLeg", "LeftFoot", "LeftToes",
    "RightUpperLeg", "RightLowerLeg", "RightFoot", "RightToes",
};

const char *jce_humanoid_bone_name(JceHumanoidBone bone)
{
    if ((unsigned)bone >= (unsigned)JCE_HB_COUNT) return "?";
    return BONE_NAMES[bone];
}

/* ── Name normalisation ──────────────────────────────────────────────
 *
 * Lowercase, separators removed, digits removed -- and the digits kept
 * SEPARATELY, because half the conventions in this tree carry the bone's
 * position in a chain as a number (`spine_01`, `torso_joint_2`,
 * `arm_joint_L_3`) and throwing it away would map three spine joints onto one
 * role.  The side is read BEFORE the separators go, because it is a separator
 * that makes `.L` a side and `l` in `clavicle` not one. */
typedef enum { SIDE_NONE = 0, SIDE_LEFT, SIDE_RIGHT } Side;

typedef struct {
    char  key[96];    /* lowercased, separators and digits removed */
    int   ord;        /* the LAST digit run, or -1 */
    Side  side;
} NormName;

static bool is_sep(char c)
{
    return c == '_' || c == '.' || c == '-' || c == ' ';
}

static void normalise(const char *name, NormName *out)
{
    memset(out, 0, sizeof(*out));
    out->ord = -1;
    if (!name) return;

    const size_t n = strlen(name);

    /* SIDE.  A standalone l/r token, or the words left/right, anywhere in the
     * name.  "Standalone" is what keeps `clavicle` from reading as left and
     * `arm_joint_R__2_` from reading as none. */
    for (size_t i = 0; i < n; i++) {
        const char c = (char)tolower((unsigned char)name[i]);
        const bool lb = (i == 0) || is_sep(name[i - 1]);
        const bool rb = (i + 1 >= n) || is_sep(name[i + 1]) ||
                        isdigit((unsigned char)name[i + 1]);
        if (lb && rb) {
            if (c == 'l') { out->side = SIDE_LEFT;  break; }
            if (c == 'r') { out->side = SIDE_RIGHT; break; }
        }
        if (lb && (n - i) >= 4 &&
            strncmp(name + i, "left", 4) == 0) { out->side = SIDE_LEFT; break; }
        if (lb && (n - i) >= 4 &&
            (strncmp(name + i, "Left", 4) == 0)) { out->side = SIDE_LEFT; break; }
        if (lb && (n - i) >= 5 &&
            (strncmp(name + i, "right", 5) == 0 ||
             strncmp(name + i, "Right", 5) == 0)) { out->side = SIDE_RIGHT; break; }
    }

    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < sizeof(out->key); i++) {
        const unsigned char c = (unsigned char)name[i];
        if (is_sep((char)c)) continue;
        if (isdigit(c)) {
            /* Remember the last digit RUN as one number: `01` is 1, not 0 then 1. */
            int v = 0;
            size_t j = i;
            while (j < n && isdigit((unsigned char)name[j])) {
                v = v * 10 + (name[j] - '0');
                j++;
            }
            out->ord = v;
            i = j - 1;
            continue;
        }
        out->key[k++] = (char)tolower(c);
    }
    out->key[k] = '\0';
}

static bool has(const NormName *nn, const char *sub)
{
    return strstr(nn->key, sub) != NULL;
}

/* ── The rule table ──────────────────────────────────────────────────
 *
 * Every entry is here because a rig IN THIS TREE names a bone that way, and
 * the four of them disagree about everything: `pelvis` / `hipcontroller` /
 * `torsojoint`+1, `spine`+1 / `torso` / `torsojoint`+2, `upperarm` /
 * `upperarm` (with a space) / `armjoint`+1.
 *
 * ORDER IS SIGNIFICANT: the first match wins, so the specific keys come
 * before the general ones -- `upperarm` before `arm`, `lowerleg` before `leg`.
 * A table sorted by anything but specificity maps every arm bone to the
 * shoulder.
 *
 * `ord` is the number the name carried, or -1 for "any".  It is what separates
 * `spine_01` from `spine_02` and `arm_joint_L_1` from `_3`. */
typedef struct {
    const char     *key;
    int             ord;      /* -1 = any */
    JceHumanoidBone bone;     /* for a sided rule this is the LEFT one; the
                               * right variant is bone + RIGHT_OFFSET */
    bool            sided;
} Rule;

/* LEFT_* and RIGHT_* are laid out so the right-hand bone of a pair is a fixed
 * distance away, which is what lets one rule serve both sides. */
#define ARM_SIDE_DELTA (JCE_HB_RIGHT_SHOULDER - JCE_HB_LEFT_SHOULDER)
#define LEG_SIDE_DELTA (JCE_HB_RIGHT_UPPER_LEG - JCE_HB_LEFT_UPPER_LEG)

static const Rule RULES[] = {
    /* --- head / spine, most specific first ------------------------- */
    { "upperchest",   -1, JCE_HB_UPPER_CHEST, false },
    { "head",         -1, JCE_HB_HEAD,        false },
    { "neckjoint",     2, JCE_HB_HEAD,        false },  /* RiggedFigure: the
                                                         * chain ends at the
                                                         * head, unnamed */
    { "neck",         -1, JCE_HB_NECK,        false },
    { "chest",        -1, JCE_HB_CHEST,       false },
    { "spine",         3, JCE_HB_UPPER_CHEST, false },
    { "spine",         2, JCE_HB_CHEST,       false },
    { "spine",        -1, JCE_HB_SPINE,       false },
    { "pelvis",       -1, JCE_HB_HIPS,        false },
    { "hipcontroller",-1, JCE_HB_HIPS,        false },
    { "hips",         -1, JCE_HB_HIPS,        false },
    { "hip",          -1, JCE_HB_HIPS,        false },
    { "torsojoint",    1, JCE_HB_HIPS,        false },
    { "torsojoint",    2, JCE_HB_SPINE,       false },
    { "torsojoint",    3, JCE_HB_CHEST,       false },
    { "torso",        -1, JCE_HB_CHEST,       false },

    /* --- arms ------------------------------------------------------ */
    { "clavicle",     -1, JCE_HB_LEFT_SHOULDER,  true },
    { "shoulder",     -1, JCE_HB_LEFT_SHOULDER,  true },
    { "upperarm",     -1, JCE_HB_LEFT_UPPER_ARM, true },
    { "lowerarm",     -1, JCE_HB_LEFT_LOWER_ARM, true },
    { "forearm",      -1, JCE_HB_LEFT_LOWER_ARM, true },
    { "hand",         -1, JCE_HB_LEFT_HAND,      true },
    { "wrist",        -1, JCE_HB_LEFT_HAND,      true },
    { "armjoint",      1, JCE_HB_LEFT_UPPER_ARM, true },
    { "armjoint",      2, JCE_HB_LEFT_LOWER_ARM, true },
    { "armjoint",      3, JCE_HB_LEFT_HAND,      true },

    /* --- legs ------------------------------------------------------ */
    { "upperleg",     -1, JCE_HB_LEFT_UPPER_LEG, true },
    { "thigh",        -1, JCE_HB_LEFT_UPPER_LEG, true },
    { "lowerleg",     -1, JCE_HB_LEFT_LOWER_LEG, true },
    { "calf",         -1, JCE_HB_LEFT_LOWER_LEG, true },
    { "shin",         -1, JCE_HB_LEFT_LOWER_LEG, true },
    { "toe",          -1, JCE_HB_LEFT_TOES,      true },
    { "ball",         -1, JCE_HB_LEFT_TOES,      true },
    { "foot",         -1, JCE_HB_LEFT_FOOT,      true },
    { "ankle",        -1, JCE_HB_LEFT_FOOT,      true },
    { "legjoint",      1, JCE_HB_LEFT_UPPER_LEG, true },
    { "legjoint",      2, JCE_HB_LEFT_LOWER_LEG, true },
    { "legjoint",      3, JCE_HB_LEFT_FOOT,      true },
};

static JceHumanoidBone right_of(JceHumanoidBone left)
{
    if (left >= JCE_HB_LEFT_SHOULDER && left <= JCE_HB_LEFT_HAND)
        return (JceHumanoidBone)(left + ARM_SIDE_DELTA);
    if (left >= JCE_HB_LEFT_UPPER_LEG && left <= JCE_HB_LEFT_TOES)
        return (JceHumanoidBone)(left + LEG_SIDE_DELTA);
    return left;
}

JceHumanoidBone jce_humanoid_bone_from_joint_name(const char *joint_name)
{
    NormName nn;
    normalise(joint_name, &nn);
    if (nn.key[0] == '\0') return JCE_HB_COUNT;

    for (size_t i = 0; i < sizeof(RULES) / sizeof(RULES[0]); i++) {
        const Rule *r = &RULES[i];
        if (!has(&nn, r->key)) continue;
        if (r->ord >= 0 && nn.ord != r->ord) continue;
        if (!r->sided) {
            /* An UNSIDED role on a name that carries a side is not that role.
             * Measured on a real rig: PSX_BagMan has `HIP CONTROLLER` (the
             * hips) and also `HIP.L` / `HIP.R` (the leg roots).  Without this
             * the left hip claims JCE_HB_HIPS, and whether the right bone won
             * came down to which joint the file happened to list first. */
            if (nn.side != SIDE_NONE) continue;
            return r->bone;
        }
        /* A PAIRED bone with no side is left unmapped.  Guessing would drive
         * one arm with the other's motion, which is worse than an arm that
         * does not move. */
        if (nn.side == SIDE_LEFT)  return r->bone;
        if (nn.side == SIDE_RIGHT) return right_of(r->bone);
        return JCE_HB_COUNT;
    }
    return JCE_HB_COUNT;
}

/* Model-space rest rotations, one forward pass.  Joints are ordered
 * parent-before-child (jce_skeleton.h says so), so a parent's model rotation
 * is always already computed. */
static void rest_model_rotations(const JceSkeleton *skel, jce_quat *out,
                                 uint32_t n)
{
    const jce_vec3 *t = NULL, *s = NULL;
    const jce_quat *r = NULL;
    jce_skeleton_rest_trs(skel, &t, &r, &s);
    for (uint32_t i = 0; i < n; i++) {
        const jce_quat local = r ? r[i] : jce_q_identity();
        const int p = jce_skeleton_joint_parent(skel, i);
        out[i] = (p >= 0 && (uint32_t)p < i)
               ? jce_q_multiply(out[p], local)
               : local;
    }
}

/* Model-space rest POSITION y of a joint, for the hips-height ratio.
 *
 * FROM THE REST TRS, not from jce_skeleton_rest_pose().  The two are supposed
 * to say the same thing, and rest_trs is the one jce_skeleton.h calls "the
 * original glTF node TRS values, avoiding decomposition" -- so it is the
 * authority, and it is also what rest_model_rotations above reads.  Composing
 * a height from one source and rotations from the other is how they drift.
 *
 * Measured: a skeleton built with rest_translation set and local_transform
 * left identity -- which the skeleton API permits, and which a test wrote --
 * gave a height of 0 through rest_pose and the right answer through this. */
static jce_vec3 rest_model_pos(const JceSkeleton *skel, uint32_t joint)
{
    const jce_vec3 *t = NULL, *s = NULL;
    const jce_quat *r = NULL;
    jce_skeleton_rest_trs(skel, &t, &r, &s);
    if (!t) return jce_v3(0.0f, 0.0f, 0.0f);

    /* Up the chain, rotating each parent's offset by the rotations above it.
     * Collected first, then applied from the root down, because a child's
     * offset is expressed in its parent's frame. */
    int chain[64];
    int n = 0;
    int cur = (int)joint;
    while (cur >= 0 && n < 64) {
        chain[n++] = cur;
        cur = jce_skeleton_joint_parent(skel, (uint32_t)cur);
    }

    jce_quat rot = jce_q_identity();
    jce_vec3 pos = jce_v3(0.0f, 0.0f, 0.0f);
    for (int i = n - 1; i >= 0; i--) {
        const int j = chain[i];
        const jce_vec3 off = jce_q_rotate(rot, t[j]);
        pos.x += off.x; pos.y += off.y; pos.z += off.z;
        if (r) rot = jce_q_multiply(rot, r[j]);
    }
    return pos;
}

/* How far the hips stand above the feet, in the rest pose.
 *
 * IT USED TO READ `.y`, AND THAT IS WRONG ON A Z-UP RIG.  glTF is Y-up by
 * convention but exporters ship Z-up rigs under a converting root node, and
 * the Khronos CesiumMan sample in this tree is one: its hips joint sits at
 * (1.6e-8, 0.005, 0.679), so `.y` answered 5 MILLIMETRES for a 0.68 m
 * character -- a factor of 136.  That number scales the root translation on
 * every role retarget (a 1 m stride would arrive as 206 m) and, since
 * 2026-09-19, the effector targets too.  The existing test did not catch it
 * because both of its fixture rigs are Y-up; a defect in an axis assumption
 * cannot be found by a fixture that shares the assumption.
 *
 * So: measure a DISTANCE, which no axis convention can spoil -- hips to the
 * midpoint of the feet, which is also the quantity the ratio is for (a short
 * character takes the same stride relative to its own legs).  On a Y-up rig
 * with the feet on the ground this is the old answer to within the feet's own
 * height, so existing content does not move; what moves is the rigs the old
 * expression was silently wrong about.
 *
 * With no feet mapped it falls back to the hips' DISTANCE from the skeleton
 * root rather than to `.y`: still axis-free, and a strictly better wrong
 * answer than five millimetres. */
static float rest_hips_height(const JceSkeleton *skel, uint32_t hips,
                              int32_t foot_l, int32_t foot_r)
{
    const jce_vec3 h = rest_model_pos(skel, hips);
    jce_vec3 f;
    int n = 0;
    f = jce_v3(0.0f, 0.0f, 0.0f);
    if (foot_l >= 0) { f = jce_v3_add(f, rest_model_pos(skel, (uint32_t)foot_l)); n++; }
    if (foot_r >= 0) { f = jce_v3_add(f, rest_model_pos(skel, (uint32_t)foot_r)); n++; }
    if (n == 0) return jce_v3_len(h);
    f = jce_v3_scale(f, 1.0f / (float)n);
    return jce_v3_len(jce_v3_sub(h, f));
}

bool jce_humanoid_map_build(const JceSkeleton *skel, JceHumanoidMap *out)
{
    if (!skel || !out) return false;
    memset(out, 0, sizeof(*out));
    for (int b = 0; b < JCE_HB_COUNT; b++) {
        out->joint[b] = -1;
        out->rest_model[b] = jce_q_identity();
    }

    const uint32_t n = jce_skeleton_joint_count(skel);
    if (n == 0) return true;

    jce_quat *model = (jce_quat *)JCE_MALLOC((size_t)n * sizeof(jce_quat));
    if (!model) return false;
    rest_model_rotations(skel, model, n);

    for (uint32_t i = 0; i < n; i++) {
        const char *nm = jce_skeleton_joint_name(skel, i);
        const JceHumanoidBone b = jce_humanoid_bone_from_joint_name(nm);
        if (b >= JCE_HB_COUNT) continue;
        /* FIRST WINS.  A rig with `hand_l` and `hand_l_end` would otherwise
         * end with the tip, and the tip is not the hand. */
        if (out->joint[b] >= 0) continue;
        out->joint[b] = (int32_t)i;
        out->rest_model[b] = model[i];
        out->mapped_count++;
    }

    if (out->joint[JCE_HB_HIPS] >= 0)
        out->hips_height =
            rest_hips_height(skel, (uint32_t)out->joint[JCE_HB_HIPS],
                             out->joint[JCE_HB_LEFT_FOOT],
                             out->joint[JCE_HB_RIGHT_FOOT]);

    JCE_FREE(model);
    return true;
}

void jce_humanoid_rest_locals(const JceSkeleton *skel, jce_quat *out_local)
{
    if (!skel || !out_local) return;
    const uint32_t n = jce_skeleton_joint_count(skel);
    const jce_vec3 *t = NULL, *s = NULL;
    const jce_quat *r = NULL;
    jce_skeleton_rest_trs(skel, &t, &r, &s);
    for (uint32_t i = 0; i < n; i++)
        out_local[i] = r ? r[i] : jce_q_identity();
}

uint32_t jce_humanoid_retarget(const JceHumanoidMap *src,
                               const JceSkeleton    *src_skel,
                               const jce_quat       *src_local,
                               const JceHumanoidMap *dst,
                               const JceSkeleton    *dst_skel,
                               jce_quat             *dst_local,
                               jce_vec3             *root_translation)
{
    /* THE OLD ENTRY POINT, AND IT STILL DOES EXACTLY WHAT IT DID.  Clamping
     * is off here on purpose: existing content animates bit-for-bit as
     * before, and a caller that wants the muscle range says so. */
    return jce_humanoid_retarget_clamped(src, src_skel, src_local,
                                         dst, dst_skel, dst_local,
                                         root_translation, false, NULL);
}

uint32_t jce_humanoid_retarget_clamped(const JceHumanoidMap *src,
                                       const JceSkeleton    *src_skel,
                                       const jce_quat       *src_local,
                                       const JceHumanoidMap *dst,
                                       const JceSkeleton    *dst_skel,
                                       jce_quat             *dst_local,
                                       jce_vec3             *root_translation,
                                       bool                  clamp_muscles,
                                       uint32_t             *out_clamped)
{
    if (out_clamped) *out_clamped = 0;
    if (!src || !src_skel || !src_local || !dst || !dst_skel || !dst_local)
        return 0;

    const uint32_t sn = jce_skeleton_joint_count(src_skel);
    const uint32_t dn = jce_skeleton_joint_count(dst_skel);
    if (sn == 0 || dn == 0) return 0;

    /* The source pose in MODEL space, one forward pass. */
    jce_quat *smodel = (jce_quat *)JCE_MALLOC((size_t)sn * sizeof(jce_quat));
    jce_quat *dmodel = (jce_quat *)JCE_MALLOC((size_t)dn * sizeof(jce_quat));
    if (!smodel || !dmodel) { JCE_FREE(smodel); JCE_FREE(dmodel); return 0; }

    for (uint32_t i = 0; i < sn; i++) {
        const int p = jce_skeleton_joint_parent(src_skel, i);
        smodel[i] = (p >= 0 && (uint32_t)p < i)
                  ? jce_q_multiply(smodel[p], src_local[i])
                  : src_local[i];
    }

    /* What each destination joint is ASKED to be, in model space.  Only the
     * mapped ones get an answer; everything else keeps its rest local, which
     * is what dst_local already holds. */
    jce_quat wanted[JCE_HB_COUNT];
    bool     has_wanted[JCE_HB_COUNT];
    uint32_t moved = 0;
    for (int b = 0; b < JCE_HB_COUNT; b++) {
        has_wanted[b] = false;
        wanted[b] = jce_q_identity();
        if (src->joint[b] < 0 || dst->joint[b] < 0) continue;
        /* The rotation the ANIMATION added, in the source's model space. */
        jce_quat delta =
            jce_q_multiply(q_inv(src->rest_model[b]),
                           smodel[src->joint[b]]);

        /* CLAMPED HERE, on the delta, BEFORE it is rebased onto the target.
         * This is the only place in the transfer where the value has the
         * meaning the limits are about -- "how far did the animation push
         * this bone from its own rest".  One step later it is an absolute
         * target orientation and the same numbers would mean nothing.
         *
         * The axis comes from the SOURCE rig, because the delta is in the
         * source's model space; a bone with no axis (a tip, or a role whose
         * child is unmapped) is left alone rather than clamped about a
         * guess. */
        if (clamp_muscles) {
            jce_vec3 axis;
            if (jce_humanoid_bone_axis(src, src_skel, (JceHumanoidBone)b,
                                       &axis)) {
                const jce_quat c =
                    jce_humanoid_clamp_delta((JceHumanoidBone)b, axis, delta);
                if (c.x != delta.x || c.y != delta.y ||
                    c.z != delta.z || c.w != delta.w) {
                    if (out_clamped) (*out_clamped)++;
                    delta = c;
                }
            }
            /* HINGE DIRECTION, after the magnitude clamp and in the same
             * space.  The clamp above bounds how far the animation pushed the
             * bone; this bounds WHICH WAY, which it cannot express -- a cone
             * plus a signed twist passes an inverted knee, and an inverted
             * knee is the most recognisable retargeting artefact there is.
             * Knees only; jce_humanoid.h says why an elbow is not derivable
             * the same way.  5 degrees of hyperextension, because real knees
             * have some and a clamp that fires at exactly zero trips on poses
             * an animator meant. */
            jce_quat k =
                jce_humanoid_clamp_knee_direction(src, src_skel,
                                                  (JceHumanoidBone)b,
                                                  5.0f, delta);
            /* Elbows too, where the rig's rest bend gives a plane.  Both are
             * no-ops on every role they do not name, so the pair reads as one
             * rule -- "a hinge joint bends one way" -- without either pretending
             * to derive its axis the other one's way. */
            k = jce_humanoid_clamp_elbow_direction(src, src_skel,
                                                   (JceHumanoidBone)b,
                                                   5.0f, k);
            if (k.x != delta.x || k.y != delta.y ||
                k.z != delta.z || k.w != delta.w) {
                if (out_clamped) (*out_clamped)++;
                delta = k;
            }
        }

        wanted[b] = jce_q_normalize(jce_q_multiply(dst->rest_model[b], delta));
        has_wanted[b] = true;
        moved++;
    }

    /* One forward pass over the destination, converting each answer back into
     * a LOCAL rotation against whatever its actual parent ended up being --
     * which may be an unmapped joint still sitting at its rest pose. */
    for (uint32_t i = 0; i < dn; i++) {
        const int p = jce_skeleton_joint_parent(dst_skel, i);
        const jce_quat pm = (p >= 0 && (uint32_t)p < i) ? dmodel[p]
                                                        : jce_q_identity();
        int found = -1;
        for (int b = 0; b < JCE_HB_COUNT; b++) {
            if (has_wanted[b] && dst->joint[b] == (int32_t)i) { found = b; break; }
        }
        if (found >= 0) {
            dst_local[i] = jce_q_normalize(jce_q_multiply(q_inv(pm),
                                                          wanted[found]));
            dmodel[i] = wanted[found];
        } else {
            dmodel[i] = jce_q_multiply(pm, dst_local[i]);
        }
    }

    /* Root translation, scaled by the ratio of hip heights: a small character
     * takes the same stride RELATIVE TO ITSELF rather than the source's
     * absolute metres. */
    if (root_translation && src->hips_height > 1e-4f && dst->hips_height > 1e-4f) {
        const float k = dst->hips_height / src->hips_height;
        root_translation->x *= k;
        root_translation->y *= k;
        root_translation->z *= k;
    }

    JCE_FREE(smodel);
    JCE_FREE(dmodel);
    return moved;
}

/* ================================================================== */
/* Muscle-space clamping -- see jce_humanoid.h for what it is and is not */
/* ================================================================== */

/* Limits on the DELTA, in degrees.  Derived from Unity's Humanoid defaults and
 * deliberately on the generous side: a clamp that fires on motion an animator
 * meant is worse than one that only catches the transfers that were going to
 * look broken anyway, because the first kind gets switched off. */
static const JceHumanoidMuscleLimits k_muscle[JCE_HB_COUNT] = {
    /* twist_min, twist_max, swing_max */
    { -40.0f,  40.0f,  40.0f },   /* HIPS            */
    { -40.0f,  40.0f,  40.0f },   /* SPINE           */
    { -40.0f,  40.0f,  40.0f },   /* CHEST           */
    { -40.0f,  40.0f,  40.0f },   /* UPPER_CHEST     */
    { -40.0f,  40.0f,  40.0f },   /* NECK            */
    { -40.0f,  40.0f,  40.0f },   /* HEAD            */
    { -15.0f,  15.0f,  30.0f },   /* LEFT_SHOULDER   */
    { -90.0f,  90.0f, 100.0f },   /* LEFT_UPPER_ARM  */
    { -90.0f,  90.0f,  80.0f },   /* LEFT_LOWER_ARM  */
    { -40.0f,  40.0f,  80.0f },   /* LEFT_HAND       */
    { -15.0f,  15.0f,  30.0f },   /* RIGHT_SHOULDER  */
    { -90.0f,  90.0f, 100.0f },   /* RIGHT_UPPER_ARM */
    { -90.0f,  90.0f,  80.0f },   /* RIGHT_LOWER_ARM */
    { -40.0f,  40.0f,  80.0f },   /* RIGHT_HAND      */
    { -60.0f,  60.0f,  90.0f },   /* LEFT_UPPER_LEG  */
    { -30.0f,  30.0f,  80.0f },   /* LEFT_LOWER_LEG  */
    { -30.0f,  30.0f,  50.0f },   /* LEFT_FOOT       */
    { -10.0f,  10.0f,  45.0f },   /* LEFT_TOES       */
    { -60.0f,  60.0f,  90.0f },   /* RIGHT_UPPER_LEG */
    { -30.0f,  30.0f,  80.0f },   /* RIGHT_LOWER_LEG */
    { -30.0f,  30.0f,  50.0f },   /* RIGHT_FOOT      */
    { -10.0f,  10.0f,  45.0f },   /* RIGHT_TOES      */
};

/* The role BELOW each one, i.e. the joint whose position gives this bone its
 * direction.  JCE_HB_COUNT where the chain ends (a hand, a toe, a head): a tip
 * has no bone axis to speak of and is left alone. */
static const JceHumanoidBone k_child_role[JCE_HB_COUNT] = {
    JCE_HB_SPINE,            /* HIPS            */
    JCE_HB_CHEST,            /* SPINE           */
    JCE_HB_NECK,             /* CHEST           */
    JCE_HB_NECK,             /* UPPER_CHEST     */
    JCE_HB_HEAD,             /* NECK            */
    JCE_HB_COUNT,            /* HEAD            */
    JCE_HB_LEFT_UPPER_ARM,   /* LEFT_SHOULDER   */
    JCE_HB_LEFT_LOWER_ARM,   /* LEFT_UPPER_ARM  */
    JCE_HB_LEFT_HAND,        /* LEFT_LOWER_ARM  */
    JCE_HB_COUNT,            /* LEFT_HAND       */
    JCE_HB_RIGHT_UPPER_ARM,  /* RIGHT_SHOULDER  */
    JCE_HB_RIGHT_LOWER_ARM,  /* RIGHT_UPPER_ARM */
    JCE_HB_RIGHT_HAND,       /* RIGHT_LOWER_ARM */
    JCE_HB_COUNT,            /* RIGHT_HAND      */
    JCE_HB_LEFT_LOWER_LEG,   /* LEFT_UPPER_LEG  */
    JCE_HB_LEFT_FOOT,        /* LEFT_LOWER_LEG  */
    JCE_HB_LEFT_TOES,        /* LEFT_FOOT       */
    JCE_HB_COUNT,            /* LEFT_TOES       */
    JCE_HB_RIGHT_LOWER_LEG,  /* RIGHT_UPPER_LEG */
    JCE_HB_RIGHT_FOOT,       /* RIGHT_LOWER_LEG */
    JCE_HB_RIGHT_TOES,       /* RIGHT_FOOT      */
    JCE_HB_COUNT,            /* RIGHT_TOES      */
};

bool jce_humanoid_muscle_limits(JceHumanoidBone bone,
                                JceHumanoidMuscleLimits *out)
{
    if (!out || bone < 0 || bone >= JCE_HB_COUNT) return false;
    *out = k_muscle[bone];
    return true;
}

bool jce_humanoid_bone_axis(const JceHumanoidMap *map,
                            const JceSkeleton    *skel,
                            JceHumanoidBone       bone,
                            jce_vec3             *out_axis)
{
    if (!map || !skel || !out_axis) return false;
    if (bone < 0 || bone >= JCE_HB_COUNT) return false;

    const JceHumanoidBone child = k_child_role[bone];
    if (child >= JCE_HB_COUNT) return false;               /* a tip */
    if (map->joint[bone] < 0 || map->joint[child] < 0) return false;

    const jce_vec3 a = rest_model_pos(skel, (uint32_t)map->joint[bone]);
    const jce_vec3 b = rest_model_pos(skel, (uint32_t)map->joint[child]);
    jce_vec3 d = jce_v3(b.x - a.x, b.y - a.y, b.z - a.z);
    const float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    /* Coincident joints give no direction.  Returning false rather than a
     * made-up axis is the point: a caller that decomposed about a guess would
     * clamp the wrong component and the pose would be wrong in a way nothing
     * downstream could attribute. */
    if (len < 1e-5f) return false;
    d.x /= len; d.y /= len; d.z /= len;
    *out_axis = d;
    return true;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static jce_quat quat_about(jce_vec3 axis, float rad)
{
    const float h = rad * 0.5f;
    const float s = sinf(h);
    jce_quat q;
    q.x = axis.x * s; q.y = axis.y * s; q.z = axis.z * s; q.w = cosf(h);
    return jce_q_normalize(q);
}

/* The TWIST component of `q` about `axis` -- the swing-twist split, factored
 * out because the clamp below and the twist redistribution further down both
 * need exactly this and two copies of a quaternion decomposition is two things
 * to keep in agreement.  Returns identity when the rotation is a pure swing
 * (the degenerate 180-about-a-perpendicular case), which is the honest answer:
 * there is no twist to move. */
static jce_quat twist_about(jce_quat q, jce_vec3 axis)
{
    q = jce_q_normalize(q);
    if (q.w < 0.0f) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
    const float d = q.x * axis.x + q.y * axis.y + q.z * axis.z;
    jce_quat tw;
    tw.x = axis.x * d; tw.y = axis.y * d; tw.z = axis.z * d; tw.w = q.w;
    const float tn = sqrtf(tw.x * tw.x + tw.y * tw.y + tw.z * tw.z
                           + tw.w * tw.w);
    if (tn < 1e-8f) return jce_q_identity();
    tw.x /= tn; tw.y /= tn; tw.z /= tn; tw.w /= tn;
    if (tw.w < 0.0f) { tw.x = -tw.x; tw.y = -tw.y; tw.z = -tw.z; tw.w = -tw.w; }
    return tw;
}

/* The rig's own body frame, from its REST pose.
 *
 * right: hip to hip (shoulders if the legs are unmapped) -- a pair that is
 *        never collinear on a humanoid, which is the whole point.
 * up:    hips to the highest mapped spine role, orthogonalised against right.
 * fwd:   right x up.
 *
 * No axis convention is assumed and nothing is configured: every input is a
 * DIFFERENCE between two joints this rig itself nominated for a role. */
static bool body_frame(const JceHumanoidMap *map, const JceSkeleton *skel,
                       jce_vec3 *out_right, jce_vec3 *out_up, jce_vec3 *out_fwd)
{
    const int32_t hips = map->joint[JCE_HB_HIPS];
    if (hips < 0) return false;

    int32_t la = map->joint[JCE_HB_LEFT_UPPER_LEG];
    int32_t ra = map->joint[JCE_HB_RIGHT_UPPER_LEG];
    if (la < 0 || ra < 0) {
        la = map->joint[JCE_HB_LEFT_UPPER_ARM];
        ra = map->joint[JCE_HB_RIGHT_UPPER_ARM];
    }
    if (la < 0 || ra < 0) return false;

    int32_t top = map->joint[JCE_HB_HEAD];
    if (top < 0) top = map->joint[JCE_HB_NECK];
    if (top < 0) top = map->joint[JCE_HB_UPPER_CHEST];
    if (top < 0) top = map->joint[JCE_HB_CHEST];
    if (top < 0) top = map->joint[JCE_HB_SPINE];
    if (top < 0) return false;

    jce_vec3 right = jce_v3_sub(rest_model_pos(skel, (uint32_t)ra),
                                rest_model_pos(skel, (uint32_t)la));
    jce_vec3 up    = jce_v3_sub(rest_model_pos(skel, (uint32_t)top),
                                rest_model_pos(skel, (uint32_t)hips));
    if (jce_v3_len(right) < 1e-5f || jce_v3_len(up) < 1e-5f) return false;
    right = jce_v3_normalize(right);
    up    = jce_v3_sub(up, jce_v3_scale(right, jce_v3_dot(up, right)));
    if (jce_v3_len(up) < 1e-5f) return false;   /* spine parallel to the hips */
    up = jce_v3_normalize(up);

    *out_right = right;
    *out_up    = up;
    *out_fwd   = jce_v3_normalize(jce_v3_cross(right, up));
    return true;
}

bool jce_humanoid_hinge_axis(const JceHumanoidMap *map,
                             const JceSkeleton    *skel,
                             JceHumanoidBone       bone,
                             jce_vec3             *out_axis,
                             float                *out_bend_sign)
{
    if (!map || !skel || !out_axis || !out_bend_sign) return false;

    const int knee = (bone == JCE_HB_LEFT_LOWER_LEG ||
                      bone == JCE_HB_RIGHT_LOWER_LEG);
    const int elbow = (bone == JCE_HB_LEFT_LOWER_ARM ||
                       bone == JCE_HB_RIGHT_LOWER_ARM);
    if (!knee && !elbow) return false;

    const int is_left = (bone == JCE_HB_LEFT_LOWER_LEG ||
                         bone == JCE_HB_LEFT_LOWER_ARM);
    const JceHumanoidBone ub = knee
        ? (is_left ? JCE_HB_LEFT_UPPER_LEG : JCE_HB_RIGHT_UPPER_LEG)
        : (is_left ? JCE_HB_LEFT_UPPER_ARM : JCE_HB_RIGHT_UPPER_ARM);
    const JceHumanoidBone eb = knee
        ? (is_left ? JCE_HB_LEFT_FOOT : JCE_HB_RIGHT_FOOT)
        : (is_left ? JCE_HB_LEFT_HAND : JCE_HB_RIGHT_HAND);
    const int32_t uj = map->joint[ub], mj = map->joint[bone],
                  ej = map->joint[eb];
    if (uj < 0 || mj < 0 || ej < 0) return false;

    const jce_vec3 pu = rest_model_pos(skel, (uint32_t)uj);
    const jce_vec3 pm = rest_model_pos(skel, (uint32_t)mj);
    const jce_vec3 pe = rest_model_pos(skel, (uint32_t)ej);
    const jce_vec3 lower = jce_v3_sub(pe, pm);
    if (jce_v3_len(lower) < 1e-5f) return false;

    jce_vec3 axis, fwd = jce_v3(0.0f, 0.0f, 0.0f);
    if (knee) {
        /* The BODY's lateral axis.  It survives a straight rest leg, which is
         * why the knee gets this and the elbow cannot: the two hips are never
         * collinear, while the two bones of a straight leg are. */
        jce_vec3 right, up;
        if (!body_frame(map, skel, &right, &up, &fwd)) return false;
        axis = right;
    } else {
        /* The plane the arm is ALREADY BENT IN.  Which way a forearm flexes
         * depends on how the rig rolls the upper arm and that differs per
         * tool; the rest bend is the only rig-intrinsic answer, and a rig that
         * leaves none gets a refusal rather than a guess. */
        const jce_vec3 upper = jce_v3_sub(pm, pu);
        const float lu = jce_v3_len(upper), ll = jce_v3_len(lower);
        if (lu < 1e-5f) return false;
        axis = jce_v3_cross(upper, lower);
        /* sin(bend) = |cross| / (|u||l|); below 5 degrees the plane is the
         * artist's rounding rather than their intent. */
        if (jce_v3_len(axis) / (lu * ll) < 0.0871557f) return false;
        axis = jce_v3_normalize(axis);
    }

    /* WHICH WAY FOLDS.  Rotating by +theta displaces the end along
     * cross(axis, lower) in both cases; what differs is the reference the
     * displacement is judged against, and it differs for the same reason the
     * axis does.
     *
     * The tempting single rule -- "folding brings the end toward the limb's
     * root" -- IS DEGENERATE ON A STRAIGHT LIMB, which is most legs.  There
     * cross(axis, lower) is perpendicular to the bone while (root - end) lies
     * along it, so their dot product is exactly zero and the derivation
     * refuses a knee it should have answered.  It reads as a knee clamp that
     * silently does nothing.  Measured on make_full_rig: 0.0, not nearly.
     *
     * So a knee is judged against the BODY's forward -- a knee sends the foot
     * BACKWARD -- which a straight leg still answers; and an elbow against the
     * root, which is well-conditioned precisely because the elbow branch above
     * already required a rest bend. */
    const float toward = knee
        ? -jce_v3_dot(jce_v3_cross(axis, lower), fwd)
        :  jce_v3_dot(jce_v3_cross(axis, lower), jce_v3_sub(pu, pe));
    if (fabsf(toward) < 1e-9f) return false;

    *out_axis = axis;
    *out_bend_sign = (toward > 0.0f) ? 1.0f : -1.0f;
    return true;
}

/* Clamp `delta`'s rotation about `axis` so its signed angle, measured toward
 * `bend_sign`, never goes below -`allow_deg`.  Shared by the knee and the
 * elbow, which differ only in where the axis and the sign come from. */
static jce_quat clamp_hinge_about(jce_vec3 axis, float bend_sign,
                                  float allow_deg, jce_quat delta)
{
    const jce_quat h = twist_about(delta, axis);
    const float k_rad2deg = 57.29577951308232f;
    float ang = 2.0f * acosf(clampf(h.w, -1.0f, 1.0f)) * k_rad2deg;
    const float d = h.x * axis.x + h.y * axis.y + h.z * axis.z;
    if (d < 0.0f) ang = -ang;

    const float bend = ang * bend_sign;
    if (bend >= -allow_deg) return delta;         /* legal: caller's own bits */
    const float excess = (bend + allow_deg) * bend_sign;
    const jce_quat fix = quat_about(axis, -excess * 0.017453292519943295f);
    return jce_q_normalize(jce_q_multiply(fix, delta));
}

jce_quat jce_humanoid_clamp_elbow_direction(const JceHumanoidMap *map,
                                            const JceSkeleton    *skel,
                                            JceHumanoidBone       bone,
                                            float                 hyperextend_deg,
                                            jce_quat              delta)
{
    if (!map || !skel) return delta;
    if (bone != JCE_HB_LEFT_LOWER_ARM && bone != JCE_HB_RIGHT_LOWER_ARM)
        return delta;

    /* THE AXIS COMES FROM THE REST BEND, and there may not be one.
     *
     * A knee's hinge axis can be taken from the body (the two hips are never
     * collinear).  An elbow's cannot: which way a forearm flexes depends on
     * how the rig ROLLS the upper arm, and that differs per tool.  What is
     * rig-intrinsic is the plane the arm is already bent in, when the artist
     * left a bend -- which is exactly why riggers leave one, so IK solvers
     * know the side.
     *
     * MEASURED on the two rigged humans in this tree: CesiumMan's arm rests at
     * 147 degrees (33 of bend) and gives a plane; PSX_BagMan's rests at
     * EXACTLY 180 and does not.  So this derives where the rig allows it and
     * REFUSES where it does not, rather than guessing -- an elbow clamped
     * about an invented axis rejects correct animation, which is worse than
     * not clamping. */
    jce_vec3 axis; float bend_sign;
    if (!jce_humanoid_hinge_axis(map, skel, bone, &axis, &bend_sign))
        return delta;
    return clamp_hinge_about(axis, bend_sign, hyperextend_deg, delta);
}

jce_quat jce_humanoid_clamp_knee_direction(const JceHumanoidMap *map,
                                           const JceSkeleton    *skel,
                                           JceHumanoidBone       bone,
                                           float                 hyperextend_deg,
                                           jce_quat              delta)
{
    if (!map || !skel) return delta;
    if (bone != JCE_HB_LEFT_LOWER_LEG && bone != JCE_HB_RIGHT_LOWER_LEG)
        return delta;

    jce_vec3 axis; float bend_sign;
    if (!jce_humanoid_hinge_axis(map, skel, bone, &axis, &bend_sign))
        return delta;
    return clamp_hinge_about(axis, bend_sign, hyperextend_deg, delta);
}

uint32_t jce_humanoid_redistribute_twist(const JceHumanoidMap *map,
                                         const JceSkeleton    *skel,
                                         float                 fraction,
                                         jce_quat             *io_local)
{
    if (!map || !skel || !io_local) return 0;
    if (fraction <= 0.0f) return 0;          /* 0 is "do nothing", exactly */
    if (fraction > 1.0f) fraction = 1.0f;

    const uint32_t nj = jce_skeleton_joint_count(skel);
    uint32_t moved = 0;

    for (int li = 0; li < JCE_HUMANOID_LIMB_COUNT; li++) {
        JceHumanoidBone ub, lb, eb;
        if (!jce_humanoid_limb_bones((JceHumanoidLimb)li, &ub, &lb, &eb))
            continue;
        const int32_t lj = map->joint[lb], ej = map->joint[eb];
        if (lj < 0 || ej < 0) continue;
        if ((uint32_t)lj >= nj || (uint32_t)ej >= nj) continue;
        /* Only a real parent chain: moving twist "up" to a joint the end does
         * not descend from would move the end itself.  Same reason the IK
         * solver checks (PSX_BagMan's foot is a control bone off the root). */
        if (jce_skeleton_joint_parent(skel, (uint32_t)ej) != lj) continue;

        /* The axis is the LOWER bone's own direction, taken from the rig --
         * which local axis runs down a bone is a different answer in every
         * tool, and this tree carries three conventions in four models. */
        jce_vec3 axis;
        if (!jce_humanoid_bone_axis(map, skel, lb, &axis)) continue;

        const jce_quat tw = twist_about(io_local[ej], axis);
        if (tw.w >= 1.0f - 1e-7f) continue;          /* no twist to move */

        /* Take `fraction` of it off the end and put it on the lower bone.
         * Because the axis runs from the lower joint THROUGH the end joint,
         * rotating the lower bone about it leaves the end joint's POSITION
         * untouched, and removing the same rotation from the end's local
         * leaves its ORIENTATION untouched too.  The pose is geometrically
         * identical; what changes is where the twist is expressed, which is
         * all that skinning sees.  That is the whole point: all of it at the
         * wrist pinches the mesh into the candy wrapper, spread along the
         * forearm it does not. */
        const float ang = 2.0f * acosf(clampf(tw.w, -1.0f, 1.0f));
        const float d = tw.x * axis.x + tw.y * axis.y + tw.z * axis.z;
        const float signed_ang = (d < 0.0f) ? -ang : ang;
        const jce_quat part  = quat_about(axis, signed_ang * fraction);
        jce_quat parti; parti.x = -part.x; parti.y = -part.y;
                        parti.z = -part.z; parti.w =  part.w;

        io_local[lj] = jce_q_normalize(jce_q_multiply(io_local[lj], part));
        io_local[ej] = jce_q_normalize(jce_q_multiply(parti, io_local[ej]));
        moved++;
    }
    return moved;
}

jce_quat jce_humanoid_clamp_delta(JceHumanoidBone bone, jce_vec3 axis,
                                  jce_quat delta)
{
    if (bone < 0 || bone >= JCE_HB_COUNT) return delta;

    jce_quat q = jce_q_normalize(delta);
    /* Quaternion double cover: q and -q are the same rotation, and only one of
     * them has the small-angle reading.  Every angle below is taken from the
     * w >= 0 representative, so a 10-degree delta cannot read as 350. */
    if (q.w < 0.0f) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }

    /* SWING-TWIST about the bone's own axis: project the vector part onto the
     * axis for the twist, and whatever is left over is the swing. */
    const float d = q.x * axis.x + q.y * axis.y + q.z * axis.z;
    jce_quat tw;
    tw.x = axis.x * d; tw.y = axis.y * d; tw.z = axis.z * d; tw.w = q.w;
    const float tn = sqrtf(tw.x * tw.x + tw.y * tw.y + tw.z * tw.z
                           + tw.w * tw.w);
    if (tn < 1e-8f) return delta;          /* 180 deg about a swing axis */
    tw.x /= tn; tw.y /= tn; tw.z /= tn; tw.w /= tn;
    if (tw.w < 0.0f) { tw.x = -tw.x; tw.y = -tw.y; tw.z = -tw.z; tw.w = -tw.w; }

    jce_quat twi; twi.x = -tw.x; twi.y = -tw.y; twi.z = -tw.z; twi.w = tw.w;
    jce_quat sw = jce_q_normalize(jce_q_multiply(q, twi));
    if (sw.w < 0.0f) { sw.x = -sw.x; sw.y = -sw.y; sw.z = -sw.z; sw.w = -sw.w; }

    const float k_rad2deg = 57.29577951308232f;
    const float k_deg2rad = 0.017453292519943295f;

    /* Twist, SIGNED about the axis. */
    float tw_ang = 2.0f * acosf(clampf(tw.w, -1.0f, 1.0f)) * k_rad2deg;
    if (d < 0.0f) tw_ang = -tw_ang;

    /* Swing, a magnitude: its axis is whatever perpendicular direction the
     * rotation actually used, and that direction is kept. */
    float sw_ang = 2.0f * acosf(clampf(sw.w, -1.0f, 1.0f)) * k_rad2deg;

    const JceHumanoidMuscleLimits L = k_muscle[bone];
    const float tw_c = clampf(tw_ang, L.twist_min, L.twist_max);
    const float sw_c = clampf(sw_ang, 0.0f, L.swing_max);

    /* UNCHANGED MEANS UNCHANGED.  A delta already inside the range comes back
     * as the caller's own bits, not as a recomposition that differs in the
     * last place -- so a test asking "did the clamp fire" has an exact
     * answer, and a pose that needed no clamping is not quietly re-quantised
     * every frame. */
    if (tw_c == tw_ang && sw_c == sw_ang) return delta;

    jce_quat tw_out = quat_about(axis, tw_c * k_deg2rad);

    jce_quat sw_out = sw;
    if (sw_c != sw_ang) {
        const float sn = sqrtf(sw.x * sw.x + sw.y * sw.y + sw.z * sw.z);
        if (sn > 1e-8f) {
            const jce_vec3 sa = jce_v3(sw.x / sn, sw.y / sn, sw.z / sn);
            sw_out = quat_about(sa, sw_c * k_deg2rad);
        }
    }
    return jce_q_normalize(jce_q_multiply(sw_out, tw_out));
}

/* ── Two-bone IK ─────────────────────────────────────────────────────── */

bool jce_humanoid_limb_bones(JceHumanoidLimb limb,
                             JceHumanoidBone *out_upper,
                             JceHumanoidBone *out_lower,
                             JceHumanoidBone *out_end)
{
    JceHumanoidBone u, l, e;
    switch (limb) {
    case JCE_HUMANOID_LIMB_LEFT_ARM:
        u = JCE_HB_LEFT_UPPER_ARM;  l = JCE_HB_LEFT_LOWER_ARM;  e = JCE_HB_LEFT_HAND;  break;
    case JCE_HUMANOID_LIMB_RIGHT_ARM:
        u = JCE_HB_RIGHT_UPPER_ARM; l = JCE_HB_RIGHT_LOWER_ARM; e = JCE_HB_RIGHT_HAND; break;
    case JCE_HUMANOID_LIMB_LEFT_LEG:
        u = JCE_HB_LEFT_UPPER_LEG;  l = JCE_HB_LEFT_LOWER_LEG;  e = JCE_HB_LEFT_FOOT;  break;
    case JCE_HUMANOID_LIMB_RIGHT_LEG:
        u = JCE_HB_RIGHT_UPPER_LEG; l = JCE_HB_RIGHT_LOWER_LEG; e = JCE_HB_RIGHT_FOOT; break;
    default: return false;
    }
    if (out_upper) *out_upper = u;
    if (out_lower) *out_lower = l;
    if (out_end)   *out_end   = e;
    return true;
}

/* Model-space transform of one joint under the POSE in `local`.
 *
 * Not rest_model_pos: that one composes the REST rotations, which is the
 * right answer for "how long is this bone" and the wrong one for "where is
 * the hand right now".  Both walks are the same shape and they are kept
 * apart on purpose -- a solver that measured the posed limb with rest
 * rotations would converge on the rest pose no matter what it was handed. */
static void posed_model_xform(const JceSkeleton *skel, const jce_quat *local,
                              uint32_t joint, jce_vec3 *out_pos,
                              jce_quat *out_rot)
{
    const jce_vec3 *t = NULL, *s = NULL;
    const jce_quat *r = NULL;
    jce_skeleton_rest_trs(skel, &t, &r, &s);

    int chain[64];
    int n = 0;
    int cur = (int)joint;
    while (cur >= 0 && n < 64) {
        chain[n++] = cur;
        cur = jce_skeleton_joint_parent(skel, (uint32_t)cur);
    }

    jce_quat rot = jce_q_identity();
    jce_vec3 pos = jce_v3(0.0f, 0.0f, 0.0f);
    for (int i = n - 1; i >= 0; i--) {
        const int j = chain[i];
        const jce_vec3 off = t ? jce_q_rotate(rot, t[j]) : jce_v3(0.0f, 0.0f, 0.0f);
        pos.x += off.x; pos.y += off.y; pos.z += off.z;
        rot = jce_q_multiply(rot, local ? local[j] : (r ? r[j] : jce_q_identity()));
    }
    if (out_pos) *out_pos = pos;
    if (out_rot) *out_rot = rot;
}

/* Model-space rotation of a joint's PARENT, identity at the root. */
static jce_quat posed_parent_rot(const JceSkeleton *skel, const jce_quat *local,
                                 uint32_t joint)
{
    const int p = jce_skeleton_joint_parent(skel, joint);
    if (p < 0) return jce_q_identity();
    jce_quat rot;
    posed_model_xform(skel, local, (uint32_t)p, NULL, &rot);
    return rot;
}

/* Apply a MODEL-space delta to a joint by editing its LOCAL rotation.
 *
 *   model_new = delta * model_old,  model = parent_model * local
 *   => local_new = inverse(parent_model) * delta * parent_model * local_old
 *
 * q_inv is this file's unit-quaternion inverse (the conjugate); every
 * quaternion reaching it came from jce_q_normalize or from a product of unit
 * quaternions, which is the precondition it states. */
static void apply_model_delta(const JceSkeleton *skel, jce_quat *local,
                              uint32_t joint, jce_quat delta)
{
    const jce_quat pm = posed_parent_rot(skel, local, joint);
    const jce_quat pm_inv = q_inv(pm);   /* unit: inverse IS the conjugate */
    const jce_quat m = jce_q_multiply(pm, local[joint]);
    local[joint] = jce_q_normalize(jce_q_multiply(pm_inv, jce_q_multiply(delta, m)));
}

/* The rotation taking unit vector `from` to unit vector `to`. */
static jce_quat rot_between(jce_vec3 from, jce_vec3 to)
{
    const float d = jce_v3_dot(from, to);
    if (d > 0.99999f) return jce_q_identity();
    if (d < -0.99999f) {
        /* Opposite: any perpendicular axis is a half turn.  Pick one that is
         * not parallel to `from` rather than a fixed axis, which would be
         * degenerate for exactly one input. */
        jce_vec3 axis = jce_v3_cross(from, jce_v3(1.0f, 0.0f, 0.0f));
        if (jce_v3_len(axis) < 1e-4f) axis = jce_v3_cross(from, jce_v3(0.0f, 1.0f, 0.0f));
        return jce_q_from_axis_angle(jce_v3_normalize(axis), 3.14159265358979f);
    }
    const jce_vec3 axis = jce_v3_normalize(jce_v3_cross(from, to));
    return jce_q_from_axis_angle(axis, acosf(d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d)));
}

static float safe_acos(float x)
{
    if (x <= -1.0f) return 3.14159265358979f;
    if (x >=  1.0f) return 0.0f;
    return acosf(x);
}

bool jce_humanoid_role_model_xform(const JceHumanoidMap *map,
                                   const JceSkeleton    *skel,
                                   const jce_quat       *local,
                                   JceHumanoidBone       role,
                                   jce_vec3             *out_pos,
                                   jce_quat             *out_rot)
{
    if (!map || !skel || !local) return false;
    if ((int)role < 0 || (int)role >= JCE_HB_COUNT) return false;
    const int32_t j = map->joint[role];
    if (j < 0 || (uint32_t)j >= jce_skeleton_joint_count(skel)) return false;
    posed_model_xform(skel, local, (uint32_t)j, out_pos, out_rot);
    return true;
}

bool jce_humanoid_ik_two_bone(const JceHumanoidMap *map,
                              const JceSkeleton    *skel,
                              JceHumanoidLimb       limb,
                              jce_vec3              target_model,
                              const jce_vec3       *pole_model,
                              float                 weight,
                              jce_quat             *io_local)
{
    JceHumanoidBone ub, lb, eb;
    if (!map || !skel || !io_local) return false;
    if (!jce_humanoid_limb_bones(limb, &ub, &lb, &eb)) return false;

    const int uj = map->joint[ub], lj = map->joint[lb], ej = map->joint[eb];
    if (uj < 0 || lj < 0 || ej < 0) return false;

    /* THE TRIPLE HAS TO BE A PARENT CHAIN, and on real rigs it sometimes is
     * not.  This solver is analytic: it rotates upper about its own origin and
     * lower about the mid joint, which only moves the end joint at all if the
     * end is a descendant of the mid and the mid of the upper.
     *
     * The rig that found this is PSX_BagMan, and its shape is the ordinary
     * Blender one: FOOT.L is an IK CONTROL bone parented to the root, not a
     * child of SHIN.L.  The role matcher is right to call it the left foot --
     * it is the joint the shoe is skinned to -- and the chain assumption is
     * still violated.  Solving anyway rotated the leg while the foot stayed
     * where the control bone put it, which on screen is a shoe detached from
     * its own leg.  Nothing reported an error; the pose was simply wrong.
     *
     * Refusing leaves the retargeted pose exactly as it was, which is the same
     * contract the unreachable-target and missing-role cases already keep. */
    if (jce_skeleton_joint_parent(skel, (uint32_t)ej) != lj ||
        jce_skeleton_joint_parent(skel, (uint32_t)lj) != uj)
        return false;

    /* weight 0 is "do nothing", and it RETURNS TRUE: the limb was solvable,
     * the caller asked for none of it.  Returning false would make "blend is
     * at zero" indistinguishable from "this rig has no arm". */
    if (weight <= 0.0f) return true;
    if (weight > 1.0f) weight = 1.0f;

    const uint32_t n = jce_skeleton_joint_count(skel);
    if ((uint32_t)uj >= n || (uint32_t)lj >= n || (uint32_t)ej >= n) return false;

    /* Keep the incoming pose: the blend at the end is against THIS, not
     * against rest, so weight 0.5 is half of the correction rather than half
     * way to a pose nobody asked for. */
    const jce_quat u_in = io_local[uj];
    const jce_quat l_in = io_local[lj];

    jce_vec3 A, B, C;
    posed_model_xform(skel, io_local, (uint32_t)uj, &A, NULL);
    posed_model_xform(skel, io_local, (uint32_t)lj, &B, NULL);
    posed_model_xform(skel, io_local, (uint32_t)ej, &C, NULL);

    const float L1 = jce_v3_len(jce_v3_sub(B, A));
    const float L2 = jce_v3_len(jce_v3_sub(C, B));
    if (L1 < 1e-5f || L2 < 1e-5f) return false;   /* nothing to solve about */

    jce_vec3 toTarget = jce_v3_sub(target_model, A);
    float d = jce_v3_len(toTarget);
    if (d < 1e-5f) return false;                  /* target sits on the root */
    const jce_vec3 dir = jce_v3_scale(toTarget, 1.0f / d);

    /* CLAMP the reach rather than refuse it.  The limits are the triangle
     * inequality's: past L1+L2 the limb is straight, inside |L1-L2| it is
     * folded as far as it goes.  The epsilons keep acos off its endpoints,
     * where the derivative is infinite and a solved angle jitters. */
    const float dmax = L1 + L2 - 1e-4f;
    const float dmin = fabsf(L1 - L2) + 1e-4f;
    if (d > dmax) d = dmax;
    if (d < dmin) d = dmin;

    /* The plane the limb bends in.  From the CURRENT pose, so a solve that
     * only moves the target keeps the elbow where the animator put it; the
     * pole hint is for the case that has no plane of its own -- a straight
     * limb, where cross() is zero and any plane would do. */
    jce_vec3 axis = jce_v3_cross(jce_v3_sub(B, A), jce_v3_sub(C, A));
    if (jce_v3_len(axis) < 1e-5f) {
        /* A STRAIGHT LIMB HAS NO PLANE OF ITS OWN, and the plane to invent
         * has to contain the LIMB as well as the target -- an axis chosen
         * perpendicular to the target direction alone can come out PARALLEL
         * to the limb, and rotating a bone about its own length does not
         * bend it.  Measured: an arm hanging -Y with the target straight out
         * +X produced axis (0,-1,0) and the hand missed by 0.25 of a 1.0
         * reach, on that one direction out of eight. */
        const jce_vec3 limb = jce_v3_sub(C, A);
        axis = jce_v3_cross(limb, dir);
        if (jce_v3_len(axis) < 1e-5f && pole_model)
            axis = jce_v3_cross(limb, jce_v3_sub(*pole_model, A));
        /* Target along the limb: every plane through it is as good as any
         * other, so take the first perpendicular that is not degenerate. */
        if (jce_v3_len(axis) < 1e-5f)
            axis = jce_v3_cross(limb, jce_v3(0.0f, 0.0f, 1.0f));
        if (jce_v3_len(axis) < 1e-5f)
            axis = jce_v3_cross(limb, jce_v3(0.0f, 1.0f, 0.0f));
        if (jce_v3_len(axis) < 1e-5f)
            axis = jce_v3_cross(limb, jce_v3(1.0f, 0.0f, 0.0f));
        if (jce_v3_len(axis) < 1e-5f) return false;
    }
    axis = jce_v3_normalize(axis);
    /* The pole is NOT folded into this axis.  It was, and it only reached the
     * cases where the limb was already bent -- a straight limb took the
     * fallback and the hint was never consulted, so two opposite poles put
     * the elbow in the same place.  It is applied as an explicit roll after
     * the aim instead, below, where "which side is the elbow on" is a
     * question that can be asked and answered rather than encoded in the
     * sign of a cross product. */

    /* Law of cosines, both interior angles. */
    const float cur_a = safe_acos(jce_v3_dot(
        jce_v3_normalize(jce_v3_sub(B, A)), jce_v3_normalize(jce_v3_sub(C, A))));
    const float cur_b = safe_acos(jce_v3_dot(
        jce_v3_normalize(jce_v3_sub(A, B)), jce_v3_normalize(jce_v3_sub(C, B))));
    const float want_a = safe_acos((L1 * L1 + d * d - L2 * L2) / (2.0f * L1 * d));
    const float want_b = safe_acos((L1 * L1 + L2 * L2 - d * d) / (2.0f * L1 * L2));

    /* Bend first, aim second.  The other order aims a limb whose end is about
     * to move, so the aim is stale by exactly the bend. */
    apply_model_delta(skel, io_local, (uint32_t)uj,
                      jce_q_from_axis_angle(axis, want_a - cur_a));
    apply_model_delta(skel, io_local, (uint32_t)lj,
                      jce_q_from_axis_angle(axis, want_b - cur_b));

    posed_model_xform(skel, io_local, (uint32_t)ej, &C, NULL);
    const jce_vec3 cur_dir = jce_v3_sub(C, A);
    if (jce_v3_len(cur_dir) >= 1e-5f) {
        apply_model_delta(skel, io_local, (uint32_t)uj,
                          rot_between(jce_v3_normalize(cur_dir), dir));
    }

    /* THE POLE, as a roll about the line the limb now lies along.
     *
     * The interior angles are already right and the end effector is already
     * on the target; spinning the whole limb about A->target changes neither,
     * and it is the only remaining freedom -- which is exactly the freedom a
     * pole is for.  Both the elbow and the pole are projected onto the plane
     * perpendicular to that line and the elbow is turned onto the pole's
     * side.  A projection that collapses means the thing sits ON the axis and
     * names no side, so there is nothing to turn toward and the pose stays. */
    if (pole_model) {
        posed_model_xform(skel, io_local, (uint32_t)lj, &B, NULL);
        const jce_vec3 eb = jce_v3_sub(B, A);
        const jce_vec3 pb = jce_v3_sub(*pole_model, A);
        const jce_vec3 e_perp = jce_v3_sub(eb, jce_v3_scale(dir, jce_v3_dot(eb, dir)));
        const jce_vec3 p_perp = jce_v3_sub(pb, jce_v3_scale(dir, jce_v3_dot(pb, dir)));
        if (jce_v3_len(e_perp) >= 1e-5f && jce_v3_len(p_perp) >= 1e-5f) {
            apply_model_delta(skel, io_local, (uint32_t)uj,
                              rot_between(jce_v3_normalize(e_perp),
                                          jce_v3_normalize(p_perp)));
        }
    }

    if (weight < 1.0f) {
        io_local[uj] = jce_q_normalize(jce_q_slerp(u_in, io_local[uj], weight));
        io_local[lj] = jce_q_normalize(jce_q_slerp(l_in, io_local[lj], weight));
    }
    return true;
}
