"""
Conan 2 hook: Disable /WX (treat-warnings-as-errors) in ozz-animation when
cross-compiling for Windows ARM64 with MSVC.

ozz-animation's build-utils/cmake/compiler_settings.cmake unconditionally
adds:
    add_compile_options(/WX)
for MSVC builds. On the ARM64 cross-compiler blending_job.cc:203 emits a
C4xxx warning that is promoted to a fatal error via C2220.

Fix: comment out the /WX line in compiler_settings.cmake before the build.
"""

import os
import re


def pre_build(conanfile):
    if conanfile.name != "ozz-animation":
        return
    settings = conanfile.settings
    if (str(settings.get_safe("os")) != "Windows" or
            str(settings.get_safe("compiler")) != "msvc"):
        return

    _patch_compiler_settings(conanfile)


def _patch_compiler_settings(conanfile):
    src = getattr(conanfile, "source_folder", None)
    if not src:
        conanfile.output.warning(
            "hook_ozz_animation_arm64_fix: no source_folder available")
        return

    cmake_file = os.path.join(
        src, "build-utils", "cmake", "compiler_settings.cmake")
    if not os.path.isfile(cmake_file):
        conanfile.output.warning(
            "hook_ozz_animation_arm64_fix: compiler_settings.cmake not found "
            f"at {cmake_file}")
        return

    with open(cmake_file, "r", encoding="utf-8") as fh:
        content = fh.read()

    # Comment out the /WX line that promotes warnings to errors on MSVC.
    patched = re.sub(
        r'\badd_compile_options\(/WX\)',
        '# add_compile_options(/WX)  # disabled by hook_ozz_animation_arm64_fix',
        content
    )

    if patched == content:
        conanfile.output.info(
            "hook_ozz_animation_arm64_fix: /WX already absent, no patch needed")
        return

    with open(cmake_file, "w", encoding="utf-8") as fh:
        fh.write(patched)

    conanfile.output.info(
        "hook_ozz_animation_arm64_fix: patched compiler_settings.cmake "
        "(commented out add_compile_options(/WX))")
