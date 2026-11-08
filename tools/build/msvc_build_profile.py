"""Derive a native MSVC build profile from ordinary installed toolset files.

Only selected file bytes and filesystem identity are observed. Compiler
execution, PE RT_VERSION and complete package ABI compatibility are not verified.
"""
from __future__ import annotations
import hashlib
import json
import os
from pathlib import Path
import re
import stat

MAX_ENTRIES = 64
MAX_PROFILE_BYTES = 65536
MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 128 * 1024 * 1024
PROFILE_NAME = "build.profile"


def _ordinary(value):
    if not isinstance(value, (str, os.PathLike)):
        raise ValueError("MSVC profile path is required")
    if any(ord(character) < 32 or ord(character) == 127 for character in str(value)):
        raise ValueError("MSVC profile control characters in paths are not admitted")
    if ".." in re.split(r"[\\/]", str(value)):
        raise ValueError("MSVC profile path traversal is not admitted")
    path = Path(value).absolute()
    for item in (path, *path.parents):
        if item.exists() or item.is_symlink():
            found = item.lstat()
            if stat.S_ISLNK(found.st_mode) or getattr(found, "st_file_attributes", 0) & 0x400:
                raise ValueError("MSVC profile aliased path is not admitted")
    return path.resolve()


def _stamp(found):
    # Windows path/fd stat can disagree on ctime semantics; compare stable
    # file identity/size/mtime and separately require full SHA256 equality.
    return [getattr(found, key) for key in
            ("st_dev", "st_ino", "st_size", "st_mtime_ns")]


def _observe(value, limit, contents=False):
    path = _ordinary(value)
    before = path.stat()
    if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= limit:
        raise ValueError("MSVC profile input type/byte budget refusal")
    digest, chunks, size = hashlib.sha256(), [], 0
    with path.open("rb") as stream:
        if _stamp(os.fstat(stream.fileno())) != _stamp(before):
            raise ValueError("MSVC profile input replaced before read")
        while chunk := stream.read(65536):
            size += len(chunk)
            if size > limit:
                raise ValueError("MSVC profile input exceeded its byte budget")
            digest.update(chunk)
            if contents:
                chunks.append(chunk)
        held = os.fstat(stream.fileno())
    after = _ordinary(path).stat()
    if size != before.st_size or _stamp(before) != _stamp(held) or _stamp(held) != _stamp(after):
        raise ValueError("MSVC profile input changed during read")
    row = {"path": path.as_posix(), "size_bytes": size, "sha256": digest.hexdigest(),
           "filesystem_identity": _stamp(after)}
    return row, b"".join(chunks) if contents else None


def _base(text, arch):
    settings, section = {}, False
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if not section and re.fullmatch(r"include\([A-Za-z0-9_./:\\ ()-]+\)", line):
            continue
        if line == "[settings]" and not section:
            section = True
            continue
        match = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_.]*)=([A-Za-z0-9_.+-]+)", line)
        if not section or not match:
            raise ValueError("automatic MSVC profile requires a plain first-party settings profile")
        key, value = match.groups()
        if key in settings:
            raise ValueError("duplicate MSVC base-profile setting: " + key)
        settings[key] = value
    if any(settings.get(key) != value for key, value in
           (("compiler", "msvc"), ("arch", arch), ("os", "Windows"))):
        raise ValueError("automatic MSVC profile requires native Windows x86_64 MSVC")
    family = settings.get("compiler.version", "")
    if not re.fullmatch(r"19[0-9]", family) or "compiler.update" in settings:
        raise ValueError("automatic MSVC profile requires a coarse MSVC 19x compiler family")
    runtime = settings.get("compiler.runtime")
    if runtime not in ("dynamic", "static"):
        raise ValueError("automatic MSVC profile requires an explicit dynamic/static runtime")
    return family, runtime


def _select(vs, family, runtime):
    tools = _ordinary(vs / "VC/Tools/MSVC")
    major, tens = int(family[:-1]) - 5, int(family[-1])
    required = {"cl": "bin/Hostx64/x64/cl.exe", "link": "bin/Hostx64/x64/link.exe",
                "stl_header": "include/yvals_core.h",
                "stl_library": "lib/x64/" + ("msvcprt.lib" if runtime == "dynamic" else "libcpmt.lib")}
    candidates = []
    for index, entry in enumerate(tools.iterdir()):
        if index >= MAX_ENTRIES:
            raise ValueError("MSVC installed-toolset entry budget exceeded")
        entry = _ordinary(entry)
        match = re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", entry.name)
        if not match:
            continue
        version = tuple(map(int, match.groups()))
        if version[0] != major or version[1] // 10 != tens:
            continue
        if not entry.is_dir():
            raise ValueError("MSVC numeric toolset entry is not a directory")
        try:
            paths = {role: _ordinary(entry / relative) for role, relative in required.items()}
            for path in paths.values():
                if not stat.S_ISREG(path.stat().st_mode):
                    raise ValueError("MSVC critical toolset input is not an ordinary file")
        except FileNotFoundError:
            continue
        candidates.append((version, entry, paths))
    if not candidates:
        raise ValueError("no complete native x64 MSVC toolset matches the base compiler family")
    return max(candidates, key=lambda item: item[0])


