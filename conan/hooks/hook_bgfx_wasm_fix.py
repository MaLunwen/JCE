"""
Conan 2 hook: fix bgfx/1.129.8930-495 Emscripten build.

Two issues patched in conan_cmake_project_include.cmake:

1. UNIX AND NOT APPLE guard is TRUE for Emscripten (CMake sets UNIX=TRUE for
   wasm32 targets), causing find_package(wayland REQUIRED CONFIG) to fail.
   Fix: replace with an explicit CMAKE_SYSTEM_NAME check.

2. bgfx's multi-threaded render loop uses bx::Thread which is unavailable on
   Emscripten without full pthreads support.  Set BGFX_CONFIG_MULTITHREADED=0
   via CMake cache so bgfx compiles in single-threaded mode.
"""

import os


def post_source(conanfile):
    if conanfile.name != "bgfx":
        return
    if str(conanfile.settings.get_safe("os")) != "Emscripten":
        return

    cmake_file = os.path.join(conanfile.source_folder, "conan_cmake_project_include.cmake")
    if not os.path.exists(cmake_file):
        conanfile.output.warning("[bgfx_wasm_fix hook] conan_cmake_project_include.cmake not found")
        return

    with open(cmake_file, "r") as f:
        content = f.read()

    # Fix 1: wayland guard — UNIX is TRUE for Emscripten but wayland is Linux-only
    patched = content.replace(
        "if(UNIX AND NOT APPLE)",
        'if(CMAKE_SYSTEM_NAME STREQUAL "Linux")',
    )

    # Fix 2: disable multi-threaded render loop (bx::Thread unavailable w/o pthreads)
    if "BGFX_CONFIG_MULTITHREADED" not in patched:
        patched += '\nset(BGFX_CONFIG_MULTITHREADED 0 CACHE STRING "Disable bgfx render thread on Emscripten" FORCE)\n'

    if content == patched:
        conanfile.output.info("[bgfx_wasm_fix hook] nothing to patch (already applied)")
        return

    with open(cmake_file, "w") as f:
        f.write(patched)

    conanfile.output.info(
        "[bgfx_wasm_fix hook] patched conan_cmake_project_include.cmake "
        "(wayland guard + BGFX_CONFIG_MULTITHREADED=0)"
    )
