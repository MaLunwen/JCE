"""Bind Conan's native MSVC toolchain to the active compiler environment.

This observes executable paths and target/toolset metadata. It does not certify
compiler execution, Windows SDK completeness or dependency ABI compatibility.
"""
from __future__ import annotations

import json
from pathlib import Path
import re
import shutil


def _environment(env, name):
    return next((value for key, value in env.items() if key.upper() == name), "")


def configuration(target, env, *, host, arch_dirs, toolset_prefix, dry_run=False):
    """Return host-context Conan conf using the shared MSVC selection only."""
    if host != "windows" or target["host"] != "windows":
        return []
    path = _environment(env, "PATH")
    compiler = shutil.which("cl", path=path)
    linker = shutil.which("link", path=path)
    if not compiler or not linker or not _environment(env, "INCLUDE") or not _environment(env, "LIB"):
        if dry_run:
            return []
        raise ValueError("Conan MSVC generation needs cl, link, INCLUDE and LIB from the shared environment")
    compiler, linker = Path(compiler).resolve(), Path(linker).resolve()
    normalized = compiler.as_posix()
    match = re.fullmatch(
        r"(.+)/VC/Tools/MSVC/([0-9.]+)/bin/Host(?:x64|x86|arm64)/(x86|x64|arm64)/cl(?:\.exe)?",
        normalized, re.IGNORECASE)
    expected = arch_dirs[target["arch"]]
    if match is None or match.group(3).lower() != expected:
        raise ValueError("Conan MSVC compiler path does not match the requested target " + expected)
    if not match.group(2).startswith(toolset_prefix):
        raise ValueError("Conan MSVC compiler does not match the shared profile family " +
                         toolset_prefix)
    if linker.parent != compiler.parent:
        raise ValueError("Conan MSVC compiler and linker belong to different target toolsets")
    shell_arch = _environment(env, "VSCMD_ARG_TGT_ARCH")
    if shell_arch and shell_arch.lower() != expected:
        raise ValueError("Conan MSVC developer-shell target differs from " + expected)
    installation = Path(match.group(1))
    observed = _environment(env, "VSINSTALLDIR")
    if observed and Path(observed).resolve() != installation:
        raise ValueError("Conan MSVC compiler differs from the active VS installation")
    observed_tools = _environment(env, "VCTOOLSINSTALLDIR")
    if observed_tools and Path(observed_tools).resolve() != compiler.parents[3]:
        raise ValueError("Conan MSVC compiler differs from the active VC toolset")
    if any(ord(character) < 32 or character in '$;"' for character in normalized):
        raise ValueError("Conan MSVC compiler path cannot be represented in a CMake toolchain")
    # Conan's default Ninja/MSVC block writes bare "cl". An IDE may otherwise
    # resolve it again after preparation, changing the cached compiler and
    # causing CMake to discard the cache, including its Conan toolchain path.
    # Host context means the target graph: cross-target cl must not replace the
    # build-machine compiler used by Conan's separate build-context tools.
    return ["-c:h", "tools.microsoft.msbuild:installation_path=" + installation.as_posix(),
            "-c:h", "tools.build:compiler_executables=" + json.dumps(
                {"c": normalized, "cpp": normalized}, sort_keys=True, separators=(",", ":"))]
