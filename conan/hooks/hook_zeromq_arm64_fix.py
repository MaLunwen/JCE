"""
Conan 2 hook: Fix zeromq 4.3.5 cross-compilation for Windows ARM64.

On ARM64 cross-compile, CMAKE_SYSTEM_VERSION is empty, so
`get_win32_winnt(ZMQ_WIN32_WINNT_DEFAULT)` produces an empty string.
The condition:
    if(ZMQ_WIN32_WINNT GREATER "0x05FF" AND MSVC_VERSION GREATER 1799 ...)
is then always FALSE (empty → 0, not > 1535), so POLLER is never set to
"epoll", IPC transport is compiled with the incompatible select() poller,
and the build fails with C1189 (#error).

Fix: patch the POLLER autodetect condition to also fire when ZMQ_WIN32_WINNT
is empty (which is the ARM64 cross-compile case).
"""

import os
import re

# Old condition exactly as it appears in zeromq 4.3.5 CMakeLists.txt
_OLD_COND = (
    'if(ZMQ_WIN32_WINNT GREATER "0x05FF"\n'
    '     AND MSVC_VERSION GREATER 1799\n'
    '     AND POLLER STREQUAL ""\n'
    '     AND NOT ZMQ_HAVE_WINDOWS_UWP)'
)

# New condition: also triggers when ZMQ_WIN32_WINNT is empty (cross-compile)
_NEW_COND = (
    '# (hook_zeromq_arm64_fix: OR empty = ARM64 cross-compile)\n'
    '  if((ZMQ_WIN32_WINNT GREATER "0x05FF" OR ZMQ_WIN32_WINNT STREQUAL "")\n'
    '     AND MSVC_VERSION GREATER 1799\n'
    '     AND POLLER STREQUAL ""\n'
    '     AND NOT ZMQ_HAVE_WINDOWS_UWP)'
)


def pre_build(conanfile):
    if conanfile.name != "zeromq":
        return
    settings = conanfile.settings
    if (str(settings.get_safe("os")) != "Windows" or
            str(settings.get_safe("compiler")) != "msvc"):
        return
    arch = str(settings.get_safe("arch"))
    if arch not in ("armv8", "armv8.3", "armv8_32", "armv8.4", "armv8.5"):
        return

    _patch_cmake_lists(conanfile)


def _patch_cmake_lists(conanfile):
    src = getattr(conanfile, "source_folder", None)
    if not src:
        conanfile.output.warning("hook_zeromq_arm64_fix: no source_folder")
        return

    cmake_file = os.path.join(src, "CMakeLists.txt")
    if not os.path.isfile(cmake_file):
        conanfile.output.warning(
            f"hook_zeromq_arm64_fix: CMakeLists.txt not found at {cmake_file}")
        return

    with open(cmake_file, "r", encoding="utf-8") as fh:
        content = fh.read()

    if "hook_zeromq_arm64_fix" in content:
        conanfile.output.info("hook_zeromq_arm64_fix: already patched, skipping")
        return

    if _OLD_COND not in content:
        conanfile.output.warning(
            "hook_zeromq_arm64_fix: expected POLLER condition not found; "
            "zeromq version may have changed — skipping patch")
        return

    patched = content.replace(_OLD_COND, _NEW_COND, 1)

    with open(cmake_file, "w", encoding="utf-8") as fh:
        fh.write(patched)

    conanfile.output.info(
        "hook_zeromq_arm64_fix: patched POLLER autodetect condition "
        "to handle ARM64 cross-compile (empty ZMQ_WIN32_WINNT)")