def _json(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True) + "\n").encode("ascii")


def _verify(cohort, payloads):
    cohort = _ordinary(cohort)
    names = []
    for index, path in enumerate(cohort.iterdir()):
        if index >= len(payloads):
            raise ValueError("MSVC profile cohort has unowned entries")
        names.append(path.name)
    if set(names) != set(payloads):
        raise ValueError("MSVC profile cohort has unknown/missing entries; refusing repair")
    for name, expected in payloads.items():
        try:
            _row, actual = _observe(cohort / name, len(expected), True)
        except (OSError, ValueError) as error:
            raise ValueError("MSVC profile cohort input refused; refusing repair") from error
        if actual != expected:
            raise ValueError("MSVC profile cohort bytes changed; refusing repair")


def prepare(base_profile, vs_installation, output_root, arch="x86_64", allow_create=True):
    """Return profile/selection/identity; audit mode never creates or repairs."""
    if arch != "x86_64" or type(allow_create) is not bool:
        raise ValueError("MSVC build-profile selection requires x86_64 and strict boolean allow_create")
    base, vs, root = map(_ordinary, (base_profile, vs_installation, output_root))
    if root == vs or vs in root.parents:
        raise ValueError("MSVC profile output must stay outside the VS installation")
    base_row, data = _observe(base, MAX_PROFILE_BYTES, True)
    family, runtime = _base(data.decode("utf-8-sig"), arch)
    version, toolset, paths = _select(vs, family, runtime)
    critical = {role: _observe(path, MAX_FILE_BYTES)[0] for role, path in paths.items()}
    if base_row["size_bytes"] + sum(row["size_bytes"] for row in critical.values()) > MAX_TOTAL_BYTES:
        raise ValueError("MSVC profile total input byte budget exceeded")
    if any(character in base.as_posix() for character in ("#", "$", "{", "}", "\r", "\n")):
        raise ValueError("MSVC base-profile path cannot be represented in include()")
    if any(character in vs.as_posix() for character in ("#", "$", "{", "}", "\r", "\n")):
        raise ValueError("VS installation path cannot be represented in a plain profile")
    selection = {"compiler": "msvc", "compiler.version": family, "compiler.update": str(version[1] % 10),
                 "selected_toolset_version": toolset.name, "toolset_path": toolset.as_posix(),
                 "vs_installation": vs.as_posix(), "arch": arch, "runtime": runtime,
                 "compiler_execution_verified": False, "rt_version_verified": False}
    profile = ("include(" + base.as_posix() + ")\n\n[settings]\ncompiler.version=" + family +
               "\ncompiler.update=" + selection["compiler.update"] +
               "\n\n[conf]\ntools.microsoft.msbuild:installation_path=" + vs.as_posix() + "\n").encode("utf-8")
    owner = {"schema": "jce.msvc-build-profile.v1", "base_profile": base_row,
             "selection": selection, "critical_files": critical,
             "profile_sha256": hashlib.sha256(profile).hexdigest(),
             "scope": "File hashes are selection provenance, not complete package ABI hashing."}
    identity = hashlib.sha256(_json(owner)).hexdigest()
    cohort = _ordinary(root / identity)
    payloads = {PROFILE_NAME: profile, "owner.json": _json(owner)}
    for row in (base_row, *critical.values()):
        if _observe(row["path"], MAX_PROFILE_BYTES if row is base_row else MAX_FILE_BYTES)[0] != row:
            raise ValueError("MSVC selected input changed before profile admission")
    selected_again = _select(vs, family, runtime)
    if selected_again[:2] != (version, toolset):
        raise ValueError("MSVC toolset selection changed before profile admission")
    if cohort.exists():
        _verify(cohort, payloads)
    elif not allow_create:
        raise ValueError("MSVC profile cohort is absent/changed in read-only audit mode")
    else:
        root.mkdir(parents=True, exist_ok=True)
        _ordinary(root)
        # Reserve the final directory exclusively. Never replace an existing
        # empty/unowned directory; an interrupted cohort is refused on reuse.
        try:
            cohort.mkdir()
        except FileExistsError:
            _verify(cohort, payloads)
        else:
            for name, content in payloads.items():
                with (_ordinary(cohort) / name).open("xb") as stream:
                    stream.write(content)
            _verify(cohort, payloads)
    return {"profile": (cohort / PROFILE_NAME).as_posix(), "owner": (cohort / "owner.json").as_posix(),
            "selection": selection, "identity": identity}
