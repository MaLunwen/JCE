"""Resolve Modkit settings from one explicitly selected configured JCE tree.

This module only reads metadata and checks files. It never configures/builds,
writes settings, searches the Conan package cache, or executes shaderc. The
caller must execution-probe the returned compiler before applying settings.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import stat


_READ_LIMIT = 1024 * 1024
_ENTRY_LIMIT = 4096
_CACHE_KEYS = {
    "CMAKE_HOME_DIRECTORY", "CMAKE_BUILD_TYPE", "CMAKE_TOOLCHAIN_FILE",
    "bgfx_DIR", "BGFX_SHADER_INCLUDE_PATH", "JCE_SHADERC_EXECUTABLE",
}


def _read(path: Path) -> str:
    try:
        if not stat.S_ISREG(path.stat().st_mode):
            raise ValueError(f"Modkit metadata is not a regular file: {path}")
        with path.open("rb") as stream:
            data = stream.read(_READ_LIMIT + 1)
        if len(data) > _READ_LIMIT:
            raise ValueError(f"Modkit metadata exceeds 1 MiB: {path}")
        return data.decode("utf-8-sig")
    except (OSError, UnicodeError) as exc:
        raise ValueError(f"Cannot read Modkit metadata {path}: {exc}") from exc


def _cache(path: Path) -> dict[str, str]:
    result = {}
    for line in _read(path).splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.split(":", 1)[0]
        if key in _CACHE_KEYS:
            if key in result:
                raise ValueError(f"Duplicate {key} in {path}")
            result[key] = value
    return result


def _path(value: str, label: str, *, base: Path | None = None) -> Path:
    if not value or any(c in value for c in ("\x00", "\n", "\r", ";", "$")):
        raise ValueError(f"Missing or unresolved {label}: {value!r}")
    path = Path(value)
    if not path.is_absolute():
        if base is None:
            raise ValueError(f"{label} must be absolute: {value!r}")
        path = base / path
    try:
        return path.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise ValueError(f"Missing or invalid {label}: {path}") from exc


def _set_literal(text: str, variable: str, source: Path) -> list[str]:
    """Read bounded literal set(VAR value) forms, without executing CMake.

    Only one scalar is accepted. Quoted parentheses/spaces are supported;
    arbitrary commands, lists, cache assignments and generator expressions
    are not silently evaluated as paths.
    """
    values = []
    pattern = re.compile(r"(?m)^[ \t]*(?i:set)[ \t]*\([ \t\r\n]*" +
                         re.escape(variable) + r"(?=[\s)])")
    for match in pattern.finditer(text):
        i = match.end()
        while i < len(text) and text[i].isspace():
            i += 1
        value = []
        quoted = i < len(text) and text[i] == '"'
        if quoted:
            i += 1
            while i < len(text) and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                    if i >= len(text) or text[i] not in ('"', "\\", ";"):
                        raise ValueError(f"Unsupported CMake escape for {variable}: {source}")
                value.append(text[i])
                i += 1
            if i >= len(text):
                raise ValueError(f"Unclosed {variable}: {source}")
            i += 1
        else:
            while i < len(text) and not text[i].isspace() and text[i] != ")":
                value.append(text[i])
                i += 1
        while i < len(text) and text[i].isspace():
            i += 1
        if i >= len(text) or text[i] != ")":
            raise ValueError(f"Unsupported non-scalar {variable}: {source}")
        values.append("".join(value))
    return values


def _expand(value: str, variables: dict[str, str], source: Path) -> str:
    for _ in range(8):
        if "${" not in value:
            break
        changed = re.sub(r"\$\{([A-Za-z0-9_]+)\}",
                         lambda m: variables.get(m.group(1), m.group(0)), value)
        if changed == value:
            break
        value = changed
    if "$" in value:
        raise ValueError(f"Unresolved CMake path in {source}: {value!r}")
    return value


def _single(paths: list[Path], label: str) -> Path | None:
    distinct = list(dict.fromkeys(paths))
    if len(distinct) > 1:
        raise ValueError(f"Ambiguous {label}: " + ", ".join(str(p) for p in distinct))
    return distinct[0] if distinct else None


def resolve_settings(root: Path, build: Path) -> dict:
    """Return four Modkit settings; refuse absent, foreign or ambiguous input.

    ``build`` is the caller-selected existing shader build, not an automatic
    Editor/preset choice. Paths come only from its CMake cache and the adjacent
    Conan CMakeDeps generator directory. File presence does not certify that
    shaderc can run; the driver retains its existing --version probe.
    """
    root = _path(str(root), "JCE source root")
    build = _path(str(build), "selected shader build")
    if not root.is_dir() or not build.is_dir():
        raise ValueError("JCE source root and selected shader build must be directories")
    cache = _cache(build / "CMakeCache.txt")
    home = _path(cache.get("CMAKE_HOME_DIRECTORY", ""), "CMAKE_HOME_DIRECTORY")
    if home != root:
        raise ValueError(f"Shader build belongs to another source root: {home}")
    config = cache.get("CMAKE_BUILD_TYPE", "")
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", config):
        raise ValueError("Shader build needs one explicit CMAKE_BUILD_TYPE")
    toolchain = _path(cache.get("CMAKE_TOOLCHAIN_FILE", ""),
                      "CMAKE_TOOLCHAIN_FILE", base=build)
    if not toolchain.is_file() or toolchain.name != "conan_toolchain.cmake":
        raise ValueError("Shader build must identify its Conan generator toolchain")
    generators = toolchain.parent
    if cache.get("bgfx_DIR"):
        bgfx_dir = _path(cache["bgfx_DIR"], "bgfx_DIR")
        if bgfx_dir != generators:
            raise ValueError("bgfx_DIR disagrees with the selected Conan generator directory")
    config_file = generators / "bgfx-config.cmake"
    config_text = _read(config_file)
    variable = "bgfx_PACKAGE_FOLDER_" + config.upper()
    package_candidates = []
    try:
        with os.scandir(generators) as entries:
            for count, entry in enumerate(entries, 1):
                if count > _ENTRY_LIMIT:
                    raise ValueError("Conan generator directory exceeds 4096 entries")
                if (entry.name.startswith("bgfx-" + config.lower() + "-") and
                        entry.name.endswith("-data.cmake")):
                    source = Path(entry.path)
                    for value in _set_literal(_read(source), variable, source):
                        expanded = _expand(value, {"CMAKE_CURRENT_LIST_DIR": str(generators)}, source)
                        package_candidates.append(_path(expanded, variable))
    except OSError as exc:
        raise ValueError(f"Cannot inspect selected Conan generators: {generators}") from exc
    package = _single(package_candidates, variable)
    variables = {"CMAKE_CURRENT_LIST_DIR": str(generators)}
    if package:
        if not package.is_dir():
            raise ValueError(f"bgfx package is not a directory: {package}")
        variables[variable] = str(package)
    includes = []
    if cache.get("BGFX_SHADER_INCLUDE_PATH"):
        includes.append(_path(cache["BGFX_SHADER_INCLUDE_PATH"], "BGFX_SHADER_INCLUDE_PATH"))
    for value in _set_literal(config_text, "BGFX_SHADER_INCLUDE_PATH", config_file):
        includes.append(_path(_expand(value, variables, config_file), "BGFX_SHADER_INCLUDE_PATH"))
    include = _single(includes, "BGFX_SHADER_INCLUDE_PATH")
    if include is None and package:
        include = _path(str(package / "include" / "bgfx"), "bgfx package shader includes")
    if include is None or not include.is_dir() or not (include / "bgfx_shader.sh").is_file():
        raise ValueError("Selected build has no verified bgfx_shader.sh include directory")
    configured_compiler = cache.get("JCE_SHADERC_EXECUTABLE", "")
    if configured_compiler:
        compiler = _path(configured_compiler, "JCE_SHADERC_EXECUTABLE")
    elif package:
        executable = "shaderc.exe" if os.name == "nt" else "shaderc"
        compiler = _path(str(package / "bin" / executable), "selected bgfx package shaderc")
    else:
        raise ValueError("Selected build has no recorded shaderc or unique bgfx package")
    if not compiler.is_file():
        raise ValueError(f"Selected shaderc is not a file: {compiler}")
    include_dirs = [include]
    for directory in (root / "engine" / "shaders", root / "engine" / "shaders" / "include"):
        if directory.is_dir():
            include_dirs.append(directory.resolve())
    return {
        "modkit.shaderc.path": compiler.as_posix(),
        "modkit.shaderc.includeDirs": [p.as_posix() for p in dict.fromkeys(include_dirs)],
        "modkit.shaderc.useBuildDir": True,
        "modkit.shaderc.buildDir": build.as_posix(),
    }
