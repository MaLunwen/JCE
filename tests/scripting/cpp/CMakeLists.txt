cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  The C++ wrapper's suite.                                               #
#                                                                         #
#  Two executables, and the split is the point:                           #
#                                                                         #
#   * test_jce_script_cpp_differential — C++20.  Drives every             #
#     manifest-derived case through a REAL LUA VM and through the C++     #
#     wrapper over ONE recording mock host, and compares the full result  #
#     (type and value per slot) and the host-call trace.  Lua is the      #
#     reference implementation: batch 1 spent the hand-written oracle and #
#     said so, but Lua itself is still a shipped, tested, independently   #
#     generated implementation of the same manifest.                      #
#                                                                         #
#   * test_jce_script_cpp17_floor — C++17.  The header CLAIMS to require  #
#     only C++17.  This compiles it at exactly 17 and runs it, so the     #
#     claim fails a build instead of a code review.  A comment stating a  #
#     contract nothing enforces is a defect; this is the enforcement.     #
#                                                                         #
#  Written out rather than routed through jce_add_cxx_test because that   #
#  helper pins CXX_STANDARD 17 for every target it makes (so the C++20    #
#  half could not exist), puts editor/src on the include path, and labels #
#  its tests "editor" — none of which is true here.  The pieces the       #
#  helper provides that MATTER are reproduced explicitly below:           #
#  add_dependencies(jce_tests ...) (check_jce_tests_closure.py enforces   #
#  it for raw add_test), the link job pool, the runtime output directory, #
#  the 60 s timeout and JCE_TEST_ENVIRONMENT.                             #
# ----------------------------------------------------------------------- #

if(NOT TARGET jce_script_cpp)
    # Not silent: the differential is where "the C++ wrapper agrees with Lua"
    # is checked, and its absence belongs in the configure log rather than
    # being inferred from a test count.
    message(STATUS
        "JCE tests: C++ scripting-wrapper suite SKIPPED — jce_script_cpp is "
        "not built (-DJCE_BUILD_SCRIPT_API_SHARED=OFF)")
    return()
endif()

