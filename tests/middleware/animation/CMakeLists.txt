cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L5 animation-layer pure-logic unit tests.                              #
#                                                                         #
#  Two-bone IK and animation event dispatch are pure CPU; no ozz, no     #
#  skeleton state allocation needed.                                      #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_anim_ik
    SOURCES test_jce_anim_ik.c
    LINK    jce_core jce_animation)

# Lossy keyframe reduction (RDP curve fit): pure (timestamps, values) array
# reducer — linear-collapse / corner-preservation / reconstruction-error-bound /
# rotation-angular-tolerance / short-track + NULL safety / global opt-in.
jce_add_unit_test(test_jce_anim_compress
    SOURCES test_jce_anim_compress.c
    LINK    jce_core jce_animation)

# Full-Body IK solver core (FABRIK on a joint tree, sub-base averaging):
# reachable/unreachable single chain, two-effector shared spine, weight, bone
# rigidity, compute_lengths, NULL safety.  Pure CPU math (no skeleton/GPU).
jce_add_unit_test(test_jce_anim_fbbik
    SOURCES test_jce_anim_fbbik.c
    LINK    jce_core jce_animation)

# Animation frame-event → script dispatch (P1 anim-events): exercises the VM
# primitive jce_script_call_anim_event DIRECTLY (no renderer/runtime), so it
# links the script layer rather than jce_animation (mirrors the link set of the
# L4 script-layer tests in tests/middleware/script).
jce_add_unit_test(test_jce_anim_event_dispatch
    SOURCES test_jce_anim_event_dispatch.c
    LINK    jce_core jce_script)

jce_add_unit_test(test_jce_anim_ik_aim
    SOURCES test_jce_anim_ik_aim.c
    LINK    jce_core jce_animation)

# Foot IK: ground-adaptive foot placement built ON the two-bone solver. Pure
# CPU geometry (no scene/physics/renderer); the solver + the two-bone solver
# it reuses both live in jce_animation.
jce_add_unit_test(test_jce_anim_foot_ik
    SOURCES test_jce_anim_foot_ik.c
    LINK    jce_core jce_animation)

# Single-target rigging-IK constraint helpers (kinds 2/3/4 MultiParent/Position/
# Rotation): pure value-blend solvers (jce_anim_ik_position_solve /
# jce_anim_ik_rotation_solve).  State-free POD-float math like the chain solvers,
# so it links jce_animation (mirrors test_jce_anim_foot_ik).
jce_add_unit_test(test_jce_anim_ik_constraint
    SOURCES test_jce_anim_ik_constraint.c
    LINK    jce_core jce_animation)

jce_add_unit_test(test_jce_anim_blend_tree
    SOURCES test_jce_anim_blend_tree.c
    LINK    jce_core jce_animation)

jce_add_unit_test(test_jce_anim_blend_tree_2d
    SOURCES test_jce_anim_blend_tree_2d.c
    LINK    jce_core jce_animation)

jce_add_unit_test(test_jce_anim_sm
    SOURCES test_jce_anim_sm.c
    LINK    jce_core jce_animation)

# Root motion (FEATURE 3.2): builds a REAL clip + skeleton via the animation
# layer's internal constructors (jce_anim_clip_create / jce_skeleton_create),
# which live in the src-side header — add engine/src to this target's include
# path so the test can include "middleware/animation/jce_animation.h".
jce_add_unit_test(test_jce_root_motion
    SOURCES test_jce_root_motion.c
    LINK    jce_core jce_animation)
