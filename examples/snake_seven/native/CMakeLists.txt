# The two NATIVE script modules of the seven-language snake.
#
# Two separate MODULE libraries on purpose.  "One language, one core module"
# is the demo's whole premise, and letting C and C++ share a .dll would make
# "which language failed" a question about a symbol rather than about a file.
# The class registry is process-wide and shared between the two languages, so
# two modules is also the shape that proves the registry accepts both.
#
# Nothing here is part of the engine build: this consumes the INSTALLED SDK
# exactly as any user project would.
#
# WHY THESE EXIST AT ALL, when the shipped exe links the same two sources
# directly (with JCE_SCRIPT_MODULE_NO_ENTRY) and needs no .dll:
#
# THE EDITOR IS A DIFFERENT PROCESS.  It does not link this game, so the only
# way a C or C++ script CLASS reaches Play is a shared object the editor
# dlopens -- the one named in jce_project.json's "script_modules".  Without
# these two libraries the editor loads three of the seven languages and
# refuses the other four by name, while the shipped game runs all seven from
# the same scene file.  Measured: "NO NATIVE SCRIPT MODULE IS LOADED IN THIS
# PROCESS" for both .jcec and .jcecpp.
#
# So the same two sources are built twice on purpose, and the two builds are
# not redundant: one is how the game ships, the other is how the editor
# previews it.
cmake_minimum_required(VERSION 3.20)
project(SnakeNativeModules C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED JCE_DIR AND DEFINED ENV{JCE_SDK_DIR})
    if(EXISTS "$ENV{JCE_SDK_DIR}/lib/cmake/JCE/JCEConfig.cmake")
        set(JCE_DIR "$ENV{JCE_SDK_DIR}/lib/cmake/JCE")
    endif()
endif()
find_package(JCE REQUIRED)

if(NOT TARGET JCE::ScriptCppModule)
    message(FATAL_ERROR
        "This SDK has no JCE::ScriptCppModule: C++ scripts are not available. "
        "Testing for the TARGET rather than for an option is deliberate -- an "
        "option that is ON with a toolchain that was not found produces the "
        "same absent target, and only one of the two prints anything.")
endif()
if(NOT TARGET JCE::ScriptCModule)
    message(FATAL_ERROR "This SDK has no JCE::ScriptCModule: C scripts are not available.")
endif()

add_library(snake_cpp MODULE snake_body.cpp)
target_link_libraries(snake_cpp PRIVATE JCE::ScriptCppModule)

add_library(snake_c MODULE snake_manager.c)
target_link_libraries(snake_c PRIVATE JCE::ScriptCModule)
# Asserted, not assumed: the claim scripting/c makes is that a PURE C
# translation unit can be an entity script.  If this ever links as C++ the
# demo would still run and would stop proving that.
set_target_properties(snake_c PROPERTIES LINKER_LANGUAGE C)

# NO "lib" PREFIX, on either target.
#
# jce_project.json names these files literally ("native/build/snake_c.dll"),
# and CMake's default prefix is toolchain-dependent: MSVC produces
# snake_c.dll and MinGW produces libsnake_c.dll from the identical
# CMakeLists.  A manifest that is correct under one compiler and names a
# missing file under the other is worse than either, because the editor's
# report is "that file is not there" and the file plainly IS -- under a
# different name.  Pinning the prefix makes the manifest true everywhere.
set_target_properties(snake_c snake_cpp PROPERTIES PREFIX "")
