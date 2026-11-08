cmake_minimum_required(VERSION 3.24)
project(jce_windows_owned_launcher LANGUAGES C)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
   NOT DEFINED JCE_XP_WINNT OR NOT DEFINED JCE_XP_PE_MINOR)
  message(FATAL_ERROR "Use the explicit JCE LLVM-MinGW-XP MSVCRT toolchain")
endif()
add_executable(jce_windows_owned_launcher windows_owned_launcher.c)
set_target_properties(jce_windows_owned_launcher PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
target_compile_definitions(jce_windows_owned_launcher PRIVATE
  _WIN32_WINNT=${JCE_XP_WINNT} WINVER=${JCE_XP_WINNT} WIN32_LEAN_AND_MEAN=1)
target_compile_options(jce_windows_owned_launcher PRIVATE -O2 -Wall -Wextra -Werror)
target_link_options(jce_windows_owned_launcher PRIVATE -static -municode
  -Wl,--major-os-version=5,--minor-os-version=${JCE_XP_PE_MINOR},--major-subsystem-version=5,--minor-subsystem-version=${JCE_XP_PE_MINOR})
target_link_libraries(jce_windows_owned_launcher PRIVATE advapi32)
