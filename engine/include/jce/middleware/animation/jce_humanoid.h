/*
 * jce_humanoid.h  Humanoid rig: a common bone vocabulary, and retargeting.
 *
 * Unity's Humanoid avatars, UE's IK Retargeter.  The problem it solves: a
 * walk cycle authored on one skeleton will not play on another, because a
 * clip addresses JOINTS BY INDEX and two rigs agree on neither the count nor
 * the order nor the names.  This engine has four rigged models and they are
 * named in four different conventions -- `spine_01` / `SPINE` /
 * `torso_joint_2`, `upperarm_l` / `UPPER ARM.L` / `arm_joint_L_1` -- which is
 * the ordinary state of a project that took models from more than one place.
 *
 * WHAT A MAP IS.  A JceHumanoidMap says, for one skeleton, which joint plays
 * each of the humanoid roles below, and what that joint's REST rotation is in
 * model space.  The rest rotations are what make retargeting independent of
 * proportion: an animation is transported as the rotation it ADDS to the rest
 * pose, not as an absolute orientation, so a short character reproduces a tall
 * character's motion without inheriting its limb lengths.
 *
 * WHAT IS HERE NOW, and this paragraph used to say the opposite.  Until
 * 2026-09-17 it read "no IK, no muscle-space clamping, no per-bone twist
 * redistribution", and it was right when it was written.  All three landed,
 * and a header that still advertises an absence is a worse lie than one that
 * never mentioned it -- a reader takes it as current.  Today:
 *
 *   - muscle-space clamping           jce_humanoid_clamp_delta
 *   - two-bone IK, by humanoid limb   jce_humanoid_ik_two_bone
 *   - twist redistribution            jce_humanoid_redistribute_twist
 *   - hinge DIRECTION                 jce_humanoid_clamp_{knee,elbow}_direction
 *
 * WHAT IS STILL NOT HERE: an elbow whose rig leaves NO rest bend cannot have
 * its hinge plane derived, and that function refuses rather than guessing.
 * PSX_BagMan in this tree is such a rig; CesiumMan is not.  Saying so is the
 * point -- a retarget that silently slid the feet would be worse than one that
 * says what it does not do.
 */

#ifndef JCE_HUMANOID_H
#define JCE_HUMANOID_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceSkeleton JceSkeleton;

/* The bone vocabulary.  TWENTY-TWO, and the number is a decision.
 *
 * Unity names 55, of which 15 are fingers per hand and the rest optional.
 * Every one of them has to be RECOGNISED from a name in some rig or it is
 * dead weight, and a map that claims a bone it guessed wrong is worse than one
 * that leaves it unmapped: the retarget would then drive a joint with another
 * joint's motion.  These 22 are the ones the four rigs in this tree actually
 * name, and they are the set Unity itself marks REQUIRED. */
typedef enum {
    JCE_HB_HIPS = 0,
    JCE_HB_SPINE,
    JCE_HB_CHEST,
    JCE_HB_UPPER_CHEST,
    JCE_HB_NECK,
    JCE_HB_HEAD,
    JCE_HB_LEFT_SHOULDER,
    JCE_HB_LEFT_UPPER_ARM,
    JCE_HB_LEFT_LOWER_ARM,
    JCE_HB_LEFT_HAND,
    JCE_HB_RIGHT_SHOULDER,
    JCE_HB_RIGHT_UPPER_ARM,
    JCE_HB_RIGHT_LOWER_ARM,
    JCE_HB_RIGHT_HAND,
    JCE_HB_LEFT_UPPER_LEG,
    JCE_HB_LEFT_LOWER_LEG,
    JCE_HB_LEFT_FOOT,
    JCE_HB_LEFT_TOES,
    JCE_HB_RIGHT_UPPER_LEG,
    JCE_HB_RIGHT_LOWER_LEG,
    JCE_HB_RIGHT_FOOT,
    JCE_HB_RIGHT_TOES,
    JCE_HB_COUNT
} JceHumanoidBone;

