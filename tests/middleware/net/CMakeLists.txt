cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L4 net-layer unit tests.                                               #
#                                                                         #
#  FEATURE 7.2 — typed property replication (NetworkVariable).  Exercises #
#  the REAL replication encode -> decode delta path: the server registers #
#  the built-in NetworkVariable components on a live flecs world (via     #
#  JceScene), mutates a value, encodes a snapshot with the production      #
#  encoder, and a fresh client state decodes it.                          #
#                                                                         #
#  jce_net carries the NetworkVariable + replication substrate; jce_scene #
#  provides the flecs world the components live on; jce_resource closes    #
#  jce_scene's private jce_renderer -> resource edge (same as the scene    #
#  layer tests).                                                          #
# ----------------------------------------------------------------------- #

# Regression guard for the despawn leak fixed 2026-08-31.  Registered
# UNCONDITIONALLY, unlike the self-test wrappers below: the leak is in code
# that ships, so the guard has to run in the configuration that ships.
jce_add_unit_test(test_jce_net_despawn_reap
    SOURCES test_jce_net_despawn_reap.c
    LINK    jce_core jce_net jce_scene jce_resource)

# Three self-tests that had no caller anywhere in the tree until 2026-08-31,
# and that until 2026-09-20 could only be REGISTERED in a Debug build.
#
# THE OLD RESTRICTION AND WHY IT IS GONE.  They reported failure through bare
# assert(), which NDEBUG compiles away, so the note here used to read "a
# Release registration would be a test that passes without checking
# anything" -- honest, and still a hole: the self-tests' whole bodies sat
# inside `#ifndef NDEBUG`, so in the configuration that ships the functions
# did not exist at all.  The invariants ran only where nobody ships.
#
# Both halves are now fixed.  The checks are JCE_ASSERT
# (<jce/os/core/jce_assert.h>), which survives NDEBUG, and the bodies hang
# off JCE_SELF_TESTS, which the root CMakeLists defines whenever
# JCE_BUILD_TESTS is ON -- named after what it gates instead of riding on the
# optimisation level.  So these register UNCONDITIONALLY, like the despawn
# guard above, and they run in the configuration that ships.
#
# jce_session_self_test and jce_lan_discovery_self_test are still NOT here:
# those two also require -DJCE_NET_SELF_TEST, which no build file in this
# repo defines, and jce_session_self_test binds a real socket (port 27015).
# That extra gate is deliberate -- see the comment at jce_session.c:751.
jce_add_unit_test(test_jce_replication_selftest
    SOURCES test_jce_replication_selftest.c
    LINK    jce_core jce_net jce_scene jce_resource)
jce_add_unit_test(test_jce_net_transform_selftest
    SOURCES test_jce_net_transform_selftest.c
    LINK    jce_core jce_net jce_scene jce_resource)
jce_add_unit_test(test_jce_rpc_selftest
    SOURCES test_jce_rpc_selftest.c
    LINK    jce_core jce_net jce_scene jce_resource)

jce_add_unit_test(test_jce_network_variable
    SOURCES test_jce_network_variable.c
    LINK    jce_core jce_net jce_scene jce_resource)

# ----------------------------------------------------------------------- #
#  GAS attribute replication (F12 last-mile).                             #
#                                                                         #
#  Drives the SAME real encode -> decode path as the NetworkVariable test #
#  for the packed JceGasAttribRepl component: the server fills the replica #
#  from a live GAS, the production encoder captures wire bytes, and a      #
#  fresh client decodes + applies them back into its local GAS.           #
#                                                                         #
#  jce_world carries jce_gas + jce_gas_replication (the packed component + #
#  its net-generic serializer); jce_net carries the replication substrate; #
#  jce_scene provides the flecs world the component lives on; jce_resource #
#  closes jce_scene's private renderer -> resource edge.                  #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_gas_replication
    SOURCES test_jce_gas_replication.c
    LINK    jce_core jce_net jce_world jce_scene jce_resource)

