cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L4 physics middleware unit tests.                                      #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_cloth
    SOURCES test_jce_cloth.c
    LINK    jce_core jce_physics jce_resource)

# Voronoi box-shatter geometry core (DESTRUCTION/FRACTURE).  The fracture core
# (jce_fracture.c) lives in the physics middleware layer but has no Bullet
# dependency itself — it is pure math.h + engine alloc — so this is a focused
# geometry test that happens to link the layer the TU compiles into.
jce_add_unit_test(test_jce_fracture
    SOURCES test_jce_fracture.c
    LINK    jce_core jce_physics jce_resource)

# btRaycastVehicle wrapper (VEHICLE PHYSICS).  Drives a real Bullet world via the
# public engine vehicle API: rest-on-ground, throttle, brake, wheel transforms,
# invalid-handle safety, and determinism.
jce_add_unit_test(test_jce_vehicle
    SOURCES test_jce_vehicle.c
    LINK    jce_core jce_physics jce_resource)

# Volumetric / pressure soft body (btSoftBody CreateEllipsoid + kPR).  Drives
# the shared secondary soft world via the public soft-body API: settles on a
# static ground proxy, pressure resists collapse, higher pressure holds shape,
# node readback, invalid-handle safety, and determinism.
jce_add_unit_test(test_jce_softbody
    SOURCES test_jce_softbody.c
    LINK    jce_core jce_physics jce_resource)

# Constant-force primitive (Unity ConstantForce last-mile).  Drives a real
# Bullet world via the public force/torque API: a continuously-applied force
# accelerates a dynamic body, a continuous torque spins it up, defaults are a
# no-op, and a static body is unaffected — the exact primitives the runtime's
# rt_apply_constant_force pass invokes each fixed tick.
jce_add_unit_test(test_jce_constant_force
    SOURCES test_jce_constant_force.c
    LINK    jce_core jce_physics jce_resource)

# Per-axis 6DOF configurable joint + break-force (Unity ConfigurableJoint
# last-mile).  Drives a real Bullet world via the public per-axis joint API:
# a locked axis holds while a free axis separates, a limited axis caps the
# separation, the applied-impulse query rises under a sustained pull (the exact
# quantity the runtime break monitor compares: impulse vs break_force*dt), and
# two identical runs match — the primitives rt_spawn_configurable_joint +
# rt_monitor_configurable_joints rely on each fixed tick.
jce_add_unit_test(test_jce_configurable_joint
    SOURCES test_jce_configurable_joint.c
    LINK    jce_core jce_physics jce_resource)

# A 3D joint can be DRIVEN (physics.joint.motor-and-drive).  Hinge and slider
# velocity motors plus the configurable joint's per-axis velocity and spring
# drives, each against an UNPOWERED control built the same way -- "the body
# moved" is not "the motor moved it".  One case exists purely for the units:
# Bullet's hinge wants a maximum IMPULSE while the author gives a torque, so a
# weak motor must FAIL to hold a load a strong one holds, which brackets the
# conversion to within the factor getting it wrong would introduce.
jce_add_unit_test(test_jce_joint_motor
    SOURCES test_jce_joint_motor.c
    LINK    jce_core jce_physics jce_resource)

# 2D distance/hinge/spring joint (Box2D 3.1; Joint2D last-mile).  Drives a real
# Box2D world via the public 2D-physics joint API: a rigid distance joint holds
# its length under a separating pull, a hinge limit caps rotation, a spring
# relaxes a displaced pair back toward rest length, and destroy/invalid-handle
# safety holds — the exact primitive rt_spawn_joint2d builds from the authored
# JceJoint2DComponent in the post-spawn joint pass.  jce_physics2d.c compiles
# into jce_physics, so this links jce_core + jce_physics.
jce_add_unit_test(test_jce_physics2d_joint
    SOURCES test_jce_physics2d_joint.c
    LINK    jce_core jce_physics jce_resource)

# Collider2D.capsule_direction reaches the simulated shape: a ray that can
# only hit one orientation, asserted in both directions for both values.
# Per-wheel WheelCollider drive reaches Bullet: one-sided trim yaws the
# chassis, symmetric trim does not, zero trim is a no-op.
# Joint2D last-mile primitives: collideConnected reaches Box2D, and the
# constraint force is readable and tracks the load (break_force needs both).
jce_add_unit_test(test_jce_joint2d_fields
    SOURCES test_jce_joint2d_fields.c
    LINK    jce_core jce_physics jce_resource)

jce_add_unit_test(test_jce_vehicle_wheel_input
    SOURCES test_jce_vehicle_wheel_input.c
    LINK    jce_core jce_physics jce_resource)

# Collider2D.is_trigger reaches b2ShapeDef.isSensor: a solid platform stops a
# falling box, a trigger platform does not.  Both directions asserted.
# CapsuleCollider.axis reaches btCapsuleShapeX/Y/Z: a ray that can only hit
# one orientation, all three axes plus the zero default asserted.
jce_add_unit_test(test_jce_capsule3d_axis
    SOURCES test_jce_capsule3d_axis.c
    LINK    jce_core jce_physics jce_resource)

jce_add_unit_test(test_jce_collider2d_trigger
    SOURCES test_jce_collider2d_trigger.c
    LINK    jce_core jce_physics jce_resource)

jce_add_unit_test(test_jce_capsule2d_direction
    SOURCES test_jce_capsule2d_direction.c
    LINK    jce_core jce_physics jce_resource)

