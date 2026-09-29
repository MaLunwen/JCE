# ====================================================================== #
# JCE Unit Tests                                                          #
# ----------------------------------------------------------------------- #
# Activated by -DJCE_BUILD_TESTS=ON at the top-level configure step.      #
# Provides:                                                               #
#   * Vendored Unity (ThrowTheSwitch) test framework                      #
#   * jce_add_unit_test(<name> SOURCES ... [LINK ...]) helper             #
#   * Auto-registers each test with CTest                                 #
#                                                                         #
# Layer mapping: every test target links the engine layer it exercises,   #
# never the umbrella jce_engine — keeps tests as focused as the module.   #
# ====================================================================== #

# Vendored Unity (single static lib, reused by every test).
jce_vendor_source(unity JCE_UNITY_SOURCE_DIR)
add_library(jce_unity STATIC "${JCE_UNITY_SOURCE_DIR}/src/unity.c")
add_dependencies(jce_unity jce_vendor_verify_unity)
target_include_directories(jce_unity SYSTEM PUBLIC "${JCE_UNITY_SOURCE_DIR}/src")
# Unity prints colour ANSI by default; keep output deterministic for CI.
target_compile_definitions(jce_unity PUBLIC
    UNITY_INCLUDE_DOUBLE
    UNITY_OUTPUT_COLOR)
# Silence warnings inside the vendored translation unit only.
if(MSVC)
    target_compile_options(jce_unity PRIVATE /W0)
else()
    target_compile_options(jce_unity PRIVATE -w)
endif()

# A debug test executable may emit a 100-300 MiB linker PDB because the engine
# is linked statically.  Unbounded Ninja parallelism can make MSVC fail with
# LNK1201/LNK1285 while many distinct PDBs are written at once.  Keep compile
# parallelism unrestricted and cap only test links.  The cache entry lets
# high-throughput CI hosts raise the limit deliberately.
set(JCE_TEST_LINK_JOBS 2 CACHE STRING
    "Maximum concurrent Ninja links for JCE unit-test executables")
if(CMAKE_GENERATOR MATCHES "Ninja")
    if(NOT JCE_TEST_LINK_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "JCE_TEST_LINK_JOBS must be a positive integer")
    endif()
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS
        jce_test_link_pool=${JCE_TEST_LINK_JOBS})
endif()

# ── Test process environment ──────────────────────────────────────────────
#
# TRACY_ONLY_LOCALHOST: a test must never ask the OS for a public listening
# socket.  It does today, and not because anything here wants the network.
# JCE_ENABLE_PROFILING defaults ON, so engine/CMakeLists.txt links
# Tracy::TracyClient into jce_core, every test links jce_core, and a static
# library's PRIVATE link dependency still reaches the executable.  Tracy's
# client then listens on port 8086 for a profiler to attach.
#
# Measured, not assumed: test_jce_world_streamer_churn.exe -- a world-streaming
# test with no network code of any kind -- listens on `:::8086`, the IPv6 ANY
# address, i.e. every interface.  Windows Firewall currently holds 3242 rules
# for JCE binaries, all Inbound; test_jce_str.exe -- pure string logic -- owns 14.
#
# THIS VARIABLE IS NOT THE FIX, and on its own it never was.  Tracy reads
# TRACY_ONLY_LOCALHOST at RUNTIME (TracySocket.cpp, ListenSocket::Listen), and
# a ctest ENVIRONMENT property only exists when *ctest* launches the process.
# It does nothing for the ways these binaries are actually run most of the
# time: `./tests/test_foo.exe` straight from the build tree, or a binary
# copied to a scratch directory by an agent running a mutation battery.  Both
# were still opening `:::8087` after this line was added.
#
# The real fix is compile-time and lives in conanfile.py --
# tracy/*:only_localhost + tracy/*:no_broadcast -- which covers every launch
# path at once (ctest, direct runs, scratch copies, space_demo, the editor)
# because it is baked into TracyClient itself.  CMakeLists.txt asserts at
# configure time that the linked Tracy carries those defines.
#
# This variable is kept as defence in depth for the one configuration the
# compile-time fix deliberately does not cover: a build that opted in to
# remote profiling via -DJCE_PROFILING_ALLOW_REMOTE=ON.  There, tests should
# still stay off the public interfaces even though the editor may not.  The
# assertion further down (JCE_TESTS_WITH_ENV) is what keeps a newly added
# test from silently skipping it.
set(JCE_TEST_ENVIRONMENT "TRACY_ONLY_LOCALHOST=1"
    CACHE INTERNAL "Environment applied to every registered JCE test")