/* The mapping for ONE skeleton. */
typedef struct {
    /* joint[b] = index into the skeleton, or -1 when this rig has no such
     * bone.  A rig with no toes is normal and is not an error. */
    int32_t  joint[JCE_HB_COUNT];
    /* The mapped joint's REST rotation in MODEL space.  Stored rather than
     * recomputed because the retarget needs it per frame and it never
     * changes. */
    jce_quat rest_model[JCE_HB_COUNT];
    /* Rest model-space height of the hips, for scaling root translation
     * between rigs of different size.  0 when hips are unmapped. */
    float    hips_height;
    uint32_t mapped_count;
} JceHumanoidMap;

/* The humanoid role's name, for a panel or a log.  Never NULL. */
JCE_API const char *jce_humanoid_bone_name(JceHumanoidBone bone);

/* Which humanoid role, if any, a JOINT NAME denotes -- JCE_HB_COUNT for none.
 *
 * Name matching, and it is the practical half of this file.  It normalises
 * away case, separators and the numbering conventions the four rigs here use,
 * then requires BOTH a role keyword and (for paired bones) a side.  A name
 * that matches a role but no side is left unmapped rather than assigned to
 * one, because a left arm driven by a right arm's motion is worse than an arm
 * that does not move. */
JCE_API JceHumanoidBone jce_humanoid_bone_from_joint_name(const char *joint_name);

/* Build the map for `skel` by matching every joint name.
 *
 * Returns false only when `skel` or `out` is NULL; a skeleton that maps
 * NOTHING is a successful answer of "this is not a humanoid", which the caller
 * reads from out->mapped_count.  Later joints do not overwrite earlier ones:
 * a rig with both `spine_01` and `spine_02` keeps the first as SPINE and the
 * second falls to CHEST through the numbering rule, and two joints that
 * genuinely claim one role leave the first in place. */
JCE_API bool jce_humanoid_map_build(const JceSkeleton *skel, JceHumanoidMap *out);

/* Retarget one pose from `src` onto `dst`.
 *
 * `src_local` / `dst_local` are per-joint LOCAL rotations, the same form
 * jce_skeleton_evaluate consumes; `dst_local` must already hold the target's
 * rest local rotations (jce_humanoid_rest_locals fills them) because every
 * joint this map does not touch keeps its rest pose.
 *
 * For each role present in BOTH maps:
 *     delta        = inverse(src_rest_model) * src_pose_model
 *     wanted_model = dst_rest_model * delta
 *     dst_local    = inverse(parent_model) * wanted_model
 * so what transfers is the rotation the animation ADDED, never an absolute
 * orientation -- which is why the target's proportions and its rest pose are
 * its own.
 *
 * `root_translation` is optional and in/out: scaled by the ratio of the two
 * rigs' hips heights, so a small character takes the same STRIDE relative to
 * its own size rather than the source's absolute metres.
 *
 * Returns the number of roles actually transported. */
JCE_API uint32_t jce_humanoid_retarget(const JceHumanoidMap *src,
                                       const JceSkeleton    *src_skel,
                                       const jce_quat       *src_local,
                                       const JceHumanoidMap *dst,
                                       const JceSkeleton    *dst_skel,
                                       jce_quat             *dst_local,
                                       jce_vec3             *root_translation);

/* Fill `out_local` (dst_skel joint count entries) with the skeleton's REST
 * local rotations -- the starting pose jce_humanoid_retarget writes over. */
JCE_API void jce_humanoid_rest_locals(const JceSkeleton *skel,
                                      jce_quat          *out_local);

/* ================================================================== */
/* Muscle-space clamping                                               */
/* ================================================================== */

