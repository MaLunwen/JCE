# LLVM-MinGW-XP GNU/MSVCRT target. It never admits the modern MSVC SDK ABI.
# JCE_XP_TOOLCHAIN_ROOT is supplied by the caller (not hardcoded into source).
# CMake recreates this toolchain during compiler ABI try_compile() checks.
# Pass the custom root into those temporary test projects explicitly.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES JCE_XP_TOOLCHAIN_ROOT)
if(NOT DEFINED JCE_XP_TOOLCHAIN_ROOT AND DEFINED ENV{JCE_XP_TOOLCHAIN_ROOT})
  set(JCE_XP_TOOLCHAIN_ROOT "$ENV{JCE_XP_TOOLCHAIN_ROOT}")
endif()
if(NOT DEFINED JCE_XP_TOOLCHAIN_ROOT OR "${JCE_XP_TOOLCHAIN_ROOT}" STREQUAL "")
  message(FATAL_ERROR "JCE_XP_TOOLCHAIN_ROOT is required")
endif()
if(NOT DEFINED JCE_XP_TRIPLE OR NOT DEFINED JCE_XP_PE_MINOR)
  message(FATAL_ERROR "Use windows-x64-xp-llvm.cmake or windows-x86-xp-llvm.cmake")
endif()
file(TO_CMAKE_PATH "${JCE_XP_TOOLCHAIN_ROOT}" _jce_xp_root)
set(_jce_xp_bin "${_jce_xp_root}/bin")
set(_jce_xp_cc "${_jce_xp_bin}/${JCE_XP_TRIPLE}-clang.exe")
set(_jce_xp_cxx "${_jce_xp_bin}/${JCE_XP_TRIPLE}-clang++.exe")
if(NOT EXISTS "${_jce_xp_cc}" OR NOT EXISTS "${_jce_xp_cxx}")
  message(FATAL_ERROR "LLVM-MinGW-XP compilers missing under ${_jce_xp_bin}")
endif()
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION "5.${JCE_XP_PE_MINOR}")
set(CMAKE_C_COMPILER "${_jce_xp_cc}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_jce_xp_cxx}" CACHE FILEPATH "" FORCE)
# Separate CMake from the host's MSVC libraries and Conan-generated SDK.
set(CMAKE_FIND_ROOT_PATH "${_jce_xp_root}/${JCE_XP_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Conan's admitted target configuration packages live outside the sysroot.
# Libraries/includes still stay rooted; the producer validates the exact graph.
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
