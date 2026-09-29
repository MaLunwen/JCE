cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L3 (application) unit tests.                                           #
#                                                                         #
#  Application-layer modules that are pure-logic (no SDL / bgfx init      #
#  required) are exercised here.  Each test links only the engine layer   #
#  it touches plus jce_core for the allocator + log shims.                #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_version
    SOURCES test_jce_version.c
    LINK    jce_core jce_application)

# Regression guard for the coroutine handle-vs-sentinel collision fixed
# 2026-08-31.  Registered UNCONDITIONALLY, unlike the self-test below: the bug
# and its fix live in code that ships, so the guard has to run in the
# configuration that ships.  It touches only the public header.
jce_add_unit_test(test_jce_coroutine_handle_zero
    SOURCES test_jce_coroutine_handle_zero.c
    LINK    jce_core jce_application)

# The coroutine system's own self-test, which had no caller anywhere in the
# tree until 2026-08-31.  jce_coroutine.c builds into jce_application.
#
# REGISTERED UNCONDITIONALLY since 2026-09-20.  It used to be Debug-only
# because jce_coroutine_self_test() sat inside `#ifndef NDEBUG` -- the symbol
# did not exist in a Release build, so the two P0s this self-test found on its
# first ever run (the (gen<<32)|slot handle colliding with the 0 sentinel, and
# the despawn reap shut inside `if (SERVER && host)`) were only ever checked
# in the configuration nobody ships.  The guard is now JCE_SELF_TESTS, which
# the root CMakeLists defines whenever JCE_BUILD_TESTS is ON.
jce_add_unit_test(test_jce_coroutine_selftest
    SOURCES test_jce_coroutine_selftest.c
    LINK    jce_core jce_application)

jce_add_unit_test(test_jce_runtime_boot
    SOURCES test_jce_runtime_boot.c
    LINK    jce_core jce_application)

jce_add_unit_test(test_jce_args
    SOURCES test_jce_args.c
    LINK    jce_core jce_application)

jce_add_unit_test(test_jce_subsystem
    SOURCES test_jce_subsystem.c
    LINK    jce_core jce_application)

# Headless / dedicated-server boot (dependency/ABI audit §6 B3): boots the FULL
# engine with desc.headless=true and asserts NullRHI renderer + no window/audio,
# ticks, and shuts down clean.  Links the JCE aggregate because jce_engine_create
# pulls the whole stack (renderer NullRHI, scene, resource, ...).
jce_add_unit_test(test_jce_headless_boot
    SOURCES test_jce_headless_boot.c
    LINK    JCE)

# Regression (Plan B Task 1, 523904e8): jce_input_create() stopped installing
# a backend and left a comment claiming jce_engine.c installs
# jce_input_sdl_backend() right after -- it never did, so no gamepad could
# ever attach.  Both tests in this file prove the install by behaviour: a
# DEVICE_ADDED for the documented-invalid SDL_JoystickID 0 must be refused,
# not accepted.
#
# TWO tests, because one of them cannot be relied on in CI.  The windowed one
# boots the real engine and covers the CALL SITE, but self-ignores where a
# non-fallback windowed boot is unavailable, and Unity counts an ignore as a
# pass -- a green tick proving nothing.  The other calls
# jce_engine_create_windowed_input() (engine/src/application/
# jce_engine_windowed_input.h) directly: no window, no GPU, no SDL_Init, so it
# RUNS everywhere.  The delete-the-install mutation was run on Windows x64 only
# (plain and JCE_HEADLESS=1, the display-less condition); that it also reddens
# on the Linux and macOS runners is an inference, not a measurement -- see the
# caveat in the test file's header.
# engine/src is on the include path for that internal seam header.
# JCE_REQUIRE_WINDOWED_INPUT=1 turns the windowed test's ignores into failures
# on a host that is supposed to reach it.
jce_add_unit_test(test_jce_engine_input_backend
    SOURCES test_jce_engine_input_backend.c
    LINK    JCE)