/* WHAT THIS IS FOR.  jce_humanoid_retarget transports the rotation the
 * animation ADDED, which is the right thing and is also unbounded: a source
 * rig whose shoulder the animator swung 150 degrees hands the target 150
 * degrees, and a target whose rest pose already leans the other way ends up
 * somewhere no joint goes.  Nothing in the transfer knows that a knee has a
 * front.  Unity's Humanoid solves this by expressing every bone's rotation in
 * a normalised "muscle" range and clamping it; this is that, in the form this
 * engine can state without a configured avatar.
 *
 * THE LIMITS ARE ON THE DELTA, NOT THE POSE.  "How far may an animation push
 * this bone AWAY FROM ITS OWN REST" is rig-independent; "where may this bone
 * point" is not, because it depends on how the artist built the rest pose.
 * The first is what a retarget needs bounded and what these numbers are.
 *
 * Decomposed swing/twist about the bone's OWN axis (jce_humanoid_bone_axis):
 *   twist  signed roll about that axis, clamped to [twist_min, twist_max]
 *   swing  the remainder -- a cone half-angle, magnitude clamped to swing_max
 *
 * WHAT THIS IS NOT.  A cone cannot say "a knee bends forward and not back":
 * that needs a hinge AXIS, and a hinge axis is not derivable from a rest pose
 * where the leg is straight (the cross product of two collinear bones is
 * zero).  Unity gets it from a configured T-pose; until this engine has that
 * configuration step, hinge DIRECTION is not clamped and this header says so
 * rather than letting a reader assume it. */
typedef struct {
    float twist_min;   /* degrees, <= 0  */
    float twist_max;   /* degrees, >= 0  */
    float swing_max;   /* degrees, >= 0; the cone half-angle */
} JceHumanoidMuscleLimits;

/* The limits for one role.  False (and `out` untouched) for JCE_HB_COUNT or
 * an out-of-range bone. */
JCE_API bool jce_humanoid_muscle_limits(JceHumanoidBone bone,
                                        JceHumanoidMuscleLimits *out);

/* The bone's own axis in MODEL space at rest: the direction from the joint
 * playing `bone` to the joint playing the role BELOW it (hips->spine,
 * upper arm->lower arm, lower leg->foot, ...).
 *
 * Model space because that is the space the retarget's delta lives in.  From
 * the rig rather than from a convention, because "which local axis runs down
 * the bone" is a different answer in every tool that exports a rig -- this
 * tree already carries Unreal, Blender and Khronos-sample conventions in four
 * models.
 *
 * False when the role is absent, has no child role in this map, or the two
 * joints coincide -- and the caller then has nothing to decompose about, so
 * it must leave the rotation alone rather than pick an axis. */
JCE_API bool jce_humanoid_bone_axis(const JceHumanoidMap *map,
                                    const JceSkeleton    *skel,
                                    JceHumanoidBone       bone,
                                    jce_vec3             *out_axis);

/* Clamp a DELTA rotation into `bone`'s muscle range about `axis` (which must
 * be unit length, from jce_humanoid_bone_axis).  Returns `delta` unchanged
 * when it is already inside the range -- exactly, not approximately, so a
 * clamp that is not doing anything cannot be mistaken for one that is. */
JCE_API jce_quat jce_humanoid_clamp_delta(JceHumanoidBone bone,
                                          jce_vec3        axis,
                                          jce_quat        delta);

/* jce_humanoid_retarget, with the clamp.
 *
 * SEPARATE ENTRY POINT, and the old one is byte-identical to what it was:
 * the transfer is one thing and the policy is another, existing content
 * animates exactly as it did, and a caller that wants the clamp says so.  The
 * runtime's retarget path passes true. */
JCE_API uint32_t jce_humanoid_retarget_clamped(const JceHumanoidMap *src,
                                               const JceSkeleton    *src_skel,
                                               const jce_quat       *src_local,
                                               const JceHumanoidMap *dst,
                                               const JceSkeleton    *dst_skel,
                                               jce_quat             *dst_local,
                                               jce_vec3             *root_translation,
                                               bool                  clamp_muscles,
                                               uint32_t             *out_clamped);

/* ── Two-bone IK ─────────────────────────────────────────────────────
 *
 * The four limbs a humanoid rig can solve analytically.  Each is (upper,
 * lower, end): upper arm / lower arm / hand, upper leg / lower leg / foot.
 * A rig missing any of the three cannot be solved and says so. */
