"""
Conan 2 hook: fix libyuv ARM64 cross-compilation on older binutils.

Ubuntu 22.04 ships binutils-aarch64-linux-gnu 2.38 whose assembler does not
support the ARMv8.2-A dot-product (`udot`) or i8mm instructions used in
libyuv's neon64 source files.

Fix: Replace the entire aarch64 if-block in CMakeLists.txt (which builds
_neon64, _sve, _sme targets) with LIBYUV_DISABLE_NEON / LIBYUV_DISABLE_SVE
/ LIBYUV_DISABLE_SME definitions.  This prevents both the problematic
assembly and the undefined-reference linker errors.
"""

import os
import re


def pre_build(conanfile):
    if conanfile.name != "libyuv":
        return

    if str(conanfile.settings.get_safe("arch")) not in ("armv8", "armv8.3"):
        return

    cmake_file = os.path.join(conanfile.source_folder, "CMakeLists.txt")
    if not os.path.exists(cmake_file):
        conanfile.output.warning("[libyuv_arm64_fix hook] CMakeLists.txt not found")
        return

    with open(cmake_file, "r") as f:
        content = f.read()

    if "LIBYUV_DISABLE_NEON" in content:
        conanfile.output.info("[libyuv_arm64_fix hook] already patched")
        return

    # Replace the entire  if(arch_lowercase STREQUAL "aarch64" ...) ... endif()
    # block with simple ADD_DEFINITIONS to disable NEON/SVE/SME.
    # The block ends with "  endif()\n" right before the outer "endif()\n" that
    # closes if(NOT MSVC).  We use a lookahead to stop before the outer endif().
    pattern = (
        r'  if\(arch_lowercase STREQUAL "aarch64" OR arch_lowercase STREQUAL "arm64"\)'
        r'.*?'
        r'  endif\(\)\n'
        r'(?=endif\(\))'
    )

    replacement = (
        '  # [libyuv_arm64_fix] Replaced aarch64 neon64/sve/sme block.\n'
        '  # binutils 2.38 cannot assemble dotprod/i8mm/sve inline asm.\n'
        '  if(arch_lowercase STREQUAL "aarch64" OR arch_lowercase STREQUAL "arm64")\n'
        '    ADD_DEFINITIONS(-DLIBYUV_DISABLE_NEON)\n'
        '    ADD_DEFINITIONS(-DLIBYUV_DISABLE_SVE)\n'
        '    ADD_DEFINITIONS(-DLIBYUV_DISABLE_SME)\n'
        '  endif()\n'
    )

    new_content, count = re.subn(pattern, replacement, content, count=1, flags=re.DOTALL)

    if count == 0:
        conanfile.output.warning(
            "[libyuv_arm64_fix hook] could not find aarch64 if-block to replace"
        )
        return

    with open(cmake_file, "w") as f:
        f.write(new_content)

    conanfile.output.info(
        "[libyuv_arm64_fix hook] replaced aarch64 neon64/sve/sme block with "
        "LIBYUV_DISABLE_NEON/SVE/SME (binutils 2.38 compat)"
    )
