cmake_minimum_required(VERSION 3.20)

include(render_msaa_boot_controls.cmake)
include(default_scene_boot_controls.cmake)

jce_add_unit_test(test_jce_engine_perf_hitch
    # The test TU includes the exact private owner to inspect bounded internal
    # formatting at its real array boundary; do not compile the owner twice.
    SOURCES test_jce_engine_perf_hitch.c
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

# Remove only the actual wait-to-reporter witness wire. The unchanged fixture
# runs the complete wait owner and raw/phase/lifecycle controls without an OS.
set(_perf_pacing_owner "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_frame_cap.inc.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_perf_pacing_owner}")
file(READ "${_perf_pacing_owner}" _perf_pacing_source)
string(REGEX MATCHALL "jce_engine_perf_hitch_record_pacing[(]state, &pacing[)]"
    _perf_pacing_marks "${_perf_pacing_source}")
list(LENGTH _perf_pacing_marks _perf_pacing_count)
if(NOT _perf_pacing_count EQUAL 1)
    message(FATAL_ERROR "Perf pacing control requires one actual wait-to-observer wire")
endif()
string(REPLACE "    jce_engine_perf_hitch_record_pacing(state, &pacing);"
    "    (void)state; (void)pacing; /* old wire: completed interval loses its actual wait witness */"
    _perf_pacing_old_source "${_perf_pacing_source}")
if(_perf_pacing_old_source STREQUAL _perf_pacing_source)
    message(FATAL_ERROR "Perf pacing control did not remove the actual witness wire")
endif()
set(_perf_pacing_old_path "${CMAKE_CURRENT_BINARY_DIR}/perf-pacing-mutation/jce_engine_frame_cap_old_wire.inc.h")
unset(_perf_pacing_previous)
if(EXISTS "${_perf_pacing_old_path}")
    file(READ "${_perf_pacing_old_path}" _perf_pacing_previous)
endif()
if(NOT _perf_pacing_previous STREQUAL _perf_pacing_old_source)
    file(WRITE "${_perf_pacing_old_path}" "${_perf_pacing_old_source}")
endif()
jce_add_unit_test(test_jce_engine_perf_hitch_old_pacing_wire
    KIND contract
    SOURCES test_jce_engine_perf_hitch.c
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/application")
target_compile_definitions(test_jce_engine_perf_hitch_old_pacing_wire PRIVATE
    JCE_TEST_FRAME_CAP_OWNER="${_perf_pacing_old_path}")
set_tests_properties(test_jce_engine_perf_hitch_old_pacing_wire PROPERTIES WILL_FAIL TRUE)
unset(_perf_pacing_owner)
unset(_perf_pacing_source)
unset(_perf_pacing_marks)
unset(_perf_pacing_count)
unset(_perf_pacing_old_source)
unset(_perf_pacing_old_path)
unset(_perf_pacing_previous)

jce_add_unit_test(test_jce_frame_kpi
    SOURCES test_jce_frame_kpi.c
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_frame_kpi.c"
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_perf_hitch.c"
    LINK jce_core sdl::sdl)
target_include_directories(test_jce_frame_kpi PRIVATE "${CMAKE_SOURCE_DIR}/engine/src")


# The test TU includes the actual private writer once for allocator/terminal
# seams. No mirror queue and no test-only production API; no window or GPU.
jce_add_unit_test(test_jce_frame_kpi_writer
    SOURCES test_jce_frame_kpi_writer.c
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_frame_kpi.c"
    LINK jce_core sdl::sdl)
target_include_directories(test_jce_frame_kpi_writer PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
include("${CMAKE_CURRENT_LIST_DIR}/frame_kpi_writer_controls.cmake")

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
jce_add_unit_test(test_jce_headless_boot KIND integration
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
jce_add_unit_test(test_jce_engine_input_backend KIND integration
    SOURCES test_jce_engine_input_backend.c
    LINK    JCE)
target_include_directories(test_jce_engine_input_backend PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")
# Private configs and existing renderer coordination; no backend workaround.
foreach(_boot_fixture IN ITEMS test_jce_headless_boot test_jce_engine_input_backend)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/engine-boot-output/${_boot_fixture}")
    target_compile_definitions(${_boot_fixture} PRIVATE
        JCE_ENGINE_BOOT_TEST_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/engine-boot-output/${_boot_fixture}")
    set_tests_properties(${_boot_fixture} PROPERTIES RESOURCE_LOCK jce_renderer_context)
endforeach()
unset(_boot_fixture)

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

# Actual Runtime/Lua presentation controls and narrowly missing owner wires.
jce_add_unit_test(test_jce_runtime_viewport
    SOURCES test_jce_runtime_viewport.c LINK JCE)
target_compile_definitions(test_jce_runtime_viewport PRIVATE
    JCE_VIEWPORT_TEST_SCRIPT_FILE="${CMAKE_CURRENT_BINARY_DIR}/runtime-viewport-normal.lua")
set(_viewport_path "${CMAKE_SOURCE_DIR}/engine/src/application/jce_rt_viewport.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_viewport_path}")
file(READ "${_viewport_path}" _viewport_source)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/viewport-mutation")
foreach(_viewport_kind IN ITEMS initial notify)
    if(_viewport_kind STREQUAL "initial")
        set(_viewport_wire "return jce_runtime_set_viewport_size(rt, initial.viewport_width, initial.viewport_height)")
        set(_viewport_old "return jce_runtime_set_viewport_size(rt, 0.0f, 0.0f)")
    else()
        set(_viewport_wire "if (old_width != width || old_height != height) rt_script_viewport_changed(rt)")
        set(_viewport_old "(void)old_width; (void)old_height")
    endif()
    string(FIND "${_viewport_source}" "${_viewport_wire}" _viewport_mark)
    if(_viewport_mark LESS 0)
        message(FATAL_ERROR "Runtime viewport mutation requires its actual ${_viewport_kind} wire")
    endif()
    string(LENGTH "${_viewport_wire}" _viewport_marker_length)
    math(EXPR _viewport_after_marker "${_viewport_mark} + ${_viewport_marker_length}")
    string(SUBSTRING "${_viewport_source}" ${_viewport_after_marker} -1 _viewport_remainder)
    string(FIND "${_viewport_remainder}" "${_viewport_wire}" _viewport_second_marker)
    if(NOT _viewport_second_marker LESS 0)
        message(FATAL_ERROR "Viewport mutation requires one unique actual wire")
    endif()
    string(REPLACE "${_viewport_wire}" "${_viewport_old}" _viewport_mutant "${_viewport_source}")
    set(_viewport_mutant_path "${CMAKE_CURRENT_BINARY_DIR}/viewport-mutation/jce_rt_viewport_old_${_viewport_kind}.c")
    unset(_viewport_previous_generated)
    if(EXISTS "${_viewport_mutant_path}")
        file(READ "${_viewport_mutant_path}" _viewport_previous_generated)
    endif()
    if(NOT _viewport_previous_generated STREQUAL _viewport_mutant)
        file(WRITE "${_viewport_mutant_path}" "${_viewport_mutant}")
    endif()
    unset(_viewport_previous_generated)
    jce_add_unit_test(test_jce_runtime_viewport_old_${_viewport_kind} KIND contract
        SOURCES test_jce_runtime_viewport.c "${_viewport_mutant_path}" LINK JCE
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/application" "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(test_jce_runtime_viewport_old_${_viewport_kind} PRIVATE
        JCE_VIEWPORT_TEST_SCRIPT_FILE="${CMAKE_CURRENT_BINARY_DIR}/runtime-viewport-old-${_viewport_kind}.lua")
    set_tests_properties(test_jce_runtime_viewport_old_${_viewport_kind} PROPERTIES WILL_FAIL TRUE)
endforeach()
unset(_viewport_path)
unset(_viewport_source)
unset(_viewport_kind)
unset(_viewport_wire)
unset(_viewport_old)
unset(_viewport_mark)
unset(_viewport_mutant)
unset(_viewport_mutant_path)

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
# rather than merely parsed. The saver patches its owned fields while retaining
# unknown authoring metadata; unrelated SDK saves must not discard script data.
jce_add_unit_test(test_jce_project_script_modules
    SOURCES test_jce_project_script_modules.c
    LINK    JCE)

# Complete project owner with counted allocation-refusal ports; real JSON/IO.
jce_add_unit_test(test_jce_project_allocation
    SOURCES test_jce_project_allocation.c LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

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

# Compile the same wait owner and fixture with only its WAIT end publication
# omitted. A passing inverse CTest result still needs the ordinary assertion
# failure in LastTest.log; it is not evidence when compilation/crash failed.
file(READ "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_frame_cap.inc.h"
    _frame_wait_source)
string(REGEX MATCHALL "jce_trace_wait_end[(]" _frame_wait_marks "${_frame_wait_source}")
list(LENGTH _frame_wait_marks _frame_wait_count)
set(_frame_wait_end "            jce_trace_wait_end(wait_id, parent_id, \"engine.frame_pacing\",\n                ns >= (double)UINT64_MAX ? UINT64_MAX : (uint64_t)ns);")
string(FIND "${_frame_wait_source}" "${_frame_wait_end}" _frame_wait_at)
if(NOT _frame_wait_count EQUAL 1 OR _frame_wait_at LESS 0)
    message(FATAL_ERROR "Frame pacing wait control requires its sole actual WAIT end wire")
endif()
set(_frame_wait_replacement "            (void)ns; /* control: omit only the actual WAIT end */")
string(REPLACE "${_frame_wait_end}" "${_frame_wait_replacement}"
    _frame_wait_old_source "${_frame_wait_source}")
string(LENGTH "${_frame_wait_source}" _frame_wait_before_bytes)
string(LENGTH "${_frame_wait_old_source}" _frame_wait_after_bytes)
string(LENGTH "${_frame_wait_end}" _frame_wait_end_bytes)
string(LENGTH "${_frame_wait_replacement}" _frame_wait_replacement_bytes)
math(EXPR _frame_wait_expected_bytes
    "${_frame_wait_before_bytes} - ${_frame_wait_end_bytes} + ${_frame_wait_replacement_bytes}")
if(NOT _frame_wait_after_bytes EQUAL _frame_wait_expected_bytes)
    message(FATAL_ERROR "Frame pacing wait control must change only its exact WAIT end wire")
endif()
set(_frame_wait_old_path "${CMAKE_CURRENT_BINARY_DIR}/perf-pacing-mutation/jce_engine_frame_cap_missing_wait_end.inc.h")
unset(_frame_wait_previous)
if(EXISTS "${_frame_wait_old_path}")
    file(READ "${_frame_wait_old_path}" _frame_wait_previous)
endif()
if(NOT _frame_wait_previous STREQUAL _frame_wait_old_source)
    file(WRITE "${_frame_wait_old_path}" "${_frame_wait_old_source}")
endif()
jce_add_unit_test(test_jce_engine_frame_cap_missing_wait_end
    KIND contract
    SOURCES test_jce_engine_frame_cap.c
    LINK JCE
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/application")
target_compile_definitions(test_jce_engine_frame_cap_missing_wait_end PRIVATE
    JCE_TEST_FRAME_CAP_OWNER="${_frame_wait_old_path}")
set_tests_properties(test_jce_engine_frame_cap_missing_wait_end PROPERTIES WILL_FAIL TRUE)
unset(_frame_wait_source)
unset(_frame_wait_marks)
unset(_frame_wait_count)
unset(_frame_wait_end)
unset(_frame_wait_at)
unset(_frame_wait_replacement)
unset(_frame_wait_old_source)
unset(_frame_wait_before_bytes)
unset(_frame_wait_after_bytes)
unset(_frame_wait_end_bytes)
unset(_frame_wait_replacement_bytes)
unset(_frame_wait_expected_bytes)
unset(_frame_wait_old_path)
unset(_frame_wait_previous)

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

# Optional public service bridge: real Lua/runtime, deferred quit and borrowed store.
jce_add_unit_test(test_jce_runtime_script_services
    SOURCES test_jce_runtime_script_services.c
    LINK JCE)

# Production lifecycle registry with allocator/log seams: explicit snapshot OOM.
jce_add_unit_test(test_jce_lifecycle_allocation
    SOURCES test_jce_lifecycle_allocation.c
        "${CMAKE_SOURCE_DIR}/engine/src/application/jce_lifecycle.c")

# Bootstrap scopes consume the same public CPU trace authority.
jce_add_unit_test(test_jce_engine_trace_scope
    SOURCES test_jce_engine_trace_scope.c
    LINK jce_core)
target_include_directories(test_jce_engine_trace_scope PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Exact callback dispatch shared with the engine host; no window or GPU.
jce_add_unit_test(test_jce_engine_callbacks
    SOURCES test_jce_engine_callbacks.c
    LINK jce_core)
target_include_directories(test_jce_engine_callbacks PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Real headless engine stream hooks and post-join producer-tail evidence.
jce_add_unit_test(test_jce_engine_trace_stream
    KIND integration
    SOURCES test_jce_engine_trace_stream.c
    LINK JCE)
target_compile_definitions(test_jce_engine_trace_stream PRIVATE
    JCE_TRACE_TEST_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/trace-stream-output")

# Supply environment before CRT initialization; changing another DLL's CRT
# environment in-process does not configure the engine's libc getenv view.
set(_trace_test_output "${CMAKE_CURRENT_BINARY_DIR}/trace-stream-output")
file(MAKE_DIRECTORY "${_trace_test_output}")
set(_trace_test_env "${JCE_TEST_ENVIRONMENT};JCE_TRACE=;JCE_TRACE_EXPORT=;JCE_PERF_LOG=;JCE_MAX_FRAMES=")
set_tests_properties(test_jce_engine_trace_stream PROPERTIES
    ENVIRONMENT "${_trace_test_env};JCE_TRACE_INTEGRATION_CASE=default;JCE_TRACE_STREAM=;JCE_TRACE_STREAM_MODE=")
# Exercise explicit trace-off through the actual engine bootstrap, even when
# frame telemetry requests automatic tracing. Keep a separate zero-activity assertion.
add_test(NAME test_jce_engine_trace_stream_trace_disabled
    COMMAND test_jce_engine_trace_stream)
set_tests_properties(test_jce_engine_trace_stream_trace_disabled PROPERTIES
    TIMEOUT 60 LABELS "unit" JCE_TEST_KIND integration
    ENVIRONMENT "${_trace_test_env};JCE_TRACE_INTEGRATION_CASE=disabled;JCE_TRACE=0;JCE_PERF_LOG=1;JCE_TRACE_STREAM=;JCE_TRACE_STREAM_MODE=")
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV
    test_jce_engine_trace_stream_trace_disabled)
# Every variant runs this same executable and must reach the unit build target.
jce_test_build_dependencies(test_jce_engine_trace_stream)
foreach(_trace_mode threaded cooperative invalid)
    set(_trace_test_name "test_jce_engine_trace_stream_${_trace_mode}")
    add_test(NAME ${_trace_test_name} COMMAND test_jce_engine_trace_stream)
    if(_trace_mode STREQUAL "invalid")
        set(_trace_test_path "${_trace_test_output}/blocker/trace.json")
    else()
        set(_trace_test_path "${_trace_test_output}/${_trace_mode}.json")
    endif()
    if(_trace_mode STREQUAL "invalid")
        set(_trace_consumer_mode threaded)
    else()
        set(_trace_consumer_mode "${_trace_mode}")
    endif()
    set_tests_properties(${_trace_test_name} PROPERTIES TIMEOUT 60 LABELS "unit" JCE_TEST_KIND integration
        ENVIRONMENT "${_trace_test_env};JCE_TRACE_INTEGRATION_CASE=${_trace_mode};JCE_TRACE_STREAM=${_trace_test_path};JCE_TRACE_STREAM_MODE=${_trace_consumer_mode}")
    set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV ${_trace_test_name})
endforeach()
unset(_trace_test_env)
unset(_trace_test_output)
unset(_trace_test_name)
unset(_trace_test_path)
unset(_trace_mode)
unset(_trace_consumer_mode)

# Actual runtime/Lua bounded mounted-content reads and isolated host-miss policy.
jce_add_unit_test(test_jce_runtime_script_vfs
    KIND integration
    SOURCES test_jce_runtime_script_vfs.c
    LINK JCE)

# Same native counter ledger only; no application, rendering or new clock port.
jce_add_unit_test(test_jce_engine_kpi_clock
    SOURCES test_jce_engine_kpi_clock.c
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
file(READ "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_kpi_clock.h" _kpi_clock_source)
string(REGEX MATCHALL "\\+\\+clock->render_intervals" _kpi_clock_wires "${_kpi_clock_source}")
list(LENGTH _kpi_clock_wires _kpi_clock_wire_count)
if(NOT _kpi_clock_wire_count EQUAL 1)
    message(FATAL_ERROR "KPI clock accepted-row mutation requires one actual counter wire")
endif()
string(REPLACE "++clock->render_intervals;" "(void)row;"
    _kpi_clock_mutated "${_kpi_clock_source}")
if(_kpi_clock_mutated STREQUAL _kpi_clock_source)
    message(FATAL_ERROR "KPI clock accepted-row mutation did not change its owner")
endif()
set(_kpi_clock_old_header "${CMAKE_CURRENT_BINARY_DIR}/jce_engine_kpi_clock_old_count_wire.h")
set(_kpi_clock_old_previous "")
if(EXISTS "${_kpi_clock_old_header}")
    file(READ "${_kpi_clock_old_header}" _kpi_clock_old_previous)
endif()
if(NOT _kpi_clock_old_previous STREQUAL _kpi_clock_mutated)
    file(WRITE "${_kpi_clock_old_header}" "${_kpi_clock_mutated}")
endif()
jce_add_unit_test(test_jce_engine_kpi_clock_old_count_wire
    KIND contract
    SOURCES test_jce_engine_kpi_clock.c
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
target_compile_definitions(test_jce_engine_kpi_clock_old_count_wire PRIVATE
    JCE_TEST_ENGINE_KPI_CLOCK_HEADER="${_kpi_clock_old_header}")
set_tests_properties(test_jce_engine_kpi_clock_old_count_wire PROPERTIES WILL_FAIL TRUE)
unset(_kpi_clock_source)
unset(_kpi_clock_wires)
unset(_kpi_clock_wire_count)
unset(_kpi_clock_mutated)
unset(_kpi_clock_old_header)
unset(_kpi_clock_old_previous)

# Restore only the old zero-is-uninitialized guard in the actual arithmetic
# owner. Same runtime assertions must fail normally after successful compile.
file(READ "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_kpi_clock.h" _kpi_zero_source)
string(REGEX MATCHALL "if \\(!out_ms \\|\\| !frequency \\|\\| end < begin\\)"
    _kpi_zero_wires "${_kpi_zero_source}")
list(LENGTH _kpi_zero_wires _kpi_zero_count)
if(NOT _kpi_zero_count EQUAL 1)
    message(FATAL_ERROR "KPI wall zero-boundary mutation requires one actual arithmetic guard")
endif()
string(REPLACE "if (!out_ms || !frequency || end < begin) return false;"
    "if (!out_ms || !frequency || begin == 0 || end < begin) return false;"
    _kpi_zero_mutated "${_kpi_zero_source}")
if(_kpi_zero_mutated STREQUAL _kpi_zero_source)
    message(FATAL_ERROR "KPI wall zero-boundary mutation did not change its owner")
endif()
set(_kpi_zero_header "${CMAKE_CURRENT_BINARY_DIR}/jce_engine_kpi_clock_old_zero_guard.h")
set(_kpi_zero_previous "")
if(EXISTS "${_kpi_zero_header}")
    file(READ "${_kpi_zero_header}" _kpi_zero_previous)
endif()
if(NOT _kpi_zero_previous STREQUAL _kpi_zero_mutated)
    file(WRITE "${_kpi_zero_header}" "${_kpi_zero_mutated}")
endif()
jce_add_unit_test(test_jce_engine_kpi_clock_old_zero_guard
    KIND contract
    SOURCES test_jce_engine_kpi_clock.c
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
target_compile_definitions(test_jce_engine_kpi_clock_old_zero_guard PRIVATE
    JCE_TEST_ENGINE_KPI_CLOCK_HEADER="${_kpi_zero_header}")
set_tests_properties(test_jce_engine_kpi_clock_old_zero_guard PROPERTIES WILL_FAIL TRUE)
unset(_kpi_zero_source)
unset(_kpi_zero_wires)
unset(_kpi_zero_count)
unset(_kpi_zero_mutated)
unset(_kpi_zero_header)
unset(_kpi_zero_previous)

# Root copies reviewed v04 fixtures to tests/application and integrates actual TUs.
jce_add_unit_test(test_jce_engine_kpi_capture
    SOURCES test_jce_engine_kpi_capture.c
        "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_perf_hitch.c"
        "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_kpi_capture.c"
        "${CMAKE_SOURCE_DIR}/engine/src/application/jce_frame_kpi.c"
        "${CMAKE_SOURCE_DIR}/engine/src/application/jce_frame_kpi_writer.c"
    LINK jce_core sdl::sdl)
target_include_directories(test_jce_engine_kpi_capture PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src"
    "${CMAKE_SOURCE_DIR}/engine/src/application")
target_compile_definitions(test_jce_engine_kpi_capture PRIVATE
    JCE_KPI_CONTEXT_SELECTOR_PATH="${CMAKE_CURRENT_BINARY_DIR}/test_jce_engine_kpi_capture-context-selector.csv")

# Explicit completion witnesses must survive shipping INFO macro stripping.
# The runtime mutant changes only the actual capture footer back to LOG_INFO;
# its compiled process must fail the same real writer/logger assertions.
set(_kpi_capture_owner "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_kpi_capture.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_kpi_capture_owner}")
file(READ "${_kpi_capture_owner}" _kpi_capture_source)
string(REGEX MATCHALL "frame KPI capture complete: accepted=%llu written=%llu"
    _kpi_footer_matches "${_kpi_capture_source}")
list(LENGTH _kpi_footer_matches _kpi_footer_count)
set(_kpi_footer_wire [=[jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
                "frame KPI capture complete: accepted=%llu written=%llu "]=])
set(_kpi_footer_old_wire [=[LOG_INFO(LOG_TAG,
                "frame KPI capture complete: accepted=%llu written=%llu "]=])
string(FIND "${_kpi_capture_source}" "${_kpi_footer_wire}" _kpi_footer_offset)
if(NOT _kpi_footer_count EQUAL 1 OR _kpi_footer_offset LESS 0)
    message(FATAL_ERROR "KPI capture DIST footer mutation requires one actual direct-log wire")
endif()
string(REPLACE "${_kpi_footer_wire}" "${_kpi_footer_old_wire}"
    _kpi_capture_mutated "${_kpi_capture_source}")
if(_kpi_capture_mutated STREQUAL _kpi_capture_source)
    message(FATAL_ERROR "KPI capture DIST footer mutation did not change the actual owner")
endif()
set(_kpi_capture_old_owner "${CMAKE_CURRENT_BINARY_DIR}/jce_engine_kpi_capture_old_info.c")
set(_kpi_capture_old_previous "")
if(EXISTS "${_kpi_capture_old_owner}")
    file(READ "${_kpi_capture_old_owner}" _kpi_capture_old_previous)
endif()
if(NOT _kpi_capture_old_previous STREQUAL _kpi_capture_mutated)
    file(WRITE "${_kpi_capture_old_owner}" "${_kpi_capture_mutated}")
endif()
function(_jce_kpi_capture_dist_fixture NAME OWNER KIND)
    jce_add_unit_test(${NAME}
        KIND ${KIND}
        SOURCES test_jce_engine_kpi_capture.c "${OWNER}"
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_perf_hitch.c"
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_frame_kpi.c"
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_frame_kpi_writer.c"
        LINK jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/application")
    target_compile_definitions(${NAME} PRIVATE JCE_DIST=1)
    target_compile_definitions(${NAME} PRIVATE
        JCE_KPI_CONTEXT_SELECTOR_PATH="${CMAKE_CURRENT_BINARY_DIR}/${NAME}-context-selector.csv")
endfunction()
_jce_kpi_capture_dist_fixture(test_jce_engine_kpi_capture_dist
    "${_kpi_capture_owner}" integration)
_jce_kpi_capture_dist_fixture(test_jce_engine_kpi_capture_dist_old_info
    "${_kpi_capture_old_owner}" contract)
set_tests_properties(test_jce_engine_kpi_capture_dist_old_info PROPERTIES WILL_FAIL TRUE)

# The context header remains selected while the real capture owner loses only
# its encoder-context argument. The unchanged fixture must fail ordinary CSV
# and queued-sample assertions after this complete owner still compiles.
set(_kpi_context_encode_wire "capture->context_enabled ? &capture->context : NULL, &size)")
string(FIND "${_kpi_capture_source}" "${_kpi_context_encode_wire}" _kpi_context_encode_first)
string(FIND "${_kpi_capture_source}" "${_kpi_context_encode_wire}" _kpi_context_encode_last REVERSE)
if(_kpi_context_encode_first LESS 0 OR NOT _kpi_context_encode_first EQUAL _kpi_context_encode_last)
    message(FATAL_ERROR "KPI context control requires one actual encoder-context wire")
endif()
string(REPLACE "${_kpi_context_encode_wire}" "NULL, &size)"
    _kpi_context_old_source "${_kpi_capture_source}")
set(_kpi_context_old_owner "${CMAKE_CURRENT_BINARY_DIR}/jce_engine_kpi_capture_old_context.c")
if(EXISTS "${_kpi_context_old_owner}")
    file(READ "${_kpi_context_old_owner}" _kpi_context_previous)
else()
    unset(_kpi_context_previous)
endif()
if(NOT _kpi_context_previous STREQUAL _kpi_context_old_source)
    file(WRITE "${_kpi_context_old_owner}" "${_kpi_context_old_source}")
endif()
_jce_kpi_capture_dist_fixture(test_jce_engine_kpi_capture_old_context_wire
    "${_kpi_context_old_owner}" contract)
set_tests_properties(test_jce_engine_kpi_capture_old_context_wire PROPERTIES WILL_FAIL TRUE)
unset(_kpi_context_encode_wire)
unset(_kpi_context_encode_first)
unset(_kpi_context_encode_last)
unset(_kpi_context_old_source)
unset(_kpi_context_old_owner)
unset(_kpi_context_previous)
unset(_kpi_capture_owner)
unset(_kpi_capture_source)
unset(_kpi_footer_matches)
unset(_kpi_footer_count)
unset(_kpi_footer_wire)
unset(_kpi_footer_old_wire)
unset(_kpi_footer_offset)
unset(_kpi_capture_mutated)
unset(_kpi_capture_old_owner)
unset(_kpi_capture_old_previous)

# The Engine helper/release bodies and their real renderer adjacency are
# consumed with counted typed CPU ports. Every mutation retains declarations
# and removes one actual call/state wire, so failure must be behavioral.
set(_frame_context_owner "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_frame_context_owner}")
file(READ "${_frame_context_owner}" _frame_context_source)
function(_jce_frame_context_slice START END OUT)
    string(FIND "${_frame_context_source}" "${START}" _first)
    string(FIND "${_frame_context_source}" "${START}" _first_last REVERSE)
    string(FIND "${_frame_context_source}" "${END}" _last)
    string(FIND "${_frame_context_source}" "${END}" _last_last REVERSE)
    if(_first LESS 0 OR _last LESS_EQUAL _first OR NOT _first EQUAL _first_last OR NOT _last EQUAL _last_last)
        message(FATAL_ERROR "Engine frame-context fixture requires unique ordered actual owner markers")
    endif()
    math(EXPR _length "${_last} - ${_first}")
    string(SUBSTRING "${_frame_context_source}" ${_first} ${_length} _body)
    set(${OUT} "${_body}" PARENT_SCOPE)
endfunction()
_jce_frame_context_slice("static void engine_frame_context_begin(JceEngine *e)"
    "JceAppResult jce_engine_iterate(JceEngine *e)" _frame_context_helpers_release)
_jce_frame_context_slice("static double engine_frame_wall_ms(const JceEngine *e, uint64_t boundary)"
    "static void engine_report_kpi_clock(JceEngine *e, uint64_t records)" _frame_context_completed)
_jce_frame_context_slice("static void engine_report_kpi_clock(JceEngine *e, uint64_t records)"
    "/* Terminal events can finish capture outside an iterate interval. Guard the" _frame_context_clock)
_jce_frame_context_slice("    const uint64_t frame_kpi_records = e->frame_kpi.index;"
    "    JceTraceCapture *trace_capture = e->trace_capture;" _frame_context_direct)
set(_frame_context_begin_wire "    engine_frame_context_begin(e);\n    jce_renderer_begin_frame(e->renderer, e->window);\n")
string(FIND "${_frame_context_source}" "${_frame_context_begin_wire}" _frame_context_begin_first)
string(FIND "${_frame_context_source}" "${_frame_context_begin_wire}" _frame_context_begin_last REVERSE)
if(_frame_context_begin_first LESS 0 OR NOT _frame_context_begin_first EQUAL _frame_context_begin_last)
    message(FATAL_ERROR "Engine context begin must directly precede the actual renderer begin")
endif()
_jce_frame_context_slice("    e->frame_kpi.viewport_draw_active = engine_owner_ready(e) &&"
    "    jce_perf_phase_add(\"app_update\"," _frame_context_draw)
set(_frame_context_end_wire "    engine_frame_context_end(e);\n")
_jce_frame_context_slice("    {\n        /* end_frame includes frame submission and any API-thread wait."
    "${_frame_context_end_wire}" _frame_context_end_prefix)
set(_frame_context_end_body "${_frame_context_end_prefix}${_frame_context_end_wire}")
set(_frame_context_seal_wire "    if (full && jce_engine_kpi_capture_context_active(&e->frame_kpi))\n        e->frame_kpi.context_sealed = e->frame_kpi.context_begun && e->frame_kpi.context_ended;")
string(FIND "${_frame_context_helpers_release}" "${_frame_context_seal_wire}" _frame_context_seal_first)
string(FIND "${_frame_context_helpers_release}" "${_frame_context_seal_wire}" _frame_context_seal_last REVERSE)
if(_frame_context_seal_first LESS 0 OR NOT _frame_context_seal_first EQUAL _frame_context_seal_last)
    message(FATAL_ERROR "Engine frame context needs one actual full-render seal wire")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/frame-context-owner")
function(_jce_frame_context_fixture NAME HELPERS BEGIN END KIND)
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/frame-context-owner/${NAME}.inc.h")
    set(_content "/* Generated exact production helpers/release and actual renderer wires. */\n${_frame_context_completed}\n${_frame_context_clock}\n${HELPERS}\nstatic void test_context_app_draw(JceEngine *e)\n{\n${_frame_context_draw}}\nstatic void test_context_render_begin(JceEngine *e)\n{\n${BEGIN}}\nstatic JceAppResult test_context_render_end(JceEngine *e)\n{\n${END}    return JCE_APP_CONTINUE;\n}\nstatic bool test_context_direct_capture_close(JceEngine *e, uint64_t terminal_boundary)\n{\n    const JceEngineTraceScope cleanup_trace = {0};\n${_frame_context_direct}    return true;\n}\n")
    if(EXISTS "${_path}")
        file(READ "${_path}" _previous)
    else()
        unset(_previous)
    endif()
    if(NOT _previous STREQUAL _content)
        file(WRITE "${_path}" "${_content}")
    endif()
    jce_add_unit_test(${NAME} KIND ${KIND}
        SOURCES test_jce_engine_frame_context.c
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_kpi_capture.c"
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_perf_hitch.c"
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_frame_kpi.c"
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_frame_kpi_writer.c"
        LINK jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(${NAME} PRIVATE JCE_ENGINE_FRAME_CONTEXT_OWNER="${_path}")
endfunction()
_jce_frame_context_fixture(test_jce_engine_frame_context
    "${_frame_context_helpers_release}" "${_frame_context_begin_wire}" "${_frame_context_end_body}" integration)
set(_frame_context_actual_completed "${_frame_context_completed}")
set(_frame_context_clock_wire "    if (e->frame_kpi.context_version == 3 &&\n        !jce_engine_kpi_capture_interval(&e->frame_kpi, e->perf_freq, interval_start, boundary)) return false;")
string(FIND "${_frame_context_completed}" "${_frame_context_clock_wire}" _frame_context_clock_first)
string(FIND "${_frame_context_completed}" "${_frame_context_clock_wire}" _frame_context_clock_last REVERSE)
if(_frame_context_clock_first LESS 0 OR NOT _frame_context_clock_first EQUAL _frame_context_clock_last)
    message(FATAL_ERROR "Frame clock negative requires one actual completed-interval wire")
endif()
string(REPLACE "${_frame_context_clock_wire}" "    /* Removed actual v3 completed-clock wire. */"
    _frame_context_completed "${_frame_context_actual_completed}")
_jce_frame_context_fixture(test_jce_engine_frame_context_missing_clock
    "${_frame_context_helpers_release}" "${_frame_context_begin_wire}" "${_frame_context_end_body}" contract)
set_tests_properties(test_jce_engine_frame_context_missing_clock PROPERTIES WILL_FAIL TRUE)
set(_frame_context_completed "${_frame_context_actual_completed}")
set(_frame_context_actual_direct "${_frame_context_direct}")
string(REPLACE "engine_report_kpi_clock(e, frame_kpi_records);"
    "(void)frame_kpi_records; engine_report_kpi_clock(e, e->frame_kpi.index); /* Former reset-index observation. */"
    _frame_context_direct "${_frame_context_actual_direct}")
if(_frame_context_direct STREQUAL _frame_context_actual_direct)
    message(FATAL_ERROR "Direct clock ordinal negative did not change the actual reporter wire")
endif()
_jce_frame_context_fixture(test_jce_engine_frame_context_old_clock_ordinal
    "${_frame_context_helpers_release}" "${_frame_context_begin_wire}" "${_frame_context_end_body}" contract)
set_tests_properties(test_jce_engine_frame_context_old_clock_ordinal PROPERTIES WILL_FAIL TRUE)
set(_frame_context_direct "${_frame_context_actual_direct}")
string(REPLACE "    engine_frame_context_begin(e);" "    (void)engine_frame_context_begin;"
    _frame_context_old_begin "${_frame_context_begin_wire}")
_jce_frame_context_fixture(test_jce_engine_frame_context_missing_begin
    "${_frame_context_helpers_release}" "${_frame_context_old_begin}" "${_frame_context_end_body}" contract)
string(REPLACE "${_frame_context_end_wire}" "    (void)engine_frame_context_end;\n"
    _frame_context_old_end "${_frame_context_end_body}")
_jce_frame_context_fixture(test_jce_engine_frame_context_missing_end
    "${_frame_context_helpers_release}" "${_frame_context_begin_wire}" "${_frame_context_old_end}" contract)
string(REPLACE "${_frame_context_seal_wire}" "    (void)e; /* Removed actual completed-context seal. */"
    _frame_context_old_seal "${_frame_context_helpers_release}")
_jce_frame_context_fixture(test_jce_engine_frame_context_missing_seal
    "${_frame_context_old_seal}" "${_frame_context_begin_wire}" "${_frame_context_end_body}" contract)
set_tests_properties(test_jce_engine_frame_context_missing_begin
    test_jce_engine_frame_context_missing_end test_jce_engine_frame_context_missing_seal
    PROPERTIES WILL_FAIL TRUE)
unset(_frame_context_completed)
unset(_frame_context_actual_completed)
unset(_frame_context_clock_wire)
unset(_frame_context_clock_first)
unset(_frame_context_clock_last)
unset(_frame_context_owner)
unset(_frame_context_source)
unset(_frame_context_helpers_release)
unset(_frame_context_clock)
unset(_frame_context_direct)
unset(_frame_context_actual_direct)
unset(_frame_context_begin_wire)
unset(_frame_context_begin_first)
unset(_frame_context_begin_last)
unset(_frame_context_end_wire)
unset(_frame_context_end_prefix)
unset(_frame_context_end_body)
unset(_frame_context_seal_wire)
unset(_frame_context_seal_first)
unset(_frame_context_seal_last)
unset(_frame_context_old_begin)
unset(_frame_context_old_end)
unset(_frame_context_old_seal)

# Real public Engine owner. Existing unit helper supplies the build dependency,
# Unity, baseline test environment and registration; this is not a mirror TU.
jce_add_unit_test(test_jce_engine_kpi_lifecycle
    KIND integration
    SOURCES test_jce_engine_kpi_lifecycle.c
    LINK JCE sdl::sdl)
set(_kpi_lifecycle_output "${CMAKE_CURRENT_BINARY_DIR}/engine-kpi-lifecycle-output")
file(MAKE_DIRECTORY "${_kpi_lifecycle_output}")
target_compile_definitions(test_jce_engine_kpi_lifecycle PRIVATE
    JCE_ENGINE_KPI_TEST_OUTPUT="${_kpi_lifecycle_output}")
set(_kpi_lifecycle_env "${JCE_TEST_ENVIRONMENT};SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;JCE_HEADLESS=1;JCE_BACKEND=noop;JCE_TRACE=0;JCE_TRACE_STREAM=;JCE_TRACE_EXPORT=;JCE_PERF_LOG=0;JCE_MAX_FRAMES=;JCE_FRAME_DT_FIXED=0.001;JCE_KPI_ASSET_LOG=;JCE_KPI_FRAME_COUNT=0;JCE_KPI_DURABLE_FLUSH_INTERVAL=0;JCE_FRAME_LOG_CONTEXT=")
set_tests_properties(test_jce_engine_kpi_lifecycle PROPERTIES
    ENVIRONMENT "${_kpi_lifecycle_env};JCE_ENGINE_KPI_LIFECYCLE_CASE=terminal;JCE_KPI_FRAME_LOG=${_kpi_lifecycle_output}/terminal.csv;JCE_LOG_FILE=${_kpi_lifecycle_output}/terminal.log")
add_test(NAME test_jce_engine_kpi_lifecycle_exit_reentry
    COMMAND test_jce_engine_kpi_lifecycle)
set_tests_properties(test_jce_engine_kpi_lifecycle_exit_reentry PROPERTIES
    TIMEOUT 60 LABELS "unit" JCE_TEST_KIND integration
    ENVIRONMENT "${_kpi_lifecycle_env};JCE_ENGINE_KPI_LIFECYCLE_CASE=exit;JCE_KPI_FRAME_LOG=${_kpi_lifecycle_output}/exit.csv;JCE_LOG_FILE=${_kpi_lifecycle_output}/exit.log")
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV
    test_jce_engine_kpi_lifecycle_exit_reentry)
# Ordinary callbacks may request destruction while a frame/event/executor
# still borrows the owner. Each real-engine case starts in a fresh process.
foreach(_kpi_lifecycle_case completion update event manual)
    set(_kpi_lifecycle_name "test_jce_engine_kpi_lifecycle_${_kpi_lifecycle_case}")
    add_test(NAME ${_kpi_lifecycle_name} COMMAND test_jce_engine_kpi_lifecycle)
    set_tests_properties(${_kpi_lifecycle_name} PROPERTIES
        TIMEOUT 60 LABELS "unit" JCE_TEST_KIND integration
        ENVIRONMENT "${_kpi_lifecycle_env};JCE_ENGINE_KPI_LIFECYCLE_CASE=${_kpi_lifecycle_case};JCE_KPI_FRAME_LOG=${_kpi_lifecycle_output}/${_kpi_lifecycle_case}.csv;JCE_LOG_FILE=${_kpi_lifecycle_output}/${_kpi_lifecycle_case}.log")
    set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV ${_kpi_lifecycle_name})
endforeach()
unset(_kpi_lifecycle_case)
unset(_kpi_lifecycle_name)
include(engine_interval_controls.cmake)
unset(_kpi_lifecycle_env)
unset(_kpi_lifecycle_output)

# Real Engine owner plus the actual sampler TU with a deterministic native port.
# No worker, GPU, media device or duplicate application implementation is used.
jce_add_unit_test(test_jce_engine_sampler_lifetime
    KIND integration
    SOURCES test_jce_engine_sampler_lifetime.c
    LINK JCE sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/tests/os/core/stubs")
set(_sampler_lifetime_output "${CMAKE_CURRENT_BINARY_DIR}/engine-sampler-lifetime-output")
file(MAKE_DIRECTORY "${_sampler_lifetime_output}")
target_compile_definitions(test_jce_engine_sampler_lifetime PRIVATE
    JCE_ENGINE_SAMPLER_TEST_OUTPUT="${_sampler_lifetime_output}")
set(_sampler_lifetime_env "${JCE_TEST_ENVIRONMENT};SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;JCE_HEADLESS=1;JCE_BACKEND=noop;JCE_TRACE=0;JCE_TRACE_STREAM=;JCE_TRACE_EXPORT=;JCE_PERF_LOG=0;JCE_MAX_FRAMES=;JCE_KPI_FRAME_LOG=;JCE_KPI_ASSET_LOG=;JCE_SAMPLER=;JCE_KPI_FRAME_COUNT=0")
set_tests_properties(test_jce_engine_sampler_lifetime PROPERTIES
    ENVIRONMENT "${_sampler_lifetime_env};JCE_ENGINE_SAMPLER_CASE=timeout;JCE_LOG_FILE=${_sampler_lifetime_output}/timeout.log")
foreach(_sampler_lifetime_case wait_failed resume_failed)
    set(_sampler_lifetime_name "test_jce_engine_sampler_lifetime_${_sampler_lifetime_case}")
    add_test(NAME ${_sampler_lifetime_name} COMMAND test_jce_engine_sampler_lifetime)
    set_tests_properties(${_sampler_lifetime_name} PROPERTIES
        TIMEOUT 60 LABELS "unit" JCE_TEST_KIND integration
        ENVIRONMENT "${_sampler_lifetime_env};JCE_ENGINE_SAMPLER_CASE=${_sampler_lifetime_case};JCE_LOG_FILE=${_sampler_lifetime_output}/${_sampler_lifetime_case}.log")
    set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV ${_sampler_lifetime_name})
endforeach()
unset(_sampler_lifetime_case)
unset(_sampler_lifetime_name)
unset(_sampler_lifetime_env)
unset(_sampler_lifetime_output)
include("${CMAKE_CURRENT_LIST_DIR}/cleanup_owner_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/async_retirement_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/host_cleanup_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/terminal_ack_controls.cmake")

# Exact quit-result latch owner controls, not actual SDL process exit evidence.
jce_add_unit_test(test_jce_engine_quit
    SOURCES test_jce_engine_quit.c
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_quit.c"
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

# Real public headless bootstrap: no window/audio/device, real first schedulers.
function(_jce_engine_worker_budget_fixture NAME KIND)
    jce_add_unit_test(${NAME}
        KIND ${KIND}
        SOURCES test_jce_engine_worker_budget.c ${ARGN}
        LINK JCE sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/application")
    set(_worker_output "${CMAKE_CURRENT_BINARY_DIR}/${NAME}-output")
    file(MAKE_DIRECTORY "${_worker_output}")
    target_compile_definitions(${NAME} PRIVATE JCE_TEST_WORKER_OUTPUT="${_worker_output}")
    set_tests_properties(${NAME} PROPERTIES RUN_SERIAL TRUE RESOURCE_LOCK jce_renderer_context
        ENVIRONMENT "${JCE_TEST_ENVIRONMENT};SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;JCE_HEADLESS=1;JCE_BACKEND=noop;JCE_LOW_MEM=;JCE_TRACE=0;JCE_TRACE_EXPORT=;JCE_TRACE_STREAM=;JCE_PERF_LOG=0;JCE_KPI_FRAME_LOG=;JCE_MAX_SECONDS=;JCE_MAX_FRAMES=;JCE_SAMPLER=")
endfunction()
_jce_engine_worker_budget_fixture(test_jce_engine_worker_budget integration)

# Compile the actual engine owner with only its effective worker wire removed.
file(READ "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine.c" _engine_worker_owner)
string(REGEX MATCHALL "e->config.job_workers, e->low_memory[)]"
    _engine_worker_wires "${_engine_worker_owner}")
list(LENGTH _engine_worker_wires _engine_worker_wire_count)
if(NOT _engine_worker_wire_count EQUAL 1)
    message(FATAL_ERROR "Engine worker control requires one actual effective worker wire")
endif()
string(REPLACE "e->config.job_workers, e->low_memory)"
    "e->config.job_workers, e->config.machine_class == JCE_MACHINE_CLASS_LOW)"
    _engine_worker_mutant "${_engine_worker_owner}")
if(_engine_worker_mutant STREQUAL _engine_worker_owner)
    message(FATAL_ERROR "Engine worker control did not remove the effective worker wire")
endif()
set(_engine_worker_mutant_path "${CMAKE_CURRENT_BINARY_DIR}/jce_engine_old_worker_publish.c")
jce_test_write_if_changed("${_engine_worker_mutant_path}" "${_engine_worker_mutant}")
# Match the production engine TU context; the fixture remains a public consumer.
set_property(SOURCE "${_engine_worker_mutant_path}" APPEND PROPERTY
    COMPILE_DEFINITIONS JCE_BUILDING_ENGINE=1)
_jce_engine_worker_budget_fixture(test_jce_engine_worker_budget_old_publish
    contract "${_engine_worker_mutant_path}")
target_link_libraries(test_jce_engine_worker_budget_old_publish PRIVATE bgfx::bgfx)
if(MSVC)
    target_compile_options(test_jce_engine_worker_budget_old_publish PRIVATE /Zc:preprocessor)
endif()
set_tests_properties(test_jce_engine_worker_budget_old_publish PROPERTIES WILL_FAIL TRUE)
unset(_engine_worker_owner)
unset(_engine_worker_wires)
unset(_engine_worker_wire_count)
unset(_engine_worker_mutant)
unset(_engine_worker_mutant_path)

# Actual lifecycle registry + common stock-entry/Play policy, with faultable
# allocator/FPS/audio ports. No native engine/window or physical GPU starts.
# New background permission controls compile its actual existing host state
# and public accessors, not a test mirror of the process-global property.
set(_background_property_owner "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_frame_cap.inc.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_background_property_owner}")
file(READ "${_background_property_owner}" _background_property_source)
function(_jce_background_owner_body BEGIN END OUT)
    string(FIND "${_background_property_source}" "${BEGIN}" _begin)
    string(FIND "${_background_property_source}" "${BEGIN}" _last_begin REVERSE)
    string(FIND "${_background_property_source}" "${END}" _end)
    string(FIND "${_background_property_source}" "${END}" _last_end REVERSE)
    if(_begin LESS 0 OR _end LESS 0 OR NOT _begin EQUAL _last_begin OR
       NOT _end EQUAL _last_end OR NOT _end GREATER _begin)
        message(FATAL_ERROR "Background permission requires the complete actual owner bounds")
    endif()
    math(EXPR _length "${_end}-${_begin}")
    string(SUBSTRING "${_background_property_source}" ${_begin} ${_length} _body)
    set(${OUT} "${_body}" PARENT_SCOPE)
endfunction()
_jce_background_owner_body("static struct {" "void jce_engine_set_target_fps(" _background_property_state)
_jce_background_owner_body("void jce_engine_set_run_in_background(" "uint64_t jce_engine_frame_cap_advance(" _background_property_accessors)
set(_background_property_header "${CMAKE_CURRENT_BINARY_DIR}/host-policy-mutation/background_property.inc.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/host-policy-mutation")
set(_background_property_content "${_background_property_state}
${_background_property_accessors}")
unset(_background_property_previous)
if(EXISTS "${_background_property_header}")
    file(READ "${_background_property_header}" _background_property_previous)
endif()
if(NOT _background_property_previous STREQUAL _background_property_content)
    file(WRITE "${_background_property_header}" "${_background_property_content}")
endif()

function(_jce_runtime_host_policy_fixture target kind)
    jce_add_unit_test(${target} KIND ${kind}
        SOURCES test_jce_runtime_host_policy.c
                "${CMAKE_SOURCE_DIR}/engine/src/application/jce_lifecycle.c")
    target_compile_definitions(${target} PRIVATE
        JCE_TEST_BACKGROUND_PROPERTY_OWNER="${_background_property_header}")
endfunction()
_jce_runtime_host_policy_fixture(test_jce_runtime_host_policy integration)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/engine/include/jce/application/jce_runtime_host_policy.inc.h")
file(READ "${CMAKE_SOURCE_DIR}/engine/include/jce/application/jce_runtime_host_policy.inc.h" _runtime_host_policy_owner)
string(REGEX MATCHALL "if [(]state->blocked[)] return false" _runtime_host_policy_guards "${_runtime_host_policy_owner}")
list(LENGTH _runtime_host_policy_guards _runtime_host_policy_guard_count)
if(NOT _runtime_host_policy_guard_count EQUAL 1)
    message(FATAL_ERROR "Runtime host focus mutation requires one actual blocked-step guard")
endif()
string(REPLACE "if (state->blocked) return false;" "/* old host did not block simulation */"
    _runtime_host_policy_old "${_runtime_host_policy_owner}")
if(_runtime_host_policy_old STREQUAL _runtime_host_policy_owner)
    message(FATAL_ERROR "Runtime host focus mutation did not change actual owner")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/host-policy-mutation")
set(_runtime_host_policy_old_header "${CMAKE_CURRENT_BINARY_DIR}/host-policy-mutation/jce_runtime_host_policy_old_focus.inc.h")
unset(_runtime_host_policy_previous)
if(EXISTS "${_runtime_host_policy_old_header}")
    file(READ "${_runtime_host_policy_old_header}" _runtime_host_policy_previous)
endif()
if(NOT _runtime_host_policy_previous STREQUAL _runtime_host_policy_old)
    file(WRITE "${_runtime_host_policy_old_header}" "${_runtime_host_policy_old}")
endif()
_jce_runtime_host_policy_fixture(test_jce_runtime_host_policy_old_focus_gate contract)
target_compile_definitions(test_jce_runtime_host_policy_old_focus_gate PRIVATE
    JCE_TEST_RUNTIME_HOST_POLICY_HEADER="${_runtime_host_policy_old_header}")
set_tests_properties(test_jce_runtime_host_policy_old_focus_gate PROPERTIES WILL_FAIL TRUE)

function(_jce_background_policy_negative NAME WIRE REPLACEMENT)
    string(FIND "${_runtime_host_policy_owner}" "${WIRE}" _first)
    string(FIND "${_runtime_host_policy_owner}" "${WIRE}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "${NAME} requires one actual shared-policy wire")
    endif()
    string(REPLACE "${WIRE}" "${REPLACEMENT}" _source "${_runtime_host_policy_owner}")
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/host-policy-mutation/${NAME}.inc.h")
    unset(_previous)
    if(EXISTS "${_path}")
        file(READ "${_path}" _previous)
    endif()
    if(NOT _previous STREQUAL _source)
        file(WRITE "${_path}" "${_source}")
    endif()
    _jce_runtime_host_policy_fixture(${NAME} contract)
    target_compile_definitions(${NAME} PRIVATE JCE_TEST_RUNTIME_HOST_POLICY_HEADER="${_path}")
    set_tests_properties(${NAME} PROPERTIES WILL_FAIL TRUE)
endfunction()
_jce_background_policy_negative(test_jce_runtime_host_policy_old_background_publish
    "jce_engine_set_run_in_background(background);" "(void)background;")
_jce_background_policy_negative(test_jce_runtime_host_policy_old_background_restore
    "if (restore_background) jce_engine_set_run_in_background(prior_background);" "(void)restore_background; (void)prior_background;")
_jce_background_policy_negative(test_jce_runtime_host_policy_old_background_manual
    "state->run_in_background && !manual_pause && !paused" "state->run_in_background && !paused")


# Compile the actual stock SDK adapter, not an independently recreated gate.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/engine/include/jce/application/jce_default_main.inc.h")
file(READ "${CMAKE_SOURCE_DIR}/engine/include/jce/application/jce_default_main.inc.h" _default_main_host_owner)
string(REGEX MATCHALL "static bool default_player_policy_update[(]float [*]dt[)]" _default_host_adapters "${_default_main_host_owner}")
list(LENGTH _default_host_adapters _default_host_adapter_count)
string(FIND "${_default_main_host_owner}" "static bool default_player_policy_update(float *dt)" _default_host_adapter_begin)
string(FIND "${_default_main_host_owner}" "\nstatic void app_update(float dt, void *ud)" _default_host_adapter_end)
if(NOT _default_host_adapter_count EQUAL 1 OR _default_host_adapter_begin LESS 0 OR
   _default_host_adapter_end LESS_EQUAL _default_host_adapter_begin)
    message(FATAL_ERROR "Default host policy adapter bounds/unique owner changed")
endif()
math(EXPR _default_host_adapter_size "${_default_host_adapter_end} - ${_default_host_adapter_begin}")
string(SUBSTRING "${_default_main_host_owner}" ${_default_host_adapter_begin}
    ${_default_host_adapter_size} _default_host_adapter)
string(REGEX MATCHALL "return jce_runtime_host_policy_step[(]&s_default_host.policy, false, dt[)]" _default_host_wires "${_default_host_adapter}")
list(LENGTH _default_host_wires _default_host_wire_count)
if(NOT _default_host_wire_count EQUAL 1)
    message(FATAL_ERROR "Default host adapter requires its actual shared-policy wire")
endif()
string(REPLACE "return jce_runtime_host_policy_step(&s_default_host.policy, false, dt);"
    "(void)dt; return true;" _default_host_adapter_old "${_default_host_adapter}")
if(_default_host_adapter_old STREQUAL _default_host_adapter)
    message(FATAL_ERROR "Default host old policy wire mutation did not change its owner")
endif()
foreach(_default_host_variant IN ITEMS normal old)
    if(_default_host_variant STREQUAL "normal")
        set(_default_host_content "${_default_host_adapter}")
        set(_default_host_target test_jce_default_host_policy)
        set(_default_host_kind integration)
    else()
        set(_default_host_content "${_default_host_adapter_old}")
        set(_default_host_target test_jce_default_host_policy_old_wire)
        set(_default_host_kind contract)
    endif()
    set(_default_host_path "${CMAKE_CURRENT_BINARY_DIR}/host-policy-mutation/default_player_${_default_host_variant}.inc")
    unset(_default_host_previous)
    if(EXISTS "${_default_host_path}")
        file(READ "${_default_host_path}" _default_host_previous)
    endif()
    if(NOT _default_host_previous STREQUAL _default_host_content)
        file(WRITE "${_default_host_path}" "${_default_host_content}")
    endif()
    _jce_runtime_host_policy_fixture(${_default_host_target} ${_default_host_kind})
    target_compile_definitions(${_default_host_target} PRIVATE
        JCE_TEST_DEFAULT_HOST_POLICY_ADAPTER="${_default_host_path}")
endforeach()
set_tests_properties(test_jce_default_host_policy_old_wire PROPERTIES WILL_FAIL TRUE)

# Actual SDK entry helpers and draw-prefix admission: defer entity-cache GPU
# retirement until a real draw is admitted; no decoder/provider mirror.
function(_jce_default_cache_extract BEGIN END OUT)
    string(FIND "${_default_main_host_owner}" "${BEGIN}" _begin)
    string(FIND "${_default_main_host_owner}" "${BEGIN}" _last_begin REVERSE)
    string(FIND "${_default_main_host_owner}" "${END}" _end)
    string(FIND "${_default_main_host_owner}" "${END}" _last_end REVERSE)
    if(_begin LESS 0 OR _end LESS 0 OR NOT _begin EQUAL _last_begin OR
       NOT _end EQUAL _last_end OR NOT _end GREATER _begin)
        message(FATAL_ERROR "Default cache control requires exact complete helper/prefix bounds")
    endif()
    math(EXPR _length "${_end}-${_begin}")
    string(SUBSTRING "${_default_main_host_owner}" ${_begin} ${_length} _body)
    set(${OUT} "${_body}" PARENT_SCOPE)
endfunction()
_jce_default_cache_extract("static void default_player_cache_transition("
    "/* Narrow adapter retained as an actual stock-entry consumption seam. */" _default_cache_helpers)
string(FIND "${_default_main_host_owner}" "static void app_draw(const JceServices *svc, void *ud)\n{" _default_cache_draw_at)
if(_default_cache_draw_at LESS 0)
    message(FATAL_ERROR "Default cache control requires actual app_draw")
endif()
string(SUBSTRING "${_default_main_host_owner}" ${_default_cache_draw_at} -1 _default_cache_draw_tail)
string(FIND "${_default_cache_draw_tail}" "\n#if defined(JCE_APP_AUTHORED_CAMERA_ONLY)" _default_cache_draw_end)
if(_default_cache_draw_end LESS 0)
    message(FATAL_ERROR "Default cache control requires the complete draw-admission prefix")
endif()
string(SUBSTRING "${_default_cache_draw_tail}" 0 ${_default_cache_draw_end} _default_cache_draw)
# Execute the complete unchanged scene-draw branch, plus actual admission and
# final publication. UI/capture work between them remains outside this CPU fixture.
_jce_default_cache_extract("    bool present_done = false;"
    "    /* ECS-UI overlay on top of the 3D scene" _default_observation_render)
_jce_default_cache_extract("    default_frame_observation_publish(&frame_observation, frame_observation_on);"
    "static void app_exit(" _default_observation_tail)
set(_default_cache_observed_draw "${_default_cache_draw}\n${_default_observation_render}\n    ++observation.tail_visits;\n${_default_observation_tail}")
string(REPLACE "static void app_draw(" "static void actual_observed_draw("
    _default_cache_observed_draw "${_default_cache_observed_draw}")
string(APPEND _default_cache_draw "\n}\n")
function(_jce_default_cache_fixture NAME HELPERS DRAW KIND)
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/host-policy-mutation/${NAME}.inc.h")
    set(_content "${HELPERS}\n${DRAW}\n${_default_cache_observed_draw}")
    if(NAME STREQUAL "test_jce_default_frame_observation_missing_early_publish")
        foreach(_reason IN ITEMS HOST_POLICY MISSING_OWNERS)
            set(_wire "default_frame_observation_refuse(&frame_observation, frame_observation_on,\n                                        JCE_DEFAULT_OBSERVATION_${_reason});")
            string(REPLACE "${_wire}" "" _without_wire "${_content}")
            string(LENGTH "${_content}" _before_length)
            string(LENGTH "${_without_wire}" _after_length)
            string(LENGTH "${_wire}" _wire_length)
            math(EXPR _occurrences "(${_before_length} - ${_after_length}) / ${_wire_length}")
            if(NOT _occurrences EQUAL 2)
                message(FATAL_ERROR "Default observation control requires both exact ${_reason} prefix copies")
            endif()
            string(REPLACE "${_wire}" "(void)frame_observation; (void)frame_observation_on; /* ordinary omitted early publication */" _content "${_content}")
        endforeach()
    elseif(NAME STREQUAL "test_jce_default_frame_observation_missing_cpu_return")
        set(_wire "default_frame_observation_cpu_returned(&frame_observation, frame_observation_on);")
        string(REPLACE "${_wire}" "" _without_wire "${_default_cache_observed_draw}")
        string(LENGTH "${_default_cache_observed_draw}" _before_length)
        string(LENGTH "${_without_wire}" _after_length)
        string(LENGTH "${_wire}" _wire_length)
        math(EXPR _occurrences "(${_before_length} - ${_after_length}) / ${_wire_length}")
        if(NOT _occurrences EQUAL 2)
            message(FATAL_ERROR "Default observation control requires both exact CPU-return wires")
        endif()
        string(REPLACE "${_wire}" "/* ordinary omitted CPU-return observation */" _content "${_content}")
    endif()
    unset(_previous)
    if(EXISTS "${_path}")
        file(READ "${_path}" _previous)
    endif()
    if(NOT _previous STREQUAL _content)
        file(WRITE "${_path}" "${_content}")
    endif()
    jce_add_unit_test(${NAME} KIND ${KIND}
        SOURCES test_jce_default_cache_deferred.c "${CMAKE_SOURCE_DIR}/engine/src/application/jce_lifecycle.c"
        LINK jce_core)
    target_compile_definitions(${NAME} PRIVATE JCE_TEST_DEFAULT_CACHE_OWNER="${_path}"
        JCE_TEST_BACKGROUND_PROPERTY_OWNER="${_background_property_header}")
endfunction()
_jce_default_cache_fixture(test_jce_default_cache_deferred "${_default_cache_helpers}" "${_default_cache_draw}" integration)
foreach(_default_observation_negative IN ITEMS missing_early_publish missing_cpu_return)
    _jce_default_cache_fixture(test_jce_default_frame_observation_${_default_observation_negative}
        "${_default_cache_helpers}" "${_default_cache_draw}" contract)
    set_tests_properties(test_jce_default_frame_observation_${_default_observation_negative}
        PROPERTIES WILL_FAIL TRUE)
endforeach()
# Restore only the old FrameLog force inside the actual stock draw prefix.
set(_default_frame_log_natural_wire
    "frame_capture_forces_draw = !viewport_policy || strcmp(viewport_policy, \"natural\") != 0;")
string(FIND "${_default_cache_draw}" "${_default_frame_log_natural_wire}" _default_frame_log_first)
string(FIND "${_default_cache_draw}" "${_default_frame_log_natural_wire}" _default_frame_log_last REVERSE)
if(_default_frame_log_first LESS 0 OR NOT _default_frame_log_first EQUAL _default_frame_log_last)
    message(FATAL_ERROR "Default FrameLog control requires one actual natural admission wire")
endif()
string(REPLACE "${_default_frame_log_natural_wire}"
    "(void)viewport_policy; frame_capture_forces_draw = true;"
    _default_cache_old_frame_force "${_default_cache_draw}")
_jce_default_cache_fixture(test_jce_default_frame_log_old_force
    "${_default_cache_helpers}" "${_default_cache_old_frame_force}" contract)
set_tests_properties(test_jce_default_frame_log_old_force PROPERTIES WILL_FAIL TRUE)
unset(_default_frame_log_natural_wire)
unset(_default_frame_log_first)
unset(_default_frame_log_last)
unset(_default_cache_old_frame_force)
string(FIND "${_default_cache_helpers}" "s_cache_reset_pending = true;" _default_cache_note)
string(FIND "${_default_cache_helpers}" "s_cache_reset_pending = true;" _default_cache_note_last REVERSE)
string(FIND "${_default_cache_draw}" "default_player_cache_prepare();" _default_cache_prepare)
string(FIND "${_default_cache_draw}" "default_player_cache_prepare();" _default_cache_prepare_last REVERSE)
if(_default_cache_note LESS 0 OR NOT _default_cache_note EQUAL _default_cache_note_last OR
   _default_cache_prepare LESS 0 OR NOT _default_cache_prepare EQUAL _default_cache_prepare_last)
    message(FATAL_ERROR "Default cache controls require actual unique CPU note/draw consume wires")
endif()
string(REPLACE "s_cache_reset_pending = true;"
    "jce_scene_renderer_reset_entity_caches(s_scene_renderer);" _default_cache_old_cpu "${_default_cache_helpers}")
string(REPLACE "default_player_cache_prepare();" "(void)default_player_cache_prepare;"
    _default_cache_old_draw "${_default_cache_draw}")
_jce_default_cache_fixture(test_jce_default_cache_old_cpu_reset "${_default_cache_old_cpu}" "${_default_cache_draw}" contract)
_jce_default_cache_fixture(test_jce_default_cache_missing_prepare "${_default_cache_helpers}" "${_default_cache_old_draw}" contract)
set_tests_properties(test_jce_default_cache_old_cpu_reset test_jce_default_cache_missing_prepare PROPERTIES WILL_FAIL TRUE)


# Actual runtime pause binding TU + real offline mixer generations. No device,
# native focus, output or GPU. The old blanket resume must fail at runtime.
set(_runtime_audio_pause_source "${CMAKE_SOURCE_DIR}/engine/src/application/jce_rt_audio_pause.c")
file(READ "${_runtime_audio_pause_source}" _runtime_audio_pause_owner)
string(REGEX MATCHALL "if [(]paused != JCE_VOICE_INVALID && paused == entry->voice[)]"
    _runtime_audio_pause_marks "${_runtime_audio_pause_owner}")
list(LENGTH _runtime_audio_pause_marks _runtime_audio_pause_mark_count)
if(NOT _runtime_audio_pause_mark_count EQUAL 1)
    message(FATAL_ERROR "Runtime audio old resume requires one actual ownership guard")
endif()
string(REPLACE "if (paused != JCE_VOICE_INVALID && paused == entry->voice)\n            jce_audio_resume(rt->audio, paused);"
    "(void)paused;\n        jce_audio_resume(rt->audio, entry->voice);"
    _runtime_audio_pause_old "${_runtime_audio_pause_owner}")
if(_runtime_audio_pause_old STREQUAL _runtime_audio_pause_owner)
    message(FATAL_ERROR "Runtime audio old blanket resume mutation did not change owner")
endif()
set(_runtime_audio_pause_old_path "${CMAKE_CURRENT_BINARY_DIR}/host-policy-mutation/jce_rt_audio_pause_old_resume.c")
unset(_runtime_audio_pause_previous)
if(EXISTS "${_runtime_audio_pause_old_path}")
    file(READ "${_runtime_audio_pause_old_path}" _runtime_audio_pause_previous)
endif()
if(NOT _runtime_audio_pause_previous STREQUAL _runtime_audio_pause_old)
    file(WRITE "${_runtime_audio_pause_old_path}" "${_runtime_audio_pause_old}")
endif()
function(_jce_runtime_audio_pause_fixture target kind owner)
    jce_add_unit_test(${target}
        KIND ${kind}
        SOURCES test_jce_runtime_audio_pause.c "${owner}"
        # Actual audio TU also references public PAK/cooked readers, even for
        # offline PCM tests; use their resource owner, never fake those symbols.
        LINK jce_core jce_audio jce_resource
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/application")
    target_compile_definitions(${target} PRIVATE JCE_BUILDING_ENGINE=1)
    if(MSVC)
        target_compile_options(${target} PRIVATE /Zc:preprocessor)
    endif()
endfunction()
_jce_runtime_audio_pause_fixture(test_jce_runtime_audio_pause integration "${_runtime_audio_pause_source}")
_jce_runtime_audio_pause_fixture(test_jce_runtime_audio_pause_old_resume contract "${_runtime_audio_pause_old_path}")
set_tests_properties(test_jce_runtime_audio_pause_old_resume PROPERTIES WILL_FAIL TRUE)

# Compile the exact private timer and paused branch with counted host ports.
# A headless public Engine receipt does not certify desktop minimization.
set(_min_owner "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_min_owner}")
file(READ "${_min_owner}" _min_source)
function(_jce_minimized_owner_body BEGIN_MARKER END_MARKER OUT)
    string(FIND "${_min_source}" "${BEGIN_MARKER}" _begin)
    string(FIND "${_min_source}" "${BEGIN_MARKER}" _begin_last REVERSE)
    string(FIND "${_min_source}" "${END_MARKER}" _end)
    string(FIND "${_min_source}" "${END_MARKER}" _end_last REVERSE)
    if(_begin LESS 0 OR _end LESS 0 OR NOT _begin EQUAL _begin_last
       OR NOT _end EQUAL _end_last OR NOT _end GREATER _begin)
        message(FATAL_ERROR "Minimized control requires one complete actual owner body")
    endif()
    math(EXPR _length "${_end} - ${_begin}")
    string(SUBSTRING "${_min_source}" ${_begin} ${_length} _body)
    set(${OUT} "${_body}" PARENT_SCOPE)
endfunction()
_jce_minimized_owner_body("static bool jce_engine_benchmark_autostop("
    "static uint32_t read_positive_u32_env(" _min_timer)
_jce_minimized_owner_body("    if (render_blocked) {"
    "    /* Software fallback remains render-admitted. */" _min_branch)
set(_min_wire "        if (jce_engine_benchmark_autostop(e, false, false)) {\n            JCE_PROFILE_ZONE_END;\n            return engine_frame_release(e, JCE_APP_SUCCESS, false);\n        }\n")
string(FIND "${_min_branch}" "${_min_wire}" _min_wire_at)
string(FIND "${_min_branch}" "${_min_wire}" _min_wire_last REVERSE)
if(_min_wire_at LESS 0 OR NOT _min_wire_at EQUAL _min_wire_last)
    message(FATAL_ERROR "Minimized control requires exactly one actual elapsed-limit wire")
endif()
string(REPLACE "${_min_wire}" "" _min_old_wire "${_min_branch}")
string(REPLACE "jce_engine_benchmark_autostop(e, false, false)"
    "jce_engine_benchmark_autostop(e, false, true)" _min_old_count "${_min_branch}")
if(_min_old_wire STREQUAL _min_branch OR _min_old_count STREQUAL _min_branch)
    message(FATAL_ERROR "Minimized control did not change the actual owner")
endif()
_jce_minimized_owner_body("    /* Software fallback remains render-admitted. */"
    "    /* Reconcile any size change the resize watcher may have missed." _min_software)
string(FIND "${_min_source}" "\n    if (e->app_init_entered && g_app_desc.should_quit) {" _min_callback_guard)
if(_min_callback_guard LESS 0)
    set(_min_callback_begin "\n    if (g_app_desc.should_quit) {")
else()
    set(_min_callback_begin "\n    if (e->app_init_entered && g_app_desc.should_quit) {")
endif()
_jce_minimized_owner_body("${_min_callback_begin}"
    "    /* Optional no-draw resources:" _min_callback)
_jce_minimized_owner_body("static void fatal_msg("
    "/* -- Loading screen helper" _min_fatal)
string(FIND "${_min_fatal}" "fatal_msg(bool" _min_fatal_policy)
if(_min_fatal_policy LESS 0)
    set(_min_fatal_parameter 0)
else()
    set(_min_fatal_parameter 1)
endif()
function(_jce_minimized_fixture NAME BRANCH KIND)
    set(_selected_software "${_min_software}")
    set(_selected_callback "${_min_callback}")
    set(_selected_fatal "${_min_fatal}")
    if(ARGC GREATER 3)
        set(_selected_software "${ARGV3}")
    endif()
    if(ARGC GREATER 4)
        set(_selected_callback "${ARGV4}")
    endif()
    if(ARGC GREATER 5)
        set(_selected_fatal "${ARGV5}")
    endif()
    set(_header "${CMAKE_CURRENT_BINARY_DIR}/${NAME}_owner.h")
    set(_content "/* Generated exact production owner bodies; no copied implementation. */\n${_min_timer}\n${_selected_fatal}\nstatic JceAppResult test_software_iteration(JceEngine *e)\n{\n${_selected_callback}\n${_selected_software}\n    return JCE_APP_FAILURE;\n}\nstatic JceAppResult test_paused_iteration(JceEngine *e)\n{\n    const bool render_blocked = SDL_GetAtomicInt(&s_render_paused) != 0;\n    const float dt = 0.016f;\n${BRANCH}\n    return JCE_APP_FAILURE; /* Harness rejects an unpaused call. */\n}\n")
    if(EXISTS "${_header}")
        file(READ "${_header}" _previous)
    else()
        unset(_previous)
    endif()
    if(NOT _previous STREQUAL _content)
        file(WRITE "${_header}" "${_content}")
    endif()
    jce_add_unit_test(${NAME} KIND ${KIND}
        SOURCES test_jce_engine_minimized_autostop.c
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_perf_hitch.c"
        LINK jce_core INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(${NAME} PRIVATE
        JCE_ENGINE_MINIMIZED_AUTOSTOP_OWNER="${_header}"
        JCE_ENGINE_FATAL_POLICY_PARAMETER=${_min_fatal_parameter} JCE_DIST=1)
endfunction()
# Shared CPU background owner controls reuse exact source bodies, actual
# lifecycle/PlayerLoop/fixed-clock/property/pacing owners and typed host ports.
_jce_minimized_owner_body("static bool engine_drawable_allowed("
    "/* NOTE: The previous s_in_modal_loop" _background_drawable)
_jce_minimized_owner_body("static bool jce_resize_event_watch(void *userdata, SDL_Event *event)\n{"
    "/* -- Event routing" _background_watch)
file(READ "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_cpu_tick.inc.h" _background_cpu)
file(READ "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_host_lifecycle.h" _background_lifecycle)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_cpu_tick.inc.h"
    "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_host_lifecycle.h")
function(_jce_background_replace SOURCE WIRE REPLACEMENT OUT)
    string(FIND "${SOURCE}" "${WIRE}" _first)
    string(FIND "${SOURCE}" "${WIRE}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Background control requires exactly one actual wire: ${WIRE}")
    endif()
    string(REPLACE "${WIRE}" "${REPLACEMENT}" _mutated "${SOURCE}")
    set(${OUT} "${_mutated}" PARENT_SCOPE)
endfunction()
function(_jce_background_fixture NAME BRANCH DRAWABLE WATCH LIFECYCLE KIND)
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/background-owner/${NAME}.inc.h")
    set(_header "${CMAKE_CURRENT_BINARY_DIR}/background-owner/${NAME}_lifecycle.h")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/background-owner")
    set(_body "${_background_cpu}\n${DRAWABLE}\n${WATCH}\n${_min_timer}\nstatic JceAppResult test_background_iteration(JceEngine *e, float raw_dt)\n{\n    const bool render_blocked = !engine_drawable_allowed(e);\n    const float dt = engine_host_step_dt(&e->host_lifecycle, raw_dt);\n${BRANCH}\n    return JCE_APP_FAILURE; /* This harness only accepts skipped-render paths. */\n}\n")
    foreach(_file IN ITEMS _path _header)
        if(_file STREQUAL "_path")
            set(_content "${_body}")
        else()
            set(_content "${LIFECYCLE}")
        endif()
        unset(_previous)
        if(EXISTS "${${_file}}")
            file(READ "${${_file}}" _previous)
        endif()
        if(NOT _previous STREQUAL _content)
            file(WRITE "${${_file}}" "${_content}")
        endif()
    endforeach()
    jce_add_unit_test(${NAME} KIND ${KIND}
        SOURCES test_jce_engine_background.c
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_lifecycle.c"
            "${CMAKE_SOURCE_DIR}/engine/src/runtime/jce_player_loop.c"
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_perf_hitch.c"
        LINK jce_core sdl::sdl INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(${NAME} PRIVATE
        JCE_TEST_ENGINE_BACKGROUND_OWNER="${_path}"
        JCE_TEST_ENGINE_HOST_LIFECYCLE="${_header}")
endfunction()
_jce_background_fixture(test_jce_engine_background "${_min_branch}"
    "${_background_drawable}" "${_background_watch}" "${_background_lifecycle}" integration)
jce_test_background_interval_controls()
set(_background_ready_cpu "${_background_cpu}")
string(REPLACE "if (!engine_owner_ready(e)) return;" "/* Former admission omitted. */"
    _background_cpu "${_background_ready_cpu}")
if(_background_cpu STREQUAL _background_ready_cpu)
    message(FATAL_ERROR "Background cleanup control requires actual CPU admission wires")
endif()
_jce_background_fixture(test_jce_engine_background_old_cleanup_admission "${_min_branch}"
    "${_background_drawable}" "${_background_watch}" "${_background_lifecycle}" contract)
set(_background_cpu "${_background_ready_cpu}")
unset(_background_ready_cpu)
set_tests_properties(test_jce_engine_background_old_cleanup_admission PROPERTIES WILL_FAIL TRUE)
_jce_background_replace("${_min_branch}" "            engine_cpu_tick(e, dt);"
    "            (void)engine_cpu_tick; /* Previous early skip omitted CPU hooks. */" _background_old_cpu)
_jce_background_fixture(test_jce_engine_background_old_cpu_hooks "${_background_old_cpu}"
    "${_background_drawable}" "${_background_watch}" "${_background_lifecycle}" contract)
_jce_background_replace("${_background_lifecycle}"
    "case SDL_EVENT_WINDOW_MINIMIZED:\n        state->render_blocked = true;\n        jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST); break;"
    "case SDL_EVENT_WINDOW_MINIMIZED:\n        state->render_blocked = true;\n        jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE); break;" _background_old_minimize)
_jce_background_fixture(test_jce_engine_background_old_minimize_pause "${_min_branch}"
    "${_background_drawable}" "${_background_watch}" "${_background_old_minimize}" contract)
_jce_background_replace("${_background_drawable}"
    "observation.allowed = e->headless || engine_host_render_allowed(&e->host_lifecycle,\n        observation.minimized_gate, observation.lifecycle_paused, observation.width, observation.height);"
    "observation.allowed = e->headless || (!observation.minimized_gate && !observation.lifecycle_paused); /* Previous zero drawable omission. */" _background_old_drawable)
_jce_background_fixture(test_jce_engine_background_old_zero_drawable "${_min_branch}"
    "${_background_old_drawable}" "${_background_watch}" "${_background_lifecycle}" contract)
_jce_background_replace("${_background_drawable}"
    "engine_host_observe_drawable(&e->host_lifecycle, &observation, e->window);"
    "(void)observation; /* Old owner published no drawable transition. */" _background_missing_observation)
_jce_background_fixture(test_jce_engine_background_missing_drawable_observation "${_min_branch}"
    "${_background_missing_observation}" "${_background_watch}" "${_background_lifecycle}" contract)
_jce_background_replace("${_background_lifecycle}"
    "const bool extent_known = observation->extent_query_valid;"
    "const bool extent_known = observation->extent_query_attempted; /* Unknown outputs mislabeled as known. */" _background_unknown_zero)
_jce_background_fixture(test_jce_engine_background_unknown_drawable_zero "${_min_branch}"
    "${_background_drawable}" "${_background_watch}" "${_background_unknown_zero}" contract)
_jce_background_replace("${_background_watch}"
    "if (SDL_GetAtomicInt(&s_render_paused) != 0 || jce_lifecycle_is_paused())"
    "if (SDL_GetAtomicInt(&s_render_paused) != 0)" _background_old_watch)
_jce_background_fixture(test_jce_engine_background_old_paused_resize "${_min_branch}"
    "${_background_drawable}" "${_background_old_watch}" "${_background_lifecycle}" contract)
_jce_background_replace("${_min_branch}"
    "engine_frame_cap_wait(&e->perf_hitch);" "jce_thread_sleep_ms(33);" _background_old_pacing)
_jce_background_fixture(test_jce_engine_background_old_pacing "${_background_old_pacing}"
    "${_background_drawable}" "${_background_watch}" "${_background_lifecycle}" contract)
set_tests_properties(test_jce_engine_background_old_cpu_hooks
    test_jce_engine_background_old_minimize_pause test_jce_engine_background_old_zero_drawable
    test_jce_engine_background_old_paused_resize test_jce_engine_background_old_pacing
    test_jce_engine_background_missing_drawable_observation test_jce_engine_background_unknown_drawable_zero
    PROPERTIES WILL_FAIL TRUE)

_jce_minimized_fixture(test_jce_engine_minimized_autostop "${_min_branch}" integration)
_jce_minimized_fixture(test_jce_engine_minimized_autostop_old_wire "${_min_old_wire}" contract)
_jce_minimized_fixture(test_jce_engine_minimized_autostop_old_count "${_min_old_count}" contract)
set_tests_properties(test_jce_engine_minimized_autostop_old_wire
    test_jce_engine_minimized_autostop_old_count PROPERTIES WILL_FAIL TRUE)
set(_min_software_wire "        if (jce_engine_benchmark_autostop(e, false, true)) {\n            JCE_PROFILE_ZONE_END;\n            return engine_frame_release(e, JCE_APP_SUCCESS, false);\n        }\n")
_jce_background_replace("${_min_software}" "${_min_software_wire}" "" _min_old_software_limit)
_jce_background_replace("${_min_software}"
    "return engine_frame_release(e, JCE_APP_CONTINUE, false);"
    "return engine_frame_release(e, JCE_APP_CONTINUE, true);" _min_old_software_full)
_jce_background_replace("${_min_callback}" "e->app_init_entered && g_app_desc.should_quit"
    "g_app_desc.should_quit" _min_old_callback)
_jce_background_replace("${_min_branch}" "e->app_init_entered && !jce_lifecycle_is_paused()"
    "!jce_lifecycle_is_paused()" _min_old_paused_callback)
_jce_background_replace("${_min_fatal}" "if (allow_dialog)" "if (true)" _min_old_modal)
_jce_minimized_fixture(test_jce_engine_fallback_missing_limit "${_min_branch}" contract "${_min_old_software_limit}")
_jce_minimized_fixture(test_jce_engine_fallback_false_full "${_min_branch}" contract "${_min_old_software_full}")
_jce_minimized_fixture(test_jce_engine_fallback_missing_callback_guard "${_min_branch}" contract
    "${_min_software}" "${_min_old_callback}")
_jce_minimized_fixture(test_jce_engine_fallback_missing_paused_guard "${_min_old_paused_callback}" contract)
_jce_minimized_fixture(test_jce_engine_fatal_missing_modal_guard "${_min_branch}" contract
    "${_min_software}" "${_min_callback}" "${_min_old_modal}")
set_tests_properties(test_jce_engine_fallback_missing_limit test_jce_engine_fallback_false_full
    test_jce_engine_fallback_missing_callback_guard test_jce_engine_fallback_missing_paused_guard
    test_jce_engine_fatal_missing_modal_guard PROPERTIES WILL_FAIL TRUE)
unset(_min_owner)
unset(_min_source)
unset(_min_timer)
unset(_min_branch)
unset(_min_wire)
unset(_min_wire_at)
unset(_min_wire_last)
unset(_min_old_wire)
unset(_min_old_count)

unset(_viewport_marker_length)
unset(_viewport_after_marker)
unset(_viewport_remainder)
unset(_viewport_second_marker)

# Actual runtime networking interpolation call-site; preserve L4's independent
# no-session loopback API. This fixture does not boot a runtime, socket or GPU.
set(_runtime_net_render_owner_path "${CMAKE_SOURCE_DIR}/engine/src/application/jce_runtime.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_runtime_net_render_owner_path}")
file(READ "${_runtime_net_render_owner_path}" _runtime_net_render_source)
set(_runtime_net_render_begin "\t/* Networking render-step: write interpolated poses for non-owned")
set(_runtime_net_render_end "\t/* Client-side prediction reconcile (runtime-driven so the net layer stays")
string(FIND "${_runtime_net_render_source}" "${_runtime_net_render_begin}" _runtime_net_render_first)
string(FIND "${_runtime_net_render_source}" "${_runtime_net_render_begin}" _runtime_net_render_first_last REVERSE)
string(FIND "${_runtime_net_render_source}" "${_runtime_net_render_end}" _runtime_net_render_last)
string(FIND "${_runtime_net_render_source}" "${_runtime_net_render_end}" _runtime_net_render_last_last REVERSE)
if(_runtime_net_render_first LESS 0 OR NOT _runtime_net_render_first EQUAL _runtime_net_render_first_last OR
   NOT _runtime_net_render_last EQUAL _runtime_net_render_last_last OR
   NOT _runtime_net_render_last GREATER _runtime_net_render_first)
    message(FATAL_ERROR "Runtime net render gate requires its one actual call-site block")
endif()
math(EXPR _runtime_net_render_length "${_runtime_net_render_last}-${_runtime_net_render_first}")
string(SUBSTRING "${_runtime_net_render_source}" ${_runtime_net_render_first} ${_runtime_net_render_length} _runtime_net_render_current)
set(_runtime_net_render_current_wire [=[	if (rt->net_bridged && jce_session_mode() != JCE_SESSION_MODE_NONE) {
		jce_net_transform_render_step((double)rt->clock.alpha);
		jce_net_animator_render_step((double)rt->clock.alpha);
	}
]=])
set(_runtime_net_render_old_wire [=[	if (rt->net_bridged && jce_session_mode() != JCE_SESSION_MODE_NONE)
		jce_net_transform_render_step((double)rt->clock.alpha);
		jce_net_animator_render_step((double)rt->clock.alpha);
]=])
string(FIND "${_runtime_net_render_current}" "${_runtime_net_render_current_wire}" _runtime_net_render_wire_first)
string(FIND "${_runtime_net_render_current}" "${_runtime_net_render_current_wire}" _runtime_net_render_wire_last REVERSE)
if(_runtime_net_render_wire_first LESS 0 OR NOT _runtime_net_render_wire_first EQUAL _runtime_net_render_wire_last)
    message(FATAL_ERROR "Runtime net render mutation requires the actual shared two-call guard")
endif()
string(REPLACE "${_runtime_net_render_current_wire}" "${_runtime_net_render_old_wire}"
    _runtime_net_render_old_braces "${_runtime_net_render_current}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime-net-render-gate")
foreach(_runtime_net_render_variant IN ITEMS current old_braces)
    set(_runtime_net_render_content "${_runtime_net_render_${_runtime_net_render_variant}}")
    set(_runtime_net_render_path "${CMAKE_CURRENT_BINARY_DIR}/runtime-net-render-gate/${_runtime_net_render_variant}.inc.h")
    unset(_runtime_net_render_previous)
    if(EXISTS "${_runtime_net_render_path}")
        file(READ "${_runtime_net_render_path}" _runtime_net_render_previous)
    endif()
    if(NOT _runtime_net_render_previous STREQUAL _runtime_net_render_content)
        file(WRITE "${_runtime_net_render_path}" "${_runtime_net_render_content}")
    endif()
    set(_runtime_net_render_target test_jce_runtime_net_render_gate)
    set(_runtime_net_render_kind unit)
    if(_runtime_net_render_variant STREQUAL "old_braces")
        string(APPEND _runtime_net_render_target "_old_braces")
        set(_runtime_net_render_kind contract)
    endif()
    jce_add_unit_test(${_runtime_net_render_target} KIND ${_runtime_net_render_kind}
        SOURCES test_jce_runtime_net_render_gate.c LINK jce_core)
    target_compile_definitions(${_runtime_net_render_target} PRIVATE
        JCE_TEST_RUNTIME_NET_RENDER_OWNER="${_runtime_net_render_path}")
    if(_runtime_net_render_variant STREQUAL "old_braces")
        set_tests_properties(${_runtime_net_render_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_runtime_net_render_variable IN ITEMS owner_path source begin end first first_last last last_last
    length current current_wire old_wire wire_first wire_last old_braces variant content path previous target kind)
    unset(_runtime_net_render_${_runtime_net_render_variable})
endforeach()
unset(_runtime_net_render_variable)

# Exact lost-context Engine refusal branches; no new lifecycle/test registry.
set(_engine_context_path "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_engine_context_path}")
file(READ "${_engine_context_path}" _engine_context_source)
function(_jce_engine_context_slice BEGIN END OUT)
    string(FIND "${_engine_context_source}" "${BEGIN}" _first)
    string(FIND "${_engine_context_source}" "${BEGIN}" _first_last REVERSE)
    string(FIND "${_engine_context_source}" "${END}" _last)
    string(FIND "${_engine_context_source}" "${END}" _last_last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _first_last OR
       NOT _last EQUAL _last_last OR NOT _last GREATER _first)
        message(FATAL_ERROR "Engine context control requires a complete unique production branch")
    endif()
    math(EXPR _length "${_last}-${_first}")
    string(SUBSTRING "${_engine_context_source}" ${_first} ${_length} _body)
    set(${OUT} "${_body}" PARENT_SCOPE)
endfunction()
_jce_engine_context_slice("    /* Context loss terminates this host before completions, user callbacks,"
    "    const bool render_blocked = !engine_drawable_allowed(e);" _engine_context_main)
_jce_engine_context_slice("JceAppResult jce_engine_event(JceEngine *e, const void *platform_event)\n{"
    "    const bool dispatching = e->event_dispatching;\n    e->event_dispatching = true;" _engine_context_event_prefix)
string(FIND "${_engine_context_event_prefix}" "    if (!jce_renderer_frame_admission_current_internal(e->renderer)) {" _engine_context_event_start)
if(_engine_context_event_start LESS 0)
    message(FATAL_ERROR "Engine event context control needs its real pre-callback gate")
endif()
string(SUBSTRING "${_engine_context_event_prefix}" ${_engine_context_event_start} -1 _engine_context_event)
_jce_engine_context_slice("    /* A lost Web device is not a minimized drawable. The next main iteration"
    "    const Uint32 t = event->type;" _engine_context_watch)
_jce_engine_context_slice("    jce_renderer_begin_frame(e->renderer, e->window);\n"
    "    /* Finalize async asset loads (GPU resource creation). */" _engine_context_after_begin_prefix)
string(REPLACE "    jce_renderer_begin_frame(e->renderer, e->window);\n" ""
    _engine_context_after_begin "${_engine_context_after_begin_prefix}")
_jce_engine_context_slice("    /* A loss during callbacks/submit is an incomplete failed interval. */"
    "    engine_frame_context_end(e);" _engine_context_after_submit)
_jce_engine_context_slice("    if (SDL_GetAtomicInt(&s_in_render_frame) != 0) {"
    "    SDL_SetAtomicInt(&s_in_render_frame, 1);\n\n    engine_cpu_tick(e, dt);" _engine_context_guard)
foreach(_engine_context_body IN ITEMS main event watch after_begin after_submit)
    string(FIND "${_engine_context_${_engine_context_body}}" "!jce_renderer_frame_admission_current_internal(e->renderer)" _engine_context_wire_first)
    string(FIND "${_engine_context_${_engine_context_body}}" "!jce_renderer_frame_admission_current_internal(e->renderer)" _engine_context_wire_last REVERSE)
    if(_engine_context_wire_first LESS 0 OR NOT _engine_context_wire_first EQUAL _engine_context_wire_last)
        message(FATAL_ERROR "Engine context negative needs one exact owner gate in each entry")
    endif()
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/engine-context-admission")
foreach(_engine_context_variant IN ITEMS current old_gate)
    set(_engine_context_content "")
    foreach(_engine_context_body IN ITEMS main event watch after_begin after_submit guard)
        set(_engine_context_body_content "${_engine_context_${_engine_context_body}}")
        if(_engine_context_variant STREQUAL "old_gate")
            set(_engine_context_body_content "") # Former missing refusal branch.
        endif()
        if(_engine_context_body STREQUAL "watch")
            set(_engine_context_result_type bool)
            set(_engine_context_default true)
        else()
            set(_engine_context_result_type JceAppResult)
            set(_engine_context_default JCE_APP_CONTINUE)
        endif()
        string(APPEND _engine_context_content
            "static ${_engine_context_result_type} actual_${_engine_context_body}(JceEngine *e)\n{\n${_engine_context_body_content}    ++next_work; return ${_engine_context_default};\n}\n")
    endforeach()
    set(_engine_context_output "${CMAKE_CURRENT_BINARY_DIR}/engine-context-admission/${_engine_context_variant}.inc.h")
    unset(_engine_context_previous)
    if(EXISTS "${_engine_context_output}")
        file(READ "${_engine_context_output}" _engine_context_previous)
    endif()
    if(NOT _engine_context_previous STREQUAL _engine_context_content)
        file(WRITE "${_engine_context_output}" "${_engine_context_content}")
    endif()
    set(_engine_context_target test_jce_engine_context_admission)
    set(_engine_context_kind unit)
    if(_engine_context_variant STREQUAL "old_gate")
        string(APPEND _engine_context_target "_old_gate")
        set(_engine_context_kind contract)
    endif()
    jce_add_unit_test(${_engine_context_target} KIND ${_engine_context_kind}
        SOURCES test_jce_engine_context_admission.c
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_perf_hitch.c"
        LINK jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(${_engine_context_target} PRIVATE
        JCE_TEST_ENGINE_CONTEXT_OWNER="${_engine_context_output}")
    if(_engine_context_kind STREQUAL "contract")
        set_tests_properties(${_engine_context_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_engine_context_var IN ITEMS path source main event_prefix event_start event watch after_begin_prefix
    after_begin after_submit guard body wire_first wire_last variant content body_content result_type default output previous target kind)
    unset(_engine_context_${_engine_context_var})
endforeach()
unset(_engine_context_var)

# Whole runtime-audio owner, real mixer parser/offline device sends and exact
# FS/allocator ports; default absence and custom UNKNOWN remain different.
set(_rt_mixer_path "${CMAKE_SOURCE_DIR}/engine/src/application/jce_rt_audio.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_rt_mixer_path}")
file(READ "${_rt_mixer_path}" _rt_mixer_source)
function(_jce_rt_mixer_replace_once SOURCE_VARIABLE NEEDLE REPLACEMENT)
    string(FIND "${${SOURCE_VARIABLE}}" "${NEEDLE}" _first)
    string(FIND "${${SOURCE_VARIABLE}}" "${NEEDLE}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Mixer negative requires one actual resolution wire")
    endif()
    string(REPLACE "${NEEDLE}" "${REPLACEMENT}" _result "${${SOURCE_VARIABLE}}")
    set(${SOURCE_VARIABLE} "${_result}" PARENT_SCOPE)
endfunction()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mixer-resolution")
foreach(_rt_mixer_variant IN ITEMS current missing_pak_fallback skip_unknown_reader)
    set(_rt_mixer_owner "${_rt_mixer_source}")
    set(_rt_mixer_target test_jce_runtime_mixer_resolution)
    set(_rt_mixer_kind integration)
    if(_rt_mixer_variant STREQUAL "missing_pak_fallback")
        _jce_rt_mixer_replace_once(_rt_mixer_owner
            "    if (!rt || !rt->pak) return NULL;"
            "    if (!rt || !rt->pak || observation->presence == JCE_FS_PRESENCE_ABSENT) return NULL;")
    elseif(_rt_mixer_variant STREQUAL "skip_unknown_reader")
        _jce_rt_mixer_replace_once(_rt_mixer_owner
            "    if (observation->explicit_path || observation->presence != JCE_FS_PRESENCE_ABSENT) {"
            "    if (observation->explicit_path || observation->presence == JCE_FS_PRESENCE_PRESENT) {")
    endif()
    if(NOT _rt_mixer_variant STREQUAL "current")
        set(_rt_mixer_kind contract)
        string(APPEND _rt_mixer_target "_${_rt_mixer_variant}")
    endif()
    set(_rt_mixer_output "${CMAKE_CURRENT_BINARY_DIR}/mixer-resolution/${_rt_mixer_variant}.c")
    unset(_rt_mixer_previous)
    if(EXISTS "${_rt_mixer_output}")
        file(READ "${_rt_mixer_output}" _rt_mixer_previous)
    endif()
    if(NOT _rt_mixer_previous STREQUAL _rt_mixer_owner)
        file(WRITE "${_rt_mixer_output}" "${_rt_mixer_owner}")
    endif()
    jce_add_unit_test(${_rt_mixer_target} KIND ${_rt_mixer_kind}
        SOURCES test_jce_runtime_mixer_resolution.c LINK JCE
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/application")
    target_compile_definitions(${_rt_mixer_target} PRIVATE JCE_TEST_RT_MIXER_OWNER="${_rt_mixer_output}")
    if(MSVC)
        target_compile_options(${_rt_mixer_target} PRIVATE /Zc:preprocessor)
    endif()
    if(_rt_mixer_kind STREQUAL "contract")
        set_tests_properties(${_rt_mixer_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_rt_mixer_variable IN ITEMS path source variant owner target kind output previous)
    unset(_rt_mixer_${_rt_mixer_variable})
endforeach()
unset(_rt_mixer_variable)

# Shared resources use the actual private owner and exact Engine boot/no-draw/
# normal-frame wires. Public stats are counted ports; the trace ring is real.
set(_resource_observation_owner "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_resource_observation.h")
set(_resource_observation_engine "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_resource_observation_owner}" "${_resource_observation_engine}")
file(READ "${_resource_observation_owner}" _resource_observation_source)
file(READ "${_resource_observation_engine}" _resource_observation_engine_source)
function(_jce_resource_observation_slice BEGIN END OUT)
    string(FIND "${_resource_observation_engine_source}" "${BEGIN}" _first)
    string(FIND "${_resource_observation_engine_source}" "${BEGIN}" _first_last REVERSE)
    string(FIND "${_resource_observation_engine_source}" "${END}" _last)
    string(FIND "${_resource_observation_engine_source}" "${END}" _last_last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _first_last OR
       _last LESS 0 OR NOT _last EQUAL _last_last OR NOT _last GREATER _first)
        message(FATAL_ERROR "Resource observations need one complete unique Engine wire")
    endif()
    math(EXPR _length "${_last}-${_first}")
    string(SUBSTRING "${_resource_observation_engine_source}" ${_first} ${_length} _body)
    set(${OUT} "${_body}" PARENT_SCOPE)
endfunction()
_jce_resource_observation_slice("static uint32_t read_positive_u32_env("
    "/* -- Fatal error dialog" _resource_observation_parse)
_jce_resource_observation_slice("    engine_resource_observation_init(&e->resource_observation,"
    "    e->config = jce_config_defaults();" _resource_observation_init)
_jce_resource_observation_slice("    JceAsyncExecutor *boot_executor = jce_async_default_executor();"
    "    /* Force landscape on mobile" _resource_observation_bind)
_jce_resource_observation_slice("    /* Optional no-draw resources:"
    "    /* ── Headless / dedicated-server fast path" _resource_observation_no_draw)
_jce_resource_observation_slice("    /* Optional normal resources:"
    "    {\n        /* end_frame includes" _resource_observation_normal)
set(_resource_observation_begin "    jce_renderer_begin_frame(e->renderer, e->window);\n")
set(_resource_observation_draw "    if (engine_owner_ready(e)) engine_app_draw_only(&g_app_desc, &e->svc);\n")
set(_resource_observation_end "        jce_renderer_end_frame(e->renderer);\n")
foreach(_resource_observation_wire IN ITEMS begin draw end)
    string(FIND "${_resource_observation_engine_source}" "${_resource_observation_${_resource_observation_wire}}" _first)
    string(FIND "${_resource_observation_engine_source}" "${_resource_observation_${_resource_observation_wire}}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Resource observation frame wire must be unique")
    endif()
    set(_resource_observation_${_resource_observation_wire}_position "${_first}")
endforeach()
string(FIND "${_resource_observation_engine_source}" "${_resource_observation_normal}" _resource_observation_normal_position)
string(FIND "${_resource_observation_engine_source}" "${_resource_observation_no_draw}" _resource_observation_no_draw_position)
if(NOT _resource_observation_begin_position LESS _resource_observation_draw_position OR
   NOT _resource_observation_draw_position LESS _resource_observation_normal_position OR
   NOT _resource_observation_normal_position LESS _resource_observation_end_position OR
   NOT _resource_observation_no_draw_position LESS _resource_observation_begin_position)
    message(FATAL_ERROR "Resource observations must preserve real no-draw and active-frame placement")
endif()
set(_resource_observation_wires "${_resource_observation_parse}\nstatic void actual_resource_init(struct JceEngine *e)\n{\n${_resource_observation_init}}\nstatic bool actual_resource_bind(struct JceEngine *e)\n{\n    const bool allow_error_dialog = false; /* headless fixture: never open an SDL modal */\n${_resource_observation_bind}    return true;\nfail: return false;\n}\nstatic void actual_resource_no_draw(struct JceEngine *e, bool render_blocked)\n{\n${_resource_observation_no_draw}}\nstatic void actual_resource_normal(struct JceEngine *e)\n{\n${_resource_observation_begin}${_resource_observation_draw}${_resource_observation_normal}${_resource_observation_end}}\n")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/resource-observation")
set(_resource_observation_wires_path "${CMAKE_CURRENT_BINARY_DIR}/resource-observation/engine-wires.inc.h")
unset(_resource_observation_previous)
if(EXISTS "${_resource_observation_wires_path}")
    file(READ "${_resource_observation_wires_path}" _resource_observation_previous)
endif()
if(NOT _resource_observation_previous STREQUAL _resource_observation_wires)
    file(WRITE "${_resource_observation_wires_path}" "${_resource_observation_wires}")
endif()
foreach(_resource_observation_variant IN ITEMS current missing_opt_in missing_validity old_zero_guard)
    set(_resource_observation_content "${_resource_observation_source}")
    set(_resource_observation_kind unit)
    set(_resource_observation_target test_jce_engine_resource_observation)
    if(_resource_observation_variant STREQUAL "missing_opt_in")
        set(_resource_observation_needle "    state->enabled = jce_env_flag_enabled(enabled) != 0;")
        set(_resource_observation_replacement "    state->enabled = true; (void)enabled; /* Former unconditional observer. */")
    elseif(_resource_observation_variant STREQUAL "missing_validity")
        set(_resource_observation_needle "    const bool gpu_timer_available = gpu_backend_available && gpu.timer_available;")
        set(_resource_observation_replacement "    const bool gpu_timer_available = gpu_copied; /* Former copied-is-available inference. */")
    elseif(_resource_observation_variant STREQUAL "old_zero_guard")
        set(_resource_observation_needle "    if (jce_thread_current_id() != state->owner_thread) return;")
        set(_resource_observation_replacement "    if (!state->owner_thread || jce_thread_current_id() != state->owner_thread) return;")
    endif()
    if(NOT _resource_observation_variant STREQUAL "current")
        string(FIND "${_resource_observation_content}" "${_resource_observation_needle}" _first)
        string(FIND "${_resource_observation_content}" "${_resource_observation_needle}" _last REVERSE)
        if(_first LESS 0 OR NOT _first EQUAL _last)
            message(FATAL_ERROR "Resource observation negative needs one exact owner wire")
        endif()
        string(REPLACE "${_resource_observation_needle}" "${_resource_observation_replacement}"
            _resource_observation_content "${_resource_observation_content}")
        set(_resource_observation_kind contract)
        string(APPEND _resource_observation_target "_${_resource_observation_variant}")
    endif()
    set(_resource_observation_output "${CMAKE_CURRENT_BINARY_DIR}/resource-observation/${_resource_observation_variant}.h")
    unset(_resource_observation_previous)
    if(EXISTS "${_resource_observation_output}")
        file(READ "${_resource_observation_output}" _resource_observation_previous)
    endif()
    if(NOT _resource_observation_previous STREQUAL _resource_observation_content)
        file(WRITE "${_resource_observation_output}" "${_resource_observation_content}")
    endif()
    jce_add_unit_test(${_resource_observation_target} KIND ${_resource_observation_kind}
        SOURCES test_jce_engine_resource_observation.c LINK jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(${_resource_observation_target} PRIVATE
        JCE_TEST_RESOURCE_OBSERVATION_OWNER="${_resource_observation_output}"
        JCE_TEST_RESOURCE_OBSERVATION_ENGINE_WIRES="${_resource_observation_wires_path}")
    if(_resource_observation_kind STREQUAL "contract")
        set_tests_properties(${_resource_observation_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_resource_observation_variable IN ITEMS owner engine source engine_source parse init bind no_draw normal begin draw end wire
    begin_position draw_position end_position normal_position no_draw_position wires wires_path previous variant content kind target
    needle replacement output)
    unset(_resource_observation_${_resource_observation_variable})
endforeach()
unset(_resource_observation_variable)

include("${CMAKE_CURRENT_SOURCE_DIR}/runtime_trace_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/frame_kpi_prefix_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/launch_policy_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/audio_retirement_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/audio_pending_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/rml_scene_batch_controls.cmake")