function(jce_configure_test_link_pool TARGET_NAME)
    if(CMAKE_GENERATOR MATCHES "Ninja")
        set_property(TARGET ${TARGET_NAME} PROPERTY
            JOB_POOL_LINK jce_test_link_pool)
    endif()
endfunction()

# One target that builds EVERY registered test.
#
# ctest runs the tests it was told about; a test whose executable was never
# built fails with "Unable to find executable", which reads exactly like a real
# failure.  Building with no target instead builds the editor and every
# vendored codec -- half an hour on a hosted runner for a suite that links
# jce_core and jce_platform.  This target is the dependency closure of the
# suite and nothing else.  CI builds it; see .github/workflows/ci.yml.
add_custom_target(jce_tests)

# ---------------------------------------------------------------- #
#  jce_test_suite_skipped(<suite> <detail>)                         #
#                                                                    #
#  ONE call that both REGISTERS and PRINTS.  Two calls would let a    #
#  site print without registering, and the summary at the bottom of   #
#  this file would then under-report while the configure log looked   #
#  complete -- MEASURED: removing one registration left its STATUS    #
#  line on screen, left the count at 351, and quietly dropped the      #
#  summary from 2 suites to 1.  A site that forgets this helper        #
#  prints NOTHING, which is visible in review; a site that forgets a   #
#  second bookkeeping call is invisible.                               #
#                                                                     #
#  Same shape as jce_add_unit_test owning the TRACY_ONLY_LOCALHOST     #
#  arming: the thing that must not be forgotten is not separately      #
#  callable.                                                           #
# ---------------------------------------------------------------- #
function(jce_test_suite_skipped SUITE DETAIL)
    # A ';' in the detail would SPLIT the entry: the property is a CMake list
    # and that is its separator.  Measured on the first caller -- one entry
    # containing "run this rather than setting it here; this directory keeps
    # a JDK out" became two lines and made the summary say 3 suites when there
    # were 2.  The detail is prose, where a semicolon carries nothing a comma
    # cannot, so it is replaced rather than escaped: escaping would survive
    # the store and reappear as a backslash in the message.
    string(REPLACE ";" "," _d "${DETAIL}")
    set_property(GLOBAL APPEND PROPERTY JCE_TEST_SUITES_SKIPPED
        "${SUITE}: ${_d}")
    message(STATUS "JCE tests: SKIPPED ${SUITE} — ${_d}")
endfunction()

