cmake_minimum_required(VERSION 3.24)
if(NOT IS_ABSOLUTE "${CMAKE_TOOLCHAIN_FILE}" OR NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
  message(FATAL_ERROR "Supply the explicit XP LLVM-MinGW toolchain")
endif()
project(jce_xp_mimalloc_controls LANGUAGES C)
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(JCE_WINXP_LEGACY ON CACHE BOOL "Exercise the actual XP platform contract" FORCE)
include("${_root}/cmake/JCEWindowsTarget.cmake")
if(NOT IS_ABSOLUTE "${JCE_XP_MIMALLOC_INCLUDE}" OR
   NOT EXISTS "${JCE_XP_MIMALLOC_INCLUDE}/mimalloc/atomic.h")
  message(FATAL_ERROR "Supply the explicit unchanged mimalloc include directory")
endif()
if(NOT IS_ABSOLUTE "${JCE_XP_MIMALLOC_OUTPUT}")
  message(FATAL_ERROR "Supply the explicit fixture output directory")
endif()
cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${JCE_XP_MIMALLOC_OUTPUT}" NORMALIZE _owned_output)
if(NOT _owned_output OR JCE_XP_MIMALLOC_OUTPUT STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Fixture output must be a strict child of its build directory")
endif()
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
function(jce_xp_mimalloc_target target)
  add_executable(${target} main.c)
  target_include_directories(${target} PRIVATE "${_root}/engine/src" "${JCE_XP_MIMALLOC_INCLUDE}")
  target_compile_definitions(${target} PRIVATE
    JCE_WINDOWS_XP_SYNC_PORT=1 JCE_WINDOWS_XP_MIMALLOC_PORT=1)
  target_compile_options(${target} PRIVATE -O2 -Wall -Wextra -Werror)
  target_link_options(${target} PRIVATE -static -pthread
    -Wl,--major-os-version=5,--minor-os-version=${JCE_XP_PE_MINOR},--major-subsystem-version=5,--minor-subsystem-version=${JCE_XP_PE_MINOR})
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${JCE_XP_MIMALLOC_OUTPUT}")
endfunction()
jce_xp_mimalloc_target(xp_mimalloc_port_control)
include(CTest)
foreach(_case IN ITEMS locks concurrency exports-present exports-missing kernel-missing ordinals forwarding)
  add_test(NAME "xp_mimalloc_${_case}" COMMAND xp_mimalloc_port_control "${_case}")
endforeach()
if(JCE_XP_MIMALLOC_MUTANT_DIR)
  if(NOT IS_ABSOLUTE "${JCE_XP_MIMALLOC_MUTANT_DIR}" OR
     NOT EXISTS "${JCE_XP_MIMALLOC_MUTANT_DIR}/omitted_try/jce_windows_xp_mimalloc_internal.h" OR
     NOT EXISTS "${JCE_XP_MIMALLOC_MUTANT_DIR}/omitted_try/jce_windows_xp_sync_internal.h")
    message(FATAL_ERROR "Supply fresh generated complete first-party omitted-try headers")
  endif()
  jce_xp_mimalloc_target(xp_mimalloc_port_omitted_try_control)
  target_compile_definitions(xp_mimalloc_port_omitted_try_control PRIVATE
    JCE_XP_MIMALLOC_OMITTED_TRY_CONTROL=1
    "JCE_XP_MIMALLOC_PORT_HEADER=\"${JCE_XP_MIMALLOC_MUTANT_DIR}/omitted_try/jce_windows_xp_mimalloc_internal.h\"")
  add_test(NAME xp_mimalloc_omitted_try
    COMMAND xp_mimalloc_port_omitted_try_control locks)
  set_tests_properties(xp_mimalloc_omitted_try PROPERTIES WILL_FAIL TRUE)
endif()
# fail-current, fail-lock and fail-release are separate direct-process controls:
# require exact exit 91; WILL_FAIL alone cannot certify that contract.
