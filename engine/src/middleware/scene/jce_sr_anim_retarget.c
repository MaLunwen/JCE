/*
 * jce_sr_anim_retarget.c -- see the header for why this is not in jce_sr_anim.c.
 */

#include "jce_sr_anim_retarget.h"

#include "os/core/jce_memory.h"

#include <string.h>
#include <stdio.h>
#include <jce/middleware/animation/jce_humanoid.h> /* effector preservation */

#define LOG_TAG "scene_renderer"

/* ── Animation retargeting (optional per-instance path) ──────────────
 *
 * Play a clip authored for a DIFFERENT (source) skeleton on the entity's own
 * (dst) skeleton.  Self-contained: samples the active clip against the SOURCE
 * rig's rest TRS (clip channels are indexed for the source skeleton), transfers
 * the pose onto the dst skeleton via the cached bind-relative retarget map, then
 * evaluates the dst skeleton into the instance's skin palette.  The map is
 * (re)built when either skeleton pointer or the source path changes.
 *
 * Uses the player purely as a playhead CLOCK (advance + get_time): its own
 * sampled palette — bound to the dst skeleton — is discarded.  This keeps the
 * editor timeline/progress query (jce_scene_renderer_get_anim_player) live while
 * the actual deformation comes from the retargeted dst locals.
 *
 * Returns true if it handled the instance (caller must skip the legacy path);
 * false to fall through to legacy playback (no/invalid retarget source). */