/* A limb, for goals that name one instead of naming three joints.
 *
 * WHY THE DISTINCTION EXISTS.  JceIkConstraint (jce_scene.h) normally carries
 * three joint NAMES, which pins it to one rig: two humanoid rigs from
 * different tools share no bone names at all, so a goal authored on one covers
 * nothing on the other.  Kind 7 names a limb instead -- "LeftArm", "RightArm",
 * "LeftLeg", "RightLeg" in its root_bone field -- and the renderer resolves
 * the triple through this rig's JceHumanoidMap.  That is the difference
 * Unity's AvatarIKGoal exists for: "pin the left hand" has to work on a rig
 * the scene has never seen.
 *
 * A rig with no humanoid roles resolves to nothing and the constraint is
 * skipped, which is what keeps it inert rather than bending whichever joint
 * happens to sit at the index a guess would produce.
 *
 * The field's meaning is already kind-dependent over there (kinds 2/3/4 read
 * root_bone and target_entity and nothing else), so this follows the struct's
 * own convention rather than appending a field to a serialized public type. */
typedef enum {
    JCE_HUMANOID_LIMB_LEFT_ARM = 0,
    JCE_HUMANOID_LIMB_RIGHT_ARM,
    JCE_HUMANOID_LIMB_LEFT_LEG,
    JCE_HUMANOID_LIMB_RIGHT_LEG,
    JCE_HUMANOID_LIMB_COUNT
} JceHumanoidLimb;

/* Put `limb`'s end effector on `target_model`, editing `io_local` in place.
 *
 * WHY THIS EXISTS.  jce_humanoid_retarget transfers ROTATIONS, and that is
 * the right transfer -- limb lengths belong to the target rig.  The cost is
 * that the same shoulder and elbow angles put the hand somewhere else on a
 * rig whose forearm is longer, so a clip authored with a hand on a railing
 * has it through the railing or short of it.  This is the correction Unity's
 * Humanoid IK goals and UE's Two Bone IK node make.
 *
 * ANALYTIC: the law of cosines gives both interior angles in closed form, so
 * there is no iteration count and no convergence to fail.
 *
 *   map, skel   the DESTINATION rig and its humanoid map
 *   io_local    per-joint LOCAL rotations, [jce_skeleton_joint_count]; read
 *               for the current pose and written for the solved one.  The
 *               pose the retarget produced is exactly what to pass.
 *   target_model  where the end effector should be, in MODEL space -- the
 *               same space jce_humanoid_bone_axis speaks.
 *   pole_model  optional hint for which way the joint bends, in model space;
 *               NULL keeps the pose's own bend plane.  A straight limb has no
 *               plane of its own, which is the case the hint is for.
 *   weight      0 leaves io_local untouched and returns true; 1 is the full
 *               solve; between them slerps each joint toward it, so a caller
 *               can blend IK in over a few frames rather than snapping.
 *
 * UNREACHABLE TARGETS ARE CLAMPED, NOT REFUSED: a target beyond upper+lower
 * reach makes the limb point at it straight, which is what every solver does
 * and what an animator expects to see.  Returning false there would leave the
 * pose untouched and look like the IK was not running.
 *
 * False -- and io_local untouched -- when an argument is NULL, the limb is
 * not mapped in this rig, or the limb is degenerate (a zero-length bone).
 * Those are "cannot solve", not "solved to nothing". */
/* Where a role's joint sits in MODEL space under a given set of local
 * rotations, and which way it faces.  Either output may be NULL.  False (and
 * neither written) when the role is not in this rig.
 *
 * THE ROTATION IS NOT DECORATION.  An effector offset carried from one rig to
 * another crosses a FRAME, not just a scale: glTF is Y-up by convention and
 * exporters ship Z-up rigs under a converting root node, so the source's
 * "0.6 m above the hips" lands 0.6 m FORWARD on the destination if the offset
 * is copied raw.  Measured: doing exactly that folded the legs up and detached
 * the feet, on a transfer that was otherwise correct.  The hips' POSED
 * rotation is the frame the two rigs agree on.
 *
 * Exposed rather than re-walked by the caller: jce_humanoid_ik_two_bone
 * already computes exactly this internally, so a second parent walk beside it
 * would be a second thing to keep correct.  Its consumer is effector
 * preservation on retarget -- "where did the SOURCE rig's hand actually end
 * up" -- which is a question about a pose, not about a rest pose, so
 * jce_humanoid_rest_locals cannot answer it. */