# ----------------------------------------------------------------------- #
#  Client-side prediction + reconciliation/rollback core.                 #
#                                                                         #
#  The prediction buffer is a standalone module — no transport, flecs,    #
#  scene, or physics dependency — so it links ONLY the net layer + core   #
#  (for the mimalloc-backed JCE_MALLOC the impl uses).  Fully headless +  #
#  deterministic: a tiny pure sim (x' = x + vx) drives predict + replay.  #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_net_prediction
    SOURCES test_jce_net_prediction.c
    LINK    jce_core jce_net)

# ----------------------------------------------------------------------- #
#  SERVER-side re-tick on a late input: jce_prediction_resim_from_input    #
#  (the server analogue of reconcile — correction source is a late INPUT)  #
#  + the eviction base-floor fix.  Pure (net + core only), deterministic.  #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_net_server_resim
    SOURCES test_jce_net_server_resim.c
    LINK    jce_core jce_net)

# ----------------------------------------------------------------------- #
#  Client-prediction WIRING: the deterministic locomotion step composed   #
#  through the predict ring + reconcile core.                             #
#                                                                         #
#  Proves the runtime's apply_input -> get_current_state ->               #
#  reconcile(rollback/replay) pipeline against the SAME pure step fn +    #
#  tolerant compare the runtime composes (jce_predict_locomotion_*).      #
#  Both the locomotion module and the prediction core are pure, so this   #
#  links ONLY the net layer + core — no ECS / scene / physics / runtime.  #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_prediction_wiring
    SOURCES test_jce_prediction_wiring.c
    LINK    jce_core jce_net)

# ----------------------------------------------------------------------- #
#  CLIENT->SERVER input command channel (F12 slice).                      #
#                                                                         #
#  Proves the PURE, transport-free core of the upstream input channel:    #
#  the fixed-layout little-endian codec, the bounded per-client latest-   #
#  wins store, and the server-receive (decode+store) seam the runtime's   #
#  RPC handler forwards to.  The codec + store carry NO transport / RPC /  #
#  runtime / physics dependency, so this links ONLY the net layer + core  #
#  (for the mimalloc-backed allocator the engine links).  Live over-the-  #
#  wire client->server RPC is F12.                                        #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_input_command
    SOURCES test_jce_input_command.c
    LINK    jce_core jce_net)

# REF-005 (dedup audit) — ENet subsystem ownership.  Two modules used to
# initialise ENet independently with asymmetric shutdown; it is now a single
# refcount in jce_net.c.  These tests exercise interleaved lifetimes across
# jce_net and jce_lan_discovery, and reuse after the count reaches zero —
# the failure modes a refcount introduces.
jce_add_unit_test(test_jce_net_enet_lifecycle
    SOURCES test_jce_net_enet_lifecycle.c
    LINK    jce_core jce_net)

# C5-03: replicated entities must go through the scene layer.  Raw
# ecs_new/ecs_delete skipped the roster epoch, the default transform and
# JceTagActive — the last making a replicated entity invisible to every
# active-filtered system.
jce_add_unit_test(test_jce_net_entity_hooks
    SOURCES test_jce_net_entity_hooks.c
    LINK    jce_core jce_net jce_scene jce_resource)

# A NetRigidbody's four knobs reach transform replication: a non-default
# config registers, is idempotent, and unregisters cleanly.
jce_add_unit_test(test_jce_net_rigidbody_config
    SOURCES test_jce_net_rigidbody_config.c
    LINK    jce_core jce_net jce_resource)

# Animator state replication: the three things that make it different from a
# transform -- normalized time WRAPS, a state change is not a blend, and a
# trigger is an edge on a reliable channel.  Pure logic via the loopback seam.
jce_add_unit_test(test_jce_net_animator
    SOURCES test_jce_net_animator.c
    LINK    jce_core jce_net jce_resource)