# Helper: jce_add_unit_test(<name>
#                          SOURCES  src1.c [...]
#                          [LINK    lib1  [...]]
#                          [INCLUDES dir1 [...]])
# INCLUDES is *prepended* (BEFORE PRIVATE), matching jce_add_cxx_test, so a
# test may reach an engine-private header (e.g. jce_input_sdl.h) or shadow one
# with a test-only stub.  A C test that needs a private header is the normal
# case for seam tests, not an exception.
function(jce_add_unit_test NAME)
    cmake_parse_arguments(ARG "" "" "SOURCES;LINK;INCLUDES" ${ARGN})

    add_executable(${NAME} ${ARG_SOURCES})
    set_target_properties(${NAME} PROPERTIES
        C_STANDARD          99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS        OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    jce_configure_test_link_pool(${NAME})
    target_link_libraries(${NAME} PRIVATE jce_unity ${ARG_LINK})
    target_include_directories(${NAME} BEFORE PRIVATE ${ARG_INCLUDES})
    target_include_directories(${NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/include")

    add_dependencies(jce_tests ${NAME})

    add_test(NAME ${NAME} COMMAND ${NAME})
    set_tests_properties(${NAME} PROPERTIES
        TIMEOUT 60
        LABELS  "unit"
        ENVIRONMENT "${JCE_TEST_ENVIRONMENT}")
    set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV ${NAME})
endfunction()

# Vendored doctest (single header; one .cpp picks up DOCTEST_CONFIG_IMPLEMENT
# from inside each test, so no shared static lib).  Editor-only.
add_library(jce_doctest INTERFACE)
jce_vendor_source(doctest JCE_DOCTEST_SOURCE_DIR)
add_dependencies(jce_doctest jce_vendor_verify_doctest)
target_include_directories(jce_doctest SYSTEM INTERFACE "${JCE_DOCTEST_SOURCE_DIR}/doctest")

# Helper: jce_add_cxx_test(<name>
#                         SOURCES  src1.cpp [...]
#                         [LINK     lib1   [...]]
#                         [INCLUDES dir1   [...]])
# C++17 doctest-style scaffold.  INCLUDES is *prepended* so callers can
# shadow engine/editor headers with test-only stubs (e.g. a minimal
# ImGui shim to break the dependency on the full Tools layer).
function(jce_add_cxx_test NAME)
    cmake_parse_arguments(ARG "" "" "SOURCES;LINK;INCLUDES" ${ARGN})

    add_executable(${NAME} ${ARG_SOURCES})
    set_target_properties(${NAME} PROPERTIES
        CXX_STANDARD          17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS        OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    jce_configure_test_link_pool(${NAME})
    target_link_libraries(${NAME} PRIVATE jce_doctest ${ARG_LINK})
    target_include_directories(${NAME} BEFORE PRIVATE ${ARG_INCLUDES})
    target_include_directories(${NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/include"
        "${CMAKE_SOURCE_DIR}/editor/src")
    if(MSVC)
        target_compile_options(${NAME} PRIVATE /wd4267 /wd4244)
    endif()

    add_dependencies(jce_tests ${NAME})

    add_test(NAME ${NAME} COMMAND ${NAME})
    set_tests_properties(${NAME} PROPERTIES
        TIMEOUT 60
        LABELS  "unit;cxx;editor"
        ENVIRONMENT "${JCE_TEST_ENVIRONMENT}")
    set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV ${NAME})
endfunction()

# --- Test suites (one add_subdirectory per layer) ---------------------- #
add_subdirectory(os/core)
add_subdirectory(os/platform)
add_subdirectory(os/resource)
add_subdirectory(application)
add_subdirectory(renderer)
add_subdirectory(middleware/scene)
add_subdirectory(middleware/net)
add_subdirectory(middleware/world)
add_subdirectory(middleware/save)
add_subdirectory(middleware/llm)
if(JCE_ENABLE_AI_DISPATCH)
    add_subdirectory("${CMAKE_SOURCE_DIR}/private/tests/ai_dispatch"
                     "${CMAKE_BINARY_DIR}/private/tests/ai_dispatch")
endif()
add_subdirectory(middleware/animation)
add_subdirectory(middleware/audio)
add_subdirectory(middleware/video)
add_subdirectory(middleware/ai)
add_subdirectory(middleware/physics)
add_subdirectory(middleware/script)

# The script-facing shared library's own suite.  It is NOT under
# middleware/script: those tests link jce_script and drive Lua, while these
# load a DLL by absolute path and read its export table — the same way a
# ctypes or JNA binding will.
add_subdirectory(scripting)

# Consumer scientific validation remains outside the generic engine.  The
# reference solver links public JCE facilities only and is never part of the
# shipped Caged Kingdom executable.
#
# Guarded because the directory is present-but-empty in worktrees that do not
# carry the suite: an unconditional add_subdirectory() on a directory without a
# CMakeLists.txt is a hard CMake error, so a fresh configure died outright and
# every gate below it became unreachable.  Absent means SKIPPED with a stated
# reason -- never a silent pass, and never a failure that hides the real one.
if(EXISTS "${CMAKE_SOURCE_DIR}/examples/caged_kingdom/tests/black_hole/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/examples/caged_kingdom/tests/black_hole"
                     "${CMAKE_CURRENT_BINARY_DIR}/ck_black_hole")
else()
    jce_test_suite_skipped("ck_black_hole"
        "examples/caged_kingdom/tests/black_hole/CMakeLists.txt is not present in this worktree")
endif()

# Editor (C++17) suites — desktop-only, mirror editor/CMakeLists.txt guard.
if(NOT (JCE_PLATFORM_ANDROID OR JCE_PLATFORM_IOS
        OR JCE_PLATFORM_WEB OR JCE_BUILD_JNI))
    add_subdirectory(editor)
endif()

# Project-specific assertions are owned by the consumer.
if(EXISTS "${CMAKE_SOURCE_DIR}/examples/caged_kingdom/tests/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/examples/caged_kingdom/tests"
                     "${CMAKE_CURRENT_BINARY_DIR}/consumers/caged_kingdom")
endif()


# Configure-time contract test for the SDK's shipping target policy.  This is
# EXCLUDE_FROM_ALL: it validates target properties without adding another test
# executable to normal builds.
include("${CMAKE_SOURCE_DIR}/cmake/JCESDKHelpers.cmake")
set(_jce_saved_variant "${JCE_BUILD_VARIANT}")
set(JCE_BUILD_VARIANT dist)
add_executable(jce_dist_policy_probe EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/dist_policy_probe.c")
jce_configure_application_target(jce_dist_policy_probe)
get_target_property(_jce_dist_defs jce_dist_policy_probe COMPILE_DEFINITIONS)
if(NOT "JCE_DIST=1" IN_LIST _jce_dist_defs)
    message(FATAL_ERROR "dist policy probe: JCE_DIST=1 is missing")
endif()
get_target_property(_jce_dist_ipo jce_dist_policy_probe
    INTERPROCEDURAL_OPTIMIZATION_RELEASE)
if(NOT _jce_dist_ipo)
    message(FATAL_ERROR "dist policy probe: release IPO is not enabled")
endif()
if(WIN32)
    get_target_property(_jce_dist_gui jce_dist_policy_probe WIN32_EXECUTABLE)
    if(NOT _jce_dist_gui)
        message(FATAL_ERROR "dist policy probe: Windows GUI subsystem missing")
    endif()
endif()
set(JCE_BUILD_VARIANT "${_jce_saved_variant}")
unset(_jce_saved_variant)
unset(_jce_dist_defs)
unset(_jce_dist_ipo)
unset(_jce_dist_gui)

# ── Assert every registered test really carries JCE_TEST_ENVIRONMENT ──────
#
# Same reasoning as the patented-codec gate in the root CMakeLists: without
# this, "no test asks for a public listening socket" is only a property of the
# two helpers above happening to set it -- and the helpers are not the only way
# in.  tests/middleware/ai_dispatch/CMakeLists.txt registers aid_demo_selftest
# with a bare add_test(), deliberately, because its ctest name must differ from
# its target name and the helper hard-codes add_test(NAME ${NAME} COMMAND
# ${NAME}).  A guarantee that lives in a wrapper is voided by the next person
# with a reason to bypass the wrapper; this repo already grew
# check_jce_tests_closure.py after exactly that.
#
# Reading the ENVIRONMENT property back would be the direct check, but
# get_property(TEST ... DIRECTORY ...) needs CMake 3.28 and this project
# requires 3.20 (3.27.9 here).  So instead each helper RECORDS the tests it
# armed, and this compares that record against everything ctest actually
# registered.  A bare add_test() that also sets the environment must append to
# JCE_TESTS_WITH_ENV to say so -- which makes the opt-out explicit and greppable
# rather than silent.
function(_jce_collect_tests DIR OUT)
    get_property(_here DIRECTORY "${DIR}" PROPERTY TESTS)
    get_property(_subs DIRECTORY "${DIR}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subs)
        _jce_collect_tests("${_sub}" _from_sub)
        list(APPEND _here ${_from_sub})
    endforeach()
    set(${OUT} "${_here}" PARENT_SCOPE)
endfunction()

_jce_collect_tests("${CMAKE_CURRENT_SOURCE_DIR}" _jce_all_tests)
get_property(_jce_armed GLOBAL PROPERTY JCE_TESTS_WITH_ENV)
set(_jce_naked_tests "")
foreach(_t IN LISTS _jce_all_tests)
    if(NOT "${_t}" IN_LIST _jce_armed)
        list(APPEND _jce_naked_tests "${_t}")
    endif()
endforeach()
list(LENGTH _jce_all_tests _jce_test_count)
if(_jce_naked_tests)
    string(REPLACE ";" "
  - " _jce_naked_pretty "${_jce_naked_tests}")
    message(FATAL_ERROR
        "These registered tests were not armed with ${JCE_TEST_ENVIRONMENT}, so "
        "each opens a listening socket on every network interface and makes "
        "Windows raise a public-network authorisation dialog on every rebuild:"
        "
  - ${_jce_naked_pretty}
"
        "Register through jce_add_unit_test/jce_add_cxx_test.  If a bare "
        "add_test() is genuinely required, follow it with
"
        "  set_tests_properties(<name> PROPERTIES ENVIRONMENT \"\${JCE_TEST_ENVIRONMENT}\")
"
        "  set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV <name>)")
endif()
# ---------------------------------------------------------------- #
#  WHAT THIS CONFIGURE COULD NOT REGISTER.                           #
#                                                                     #
#  The count above is a property of THIS MACHINE'S configure, not of  #
#  the repository: a suite whose backend is off registers nothing and #
#  its tests simply are not in the list.  Measured -- the default      #
#  configure registers 351 and one with JCE_BUILD_SCRIPT_JAVA=ON      #
#  registers 355, the four extra including the JNI/Lua DIFFERENTIAL,   #
#  which is the only thing that checks the generated Java bindings.    #
#                                                                      #
#  Printing the shortfall beside the count is what stops "351" from    #
#  being read as "all of them".  jce_test_suite_skipped() is the       #
#  registration side, mirroring how jce_add_unit_test registers into   #
#  JCE_TESTS_WITH_ENV -- one global property, one reader.              #
# ---------------------------------------------------------------- #
get_property(_jce_skipped GLOBAL PROPERTY JCE_TEST_SUITES_SKIPPED)
list(LENGTH _jce_skipped _jce_skipped_count)
# ---------------------------------------------------------------- #
#  WHAT THIS CONFIGURE COULD NOT REGISTER.                          #
#                                                                    #
#  The count above is a property of THIS MACHINE'S configure, not of #
#  the repository: a suite whose backend is off registers nothing and #
#  its tests are simply not in the list.  MEASURED -- the default     #
#  configure registers 351; one with JCE_BUILD_SCRIPT_JAVA=ON        #
#  registers 355, and the four extra include the JNI/Lua             #
#  DIFFERENTIAL, which is the only check on the generated Java       #
#  bindings.  They were green the first time anybody ran them, and    #
#  nothing had: this worktree carries two hand-made java build        #
#  directories, no preset and no script.                              #
#                                                                     #
#  Printing the shortfall beside the count is what stops "351" being  #
#  read as "all of them".  Registration mirrors JCE_TESTS_WITH_ENV:   #
#  one global property, appended at each site, read once here.        #
# ---------------------------------------------------------------- #
get_property(_jce_skipped GLOBAL PROPERTY JCE_TEST_SUITES_SKIPPED)
list(LENGTH _jce_skipped _jce_skipped_count)
message(STATUS
    "JCE tests: ${_jce_test_count} registered, all carry ${JCE_TEST_ENVIRONMENT}")
if(_jce_skipped)
    message(STATUS
        "JCE tests: ${_jce_skipped_count} SUITE(S) SKIPPED — ${_jce_test_count} "
        "is what THIS configure could build, NOT the whole suite:")
    foreach(_s IN LISTS _jce_skipped)
        message(STATUS "JCE tests:   - ${_s}")
    endforeach()
    # The advice is only advice for the suites it can actually restore.  A
    # line telling you to run scripting-check when the only skip is an absent
    # caged_kingdom directory is a line that trains people to ignore it.
    set(_jce_skipped_script FALSE)
    foreach(_s IN LISTS _jce_skipped)
        if(_s MATCHES "^script")
            set(_jce_skipped_script TRUE)
        endif()
    endforeach()
    if(_jce_skipped_script)
        message(STATUS
            "JCE tests: those suites switch themselves ON when their "
            "toolchain is present — a JDK on JAVA_HOME for script-java, an "
            "embeddable CPython for script-python. Install the one you want "
            "and re-configure; there is no flag to remember.")
    endif()
    unset(_jce_skipped_script)
endif()
unset(_jce_skipped)
unset(_jce_skipped_count)
if(_jce_skipped)
    message(STATUS
        "JCE tests: ${_jce_skipped_count} SUITE(S) SKIPPED — "
        "${_jce_test_count} is what THIS configure could build, not the whole "
        "suite:")
    foreach(_s IN LISTS _jce_skipped)
        message(STATUS "JCE tests:   - ${_s}")
    endforeach()
    # A SECOND copy of this advice existed and named a `jce.py scripting-check`
    # command that was reverted; a message pointing at a command that does not
    # exist is worse than no message.  The suites switch themselves on when
    # their toolchain is present, so the action is to install the toolchain.
    set(_jce_skipped_script FALSE)
    foreach(_s IN LISTS _jce_skipped)
        if(_s MATCHES "^script")
            set(_jce_skipped_script TRUE)
        endif()
    endforeach()
    if(_jce_skipped_script)
        message(STATUS
            "JCE tests: those suites switch themselves ON when their "
            "toolchain is present — a JDK on JAVA_HOME for script-java, an "
            "embeddable CPython for script-python, a resolved quickjs-ng for "
            "script-js. Install the one you want and re-configure; there is "
            "no flag to remember.")
    endif()
    unset(_jce_skipped_script)
endif()
unset(_jce_skipped)
unset(_jce_skipped_count)
unset(_jce_all_tests)
unset(_jce_armed)
unset(_jce_naked_tests)
unset(_jce_test_count)