/* HINGE DIRECTION for a knee: a knee bends one way, and the muscle clamp below
 * cannot say which.  That clamp is a cone plus a signed twist, so it bounds HOW
 * FAR a bone was pushed and not TOWARD WHAT -- a transfer that inverts a knee
 * passes it, and an inverted knee is the single most recognisable retargeting
 * artefact there is.
 *
 * NO CONFIGURED T-POSE IS NEEDED, which is what this row had assumed.  The
 * blocker recorded against it was that "a hinge axis is not derivable from a
 * rest pose where the leg is straight, since the cross product of two collinear
 * bones is zero".  True of the two BONES; false of the pair this uses.  The
 * hinge axis of a knee is the body's LATERAL axis, and that comes from the two
 * hips -- never collinear on a humanoid -- with up from the hips to the highest
 * mapped spine role.  Every input is a difference between two joints the rig
 * itself nominated for a role; nothing is authored and no axis convention is
 * assumed.
 *
 * Which SIGN about that axis is the legal one is closed form too: rotating by
 * +theta displaces the ankle along cross(axis, ankle - knee) to first order, so
 * the sign of that displacement along forward says whether +theta sends the
 * foot forward.  A knee sends it back.
 *
 * `hyperextend_deg` is the allowance in the wrong direction, because real knees
 * have a few degrees of it and a clamp that fires at exactly zero would trip on
 * poses an animator meant.  A delta already legal comes back as the caller's
 * own bits -- not a recomposition differing in the last place -- so a test
 * asking "did it fire" has an exact answer.
 *
 * KNEES ONLY, deliberately.  An elbow's hinge axis is not the body's lateral
 * axis: it depends on how the rig rolls the upper arm, which differs per tool,
 * and guessing it would clamp correct animation.  Elbows stay unclamped in
 * direction and this comment is where that is said. */
JCE_API jce_quat jce_humanoid_clamp_knee_direction(const JceHumanoidMap *map,
                                                   const JceSkeleton    *skel,
                                                   JceHumanoidBone       bone,
                                                   float                 hyperextend_deg,
                                                   jce_quat              delta);

/* HINGE DIRECTION for an elbow -- the same defect as the knee's, and a
 * different derivation, which is why it is a different function.
 *
 * A knee's axis comes from the body, because the two hips are never collinear.
 * An elbow's cannot: which way a forearm flexes depends on how the rig ROLLS
 * the upper arm, and that differs per tool.  What IS rig-intrinsic is the
 * plane the arm is already bent in at rest -- which is exactly why riggers
 * leave a bend, so IK solvers know the side.
 *
 * MEASURED on the two rigged humans in this tree: CesiumMan's arm rests at 147
 * degrees, 33 of bend, and gives a plane; PSX_BagMan's rests at EXACTLY 180
 * and does not.  Below 5 degrees of rest bend this REFUSES and returns the
 * delta untouched, because an elbow clamped about an invented axis rejects
 * correct animation -- worse than not clamping.  The Animation Rigging panel
 * is where a rig that cannot be clamped should say so.
 *
 * The legal direction is convention-free: rotating the forearm about the axis
 * moves the hand along cross(axis, forearm), and folding brings the hand
 * TOWARD the shoulder.
 *
 * `hyperextend_deg` and the bit-exact return for an already-legal delta work
 * exactly as they do for the knee. */
JCE_API jce_quat jce_humanoid_clamp_elbow_direction(const JceHumanoidMap *map,
                                                    const JceSkeleton    *skel,
                                                    JceHumanoidBone       bone,
                                                    float                 hyperextend_deg,
                                                    jce_quat              delta);

/* TWIST REDISTRIBUTION -- Unity's Upper Arm / Fore Arm / Upper Leg / Leg Twist.
 *
 * All of a limb's twist expressed at the END joint pinches the skin into the
 * "candy wrapper": the forearm stays unrotated along its length and the wrist
 * takes the whole turn, so the vertices weighted to both collapse toward the
 * axis.  Moving a FRACTION of that twist onto the lower bone spreads it.
 *
 * THE POSE DOES NOT MOVE.  The axis runs from the lower joint THROUGH the end
 * joint, so rotating the lower bone about it leaves the end joint's position
 * unchanged, and removing the same rotation from the end's local leaves its
 * orientation unchanged.  Geometrically identical; only where the twist is
 * expressed differs, which is all that skinning sees.  That is also the
 * assertion the test makes, because it is the one that can fail silently.
 *
 * `fraction` 0 does nothing AT ALL -- not a recomposition that differs in the
 * last place -- so a project that never sets it renders byte-identically.
 * Unity's default is 0.5; this engine's is 0, because turning it on changes
 * the skinning of content that is already authored.
 *
 * Limbs whose end joint is not a child of the lower one are skipped: on a
 * Blender IK rig the foot is a control bone off the root, and moving twist
 * "up" to a joint the end does not descend from would move the end itself.
 *
 * Returns how many limbs were touched. */