target_include_directories(test_jce_engine_input_backend PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

jce_add_unit_test(test_jce_screenshot_schedule
    SOURCES test_jce_screenshot_schedule.c
    LINK    jce_core jce_application)

jce_add_unit_test(test_jce_player_loop
    SOURCES test_jce_player_loop.c
    LINK    jce_core jce_application)

# Guards the Phase-0 scripting keystone ordering bug (script VM / bt_ctx must be
# created BEFORE the rt_spawn_gameplay walk, else authored scripts never load).
# Links the full engine aggregate because jce_runtime_create pulls the whole
# middleware stack (scene/physics/audio/net/loc/script).
jce_add_unit_test(test_jce_runtime_script_load
    SOURCES test_jce_runtime_script_load.c
    LINK    JCE)

# Per-script language selection: a .lua and a SECOND language in ONE scene,
# each running in its own VM, through the real jce_runtime_create /
# jce_runtime_step path.  The second language is a test-owned JceScriptVM on
# purpose -- Python and Java are OFF in the default build, and this must hold
# in the configuration people actually build.  Also pins the absent-backend
# case: a script whose extension nobody claims does NOT silently become Lua.
jce_add_unit_test(test_jce_runtime_multilang
    SOURCES test_jce_runtime_multilang.c
    LINK    JCE)

# jce.is_key_down through the real runtime host table.  The mock-host binding
# tests install non-NULL callbacks and so cannot see an unassigned host slot.
jce_add_unit_test(test_jce_runtime_is_key_down
    SOURCES test_jce_runtime_is_key_down.c
    LINK    JCE)

# jce.get_param / jce.get_param_text through the real runtime host table, for
# the same reason.  An exposed field is wired in six places and five can be
# right while the sixth leaves every script on its default -- and
# `jce.get_param(e, "speed") or 3.0` is byte-identical at the call site
# whether the author set nothing or the feature is inert.
jce_add_unit_test(test_jce_runtime_script_params
    SOURCES test_jce_runtime_script_params.c
    LINK    JCE)

# jce.curve_eval through the real runtime host table.  The Curve Editor wrote
# curve documents that NOTHING could read; closing that needs the parser, the
# umbrella header, the host callback, the manifest and seven generated
# bindings to all be right, and four of five is indistinguishable from none.
# The case that earns the file is sampling a curve where its authored value is
# ZERO: a lossy `float curve_eval(...)` binding passes every other assertion
# here and fails only that one.
jce_add_unit_test(test_jce_runtime_curve
    SOURCES test_jce_runtime_curve.c
    LINK    JCE)

# A SCRIPT filtering a raycast by layer, and asking for every hit along it.
# The ray was the ONE physics query a script could reach that took no filter,
# while overlap_sphere/overlap_box beside it have always taken a layer_mask --
# and jce_physics_raycast_filtered / _raycast_all were implemented down to
# Bullet with no script reader at all.  The generated differential harness
# already proves the seven languages marshal these calls identically; what it
# cannot see is the HOST glue -- filter construction, body->entity mapping,
# near->far order -- so this runs a real Lua probe against a real Bullet world.
# The two mask assertions are a PAIR: a mask selecting a body's layer must
# hit, and a mask selecting an empty layer must miss.  An ignored mask passes
# the first and fails the second.
jce_add_unit_test(test_jce_runtime_script_raycast_filter
    SOURCES test_jce_runtime_script_raycast_filter.c
    LINK    JCE)

# Per-script frame cost.  The script subsystem was the only middleware with no
# instrumentation at all, so on_update was folded into the `gameplay` bucket
# with five other things.  The load-bearing assertion is a RANKING (the busy
# script costs more than the empty one), not a magnitude: absolute
# milliseconds are a property of the machine, attribution is a property of the
# measurement -- and the aggregate was already available.
jce_add_unit_test(test_jce_runtime_script_cost
    SOURCES test_jce_runtime_script_cost.c
    LINK    JCE)

# The character controller had ZERO coverage until 2026-09-01 -- nothing in the
# suite named CharacterController or jce_runtime_get_player_position.
jce_add_unit_test(test_jce_runtime_character
    SOURCES test_jce_runtime_character.c
    LINK    JCE)

# The SCENE-to-physics vehicle bridge (rt_build_vehicle).  The physics-level
# vehicle API has its own test; the path a designer uses had none.
jce_add_unit_test(test_jce_runtime_vehicle_bridge
    SOURCES test_jce_runtime_vehicle_bridge.c
    LINK    JCE)

jce_add_unit_test(test_jce_runtime_touch_input
    SOURCES test_jce_runtime_touch_input.c
    LINK    JCE)

# Script-triggered one-shots must honor the same host audio resolver as
# AudioSource spawning (editor Play uses it to resolve project-relative clips).
jce_add_unit_test(test_jce_runtime_audio_resolve
    SOURCES test_jce_runtime_audio_resolve.c
    LINK    JCE)
target_include_directories(test_jce_runtime_audio_resolve PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# An authored AudioSource sounded EXACTLY ONCE, at scene spawn, and only with
# playOnAwake -- nothing in the engine, the editor or the seven script backends
# could start, stop or restart one afterwards, so a door creak or a gunshot
# could not use the component at all.  Drives the real runtime with a real
# (device-less) JceAudio; reaches into engine/src for rt->voice_count, which is
# the only witness that tells RESTART apart from STACK.
jce_add_unit_test(test_jce_runtime_audio_source_control
    SOURCES test_jce_runtime_audio_source_control.c
    LINK    JCE)
target_include_directories(test_jce_runtime_audio_source_control PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# FEATURE 9.4: engine-level scene/level transition (jce_runtime_request_scene +
# the fade-out -> load -> fade-in state machine driven inside jce_runtime_step).
# Links the full engine aggregate because jce_runtime_create + the in-place
# scene swap pull the whole middleware stack (scene/physics/audio/script/serial).
jce_add_unit_test(test_jce_runtime_scene_transition
    SOURCES test_jce_runtime_scene_transition.c
    LINK    JCE)

# Gap 9.1 last-mile: the in-game console + cvar system now registers REAL engine
# cvars (time_scale / paused / audio.master_volume) at jce_runtime_create and
# APPLIES the console-changed ones to runtime state inside jce_runtime_step, so a
# console line actually controls the sim instead of just storing a value.  Drives
# jce_console_exec end-to-end through the real runtime; links the full aggregate.
jce_add_unit_test(test_jce_runtime_cvar
    SOURCES test_jce_runtime_cvar.c
    LINK    JCE)

# jce_project.json's "script_modules" — the only place a tool can learn which
# shared object holds a project's compiled script classes.  Round-tripped
# rather than merely parsed, because jce_project_save() REBUILDS the manifest
# and a key that is read but not written vanishes on the next unrelated save.
jce_add_unit_test(test_jce_project_script_modules
    SOURCES test_jce_project_script_modules.c
    LINK    JCE)

# A project NAME is human text; a CMake target and a C function name are
# IDENTIFIERS.  The template substituted one token into all three, so the
# editor's New Project dialog -- whose only validation is `strlen > 0` --
# could write a project that cannot configure: `project(My Game C)` makes
# cmake hunt for a compiler for a language called Game.  A hyphen is quieter
# and worse (`my-game_get_desc` is a subtraction).  The Automation layer's
# project.create had derived a sanitised target for years; two scaffolders,
# one of which knew.  Asserts BOTH directions -- the identifier is sanitised
# AND the display name keeps its space.
jce_add_unit_test(test_jce_project_template_name
    SOURCES test_jce_project_template_name.c
    LINK    JCE)

# JCE_LIFECYCLE_DEVICE_LOST shipped in the public ABI with ZERO emit sites:
# jce_lifecycle_register(DEVICE_LOST, ...) returned a live handle and the
# callback was dead code.  The producer is bgfx's fatal callback and cannot be
# raised without a genuinely lost device, so the "somebody emits it at all"
# half is a lint (check_lifecycle_events_emitted.py) and this pins DELIVERY --
# the registry routes it, with the right enumerator, and stops on unregister.
# Neither alone would have caught it: the lint cannot see reachability, and
# this test would have passed on the broken tree.
jce_add_unit_test(test_jce_lifecycle_device_lost
    SOURCES test_jce_lifecycle_device_lost.c
    LINK    JCE)

# The automated capture (JCE_CAPTURE_FRAME) photographs the BACKBUFFER, which
# in the editor carries the ImGui layer -- so an editor-sourced capture holds
# the editor, and the profiler panel draws a clock into it (53,089 of
# 3,911,680 pixels differ between two runs of one input digest).  The host can
# now take the shot itself.  A framebuffer INDEX could not express the
# subject: the editor's UI-free capture reads a texture whose y-flip depends
# on which texture it is, so that version was written and reverted.
# The producer needs a GPU; this pins the SEAM -- installed / declined /
# cleared, and that the path and user pointer arrive.  "Declined" is the case
# that matters: no Game View panel means no target, and the capture must still
# happen from the backbuffer rather than silently not happen.
jce_add_unit_test(test_jce_auto_capture_hook
    SOURCES test_jce_auto_capture_hook.c
    LINK    JCE)
# The invoker half is internal -- a host INSTALLS a hook, it does not call
# one -- and lives in jce_renderer_host_hook.h, which has no dependencies on
# purpose: its sibling jce_renderer_bgfx_callback.h pulls <bgfx/c99/bgfx.h>,
# and bgfx is PRIVATE to jce_renderer so that include path does not reach a
# test.  engine/src is all this needs.
target_include_directories(test_jce_auto_capture_hook PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
# Point the creator at the SOURCE templates, so this test measures what is in
# the tree rather than what was last installed into dist/sdk.
#
# A COMPILE DEFINITION, NOT A ctest ENVIRONMENT PROPERTY, and
# check_test_window_discipline.py's docstring is the reason: "A ctest
# ENVIRONMENT property is a property of the LAUNCH, not of the program: it
# does not reach the exe when someone runs it directly to debug it."  The test
# sets the variable itself in setUp(), so a direct run measures the tree too
# instead of silently falling back to whatever dist/sdk last received.
target_compile_definitions(test_jce_project_template_name PRIVATE
    JCE_TEST_TEMPLATES_DIR="${CMAKE_SOURCE_DIR}/engine/templates")

# GAS consumption last-mile: the runtime builds a live JceGameplayAbilitySystem
# from an entity's authored JceGameplayAbilitySystemComponent at create()
# (rt_spawn_gameplay), ticks it each step (rt_tick_gameplay), and exposes it to
# Lua via jce.gas_activate / jce.gas_get / jce.gas_apply + the public
# jce_runtime_entity_gas API.  This drives a script that fires a costed/cooldown
# ability and applies an instant effect, asserting the live attributes change.
# Links the full engine aggregate (jce_runtime_create pulls scene/script/world).
jce_add_unit_test(test_jce_runtime_gas
    SOURCES test_jce_runtime_gas.c
    LINK    JCE)

# Regression: model-based colliders (Compound / Mesh) must spawn a physics body
# in the editor / loose-asset case where the scene stores a project-relative
# model path that does not resolve against the process CWD.  The runtime now
# maps it through JceRuntimeDesc.resolve_path_fn (the same resolution the
# renderer uses) before the direct ".jcol" / model load — without it the
# collider silently failed to spawn while the mesh still rendered.  Cooks a box
# trimesh blob and asserts a downward raycast hits only when the resolver is
# wired.  Links the full aggregate (jce_runtime_create + Bullet).
jce_add_unit_test(test_jce_runtime_collider_resolve
    SOURCES test_jce_runtime_collider_resolve.c
    LINK    JCE)

# The RUNTIME half of soft-body collision.  tests/middleware/physics/
# test_jce_softbody.c calls jce_softbody_add_static_sphere directly, so it
# proves the PROXY works and says nothing about whether a scene's sphere or
# capsule collider ever becomes one -- a mutation deleting both branches from
# rt_mirror_static_collider survived that file completely.  Counts what the
# runtime actually mirrored using the INDEX add_static_box returns, so no
# accessor had to be added to the public API for a test to watch.  Links the
# full aggregate (jce_runtime_create + Bullet).
jce_add_unit_test(test_jce_runtime_softbody_colliders
    SOURCES test_jce_runtime_softbody_colliders.c
    LINK    JCE)

# audio_mixer.json read from the MOUNTED PAK, not only the host filesystem.
# rt_mixer_read_config used jce_fs_host_read_all alone, so a single-file exe --
# where the config lives inside the PAK -- seeded the DEFAULT bus tree instead:
# authored insert effects absent, and once device-side aux sends existed, those
# absent too, silently, in the only build a player runs.  The observable is the
# LIVE engine's jce_audio_bus_get_send, which proves the whole chain (PAK read
# -> parse -> route -> node graph) rather than any one link; case 1 is the
# negative control that nothing is routed with no config at all.  Offline audio
# engine via JceRuntimeDesc.audio, so no device is needed.
jce_add_unit_test(test_jce_runtime_mixer_from_pak
    SOURCES test_jce_runtime_mixer_from_pak.c
    LINK    JCE)

# An AudioSource taking an AUTHORED volume-over-distance curve instead of one
# of the four analytic attenuation models -- the first BUILT-IN consumer of the
# Curve Editor's documents (until now only a script could read one, via
# jce.curve_eval).  The load-bearing case is NOT "it gets quieter with
# distance": the analytic models already did that.  It authors a curve FLAT at
# 1.0 and asserts distance costs nothing, which no analytic model can produce,
# against a control running the identical geometry with no curve that must
# fall.  Offline audio engine through JceRuntimeDesc.audio, so no device and no
# case may skip.
jce_add_unit_test(test_jce_runtime_audio_rolloff_curve
    SOURCES test_jce_runtime_audio_rolloff_curve.c
    LINK    JCE)

# The buoyancy pass ITSELF, which no water test could reach: rt_apply_buoyancy
# is static, needs a physics world, and only runs on the fixed tick. Mutation
# proved the cost of that gap -- deleting the disturbance term from the
# buoyancy sample and flipping the impulse sign both left every existing water
# test green. Links the full engine aggregate for the same reason its
# neighbour above does.
jce_add_unit_test(test_jce_runtime_buoyancy_ripple
    SOURCES test_jce_runtime_buoyancy_ripple.c
    LINK    JCE)

# Regression (street_demo melee "collides but raycast misses"): a collider with
# a non-zero CENTER OFFSET (a capsule centered at cy=0.9 on a feet-origin body)
# must keep that offset when the entity is MOVED at runtime.  The rigid-body
# spawn baked the offset into the body origin, but rt_push_external_transforms
# re-teleported the body to the raw entity Transform on the first move, dropping
# the offset so the capsule collapsed onto the feet and a chest-height ray flew
# over it.  Drives jce_runtime_create + a move + a chest raycast.
jce_add_unit_test(test_jce_melee_offset_track
    SOURCES test_jce_melee_offset_track.c
    LINK    JCE)

# SpawnManager P0 (runtime integration): a scene SpawnManager with a
# ped_prefab_path actually instantiates ped prefabs at runtime (was inert —
# counter only).  (Distinct from the middleware/world module unit test.)
jce_add_unit_test(test_jce_runtime_spawn_manager
    SOURCES test_jce_spawn_manager.c
    LINK    JCE)

# Top 4: physics layer collision matrix loaded from JSON before
# jce_runtime_create (the shipped app_init + build-export path) filters runtime
# collisions per body — JSON round-trip + two-box end-to-end separation test.
jce_add_unit_test(test_jce_runtime_layer_matrix
    SOURCES test_jce_runtime_layer_matrix.c
    LINK    JCE)

# Regression: a DYNAMIC body driven by set_velocity must still move while a
# script faces it with set_rotation every frame (AI chase).  rt_push must not
# teleport-discard the velocity motion on rotation-only Transform edits.
jce_add_unit_test(test_jce_runtime_dynamic_rotate_move
    SOURCES test_jce_runtime_dynamic_rotate_move.c
    LINK    JCE)

# Simulation-LOD (large-world P1 #5): rt_tick_gameplay classifies a tiered
# entity by distance to the viewer and gates its Script/Nav/BT updates to the
# active tier's Hz, folding the accumulated dt so the gated logic stays
# time-correct.  Asserts: untiered + near entities tick every frame (no
# regression), a far entity ticks ~1 Hz (the CPU win) yet integrates the full
# wall time (cheap but functional), and a DISABLED SimLod is full-rate.
jce_add_unit_test(test_jce_runtime_sim_lod
    SOURCES test_jce_runtime_sim_lod.c
    LINK    JCE)

# Provider-neutral AI scene orchestration: strict response ingestion, stable
# retries, FrozenPlan cache, and deterministic offline bootstrap fallback.
jce_add_unit_test(test_jce_ai_scene_director
    SOURCES test_jce_ai_scene_director.c
    LINK    jce_core jce_application)

# End-to-end generation coordinator: Director -> FrozenPlan -> isolated scene
# transaction -> explicit frame-boundary commit -> guarded health rollback.
jce_add_unit_test(test_jce_scene_generation
    SOURCES test_jce_scene_generation.c
    LINK    jce_core jce_application)

# Scripted-RPC method allowlist (audit lua-rpc-no-method-allowlist).  The
# dispatcher takes a method name off the wire, so without opt-in a remote peer
# could call on_update / on_destroy / any private helper on any networked
# script.  The predicate is a security boundary; it is tested directly rather
# than only through a live session.
jce_add_unit_test(test_jce_runtime_rpc_allowlist
    SOURCES test_jce_runtime_rpc_allowlist.c
    LINK    JCE)

# Per-tile terrain collision, paged by distance.  Closes a hole rather than
# improving quality: tiled and procedural terrain previously had NO collider
# at all, because the only path built one body from a monolithic height grid
# that a streamed terrain does not have.  The decisive test drops a sphere and
# requires it to land.
jce_add_unit_test(test_jce_terrain_collision_stream
    SOURCES test_jce_terrain_collision_stream.c
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_terrain_collision_stream.c"
            "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_terrain_store.c"
    LINK    jce_core jce_physics jce_resource)
target_include_directories(test_jce_terrain_collision_stream PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/application"
    "${CMAKE_SOURCE_DIR}/engine/src")

# JceSavePointComponent.slot picks the save file.  Exercises rt_save_path
# itself, not a copy of its format string.
jce_add_unit_test(test_jce_save_point_slot
    SOURCES test_jce_save_point_slot.c
    LINK    JCE)
target_include_directories(test_jce_save_point_slot PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src)

# An authored NetworkRigidbody registers transform replication through the
# REAL runtime spawn walk; a bare NetworkObject registers nothing.
jce_add_unit_test(test_jce_net_rigidbody_registers
    SOURCES test_jce_net_rigidbody_registers.c
    LINK    JCE)

# TrailRenderer.time expires points through the REAL runtime step: unset
# keeps the ring, an authored lifetime bounds it, and it drains to zero.
jce_add_unit_test(test_jce_trail_time
    SOURCES test_jce_trail_time.c
    LINK    JCE)

# RigidBody.shape_type picks the placeholder shape for a body with no
# collider component; raycast probes read the silhouette directly.
jce_add_unit_test(test_jce_rb_shape_type
    SOURCES test_jce_rb_shape_type.c
    LINK    JCE)

# RigidBody.shape_type must survive a save and a load: the 3D parse/ser
# pair never touched it, so the inspector control was lost on write.
jce_add_unit_test(test_jce_rb_shape_persist
    SOURCES test_jce_rb_shape_persist.c
    LINK    JCE)

# A RETIRED component field must not survive a save and a load; the
# neighbouring fields in the same round trip are the control.
jce_add_unit_test(test_jce_bt_context_idx_retired
    SOURCES test_jce_bt_context_idx_retired.c
    LINK    JCE)

# Level transition (FEATURE 9.4).  jce_runtime_request_scene had ZERO callers
# anywhere -- no engine, no editor, no script binding -- so the FADE_OUT/LOAD/
# FADE_IN state machine had never executed.  The observable is the scene's
# contents after the swap, not the request's return value.
jce_add_unit_test(test_jce_scene_transition
    SOURCES test_jce_scene_transition.c
    LINK    JCE)

# The engine had NO frame limiter: a shipped game on a menu ran the loop flat
# out, thousands of frames a second, while Project Settings > Quality carried a
# Target Framerate field with nothing to hand it to.  Tests the DECISION (where
# the next deadline goes) with a fake clock -- asserting a capped loop takes
# 16.6 ms would be measuring this machine's scheduler.
jce_add_unit_test(test_jce_engine_frame_cap
    SOURCES test_jce_engine_frame_cap.c
    LINK    JCE)
target_include_directories(test_jce_engine_frame_cap PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# AudioReverbZone carries eleven EAX fields and the runtime mapped nine of
# them: `reverb`, the late reverb LEVEL, was dropped, so the loudest knob on
# the component did nothing.  Pure over the component, so no audio device.
# AudioListener.spatialize was authored, drawn, and deferred in a comment
# ("no public engine API yet") while jce_audio_voice_set_3d had been public and
# in use all along.  The gate is one exported predicate so the two runtime
# sites that must agree share one answer.
jce_add_unit_test(test_jce_listener_spatialize
    SOURCES test_jce_listener_spatialize.c
    LINK    JCE)
target_include_directories(test_jce_listener_spatialize PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

jce_add_unit_test(test_jce_reverb_zone_mapping
    SOURCES test_jce_reverb_zone_mapping.c
    LINK    JCE)
target_include_directories(test_jce_reverb_zone_mapping PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# The save system shipped write-only: jce_runtime_save_to_file with no
# counterpart, and the header told games to drive jce_snapshot_load_from_file
# themselves -- which clears and reloads the scene without re-running the spawn
# walk, leaving entities present and the runtime empty.  The observable here is
# rt->body_count, not entity names: a name check passes against the broken path.
jce_add_unit_test(test_jce_runtime_save_load
    SOURCES test_jce_runtime_save_load.c
    LINK    JCE)
target_include_directories(test_jce_runtime_save_load PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# rt_spawn_entity_body returned false before it looked at any collider, so
# "BoxCollider, no Rigidbody" -- how every engine authors a floor, and what the
# editor's Add Component leaves you with -- produced no collision body at all:
# the player fell through the world while the viewport drew the gizmo.  The
# cooked (Mesh/Compound) path never had the bug.
jce_add_unit_test(test_jce_static_collider_body
    SOURCES test_jce_static_collider_body.c
    LINK    JCE)
target_include_directories(test_jce_static_collider_body PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# jce_physics_overlap_sphere/_box are implemented down to Bullet, honour the
# layer filter, and had ZERO consumers -- so gameplay in all seven languages
# could not ask what was inside a volume and had to fake it with a distance
# loop.  The fixture puts a BODYLESS entity inside the radius: a distance walk
# returns it, a physics query does not, which is the distinction being tested.
jce_add_unit_test(test_jce_script_spatial_query
    SOURCES test_jce_script_spatial_query.c
    LINK    JCE)
target_include_directories(test_jce_script_spatial_query PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# One answer to "what kind of body is this".  There were two, and they
# disagreed for every scene this editor has ever written: both 3D spawn paths
# decide from is_kinematic/mass, while the soft-body static-ground mirror
# decided from JceRigidBodyComponent.body_type -- a field that is never
# parsed, never serialised and has no control, so it is 0 (== JCE_BODY_STATIC)
# everywhere, and every dynamic box became immovable soft-world ground.
jce_add_unit_test(test_jce_rigidbody_kind
    SOURCES test_jce_rigidbody_kind.c
    LINK    JCE)
target_include_directories(test_jce_rigidbody_kind PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# rt_spawn_body2d writes the created body's index back onto the component and
# its comment calls that the "3D contract" -- which the 3D side never
# implemented, so JceRigidBodyComponent.body_handle_idx was 0 in every process
# and the inspector's readout said "(inactive in editor)" for a body that was
# being simulated.  Links the aggregate: this is the real spawn walk.
jce_add_unit_test(test_jce_rigidbody_handle
    SOURCES test_jce_rigidbody_handle.c
    LINK    JCE)

# jce.vcam_activate through the REAL runtime host table.  The mock-host
# binding tests install non-NULL callbacks by construction and so cannot see
# an unassigned JceScriptHost slot -- which is exactly how jce.is_key_down
# shipped returning false for every key in seven languages.
jce_add_unit_test(test_jce_runtime_vcam_activate
    SOURCES test_jce_runtime_vcam_activate.c
    LINK    JCE)

# Doppler's missing half.  The factor was authored, read and pushed to every
# live sound every frame -- and multiplied a velocity that was always zero,
# because jce_audio_voice_set_velocity had no caller anywhere and the
# listener's own velocity was left at {0,0,0} by a memset.  This links
# rt_audio_velocity, the ONE derivation the runtime uses for both ends, so the
# test asks the engine rather than recomputing the formula.
#
# LINKS THE WHOLE ENGINE, like every other runtime-audio test beside it.
# `jce_core jce_application` looked narrower and was wrong: pulling
# rt_audio_velocity drags in jce_rt_audio.c.obj, which drags the rest of the
# runtime's object closure -- nav, localisation, scene -- and the link ended
# with 48 unresolved externals.  A .lib hands over whole OBJECTS, not
# functions, so the link set follows the object's neighbours and not the one
# symbol the test wanted.
jce_add_unit_test(test_jce_audio_doppler_velocity
    SOURCES test_jce_audio_doppler_velocity.c
    LINK    JCE)

# A ConfigurableJoint's break_torque was authored in the inspector, parsed,
# serialised, copied into the runtime's ConfigJointEntry -- and read by
# nothing, because getAppliedImpulse() has no separable angular part.  Driven
# by an authored ConstantForce torque against rt->cfg_joint_count, so the
# claim is "the runtime broke it", not "a primitive exists".  Case 1 MEASURES
# whether the backend's joint feedback carries a timestep instead of
# remembering: Bullet ships headers only here, and the two candidate answers
# are a factor of 60 apart.  Needs engine/src on the include path for the
# runtime struct the way test_jce_static_collider_body does.
jce_add_unit_test(test_jce_joint_break_torque
    SOURCES test_jce_joint_break_torque.c
    LINK    JCE)
target_include_directories(test_jce_joint_break_torque PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
