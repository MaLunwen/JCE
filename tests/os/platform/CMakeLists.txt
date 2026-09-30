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