JCE_API uint32_t jce_humanoid_redistribute_twist(const JceHumanoidMap *map,
                                                 const JceSkeleton    *skel,
                                                 float                 fraction,
                                                 jce_quat             *io_local);

/* EFFECTOR PRESERVATION, which is what the next two functions are for and why
 * JceSkeletalAnimatorComponent.retarget_effector_ik exists.
 *
 * jce_humanoid_retarget transfers ROTATIONS, and that is right -- limb lengths
 * belong to the target rig.  The cost is that identical shoulder and elbow
 * angles put the hand somewhere ELSE on a rig with a longer forearm, so a clip
 * authored with a hand resting on a railing has it through the railing.  With
 * the flag on, the renderer asks where the SOURCE rig's hand and foot actually
 * ended up, expresses that relative to the hips and scaled by the two rigs'
 * hips heights -- the same quantity the root translation is already scaled by,
 * so a short character reaches the same place relative to its own size -- and
 * IKs the destination's limb back onto it.
 *
 * OFF BY DEFAULT because it changes the pose of content that is already
 * authored, which is a decision for whoever authored it and not for a release
 * note.  Unity makes the same call (IK Pass is a per-layer opt-in).
 *
 * It runs AFTER the muscle clamp and before the pose is recomposed, so the
 * limits shape the pose the IK then corrects rather than the IK being clamped
 * away afterwards. */
JCE_API bool jce_humanoid_role_model_xform(const JceHumanoidMap *map,
                                           const JceSkeleton    *skel,
                                           const jce_quat       *local,
                                           JceHumanoidBone       role,
                                           jce_vec3             *out_pos,
                                           jce_quat             *out_rot);

JCE_API bool jce_humanoid_ik_two_bone(const JceHumanoidMap *map,
                                      const JceSkeleton    *skel,
                                      JceHumanoidLimb       limb,
                                      jce_vec3              target_model,
                                      const jce_vec3       *pole_model,
                                      float                 weight,
                                      jce_quat             *io_local);

/* The three roles `limb` is made of, in order (upper, lower, end).
 * False for an out-of-range limb.  Public because a caller that wants to
 * clamp or inspect the solved joints should not have to re-derive which ones
 * they were. */
JCE_API bool jce_humanoid_limb_bones(JceHumanoidLimb limb,
                                     JceHumanoidBone *out_upper,
                                     JceHumanoidBone *out_lower,
                                     JceHumanoidBone *out_end);

/* THE HINGE AXIS ITSELF, for a knee or an elbow, as the two clamps above
 * derive it -- one derivation, and this is it.
 *
 * `bone` must be a LOWER_LEG or a LOWER_ARM; anything else returns false.
 * `out_axis` is unit length in MODEL space, `out_bend_sign` is +1 or -1 and
 * says which way about that axis FOLDS the joint.
 *
 * It is public for two reasons, both learned the hard way.  An editor that
 * wants to tell the user "this rig's elbows cannot be clamped" needs the same
 * answer the clamp uses, not a second opinion; and a TEST that re-derives the
 * axis from its own reading of the skeleton is asserting against its own
 * derivation rather than the engine's -- ours did, and disagreed by 10
 * degrees on a rig where both derivations looked right. */
JCE_API bool jce_humanoid_hinge_axis(const JceHumanoidMap *map,
                                     const JceSkeleton    *skel,
                                     JceHumanoidBone       bone,
                                     jce_vec3             *out_axis,
                                     float                *out_bend_sign);

JCE_EXTERN_C_END

#endif /* JCE_HUMANOID_H */