function(jce_add_scripting_cpp_test NAME STD)
    cmake_parse_arguments(ARG "" "" "SOURCES;LINK" ${ARGN})

    add_executable(${NAME} ${ARG_SOURCES})
    set_target_properties(${NAME} PROPERTIES
        CXX_STANDARD          ${STD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS        OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    jce_configure_test_link_pool(${NAME})
    target_link_libraries(${NAME} PRIVATE jce_doctest jce_script_cpp ${ARG_LINK})
    target_include_directories(${NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/include"
        "${CMAKE_CURRENT_SOURCE_DIR}")

    # jce_script_api is a SHARED library and these link it.  On Windows the
    # loader searches the executable's own directory, and the DLL is built
    # under scripting/c_abi/ — so without this copy the test aborts at
    # process start with a dialog-free 0xC0000135 that reads as a crash.
    add_custom_command(TARGET ${NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:jce_script_api>"
                "$<TARGET_FILE_DIR:${NAME}>"
        COMMENT "Staging jce_script_api beside ${NAME}")

    add_dependencies(jce_tests ${NAME})

    add_test(NAME ${NAME} COMMAND ${NAME})
    set_tests_properties(${NAME} PROPERTIES
        TIMEOUT 60
        LABELS  "unit"
        ENVIRONMENT "${JCE_TEST_ENVIRONMENT}")
    set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV ${NAME})
endfunction()

# The differential needs a real Lua VM, which is jce_script; jce_script keeps
# lua::lua PRIVATE, so the public jce_script API is all that is linked here —
# the same link set tests/middleware/script uses.
jce_add_scripting_cpp_test(test_jce_script_cpp_differential 20
    SOURCES test_jce_script_cpp_differential.gen.cpp
            test_jce_script_cpp_contract.cpp
    LINK    jce_core jce_script)

# The floor test links NO engine layer.  It exercises the wrapper over the C
# ABI alone, which is exactly the shape an SDK consumer on C++17 has.
jce_add_scripting_cpp_test(test_jce_script_cpp17_floor 17
    SOURCES test_jce_script_cpp17_floor.cpp)

# ── The floor test must actually be at the floor ─────────────────────────
#
# If this target ever inherits CMAKE_CXX_STANDARD 20 (the repo-wide default at
# CMakeLists.txt:149), it compiles the header at 20, passes, and proves
# nothing about C++17 — a green light nobody measured.  Asserted at configure
# time because a wrong standard produces no build error at all.
get_target_property(_jce_cpp17_std test_jce_script_cpp17_floor CXX_STANDARD)
if(NOT _jce_cpp17_std STREQUAL "17")
    message(FATAL_ERROR
        "test_jce_script_cpp17_floor has CXX_STANDARD ${_jce_cpp17_std}, not 17 "
        "— it would compile the wrapper at a standard it does not claim, and "
        "the C++17 requirement would stop being tested.")
endif()
unset(_jce_cpp17_std)

# ======================================================================= #
#  The cpp JceScriptVM suite — the engine calling UP.                     #
#                                                                         #
#  Three pieces, and the third is the one that cannot be faked:           #
#                                                                         #
#   * jce_cpp_testplugin — a script module built as a real MODULE         #
#     library.  A statically linked stand-in cannot fail the way a plugin #
#     can: its code stays mapped whatever the registry does, so a missing #
#     unload refcount would be INVISIBLE.  Everything about               #
#     jce_script_vm_cpp_unload()'s refusal is only observable against an  #
#     actual jce_library_open / jce_library_close.                        #
#   * test_jce_script_vm_cpp — the lifecycle differential against a real  #
#     Lua VM, plus the module-ABI half in a second translation unit.      #
#   * the plugin's ABSOLUTE PATH, handed to the test as a compile         #
#     definition.  jce_script_vm_cpp_load_library REFUSES a bare name     #
#     (jce_library_open attaches to an already-resident module of the     #
#     same name), so the test cannot just spell the file name — and       #
#     $<TARGET_FILE:> is the only absolute path that stays correct across #
#     generators and configurations.                                      #
# ======================================================================= #

add_library(jce_cpp_testplugin MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/vm_plugin/jce_cpp_testplugin.cpp")
set_target_properties(jce_cpp_testplugin PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS        OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
# jce_script_cpp_module, NOT jce_script_vm_cpp: a module must not carry the
# registry.  A plugin that linked its own copy would publish its classes into
# a registry the engine never reads — a module that loads, reports success,
# and whose classes no instantiate() can find.
target_link_libraries(jce_cpp_testplugin PRIVATE jce_script_cpp_module)
jce_configure_test_link_pool(jce_cpp_testplugin)

# THE SAME MODULE, REBUILT.  Same module name, same class name, one different
# string in one method -- so a test can copy v1 over a path, load it, unload
# it, copy v2 over the SAME path and load again, and tell from the behaviour
# whether the second load actually read the file or re-attached to the image
# already mapped.  jce_library.h:36-39 records that Windows does the latter by
# default, which is precisely the failure a "reload" command would hide.
add_library(jce_cpp_testplugin_v2 MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/vm_plugin/jce_cpp_testplugin_v2.cpp")
set_target_properties(jce_cpp_testplugin_v2 PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS        OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
target_link_libraries(jce_cpp_testplugin_v2 PRIVATE jce_script_cpp_module)
jce_configure_test_link_pool(jce_cpp_testplugin_v2)

jce_add_scripting_cpp_test(test_jce_script_vm_cpp 17
    SOURCES test_jce_script_vm_cpp_lifecycle.cpp
            test_jce_script_vm_cpp_modules.cpp
    LINK    jce_core jce_script jce_script_vm_cpp)

# The plugin must EXIST before the test runs, and it is loaded by path rather
# than linked, so nothing in the link graph would say so.
add_dependencies(test_jce_script_vm_cpp jce_cpp_testplugin jce_cpp_testplugin_v2)

target_compile_definitions(test_jce_script_vm_cpp PRIVATE
    "JCE_CPP_TESTPLUGIN_PATH=\"$<TARGET_FILE:jce_cpp_testplugin>\""
    "JCE_CPP_TESTPLUGIN_V2_PATH=\"$<TARGET_FILE:jce_cpp_testplugin_v2>\""
    "JCE_CPP_VM_HEADER_DIR=\"${CMAKE_SOURCE_DIR}/scripting/cpp/include/jce/script_vm\"")

# ── The suite must be at C++17, not the repo-wide 20 ─────────────────────
#
# Same reasoning as the floor assertion above, applied to the VM half: the
# module header claims C++17 and the module author's build is the one that
# would break.  Compiling the whole suite at 17 is what makes the claim fail a
# build instead of a code review.
foreach(_jce_vm_tgt test_jce_script_vm_cpp jce_cpp_testplugin)
    get_target_property(_jce_vm_std ${_jce_vm_tgt} CXX_STANDARD)
    if(NOT _jce_vm_std STREQUAL "17")
        message(FATAL_ERROR
            "${_jce_vm_tgt} has CXX_STANDARD ${_jce_vm_std}, not 17 — the cpp "
            "script-VM headers claim C++17 and nothing would be testing it.")
    endif()
endforeach()
unset(_jce_vm_tgt)
unset(_jce_vm_std)