bool sr_anim_try_retarget(JceSceneRenderer *sr,
                                 JceScene *scene, JceEntity e,
                                 JceSkeletalAnimatorComponent *sa,
                                 SrModelCache *dst_mc, SrAnimInstance *ai,
                                 float dt_sec)
{
    (void)scene;   /* entity resolves through sr_get_model (path-keyed cache) */
    /* Gate: a non-empty source DIFFERENT from the entity's own skeleton. Empty
       or identical ⇒ legacy path (byte-identical). */
    if (!sa->retarget_source_skeleton[0]) return false;
    if (strcmp(sa->retarget_source_skeleton, sa->skeleton_path) == 0) return false;

    /* Resolve (path-keyed, cached) the SOURCE model holding the source rig +
       the clip authored for it. Pending/failed ⇒ skip this frame, but still
       claim the instance so we don't fall back to a mismatched legacy sample. */
    SrModelCache *src_mc = sr_get_model(sr, sa->retarget_source_skeleton, (uint32_t)e);
    if (!src_mc || !src_mc->model) return true;

    JceSkeleton *src_skel = jce_model_get_skeleton(src_mc->model);
    JceSkeleton *dst_skel = jce_model_get_skeleton(dst_mc->model);
    if (!src_skel || !dst_skel) return true;

    /* The active clip lives in the SOURCE model (it is authored for the source
       rig). Resolve it by the same active_clip index used for legacy playback. */
    int ac = sa->active_clip;
    uint32_t src_anim_n = jce_model_anim_count(src_mc->model);
    if (ac < 0 || ac >= (int)src_anim_n) return true;   /* nothing to play */
    JceAnimClip *clip = jce_model_get_anim(src_mc->model, (uint32_t)ac);
    if (!clip) return true;

    /* (Re)build the retarget map when the source path or either skeleton
       pointer changed (model reloads reuse the cache slot, new pointer). */
    if (!ai->retarget_map ||
        ai->retarget_src_skel != src_skel ||
        ai->retarget_dst_skel != dst_skel ||
        strcmp(ai->retarget_src, sa->retarget_source_skeleton) != 0) {
        if (ai->retarget_map) jce_anim_retarget_map_destroy(ai->retarget_map);
        ai->retarget_map = jce_anim_retarget_map_create(src_skel, dst_skel);
        ai->retarget_src_skel = src_skel;
        ai->retarget_dst_skel = dst_skel;
        ai->humanoid_valid = false;   /* rebuild the role maps with it */
        snprintf(ai->retarget_src, sizeof(ai->retarget_src), "%s",
                 sa->retarget_source_skeleton);
    }
    if (!ai->retarget_map) return true;   /* map build failed; don't mis-sample */

    /* ROLE MAPS, built beside the name map and rebuilt on the same trigger.
     *
     * The name map covers rigs that share a naming convention and covers
     * NOTHING else: two humanoid rigs from different tools share no bone
     * names, so it maps zero joints, every bone falls back to the destination
     * rest pose, and the clip visibly does nothing.  That is exactly the case
     * Unity's Humanoid exists for.
     *
     * WHICH ONE WINS IS MEASURED.  Whichever covers more joints is used, and a
     * TIE GOES TO THE NAME MAP -- so every rig pair that matched by name
     * before takes exactly the path it took and produces exactly the pose it
     * produced.  A fixed threshold would have been a number with no reason
     * behind it, and would have moved existing content the day it was picked
     * wrong. */
    if (!ai->humanoid_valid) {
        const bool ok_src = jce_humanoid_map_build(src_skel, &ai->humanoid_src);
        const bool ok_dst = jce_humanoid_map_build(dst_skel, &ai->humanoid_dst);
        ai->humanoid_valid = true;   /* attempted; do not retry every frame */
        uint32_t common = 0;
        if (ok_src && ok_dst) {
            for (int b = 0; b < JCE_HB_COUNT; b++)
                if (ai->humanoid_src.joint[b] >= 0 &&
                    ai->humanoid_dst.joint[b] >= 0) common++;
        }
        const uint32_t by_name = jce_anim_retarget_mapped_count(ai->retarget_map);
        ai->humanoid_wins = (common > by_name);
        if (ai->humanoid_wins) {
            LOG_INFO(LOG_TAG,
                     "retarget: using the HUMANOID role map (%u roles in "
                     "common) over the name map (%u joints matched) for '%s'",
                     (unsigned)common, (unsigned)by_name,
                     sa->retarget_source_skeleton);
        }
    }

    /* Advance the playhead CLOCK with the dst-bound player (palette ignored).
       Honor the component's playing/loop/speed like the legacy single-clip path
       so the timeline behaves identically. */
    float sp = (sa->speed > 0.0f ? sa->speed : 1.0f);
    bool  loop_eff = sa->loop;
    bool  clip_changed = (ai->active_clip != ac);
    if (sa->playing) {
        if (!jce_anim_player_is_playing(ai->player) || clip_changed ||
            ai->loop != loop_eff) {
            jce_anim_player_play(ai->player, clip, loop_eff, sp);
        }
        jce_anim_player_pause(ai->player, false);
        jce_anim_player_set_speed(ai->player, sp);
        jce_anim_player_update(ai->player, dt_sec, NULL, 0);  /* advance only */
    } else {
        if (clip_changed || ai->loop != loop_eff) {
            jce_anim_player_play(ai->player, clip, loop_eff, sp);
            jce_anim_player_set_time(ai->player, 0.0f);
        }
        if (jce_anim_player_is_playing(ai->player))
            jce_anim_player_pause(ai->player, true);
    }
    ai->active_clip = ac;
    ai->loop  = loop_eff;
    ai->speed = sp;
    ai->paused = !sa->playing;

    /* Sample the clip against the SOURCE skeleton's rest TRS into a SOURCE-sized
       locals buffer (clip channels are indexed for the source rig). */
    uint32_t src_n = jce_skeleton_joint_count(src_skel);
    uint32_t dst_n = jce_skeleton_joint_count(dst_skel);
    if (src_n == 0 || dst_n == 0 || src_n > JCE_MAX_BONES || dst_n > JCE_MAX_BONES)
        return true;

    const jce_vec3 *rest_t = NULL; const jce_quat *rest_r = NULL; const jce_vec3 *rest_s = NULL;
    jce_skeleton_rest_trs(src_skel, &rest_t, &rest_r, &rest_s);

    /* Seed source locals with the source rest pose so joints untouched by the
       clip carry the source bind (the retargeter then maps bind→dst bind). */
    jce_mat4 *src_locals = (jce_mat4 *)JCE_MALLOC(
        (size_t)(src_n + dst_n) * sizeof(jce_mat4));
    if (!src_locals) return true;
    jce_mat4 *dst_locals = src_locals + src_n;
    const jce_mat4 *src_rest = jce_skeleton_rest_pose(src_skel);
    if (src_rest) memcpy(src_locals, src_rest, (size_t)src_n * sizeof(jce_mat4));
    else for (uint32_t j = 0; j < src_n; j++) src_locals[j] = jce_m4_identity();

    float t = jce_anim_player_get_time(ai->player);
    jce_anim_clip_sample(clip, t, src_locals, src_n, rest_t, rest_r, rest_s);

    /* Transfer the source pose onto the dst skeleton, then evaluate. */
    if (ai->humanoid_wins) {
        /* THE ROLE PATH.  jce_humanoid_retarget speaks in local ROTATIONS
         * (the same form jce_skeleton_evaluate consumes), so the mat4 locals
         * either side of it are decomposed and recomposed here rather than in
         * the retargeter -- which keeps that function about the transfer and
         * not about a matrix layout it has no reason to know.
         *
         * The destination's TRANSLATION and SCALE are its own, taken from its
         * rest pose: limb lengths belong to the target rig.  Only the hips'
         * translation crosses, scaled by the two rigs' hips heights, so a
         * short character takes the same stride relative to its own size. */
        jce_quat *qs = (jce_quat *)JCE_MALLOC(
            (size_t)(src_n + dst_n) * sizeof(jce_quat));
        if (qs) {
            jce_quat *src_q = qs, *dst_q = qs + src_n;
            for (uint32_t j = 0; j < src_n; j++) {
                jce_vec3 tt, ss;
                jce_m4_decompose(&src_locals[j], &tt, &src_q[j], &ss);
            }
            jce_humanoid_rest_locals(dst_skel, dst_q);

            /* The hips' local translation from the sampled source pose, handed
             * in and scaled in place. */
            jce_vec3 root_t = jce_v3(0.0f, 0.0f, 0.0f);
            const int32_t src_hips = ai->humanoid_src.joint[JCE_HB_HIPS];
            if (src_hips >= 0 && (uint32_t)src_hips < src_n) {
                jce_quat rq; jce_vec3 rs;
                jce_m4_decompose(&src_locals[src_hips], &root_t, &rq, &rs);
            }
            /* CLAMPED, and the role path is the only path that can be.
             * The name map transfers between rigs that share a naming
             * convention, which in practice means rigs from one tool with one
             * bone-roll convention; the role map is what crosses tools, and
             * crossing tools is where a shin arrives twisted 120 degrees
             * about its own length because the two rigs roll their bones the
             * other way.  Limits belong where that happens.
             *
             * Logged ONCE per (instance, clip) rather than per frame: "my
             * character's knee stopped inverting" deserves a line, and 60 of
             * them a second deserves none. */
            uint32_t n_clamped = 0;
            (void)jce_humanoid_retarget_clamped(
                &ai->humanoid_src, src_skel, src_q,
                &ai->humanoid_dst, dst_skel, dst_q,
                &root_t, true, &n_clamped);
            if (n_clamped > 0 && !ai->humanoid_clamp_logged) {
                ai->humanoid_clamp_logged = true;
                LOG_INFO(LOG_TAG,
                         "humanoid retarget: %u role(s) clamped into the "
                         "muscle range on this clip -- the source pushed a "
                         "bone further from its rest than that role allows, "
                         "which is what crossing two rigs' bone-roll "
                         "conventions looks like",
                         n_clamped);
            }

            /* TWIST REDISTRIBUTION, opt-in, and BEFORE the effector IK.
             *
             * It is a pure redistribution -- the end joint does not move, only
             * where the twist is expressed along the limb -- so the order
             * would not matter for the pose.  It matters for the IK: moving
             * twist onto the lower bone changes that bone's rotation, and the
             * solver overwrites the upper and lower rotations outright.  Run
             * afterwards it would be discarded on exactly the limbs the IK
             * touched, which is the half-wired shape that reads as "the
             * setting does nothing on the arms". */
            if (sa->retarget_twist > 0.0f)
                (void)jce_humanoid_redistribute_twist(&ai->humanoid_dst,
                                                      dst_skel,
                                                      sa->retarget_twist,
                                                      dst_q);

            /* EFFECTOR PRESERVATION, opt-in (JceSkeletalAnimatorComponent
             * .retarget_effector_ik; Unity calls it the IK Pass).
             *
             * The transfer above carries ROTATIONS, which is right -- limb
             * lengths belong to the target rig -- and the cost is that
             * identical shoulder and elbow angles put the hand somewhere else
             * on a rig with a longer forearm.  A clip authored with a hand on
             * a railing has it through the railing.  So: ask where the SOURCE
             * rig's effector actually ended up, express that RELATIVE TO THE
             * HIPS and scaled by the two rigs' hips heights (the same quantity
             * the root translation is already scaled by, and for the same
             * reason -- a short character reaches the same place relative to
             * its own size), and IK the destination's limb back onto it.
             *
             * This is the caller jce_humanoid_ik_two_bone was written for: it
             * speaks local rotations, which is exactly the form dst_q is in at
             * this point.  Doing it after the clamp and before the recompose
             * means the muscle limits shape the pose the IK then corrects,
             * rather than the IK being clamped away.
             *
             * A limb missing from either rig, or a rig with no hips, is
             * skipped -- `ok` is ignored deliberately: a limb that cannot be
             * solved must leave the retargeted pose exactly as it was, which
             * is what the solver does on failure. */
            if (sa->retarget_effector_ik &&
                ai->humanoid_src.hips_height > 1e-4f &&
                ai->humanoid_dst.hips_height > 1e-4f) {
                const float k = ai->humanoid_dst.hips_height /
                                ai->humanoid_src.hips_height;
                jce_vec3 hs, hd;
                jce_quat qs_hips, qd_hips;
                if (jce_humanoid_role_model_xform(&ai->humanoid_src, src_skel,
                                                  src_q, JCE_HB_HIPS,
                                                  &hs, &qs_hips) &&
                    jce_humanoid_role_model_xform(&ai->humanoid_dst, dst_skel,
                                                  dst_q, JCE_HB_HIPS,
                                                  &hd, &qd_hips)) {
                    /* The offset crosses a FRAME, not just a scale.  glTF is
                     * Y-up by convention and exporters ship Z-up rigs under a
                     * converting root node, so "0.6 m above the hips" on the
                     * source lands 0.6 m FORWARD on the destination if the
                     * vector is copied raw -- measured, and it folds the legs
                     * up and detaches the feet.  Express the offset in the
                     * SOURCE hips' posed frame and re-express it in the
                     * DESTINATION's, which is the frame the retarget has
                     * already put into correspondence. */
                    const jce_quat qs_inv = jce_v4(-qs_hips.x, -qs_hips.y,
                                                   -qs_hips.z,  qs_hips.w);
                    static const JceHumanoidBone kEnd[JCE_HUMANOID_LIMB_COUNT] = {
                        JCE_HB_LEFT_HAND, JCE_HB_RIGHT_HAND,
                        JCE_HB_LEFT_FOOT, JCE_HB_RIGHT_FOOT,
                    };
                    for (int li = 0; li < JCE_HUMANOID_LIMB_COUNT; li++) {
                        jce_vec3 es;
                        if (!jce_humanoid_role_model_xform(&ai->humanoid_src,
                                                           src_skel, src_q,
                                                           kEnd[li], &es, NULL))
                            continue;
                        const jce_vec3 rel_hips =
                            jce_q_rotate(qs_inv, jce_v3_sub(es, hs));
                        const jce_vec3 tgt =
                            jce_v3_add(hd, jce_q_rotate(qd_hips,
                                          jce_v3_scale(rel_hips, k)));
                        (void)jce_humanoid_ik_two_bone(
                            &ai->humanoid_dst, dst_skel,
                            (JceHumanoidLimb)li, tgt, NULL, 1.0f, dst_q);
                    }
                }
            }

            /* Recompose: dst rest translation and scale, retargeted rotation --
             * except the hips, which take the scaled root translation. */
            const jce_vec3 *dt = NULL; const jce_quat *dr = NULL;
            const jce_vec3 *ds = NULL;
            jce_skeleton_rest_trs(dst_skel, &dt, &dr, &ds);
            const int32_t dst_hips = ai->humanoid_dst.joint[JCE_HB_HIPS];
            for (uint32_t j = 0; j < dst_n; j++) {
                jce_vec3 tt = dt ? dt[j] : jce_v3(0.0f, 0.0f, 0.0f);
                jce_vec3 sc = ds ? ds[j] : jce_v3(1.0f, 1.0f, 1.0f);
                if ((int32_t)j == dst_hips) tt = root_t;
                dst_locals[j] = jce_m4_from_trs(tt, dst_q[j], sc);
            }
            JCE_FREE(qs);
        } else {
            /* Out of memory: the name map is still correct, just emptier.
             * Better a rest pose than a half-written one. */
            jce_anim_retarget_pose(ai->retarget_map, src_locals, dst_locals);
        }
    } else {
        jce_anim_retarget_pose(ai->retarget_map, src_locals, dst_locals);
    }
    jce_skeleton_evaluate(dst_skel, dst_locals, ai->skin_palette, JCE_MAX_BONES);
    ai->skin_palette_count = dst_n;

    JCE_FREE(src_locals);
    return true;
}
