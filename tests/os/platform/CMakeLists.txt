cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L1 (platform) unit tests.                                              #
#                                                                         #
#  Exercise the input-action map (jce_input_actions.c) and the input      #
#  state machine (jce_input.c), both compiled into the jce_platform layer #
#  library (which links jce_core publicly).                               #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_input_composite
    SOURCES test_jce_input_composite.c
    LINK    jce_platform jce_core)

# Top 6 seam: synthesized JceInputFrame -> jce_input_apply -> jce_actions_update
# resolves named actions w/ edges (the editor live-action synthesis, minus ImGui).
jce_add_unit_test(test_jce_actions_from_input_frame
    SOURCES test_jce_actions_from_input_frame.c
    LINK    jce_platform jce_core)

jce_add_unit_test(test_jce_input_schemes
    SOURCES test_jce_input_schemes.c
    LINK    jce_platform jce_core)

jce_add_unit_test(test_jce_input_serialize
    SOURCES test_jce_input_serialize.c
    LINK    jce_platform jce_core)
# JceBinding v2 renumbers the source enum; schema 1 froze the old numbers on
# disk.  This target reads a REAL SHIPPED MAP rather than a literal, so the v1
# translation is guarded by a file that pays for its absence instead of by the
# writer and the reader agreeing with each other.
#
# THE FILE IS A FIXTURE HERE, and it used to be ${CMAKE_SOURCE_DIR}/space/.jce/
# input_actions.json -- a reach into ANOTHER PROJECT'''s directory, with no
# if(EXISTS) guard and an absolute path baked into the binary at compile time.
# Two things were wrong with that:
#
#   * space/ is a local-only sample project (.gitignore, "Sample projects
#     (local-only, deliberately unversioned)"), so on any clean checkout the
#     fopen() failed and this test was RED -- the comment that used to sit here
#     called it "the REAL tracked space map" and `git ls-files` answers 0 for
#     it.  It was never tracked; the rationale rested on a false premise.
#   * a test that breaks when an unrelated project moves is a test coupled to
#     something it does not own.  space/ has since moved out of the tree
#     entirely (2026-08-30) and this copy is what keeps the coverage.
#
# The bytes are the same 2939-byte map, copied verbatim.  Absolute path as a
# compile definition for the same reason as the .jirc fixtures below -- ctest
# runs each test from its own binary directory.
target_compile_definitions(test_jce_input_serialize PRIVATE
    JCE_SPACE_INPUT_MAP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/space_input_actions.json")

# Configure-time proof that jce_add_unit_test accepts INCLUDES.  This target
# compiles a one-line TU that can only find its header through the INCLUDES
# path, so a helper that silently drops the argument fails the BUILD, not a
# runtime assertion.
jce_add_unit_test(test_jce_includes_arg
    SOURCES  test_jce_includes_arg.c
    LINK     jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/os/platform")

# The mouse button mask is 1u << (b - 1): button 1 is bit 0.  Pinned here
# BEFORE jce_input.c stops speaking SDL, so the substitution has something to
# be measured against.  Injects state through jce_input_apply -- no window, no
# event pump, no mouse.
jce_add_unit_test(test_jce_input_mouse_mask
    SOURCES test_jce_input_mouse_mask.c
    LINK    jce_platform jce_core)

# The .jirc recorder after its port off SDL_IOStream onto jce_fs_host_*.
# Pins the batch boundary (a partial batch must still reach disk at close) and
# the truncate-on-open contract.  No window, no pump, no hardware.
jce_add_unit_test(test_jce_input_record_roundtrip
    SOURCES test_jce_input_record_roundtrip.c
    LINK    jce_platform jce_core)
include("${CMAKE_CURRENT_LIST_DIR}/input_replay_origin_controls.cmake")

# The committed v1 .jirc fixture, and the harness that produced it.
#
# Plan B Task 6 rewrites JceInputFrame to v2 and Task 7 must then prove "a v1
# file still plays".  After Task 6 nothing in the tree can write a genuine v1
# frame, so a fixture minted then would only show the new writer and the new
# reader agreeing with each other.  The bytes therefore had to be recorded
# BEFORE the struct changed; this target both guards them and, via
# `test_jce_input_v1_fixture --emit <path>`, is what recorded them.  The --emit
# path refuses to run once JCE_INPUT_FRAME_VERSION stops being 1u.
#
# The absolute fixture path is a compile definition rather than a relative
# path: ctest runs each test with its own binary directory as the working
# directory, not the source tree.  Same shape as CK_LIGHTNING_SCENE_PATH in
# tests/application/CMakeLists.txt.
jce_add_unit_test(test_jce_input_v1_fixture
    SOURCES test_jce_input_v1_fixture.c
    LINK    jce_platform jce_core)
target_compile_definitions(test_jce_input_v1_fixture PRIVATE
    JCE_INPUT_V1_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/jce_input_v1.jirc")

# SEAM A + SEAM B.  This is the ONE test target that asks for sdl::sdl by
# name: jce_platform links SDL PRIVATE by design (audit
# cmake-jce-core-public-sdl), which is what keeps every other layer honestly
# SDL-free, so a test that must BUILD an SDL_Event declares the dependency
# itself.  Explicit, single-target, reviewed -- do not "clean this up".
# INCLUDES reaches engine/src/os/platform/jce_input_sdl.h, the internal seam
# header, which contains no SDL token of its own.
jce_add_unit_test(test_jce_input_sdl_translate
    SOURCES  test_jce_input_sdl_translate.c
    LINK     jce_platform jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/os/platform")

# The device layer.  Deliberately does NOT link sdl::sdl: every device in this
# target arrives as a JceInputEvent struct literal and every capability comes
# from a recording fake backend, so the whole layer is exercised headless with
# nothing plugged in.  If this target ever needs SDL, the seam has leaked.
jce_add_unit_test(test_jce_input_devices
    SOURCES test_jce_input_devices.c
    LINK    jce_platform jce_core)

# JceInputFrame is a wire format: a .jirc file is this struct memcpy'd, once
# per frame.  Pinned by sizeof and offsetof because the ABI gate could not see
# the record at all until the balanced-brace walk landed -- its body regex
# could not cross the anonymous gamepads[] member, so the format could have
# been reordered or widened with no diff to read.
#
# It also owns the .jirc CONTAINER around that struct, which is why it needs
# the v1 fixture too: "a v1 file still plays" is only a compatibility claim
# when the file was written by the v1 writer.  Same absolute-path compile
# definition as test_jce_input_v1_fixture above, and for the same reason --
# ctest runs each test from its own binary directory, not the source tree.
jce_add_unit_test(test_jce_input_frame_wire
    SOURCES test_jce_input_frame_wire.c
    LINK    jce_platform jce_core)
target_compile_definitions(test_jce_input_frame_wire PRIVATE
    JCE_INPUT_V1_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/jce_input_v1.jirc")

# Gamepad -> action, through the door the HARDWARE uses.  Every other
# gamepad-to-action test injects with jce_input_apply() + a hand-built
# JceInputFrame, which writes pad slots directly and never asks the state
# machine to open a device -- so it stays green even when nothing can open a
# pad.  This one submits DEVICE_ADDED / DEVICE_AXIS / DEVICE_BUTTON events into
# SEAM B and records open_device on a fake backend.  No SDL, nothing plugged in.
jce_add_unit_test(test_jce_input_gamepad_actions
    SOURCES test_jce_input_gamepad_actions.c
    LINK    jce_platform jce_core)

# jce_binding_init is the only place a binding default is invented, and the
# JceBinding size prefix is the only thing standing between a stale caller and
# a misread record.  Both are pinned here, along with the fact that the v2
# source enum is NOT the schema-1 disk numbering -- the inequality two separate
# translation layers exist to absorb.
jce_add_unit_test(test_jce_binding_init
    SOURCES test_jce_binding_init.c
    LINK    jce_platform jce_core)

# The evaluator is an ENGINE-PRIVATE header, so this target asks for the
# engine's platform src dir by name (jce_add_unit_test INCLUDES, Plan A Task 2).
# Radial vs square deadzones, the half-axis side gate and the unipolar trigger
# all live behind this one function and had no coverage at all before it.
jce_add_unit_test(test_jce_input_bind_eval
    SOURCES  test_jce_input_bind_eval.c
    LINK     jce_platform jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/os/platform")

# The five tail buttons.  JCE_GAMEPAD_BUTTON_COUNT has been 26 since the enum
# was mirrored from SDL, but only 0..20 were named, so a fifth of the semantic
# button space could only be authored as a bare integer and nothing in the tree
# said which integers those were.  This target proves naming is TOTAL over the
# space -- every code named exactly once -- which is a claim a loop over
# 0..COUNT-1 cannot make, because the integers are there either way.
#
# No sdl::sdl by design: the SDL agreement is a compile-time JCE_SASSERT inside
# jce_input_sdl.c, the one TU allowed to say SDL.
jce_add_unit_test(test_jce_gamepad_tail_buttons
    SOURCES test_jce_gamepad_tail_buttons.c
    LINK    jce_platform jce_core)

# SEAM C -- the effector vtable -- with a recording fake in place of the
# platform backend, so rumble is testable headless with nothing plugged in.
#
# It is a SECOND fake, deliberately.  The one in test_jce_input_devices.c fixes
# its vtable at install time (rumble filled, set_led NULL), so it can only put
# DIFFERENT capabilities beside each other; this one keeps the vtable mutable,
# which is the only arrangement in which "this device cannot" and "this build
# cannot" can be produced for THE SAME capability -- and therefore the only one
# that proves the caps word, not the returned bool, is what tells a caller
# which refusal it got.  The mutable slot also lets "this build cannot" be
# taken away and given back on ONE device; "this device cannot" is not
# reproducible on that same record, because `caps` is written once by
# open_device on DEVICE_ADDED.
jce_add_unit_test(test_jce_input_backend_fake
    SOURCES test_jce_input_backend_fake.c
    LINK    jce_platform jce_core)

# The developer console a SHIPPED GAME can open -- the feature sentence of
# debug.console.cvars-at-runtime, whose only interactive reader in the whole
# tree was the editor's console panel.
#
# IT LIVES HERE, BESIDE JceInput, AND NOT IN middleware/ui, and that was
# forced rather than chosen: the first version was one object that both edited
# a line and drew it, and its test would not LINK, because linking jce_ui
# drags RmlUi in and RmlUi's prebuilt objects reference an MSVC STL symbol
# this toolchain does not provide.  So the console's whole behaviour -- line,
# history, Tab completion, scrollback ring, which keys are consumed -- is the
# shell, and jce_ui_console_overlay only turns what it reports into RML.
#
# The load-bearing cases: an OPEN console must consume input and a CLOSED one
# must not (either half alone is satisfied by a handler that always answers
# the same), and scrollback index 0 must stay the oldest line AFTER the ring
# wraps -- which is invisible until a session prints more than the ring holds,
# i.e. one `list` over a few hundred cvars.
jce_add_unit_test(test_jce_console_shell
    SOURCES test_jce_console_shell.c
    LINK    jce_core jce_platform)

# A real dependency chain outside the test executable's search directory.
if(JCE_PLATFORM_WINDOWS OR JCE_PLATFORM_LINUX OR JCE_PLATFORM_MACOS)
add_library(jce_library_test_dependency SHARED library_fixture_dependency.c)
add_library(jce_library_test_module SHARED library_fixture_module.c)
target_link_libraries(jce_library_test_module PRIVATE jce_library_test_dependency)
foreach(_library_fixture jce_library_test_dependency jce_library_test_module)
    set_target_properties(${_library_fixture} PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/library fixture"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/library fixture")
endforeach()
unset(_library_fixture)
jce_add_unit_test(test_jce_library SOURCES test_jce_library.c LINK jce_platform jce_core)
add_dependencies(test_jce_library jce_library_test_module)
target_compile_definitions(test_jce_library PRIVATE
    "JCE_LIBRARY_FIXTURE_PATH=\"$<TARGET_FILE:jce_library_test_module>\"")

else()
    jce_test_suite_skipped("platform-library" "dynamic module fixture requires a desktop host loader")
endif()

jce_add_unit_test(test_jce_input_mouse_tap
    SOURCES test_jce_input_mouse_tap.c
    LINK jce_platform jce_core)

# Exact adapter TU is included by the fixture with controlled SDL providers.
# No OS window, event activation or actual GPU is involved.
jce_add_unit_test(test_jce_window_flags
    SOURCES test_jce_window_flags.c
    LINK jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")


# Exact Windows owner with counted Win ports: no real HWND/mutex/UI activation.
# DIST strips INFO macros, while explicit startup refusal and opt-in lock
# diagnostics use the common direct logger. The old PID guard is a runtime fail.
if(WIN32)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/engine/src/os/platform/jce_single_instance.c"
        "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine.c")
    file(READ "${CMAKE_SOURCE_DIR}/engine/src/os/platform/jce_single_instance.c" _si_identity_owner)
    string(REGEX MATCHALL "actual_pid != published_pid" _si_identity_marks "${_si_identity_owner}")
    list(LENGTH _si_identity_marks _si_identity_mark_count)
    if(NOT _si_identity_mark_count EQUAL 1)
        message(FATAL_ERROR "Single instance old PID guard requires one actual owner comparison")
    endif()
    string(REPLACE "actual_pid != published_pid" "false" _si_identity_old "${_si_identity_owner}")
    if(_si_identity_old STREQUAL _si_identity_owner)
        message(FATAL_ERROR "Single instance identity mutation did not change actual owner")
    endif()
    set(_si_identity_old_path "${CMAKE_CURRENT_BINARY_DIR}/single-instance-mutation/jce_single_instance_old_pid.c")
    file(READ "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine.c" _si_engine_owner)
    set(_si_engine_begin_anchor "    if (!allow_multiple_instances &&\n        !jce_single_instance_lock(e->config.window_title)) {")
    set(_si_engine_end_anchor "    if (!e->headless && allow_multiple_instances)")
    set(_si_engine_failure_begin_anchor "\nfail:\n")
    set(_si_engine_failure_end_anchor "\n}\n\nJceEngine *jce_engine_create(")
    foreach(_si_anchor IN ITEMS begin end failure_begin failure_end)
        string(FIND "${_si_engine_owner}" "${_si_engine_${_si_anchor}_anchor}" _si_engine_${_si_anchor})
        string(FIND "${_si_engine_owner}" "${_si_engine_${_si_anchor}_anchor}" _si_engine_anchor_last REVERSE)
        if(_si_engine_${_si_anchor} LESS 0 OR NOT _si_engine_${_si_anchor} EQUAL _si_engine_anchor_last)
            message(FATAL_ERROR "Single instance fixture needs one actual engine ${_si_anchor} boundary")
        endif()
    endforeach()
    if(_si_engine_end LESS_EQUAL _si_engine_begin OR
       _si_engine_failure_begin LESS_EQUAL _si_engine_end OR
       _si_engine_failure_end LESS_EQUAL _si_engine_failure_begin)
        message(FATAL_ERROR "Single instance fixture needs the actual engine startup refusal block")
    endif()
    math(EXPR _si_engine_size "${_si_engine_end} - ${_si_engine_begin}")
    string(SUBSTRING "${_si_engine_owner}" ${_si_engine_begin} ${_si_engine_size} _si_engine_block)
    set(_si_engine_block_path "${CMAKE_CURRENT_BINARY_DIR}/single-instance-mutation/engine_single_instance_block.inc")
    math(EXPR _si_engine_failure_size "${_si_engine_failure_end} - ${_si_engine_failure_begin}")
    string(SUBSTRING "${_si_engine_owner}" ${_si_engine_failure_begin} ${_si_engine_failure_size} _si_engine_failure)
    set(_si_engine_failure_path "${CMAKE_CURRENT_BINARY_DIR}/single-instance-mutation/engine_single_instance_failure.inc")
    foreach(_si_kind IN ITEMS identity_old engine_block engine_failure)
        set(_si_content "${_si_${_si_kind}}")
        set(_si_path "${_si_${_si_kind}_path}")
        jce_test_write_if_changed("${_si_path}" "${_si_content}")
    endforeach()
    foreach(_si_target IN ITEMS test_jce_single_instance_identity test_jce_single_instance_identity_old_pid)
        if(_si_target STREQUAL "test_jce_single_instance_identity")
            set(_si_test_kind unit)
        else()
            set(_si_test_kind contract)
        endif()
        jce_add_unit_test(${_si_target} KIND ${_si_test_kind}
            SOURCES test_jce_single_instance_identity.c
            LINK jce_core
            INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
        target_compile_definitions(${_si_target} PRIVATE JCE_DIST=1
            JCE_TEST_SINGLE_INSTANCE_ENGINE_BLOCK="${_si_engine_block_path}"
            JCE_TEST_SINGLE_INSTANCE_ENGINE_FAILURE="${_si_engine_failure_path}")
        if(MSVC)
            target_compile_options(${_si_target} PRIVATE /Zc:preprocessor)
        endif()
    endforeach()
    target_compile_definitions(test_jce_single_instance_identity_old_pid PRIVATE
        JCE_TEST_SINGLE_INSTANCE_OWNER="${_si_identity_old_path}")
    set_tests_properties(test_jce_single_instance_identity_old_pid PROPERTIES WILL_FAIL TRUE)
else()
    jce_test_suite_skipped("single-instance-windows-identity"
        "Windows HWND/PID counted-port controls require the Windows SDK compiler; POSIX activation is not implemented")
endif()

# Exact cached point/drawable owner; providers count creation/resize queries.
set(_window_coordinates_owner "${CMAKE_SOURCE_DIR}/engine/src/os/platform/jce_window.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_window_coordinates_owner}")
file(READ "${_window_coordinates_owner}" _window_coordinates_source)
set(_window_coordinates_x "(double)x * win->pixel_w / win->point_w")
set(_window_coordinates_y "(double)y * win->pixel_h / win->point_h")
foreach(_window_coordinates_marker IN ITEMS "${_window_coordinates_x}" "${_window_coordinates_y}")
    string(FIND "${_window_coordinates_source}" "${_window_coordinates_marker}" _first)
    string(FIND "${_window_coordinates_source}" "${_window_coordinates_marker}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Window point control requires one actual coordinate scale")
    endif()
endforeach()
string(REPLACE "${_window_coordinates_x}" "(double)x" _window_coordinates_old "${_window_coordinates_source}")
string(REPLACE "${_window_coordinates_y}" "(double)y" _window_coordinates_old "${_window_coordinates_old}")
set(_window_coordinates_old_path "${CMAKE_CURRENT_BINARY_DIR}/window-coordinates/jce_window_old_scale.c")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/window-coordinates")
unset(_window_coordinates_previous)
if(EXISTS "${_window_coordinates_old_path}")
    file(READ "${_window_coordinates_old_path}" _window_coordinates_previous)
endif()
if(NOT _window_coordinates_previous STREQUAL _window_coordinates_old)
    file(WRITE "${_window_coordinates_old_path}" "${_window_coordinates_old}")
endif()
foreach(_window_coordinates_target IN ITEMS test_jce_window_coordinates test_jce_window_coordinates_old_scale)
    set(_window_coordinates_kind integration)
    if(_window_coordinates_target STREQUAL "test_jce_window_coordinates_old_scale")
        set(_window_coordinates_kind contract)
    endif()
    jce_add_unit_test(${_window_coordinates_target} KIND ${_window_coordinates_kind}
        SOURCES test_jce_window_coordinates.c
        LINK jce_platform jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/platform")
    if(_window_coordinates_target STREQUAL "test_jce_window_coordinates_old_scale")
        target_compile_definitions(${_window_coordinates_target} PRIVATE
            JCE_WINDOW_COORDINATE_OWNER="${_window_coordinates_old_path}")
    endif()
endforeach()
set_tests_properties(test_jce_window_coordinates_old_scale PROPERTIES WILL_FAIL TRUE)
unset(_window_coordinates_owner)
unset(_window_coordinates_source)
unset(_window_coordinates_x)
unset(_window_coordinates_y)
unset(_window_coordinates_marker)
unset(_window_coordinates_old)
unset(_window_coordinates_old_path)
unset(_window_coordinates_previous)
unset(_window_coordinates_target)
unset(_window_coordinates_kind)

# Complete startup observation TU with typed counted SDL/CCD/COM/heap ports.
# Runtime mutations must fail ordinary unchanged Unity assertions, never crash.
set(_window_adapter_owner "${CMAKE_SOURCE_DIR}/engine/src/os/platform/jce_window_adapter.c")
set(_window_adapter_legacy_owner "${CMAKE_CURRENT_SOURCE_DIR}/jce_test_window_adapter_legacy.inc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_window_adapter_owner}" "${_window_adapter_legacy_owner}")
file(READ "${_window_adapter_owner}" _window_adapter_source)
set(_window_adapter_marker "if(record.luid_low==source.luid_low && record.luid_high==source.luid_high){candidate=i;++matches;}")
string(FIND "${_window_adapter_source}" "${_window_adapter_marker}" _window_adapter_first)
string(FIND "${_window_adapter_source}" "${_window_adapter_marker}" _window_adapter_last REVERSE)
if(_window_adapter_first LESS 0 OR NOT _window_adapter_first EQUAL _window_adapter_last)
    message(FATAL_ERROR "Window adapter mutation requires one actual source-LUID comparison")
endif()
string(REPLACE "${_window_adapter_marker}"
    "if(record.luid_low!=source.luid_low && record.luid_high==source.luid_high){candidate=i;++matches;}"
    _window_adapter_wrong_luid "${_window_adapter_source}")
set(_window_adapter_mutant_dir "${CMAKE_CURRENT_BINARY_DIR}/window-display-adapter")
file(MAKE_DIRECTORY "${_window_adapter_mutant_dir}")
set(_window_adapter_first_key_wire "if(first_key!=candidate){out->reason=JCE_WINDOW_ADAPTER_BACKEND_KEY_MISMATCH;goto done;}")
string(FIND "${_window_adapter_source}" "${_window_adapter_first_key_wire}" _window_adapter_first)
string(FIND "${_window_adapter_source}" "${_window_adapter_first_key_wire}" _window_adapter_last REVERSE)
if(_window_adapter_first LESS 0 OR NOT _window_adapter_first EQUAL _window_adapter_last)
    message(FATAL_ERROR "Window adapter key controls require one actual backend first-match guard")
endif()
string(REPLACE "${_window_adapter_first_key_wire}"
    "if(duplicate!=1){out->reason=JCE_WINDOW_ADAPTER_BACKEND_KEY_MISMATCH;goto done;}"
    _window_adapter_old_strict_key "${_window_adapter_source}")
string(REPLACE "${_window_adapter_first_key_wire}"
    "if(false){out->reason=JCE_WINDOW_ADAPTER_BACKEND_KEY_MISMATCH;goto done;}"
    _window_adapter_old_first_only "${_window_adapter_source}")
set(_window_adapter_kinds wrong_luid old_strict_key old_first_only)
if(WIN32)
    # The old route resolves the same misleading virtual DXGI output and omits
    # CCD topology validation, as the former owner did. All other owner code is
    # kept from this exact source, including LUID-to-desc selection and cleanup.
    file(READ "${_window_adapter_legacy_owner}" _window_adapter_legacy_function)
    set(_window_adapter_ccd_wire "    bool observed=display_source_ccd(ctx,&ctx->snapshots[0],reason);")
    string(FIND "${_window_adapter_source}" "${_window_adapter_ccd_wire}" _window_adapter_first)
    string(FIND "${_window_adapter_source}" "${_window_adapter_ccd_wire}" _window_adapter_last REVERSE)
    if(_window_adapter_first LESS 0 OR NOT _window_adapter_first EQUAL _window_adapter_last)
        message(FATAL_ERROR "Legacy-output control requires exactly one actual CCD begin wire")
    endif()
    set(_window_adapter_begin "static bool display_begin(")
    string(FIND "${_window_adapter_source}" "${_window_adapter_begin}" _window_adapter_first)
    string(FIND "${_window_adapter_source}" "${_window_adapter_begin}" _window_adapter_last REVERSE)
    if(_window_adapter_first LESS 0 OR NOT _window_adapter_first EQUAL _window_adapter_last)
        message(FATAL_ERROR "Legacy-output control requires exactly one native begin owner")
    endif()
    string(REPLACE "${_window_adapter_begin}" "${_window_adapter_legacy_function}\n${_window_adapter_begin}"
        _window_adapter_old_dxgi_output "${_window_adapter_source}")
    string(REPLACE "${_window_adapter_ccd_wire}"
        "    bool observed=display_source_legacy_output(ctx,&ctx->snapshots[0],reason);"
        _window_adapter_old_dxgi_output "${_window_adapter_old_dxgi_output}")
    string(FIND "${_window_adapter_old_dxgi_output}" "static bool display_verify(" _window_adapter_verify_begin)
    string(FIND "${_window_adapter_old_dxgi_output}" "static void display_finish(" _window_adapter_verify_end)
    if(_window_adapter_verify_begin LESS 0 OR _window_adapter_verify_end LESS_EQUAL _window_adapter_verify_begin)
        message(FATAL_ERROR "Legacy-output control requires actual native topology verification body")
    endif()
    math(EXPR _window_adapter_verify_size "${_window_adapter_verify_end} - ${_window_adapter_verify_begin}")
    string(SUBSTRING "${_window_adapter_old_dxgi_output}" ${_window_adapter_verify_begin}
        ${_window_adapter_verify_size} _window_adapter_verify_owner)
    string(FIND "${_window_adapter_verify_owner}" "display_source_ccd(ctx,&ctx->snapshots[1],reason)" _window_adapter_verify_guard)
    string(FIND "${_window_adapter_verify_owner}" "memcmp(a->paths,b->paths" _window_adapter_verify_path_guard)
    string(FIND "${_window_adapter_verify_owner}" "monitor!=ctx->monitor" _window_adapter_verify_monitor_guard)
    if(_window_adapter_verify_guard LESS 0 OR _window_adapter_verify_path_guard LESS 0 OR _window_adapter_verify_monitor_guard LESS 0)
        message(FATAL_ERROR "Legacy-output control refuses an incomplete topology owner extraction")
    endif()
    string(REPLACE "${_window_adapter_verify_owner}"
        "static bool display_verify(void *user,const JceWindowDisplaySource *source,JceWindowAdapterReason *reason)\n{\n    (void)source;return display_deadline((JceWindowDxgiContext *)user,reason);\n}\n"
        _window_adapter_old_dxgi_output "${_window_adapter_old_dxgi_output}")
    list(APPEND _window_adapter_kinds old_dxgi_output)
endif()
foreach(_window_adapter_kind IN LISTS _window_adapter_kinds)
    set(_window_adapter_mutant_path "${_window_adapter_mutant_dir}/jce_window_adapter_${_window_adapter_kind}.c")
    unset(_window_adapter_previous)
    if(EXISTS "${_window_adapter_mutant_path}")
        file(READ "${_window_adapter_mutant_path}" _window_adapter_previous)
    endif()
    if(NOT _window_adapter_previous STREQUAL _window_adapter_${_window_adapter_kind})
        file(WRITE "${_window_adapter_mutant_path}" "${_window_adapter_${_window_adapter_kind}}")
    endif()
endforeach()
if(JCE_PLATFORM_WINDOWS)
    jce_add_unit_test(test_jce_window_dxgi_legacy KIND unit
        SOURCES test_jce_window_dxgi_legacy.c LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
endif()

jce_add_unit_test(test_jce_window_adapter KIND unit
    SOURCES test_jce_window_adapter.c LINK jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/platform")
if(MSVC)
    target_compile_options(test_jce_window_adapter PRIVATE /Zc:preprocessor)
endif()
foreach(_window_adapter_kind IN LISTS _window_adapter_kinds)
    set(_window_adapter_target "test_jce_window_adapter_${_window_adapter_kind}")
    jce_add_unit_test(${_window_adapter_target} KIND contract
        SOURCES test_jce_window_adapter.c LINK jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/platform")
    target_compile_definitions(${_window_adapter_target} PRIVATE
        JCE_WINDOW_ADAPTER_OWNER="${_window_adapter_mutant_dir}/jce_window_adapter_${_window_adapter_kind}.c")
    if(MSVC)
        target_compile_options(${_window_adapter_target} PRIVATE /Zc:preprocessor)
    endif()
    set_tests_properties(${_window_adapter_target} PROPERTIES WILL_FAIL TRUE)
endforeach()

# Complete private Web sample owner; legal counts remain provider observations.
set(_webgl_query_owner "${CMAKE_SOURCE_DIR}/engine/src/os/platform/jce_webgl_query_internal.c")
set(_webgl_query_js "${CMAKE_SOURCE_DIR}/engine/src/os/platform/jce_webgl_interop.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_webgl_query_owner}" "${_webgl_query_js}")
file(READ "${_webgl_query_owner}" _webgl_query_source)
set(_webgl_query_initial_wire "    if(count>0) samples[0]=0;")
set(_webgl_query_gate_wire "    if(target!=0x8d41u || pname!=0x80a9u || !renderbuffer_query_format(format,extensions)) {\n        provider->error(provider->user,JCE_WEBGL_INVALID_ENUM);\n        return JCE_WEBGL_QUERY_BAD_ENUM;\n    }")
set(_webgl_control_dir "${CMAKE_CURRENT_BINARY_DIR}/webgl-query-controls")
file(MAKE_DIRECTORY "${_webgl_control_dir}")
foreach(_webgl_variant IN ITEMS normal old_uninitialized_samples old_query_gate)
    set(_webgl_content "${_webgl_query_source}")
    set(_webgl_target test_jce_webgl_query)
    set(_webgl_kind unit)
    if(NOT _webgl_variant STREQUAL "normal")
        set(_webgl_target "test_jce_webgl_query_${_webgl_variant}")
        set(_webgl_kind contract)
        if(_webgl_variant STREQUAL "old_uninitialized_samples")
            set(_webgl_wire "${_webgl_query_initial_wire}")
        else()
            set(_webgl_wire "${_webgl_query_gate_wire}")
        endif()
        string(FIND "${_webgl_query_source}" "${_webgl_wire}" _webgl_first)
        string(FIND "${_webgl_query_source}" "${_webgl_wire}" _webgl_last REVERSE)
        if(_webgl_first LESS 0 OR NOT _webgl_first EQUAL _webgl_last)
            message(FATAL_ERROR "Web query control needs one exact actual wire")
        endif()
        string(REPLACE "${_webgl_wire}" "/* historical omitted wire */"
            _webgl_content "${_webgl_query_source}")
    endif()
    set(_webgl_path "${_webgl_control_dir}/${_webgl_variant}.c")
    unset(_webgl_previous)
    if(EXISTS "${_webgl_path}")
        file(READ "${_webgl_path}" _webgl_previous)
    endif()
    if(NOT _webgl_previous STREQUAL _webgl_content)
        file(WRITE "${_webgl_path}" "${_webgl_content}")
    endif()
    jce_add_unit_test(${_webgl_target} KIND ${_webgl_kind}
        SOURCES test_jce_webgl_query.c LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/os/platform")
    target_compile_definitions(${_webgl_target} PRIVATE
        "JCE_WEBGL_QUERY_OWNER=\"${_webgl_path}\"")
    if(MSVC)
        target_compile_options(${_webgl_target} PRIVATE /Zc:preprocessor)
    endif()
    if(NOT _webgl_variant STREQUAL "normal")
        set_tests_properties(${_webgl_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()

# Execute the complete JS bridge with counted CPU ports when Node is available.
find_program(_jce_webgl_node NAMES node nodejs)
if(_jce_webgl_node)
    file(READ "${_webgl_query_js}" _webgl_js_source)
    foreach(_webgl_variant IN ITEMS normal missing_error missing_context_reset missing_epoch)
        set(_webgl_js_path "${_webgl_query_js}")
        set(_webgl_test test_jce_webgl_interop_js)
        set(_webgl_kind unit)
        if(NOT _webgl_variant STREQUAL "normal")
            set(_webgl_kind contract)
            set(_webgl_test "test_jce_webgl_interop_js_${_webgl_variant}")
            if(_webgl_variant STREQUAL "missing_error")
                set(_webgl_wire "GL.recordError(error);")
            elseif(_webgl_variant STREQUAL "missing_context_reset")
                set(_webgl_wire "      if (JceWebGlQueryContext.context) jceWebGlInvalidateContext();")
            else()
                set(_webgl_wire "    JceWebGlQueryContext.epoch = next;")
            endif()
            string(FIND "${_webgl_js_source}" "${_webgl_wire}" _webgl_first)
            string(FIND "${_webgl_js_source}" "${_webgl_wire}" _webgl_last REVERSE)
            if(_webgl_first LESS 0 OR NOT _webgl_first EQUAL _webgl_last)
                message(FATAL_ERROR "Web JS control needs one exact actual wire")
            endif()
            string(REPLACE "${_webgl_wire}" "/* omitted wire */"
                _webgl_js_content "${_webgl_js_source}")
            set(_webgl_js_path "${_webgl_control_dir}/${_webgl_variant}.js")
            file(WRITE "${_webgl_js_path}" "${_webgl_js_content}")
        endif()
        add_test(NAME "${_webgl_test}" COMMAND "${_jce_webgl_node}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_jce_webgl_interop.js" "${_webgl_js_path}")
        jce_set_test_kind("${_webgl_test}" "${_webgl_kind}")
        set_tests_properties("${_webgl_test}" PROPERTIES
            TIMEOUT 30 ENVIRONMENT "${JCE_TEST_ENVIRONMENT}")
        set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV "${_webgl_test}")
        if(NOT _webgl_variant STREQUAL "normal")
            set_tests_properties("${_webgl_test}" PROPERTIES WILL_FAIL TRUE)
        endif()
    endforeach()
else()
    jce_test_suite_skipped("engine/webgl-query-js"
        "Node unavailable: actual JS CPU controls are not registered")
endif()

# Full SDL window owner with typed ports; model only its Web fill branch after
# native public/SDL headers. No browser context, OS window or GPU is created.
set(_window_fill_owner "${CMAKE_SOURCE_DIR}/engine/src/os/platform/jce_window.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_window_fill_owner}")
file(READ "${_window_fill_owner}" _window_fill_source)
set(_window_fill_wire "        flags |= SDL_WINDOW_FILL_DOCUMENT;")
string(FIND "${_window_fill_source}" "${_window_fill_wire}" _window_fill_first)
string(FIND "${_window_fill_source}" "${_window_fill_wire}" _window_fill_last REVERSE)
if(_window_fill_first LESS 0 OR NOT _window_fill_first EQUAL _window_fill_last)
    message(FATAL_ERROR "Web window fill control requires one exact production wire")
endif()
string(REPLACE "${_window_fill_wire}" "        (void)flags;"
    _window_fill_old "${_window_fill_source}")
set(_window_fill_old_path "${CMAKE_CURRENT_BINARY_DIR}/window-fill/jce_window_missing_fill.c")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/window-fill")
unset(_window_fill_previous)
if(EXISTS "${_window_fill_old_path}")
    file(READ "${_window_fill_old_path}" _window_fill_previous)
endif()
if(NOT _window_fill_previous STREQUAL _window_fill_old)
    file(WRITE "${_window_fill_old_path}" "${_window_fill_old}")
endif()
foreach(_window_fill_target IN ITEMS test_jce_window_web_fill test_jce_window_host_fill
        test_jce_window_web_fill_missing_flag)
    set(_window_fill_kind unit)
    set(_window_fill_web 1)
    if(_window_fill_target STREQUAL "test_jce_window_host_fill")
        set(_window_fill_web 0)
    elseif(_window_fill_target STREQUAL "test_jce_window_web_fill_missing_flag")
        set(_window_fill_kind contract)
    endif()
    jce_add_unit_test(${_window_fill_target} KIND ${_window_fill_kind}
        SOURCES test_jce_window_web_fill.c
        LINK jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/os/platform")
    target_compile_definitions(${_window_fill_target} PRIVATE
        JCE_WINDOW_FILL_WEB_CONTROL=${_window_fill_web})
    if(_window_fill_kind STREQUAL "contract")
        target_compile_definitions(${_window_fill_target} PRIVATE
            JCE_WINDOW_FILL_OWNER="${_window_fill_old_path}")
        set_tests_properties(${_window_fill_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_window_fill_variable IN ITEMS owner source wire first last old old_path
        previous target kind web)
    unset(_window_fill_${_window_fill_variable})
endforeach()
unset(_window_fill_variable)
