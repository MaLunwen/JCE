# CMake 3.24+ sets CMP0137=NEW: propagate custom platform variables
# into whole-project try_compile() checks (compiler ABI detection).
cmake_minimum_required(VERSION 3.24)
# Isolated C99 + C++20 probe compiling real first-party jce_entropy.c.
# Intentionally DOES NOT configure root JCE, Conan, bgfx, Editor, or SDK.
project(jce_xp_a1_probe LANGUAGES C CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "A1 XP probe requires LLVM-MinGW-XP Clang targeting Windows")
endif()
if(NOT DEFINED JCE_XP_WINNT OR NOT DEFINED JCE_XP_PE_MINOR)
  message(FATAL_ERROR "A1 XP probe requires a JCE XP toolchain file")
endif()
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(_jce_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(jce_xp_a1_probe
  "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
  "${_jce_root}/engine/src/os/platform/jce_entropy.c")
target_include_directories(jce_xp_a1_probe PRIVATE "${_jce_root}/engine/include")
target_compile_definitions(jce_xp_a1_probe PRIVATE
  JCE_WINXP_LEGACY=1
  _WIN32_WINNT=${JCE_XP_WINNT}
  WINVER=${JCE_XP_WINNT}
  WIN32_LEAN_AND_MEAN=1)
target_compile_options(jce_xp_a1_probe PRIVATE -O2 -pthread)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  target_compile_options(jce_xp_a1_probe PRIVATE -msse2)
endif()
target_link_options(jce_xp_a1_probe PRIVATE
  -static -pthread
  -Wl,--major-os-version=5,--minor-os-version=${JCE_XP_PE_MINOR},--major-subsystem-version=5,--minor-subsystem-version=${JCE_XP_PE_MINOR})
target_link_libraries(jce_xp_a1_probe PRIVATE advapi32)
set_target_properties(jce_xp_a1_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