# Melee raycast reproduction (street_demo "collides but raycast misses" bug).
# Mirrors the runtime's KINEMATIC capsule enemy (set_entity + set_layer 0) and
# isolates: does a forward ray hit + return the entity, does it track
# set_transform, and does a ray starting inside an untagged body self-hit?
jce_add_unit_test(test_jce_melee_raycast
    SOURCES test_jce_melee_raycast.c
    LINK    jce_core jce_physics jce_resource)

# Heightfield terrain collision.  Goes past "a sphere lands on it": three of
# the five Bullet conventions this API encapsulates cannot be caught by a
# drop-and-settle test -- AABB centring reads as a bias problem, the quad
# diagonal only matters at cell CENTRES, and ghost internal edges only appear
# when something slides.
jce_add_unit_test(test_jce_physics_heightfield
    SOURCES test_jce_physics_heightfield.c
    LINK    jce_core jce_physics jce_resource)

# Terrain has three descriptions of one surface: what the renderer draws, what
# the sampler reports, and what the player stands on.  They agree at every
# vertex no matter which diagonal each picked, so only a CELL-CENTRE probe can
# catch a mismatch -- which is how the triangle soup split on the opposite
# diagonal from the renderer for its entire life without anyone noticing.
jce_add_unit_test(test_jce_terrain_collider_agreement
    SOURCES test_jce_terrain_collider_agreement.c
    LINK    jce_core jce_physics jce_scene jce_resource)

# The character capsule vs the 32-slot layer matrix.  Bullet's own filter
# constants ARE low layer bits (CharacterFilter == 32 == layer 5), so the
# capsule was silently pinned to layer 5 and could touch only layers 0 and 1.
# A drop-and-land test passes either way, because the default matrix is
# all-ones and a default scene is all layer 0 -- the defect only shows with the
# floor on a HIGH layer, which is what case A does.
jce_add_unit_test(test_jce_character_layer
    SOURCES test_jce_character_layer.c
    LINK    jce_core jce_physics jce_resource)

# A character standing on a moving platform is carried by it.  Both mechanisms
# an engine normally has for this were absent at once: the capsule is created
# frictionless on purpose, and its horizontal velocity is recomputed
# absolutely each tick from walk_dir alone, so nothing could impart the
# surface's motion.  Five cases: P asserts the INSTRUMENT (Bullet derives a
# kinematic body's velocity from its transform delta) before anything asserts
# the subject; B is the negative control on a STATIC platform, without which
# "moved +2 m in x" is satisfied by any constant added to the command; and C
# spins the platform about its own centre, where the linear velocity is zero,
# which is the only case that separates a contact-point velocity from a body
# velocity.  Headless; Bullet needs no GPU.
jce_add_unit_test(test_jce_character_platform
    SOURCES test_jce_character_platform.c
    LINK    jce_core jce_physics jce_resource)

# JceContactEvent.applied_impulse: a real collision reports a non-zero
# impulse and it tracks the hit -- what break_impulse compares against.
jce_add_unit_test(test_jce_contact_impulse
    SOURCES test_jce_contact_impulse.c
    LINK    jce_core jce_physics jce_resource)

# The authored friction/restitution COMBINE MODE reaching a contact.
# jce_physics_material_combine() implements Unity's precedence rule and had no
# caller anywhere: jce_physics_body_set_material forwarded only the two values
# to Bullet, whose combine is a fixed multiply, so every authored
# AVERAGE/MIN/MAX/MULTIPLY was dropped between the asset and the solver.
# Two jobs, two assertions: the rule is pinned against the function itself
# (never a hardcoded number), and REACHING the solver is a simulation
# measurement -- the same push under MIN and under MAX must not slide the same
# distance.  Headless; Bullet needs no GPU.
jce_add_unit_test(test_jce_physmat_combine
    SOURCES test_jce_physmat_combine.c
    LINK    jce_core jce_physics jce_resource)

# Collider2D POLYGON builds a real convex hull from the authored points
# instead of falling back to the bounding box.
jce_add_unit_test(test_jce_collider2d_polygon
    SOURCES test_jce_collider2d_polygon.c
    LINK    jce_core jce_physics jce_resource)

# The collider overlay a SHIPPED game can now draw.  CPU-side only on
# purpose: this proves jce_physics_debug_flush delivers real geometry to an
# installed sink, which needs no device.  It does NOT prove pixels -- that is
# jce_debug_draw_flush's half and needs a renderer -- and the ledger row does
# not claim it.
jce_add_unit_test(test_jce_physics_debug_sink
    SOURCES test_jce_physics_debug_sink.c
    LINK    jce_core jce_physics jce_resource)

# The 2D collision layer matrix reaches b2ShapeDef.filter.  The 32x32 grid in
# Project Settings was editable, persisted and shipped while jce_physics2d.c
# contained ZERO uses of b2Filter/categoryBits/maskBits and JceBody2DDesc had
# no layer field -- an authored matrix that changed nothing, looking like level
# design rather than a bug.  Both directions asserted, plus: a shape added
# after creation inherits the body's layer (that defect existed in this unit's
# first draft), the 2D and 3D matrices are independent in both directions, and
# a document with no "matrix2d" key stays PERMISSIVE rather than zeroing.
jce_add_unit_test(test_jce_physics2d_layers
    SOURCES test_jce_physics2d_layers.c
    LINK    jce_core jce_physics jce_resource)