target_include_directories(test_jce_root_motion PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Additive / layered animation + real avatar mask (FEATURE 3.3): builds a REAL
# clip + skeleton via the internal constructors and exercises the real player
# blend path, so it needs engine/src on the include path too.
jce_add_unit_test(test_jce_anim_layers
    SOURCES test_jce_anim_layers.c
    LINK    jce_core jce_animation)
target_include_directories(test_jce_anim_layers PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Animation retargeting: plays a clip authored for skeleton S on a DIFFERENT
# skeleton T via a name-based bone map + bind-pose-relative rotation transfer.
# Uses the internal jce_skeleton_create constructor + the internal retarget
# header, so it needs engine/src on the include path. Headless (no GPU).
jce_add_unit_test(test_jce_anim_retarget
    SOURCES test_jce_anim_retarget.c
    LINK    jce_core jce_animation)
target_include_directories(test_jce_anim_retarget PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Ragdoll core: builds a per-bone DYNAMIC physics body chain from a skeleton's
# bind pose, simulates it in a real Bullet world, and syncs bodies <-> bone
# local transforms with an animation<->physics blend weight.  Uses the public
# jce_skeleton_create constructor + the internal ragdoll header, and links
# jce_physics for the headless physics world (like the cloth test).
# jce_resource closes physics' private renderer -> runtime-resource edge.
# No GPU context is created.
jce_add_unit_test(test_jce_ragdoll
    SOURCES test_jce_ragdoll.c
    LINK    jce_core jce_animation jce_physics jce_resource)
target_include_directories(test_jce_ragdoll PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Ragdoll joints must be BOUNDED.  Every joint used to be an unlimited ball,
# so elbows and knees hyperextended and heads rotated freely -- and "the chain
# stays connected" was already true, so it cannot be the assertion.  The
# load-bearing case pushes one joint hard and reads how far it got AGAINST A
# CONTROL running the identical push with limits disabled: same rig, same
# torque, same steps, the limit the only difference.  The bound compared
# against comes from jce_humanoid_muscle_limits rather than a literal, so
# re-tuning that table cannot leave this checking a number nobody uses.
jce_add_unit_test(test_jce_ragdoll_joint_limits
    SOURCES test_jce_ragdoll_joint_limits.c
    LINK    jce_core jce_animation jce_physics jce_resource)
target_include_directories(test_jce_ragdoll_joint_limits PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Ragdoll SCENE-PASS wiring: the authorable JceRagdoll component + the transient
# pose relay (runtime publishes per-bone LOCAL transforms; the renderer reads
# them) + the sync math the runtime publishes.  Round-trips the component through
# the REAL component JSON serializer (jce_scene_save_json -> jce_scene_load_json,
# so it links jce_scene + jce_resource), asserts the relay is NOT serialized, and
# exercises the sync_to_pose -> jce_skeleton_evaluate path against a real Bullet
# world (jce_physics).  Uses the internal ragdoll header -> needs engine/src.  No GPU.
jce_add_unit_test(test_jce_ragdoll_scene
    SOURCES test_jce_ragdoll_scene.c
    LINK    jce_core jce_scene jce_resource jce_animation jce_physics)
target_include_directories(test_jce_ragdoll_scene PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Morph targets / blendshapes (FEATURE 3.1): exercises the bgfx-free morph CPU
# core (jce_morph) AND the REAL glTF morph import via jce_gltf_decode_cpu_memory
# (decode-only, no GPU context — like test_jce_skin_palette).  Links jce_renderer
# for the loader and needs engine/src for its internal header.
jce_add_unit_test(test_jce_morph
    SOURCES test_jce_morph.c
    LINK    jce_core jce_animation jce_renderer jce_resource)
target_include_directories(test_jce_morph PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Humanoid bone matcher: the joint names of the FOUR rigged models in this
# tree, in three different naming conventions (Khronos sample, Blender,
# Unreal).  Pure string matching -- no skeleton, no GPU.
jce_add_unit_test(test_jce_humanoid_map
    SOURCES test_jce_humanoid_map.c
    LINK    jce_core jce_animation)

# Humanoid retargeting: two skeletons agreeing about nothing -- different
# names, counts, order, rest poses and size -- and the claim that a rotation
# transfers as the rotation it ADDED.  Pure math (no GPU); needs jce_renderer
# for jce_skeleton_create.
# Animator override controller: one graph, a different set of clips.  The
# REFUSALS are the point -- this asset shipped as a permanent no-op that
# reported success, so every input that would substitute nothing must come back
# NULL rather than as a valid object.
jce_add_unit_test(test_jce_anim_override_controller
    SOURCES test_jce_anim_override_controller.c
    LINK    jce_core jce_animation)
target_include_directories(test_jce_anim_override_controller PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Muscle-space clamping: the bound on how far a retarget may push a bone from
# its own rest.  The assertions come in PAIRS -- something that must be clamped
# and something that must come back bit-for-bit -- because a clamp that fires
# on everything is as wrong as one that fires on nothing, and only the second
# kind looks like a bug.
jce_add_unit_test(test_jce_humanoid_muscle
    SOURCES test_jce_humanoid_muscle.c
    LINK    jce_core jce_animation jce_renderer)

# Two-bone IK.  Every reach assertion is PAIRED with one about what must not
# have moved: a solver that quietly drags the shoulder while pinning the hand
# reads as correct for weeks, and "it got close" is not the same as "it
# landed".  The rig's bones are 0.5 each so the triangle cases can be written
# down rather than read off the solver.
jce_add_unit_test(test_jce_humanoid_ik
    SOURCES test_jce_humanoid_ik.c
    LINK    jce_core jce_animation jce_renderer)

# A humanoid-limb IK goal (JceIkConstraint kind 7) names a LIMB and lets the
# role map supply the joints.  This pins that they are the SAME joints a
# hand-authored name triple would have named -- on a symmetric rig, resolving
# the wrong side reads as "the IK is weak", not as a wrong joint.
jce_add_unit_test(test_jce_humanoid_limb_goal
    SOURCES test_jce_humanoid_limb_goal.c
    LINK    jce_core jce_animation jce_renderer)

jce_add_unit_test(test_jce_humanoid_retarget
    SOURCES test_jce_humanoid_retarget.c
    LINK    jce_core jce_animation jce_renderer)

# Clip serialisation.  jce_anim_clip_create had exactly two call sites in the
# tree -- the glTF loader and the assimp importer -- and NO serialiser
# anywhere, so a clip could only arrive by importing a model and nothing could
# persist one.  Every failure this format can have produces a clip that PLAYS:
# a dropped channel samples to the rest pose, a quaternion written with three
# components reads back with w=0, a missing duration plays frame one forever.
# None of them errors and all of them look like a badly authored animation.
# Reaches the internal header for JceAnimChannel, the way the retarget tests
# beside it do.
jce_add_unit_test(test_jce_anim_clip_io
    SOURCES test_jce_anim_clip_io.c
    LINK    jce_core jce_animation)
target_include_directories(test_jce_anim_clip_io PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
# Reaches the internal name-based retargeter it is chosen over, the same way
# test_jce_anim_retarget does.
target_include_directories(test_jce_humanoid_retarget PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
