"""Explicit GNU/MSVCRT Windows XP Editor producer; no modern-cache fallback.

The ordinary embedded Editor is built from the root project. Static admission
and actual guest execution remain separate: this module never certifies a GUI,
rendered frame, scripting execution or relocation from a PE header alone.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import time
import uuid


ROOT = Path(__file__).resolve().parents[2]
PHASES = ("prepare", "resolve", "dependencies", "configure", "build", "audit")
XP_SHADER_PROFILES = ("dx11", "glsl", "spv")
XP_CACHE_OWNER = "xp-editor-owner.json"
LOG_LIMIT = 8 * 1024 * 1024
INVENTORY_LIMIT = 16 * 1024 * 1024
XP_SYSTEM_DLLS = frozenset(("advapi32 cabinet comctl32 comdlg32 crypt32 d3d9 "
    "dbghelp dnsapi gdi32 gdiplus hid imm32 iphlpapi kernel32 mpr msimg32 "
    "msvcrt mswsock netapi32 ntdll ole32 oleaut32 opengl32 powrprof psapi rpcrt4 "
    "secur32 setupapi shell32 shlwapi user32 userenv usp10 uxtheme version "
    "winhttp wininet winmm winspool wintrust wldap32 ws2_32 wsock32 wtsapi32").split())


def _module(name):
    spec = importlib.util.spec_from_file_location("jce_xp_editor_" + name,
                                                 Path(__file__).with_name(name + ".py"))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def _sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True,
                                    separators=(",", ":")).encode()).hexdigest()


def _write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n",
                         encoding="utf-8", newline="\n")
    os.replace(temporary, path)


def _read_json(path, limit=INVENTORY_LIMIT):
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError("JSON input exceeds bounded budget: " + str(path))
    return json.loads(data.decode("utf-8"))


def _ordinary(path):
    """Refuse existing link/reparse aliases, including directory ancestors."""
    path = Path(path).absolute()
    for part in (path, *path.parents):
        if part.exists():
            status = part.lstat()
            if part.is_symlink() or getattr(status, "st_file_attributes", 0) & 0x400:
                raise ValueError("reparse/link paths are unsupported: " + str(part))
    return path.resolve()


def _owned_child(path, parent):
    path, parent = _ordinary(path), _ordinary(parent)
    if not path.is_relative_to(parent) or path == parent:
        raise ValueError("output must be a strict child of " + str(parent))
    return path


def target_profile(toolchain_root, arch, toolchain):
    """The host context means target libraries in Conan's terminology."""
    xp = _module("xp_compat")
    selected = xp.ARCH[arch]
    root = Path(toolchain_root).resolve()
    triple = selected["triple"]
    minor = selected["os"][1]
    winnt = "0x0502" if arch == "x64" else "0x0501"
    flags = ["-D_WIN32_WINNT=" + winnt, "-DWINVER=" + winnt,
             "-DPSAPI_VERSION=1", "-femulated-tls", "-pthread"]
    link = ["-static", "-pthread", "-Wl,--major-os-version,5,--minor-os-version," + str(minor),
            "-Wl,--major-subsystem-version,5,--minor-subsystem-version," + str(minor)]
    executables = {"c": (root / "bin" / (triple + "-clang.exe")).as_posix(),
                   "cpp": (root / "bin" / (triple + "-clang++.exe")).as_posix(),
                   "ar": (root / "bin/llvm-ar.exe").as_posix(),
                   "rc": (root / "bin" / (triple + "-windres.exe")).as_posix()}
    variables = {"JCE_XP_TOOLCHAIN_ROOT": root.as_posix(),
                 "THREADS_PREFER_PTHREAD_FLAG": True}
    return ("[settings]\nos=Windows\narch=" + ("x86_64" if arch == "x64" else "x86")
        + "\nbuild_type=Release\ncompiler=clang\ncompiler.version=22\n"
        "compiler.libcxx=libc++\ncompiler.cppstd=20\n\n[conf]\n"
        "tools.cmake.cmaketoolchain:generator=Ninja\n"
        "tools.build:compiler_executables=" + json.dumps(executables) + "\n"
        "tools.cmake.cmaketoolchain:user_toolchain=" + json.dumps([Path(toolchain).as_posix()]) + "\n"
        "tools.cmake.cmaketoolchain:extra_variables=" + repr(variables) + "\n"
        "tools.build:cflags=" + json.dumps(flags) + "\n"
        "tools.build:cxxflags=" + json.dumps(flags + ["-stdlib=libc++"]) + "\n"
        "tools.build:exelinkflags=" + json.dumps(link) + "\n"
        "tools.build:sharedlinkflags=" + json.dumps(link) + "\n")


def runtime_identity(toolchain_root, arch):
    """Hash actual compiler/runtime bytes, rather than the cache directory label."""
    xp = _module("xp_compat")
    root = Path(toolchain_root).resolve()
    triple = xp.ARCH[arch]["triple"]
    cpu = "x86_64" if arch == "x64" else "i386"
    paths = ["bin/clang-22.exe", "bin/clang.exe", "bin/clang++.exe", "bin/clang-target-wrapper.exe",
             "bin/libclang-cpp.dll", "bin/libLLVM-22.dll",
             "bin/libc++.dll", "bin/libunwind.dll", "bin/libwinpthread-1.dll",
             "bin/ld.lld.exe", "bin/llvm-ar.exe", "bin/llvm-ranlib.exe", "bin/llvm-windres.exe", "bin/llvm-rc.exe",
             "bin/" + triple + "-windres.exe",
             "bin/mingw32-common.cfg"]
    paths += [triple + "/lib/" + name for name in (
        "libc++.a", "libc++abi.a", "libunwind.a", "libwinpthread.a", "libpthread.a",
        "libmingw32.a", "libmingwex.a", "libmoldname.a", "libmsvcrt.a", "libmsvcrt-os.a",
        "crt2.o", "crt2u.o", "crtbegin.o", "crtend.o")]
    paths.append("lib/clang/22/lib/windows/libclang_rt.builtins-" + cpu + ".a")
    result = {}
    for relative in paths:
        path = root / relative
        if not path.is_file():
            raise ValueError("required XP compiler/runtime input is missing: " + str(path))
        result[relative] = _sha(_ordinary(path))
    result["content_inventory"] = _toolchain_inventory(root, triple)
    return result


def _toolchain_inventory(root, triple, *, max_files=100000, max_bytes=16 * 1024 ** 3):
    """Measure the installed compiler closure, with no upstream-pristine claim.

    Each path spelling is accounted separately, including hardlink aliases.
    Links/reparse points and nonregular entries fail closed. Directory traversal
    and total hashed bytes have explicit budgets; no source tree is copied.
    """
    root = _ordinary(root)
    if type(max_files) is not int or type(max_bytes) is not int or min(max_files, max_bytes) <= 0:
        raise ValueError("toolchain inventory budgets must be positive integers")
    scopes = ("include", triple + "/include", triple + "/lib", "lib/clang/22")
    optional_include = triple + "/include"
    root_states = {}
    files, total, entries_seen, directories, directory_states = [], 0, 0, 0, []

    def add(path, status):
        nonlocal total
        # On Windows DirEntry.stat may report zero file IDs. Path.lstat and
        # fstat provide comparable identities for the held-handle check below.
        status = path.lstat()
        if not stat.S_ISREG(status.st_mode):
            raise ValueError("nonregular toolchain inventory input: " + str(path))
        if path.is_symlink() or getattr(status, "st_file_attributes", 0) & 0x400:
            raise ValueError("toolchain inventory refuses reparse/link inputs: " + str(path))
        total += status.st_size
        if len(files) >= max_files or total > max_bytes:
            raise ValueError("toolchain content inventory exceeds its file/byte budget")
        files.append((path.relative_to(root).as_posix(), path, status))

    for relative in scopes:
        directory = root / relative
        try:
            initial = directory.lstat()
        except FileNotFoundError:
            if relative != optional_include:
                raise ValueError("required toolchain content directory is missing: " + relative) from None
            for header in ("include/windows.h", "include/c++/v1/__config"):
                if not (root / header).is_file():
                    raise ValueError("absent target include requires global Windows/libc++ headers: " + header)
            root_states[relative] = "absent; global include provides Windows/libc++ headers"
            parent = _ordinary(directory.parent)
            directory_states.append((parent, parent.stat().st_mtime_ns))
            continue
        if directory.is_symlink() or getattr(initial, "st_file_attributes", 0) & 0x400:
            raise ValueError("toolchain inventory refuses reparse/link directories: " + str(directory))
        if not stat.S_ISDIR(initial.st_mode):
            raise ValueError("required toolchain content path is not a directory: " + relative)
        root_states[relative] = "directory"
        pending = [directory]
        while pending:
            directory = pending.pop()
            directories += 1
            status = directory.lstat()
            if directory.is_symlink() or getattr(status, "st_file_attributes", 0) & 0x400:
                raise ValueError("toolchain inventory refuses reparse/link directories: " + str(directory))
            directory_states.append((directory, status.st_mtime_ns))
            with os.scandir(directory) as entries:
                for entry in entries:
                    entries_seen += 1
                    if entries_seen > max_files * 4:
                        raise ValueError("toolchain directory inventory exceeds its entry budget")
                    path = Path(entry.path)
                    status = entry.stat(follow_symlinks=False)
                    if entry.is_symlink() or getattr(status, "st_file_attributes", 0) & 0x400:
                        raise ValueError("toolchain inventory refuses reparse/link inputs: " + str(path))
                    if stat.S_ISDIR(status.st_mode):
                        if len(path.relative_to(root).parts) > 64 or len(pending) > max_files:
                            raise ValueError("toolchain directory inventory exceeds its depth/entry budget")
                        pending.append(path)
                    else:
                        add(path, status)
    bin_directory = _ordinary(root / "bin")
    directory_states.append((bin_directory, bin_directory.stat().st_mtime_ns))
    with os.scandir(bin_directory) as entries:
        for entry in entries:
            if entry.name.lower().endswith((".cfg", ".dll")):
                add(Path(entry.path), entry.stat(follow_symlinks=False))
    digest = hashlib.sha256()
    digest.update(json.dumps(root_states, sort_keys=True, separators=(",", ":")).encode() + b"\n")
    for index, (relative, path, before) in enumerate(sorted(files, key=lambda row: row[0]), 1):
        if index % 2048 == 0:
            print("[XP Editor] toolchain content identity: " + str(index) + "/" + str(len(files)) + " files", flush=True)
        contents = hashlib.sha256()
        with path.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            if (opened.st_ino, opened.st_dev, opened.st_size, opened.st_mtime_ns) != (
                    before.st_ino, before.st_dev, before.st_size, before.st_mtime_ns):
                raise ValueError("toolchain input changed before hashing: " + relative)
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                contents.update(block)
            after = os.fstat(stream.fileno())
        current = path.lstat()
        if (path.is_symlink() or getattr(current, "st_file_attributes", 0) & 0x400
                or (after.st_ino, after.st_dev, after.st_size, after.st_mtime_ns) != (
                    before.st_ino, before.st_dev, before.st_size, before.st_mtime_ns)
                or (current.st_ino, current.st_dev, current.st_size, current.st_mtime_ns) != (
                    before.st_ino, before.st_dev, before.st_size, before.st_mtime_ns)):
            raise ValueError("toolchain input changed while hashing: " + relative)
        digest.update(json.dumps([relative, before.st_size, contents.hexdigest()],
                                 separators=(",", ":")).encode() + b"\n")
    for relative, path, before in files:
        current = path.lstat()
        if (path.is_symlink() or getattr(current, "st_file_attributes", 0) & 0x400
                or (current.st_ino, current.st_dev, current.st_size, current.st_mtime_ns) != (
                    before.st_ino, before.st_dev, before.st_size, before.st_mtime_ns)):
            raise ValueError("toolchain inventory changed before completion: " + relative)
    for directory, modification in directory_states:
        current = directory.lstat()
        if (directory.is_symlink() or getattr(current, "st_file_attributes", 0) & 0x400
                or current.st_mtime_ns != modification):
            raise ValueError("toolchain directory inventory changed before completion: " + str(directory))
    return {"schema": "jce.xp.toolchain-content.v1", "sha256": digest.hexdigest(),
            "files": len(files), "bytes": total, "directories": directories, "roots": list(scopes),
            "root_states": root_states,
            "bin_inputs": "all cfg/DLL files plus separately recorded required executables",
            "max_files": max_files, "max_bytes": max_bytes,
            "scope": "actual installed content identity; official pristine archive provenance is unverified"}


def _default_conan_home(repo, cache):
    """Version small hook configuration; keep the independent package store."""
    directory = _ordinary(repo / "conan/hooks")
    hooks = sorted(directory.glob("hook_*.py"))
    if not hooks or len(hooks) > 64:
        raise ValueError("bounded first-party Conan hook inventory is required")
    contents = {}
    total = 0
    for path in hooks:
        path = _ordinary(path)
        before = path.stat()
        if not path.is_file() or before.st_size > 1024 * 1024:
            raise ValueError("invalid or oversized first-party Conan hook")
        data = path.read_bytes()
        after = path.stat()
        total += len(data)
        if (len(data) != before.st_size or total > 4 * 1024 * 1024
                or (before.st_size, before.st_mtime_ns, before.st_ino) !=
                   (after.st_size, after.st_mtime_ns, after.st_ino)):
            raise ValueError("first-party Conan hooks changed during selection")
        contents[path.name] = hashlib.sha256(data).hexdigest()
    return cache / "conan-homes" / _digest(contents)[:24]


def _guard_cache(cache, identity):
    marker = _owned_child(cache / XP_CACHE_OWNER, cache)
    if marker.exists():
        if _read_json(marker) != identity:
            raise ValueError("XP Editor cache tuple mismatch; refusing implicit reuse: " + str(cache))
    elif cache.exists() and any(cache.iterdir()):
        raise ValueError("refusing a nonempty unowned XP Editor cache: " + str(cache))
    cache.mkdir(parents=True, exist_ok=True)
    if not marker.exists():
        _write(marker, identity)


class _Stages:
    """Every native launch gets bounded retained stdout/stderr and an outcome."""
    def __init__(self, directory, receipt, runner=None):
        self.directory, self.receipt = directory, receipt
        self.runner = runner or subprocess.run

    def _log(self, path, contents):
        data = contents if isinstance(contents, bytes) else (contents or "").encode("utf-8", "replace")
        path.write_bytes(data[:LOG_LIMIT])
        return {"path": str(path), "bytes": len(data), "retained_bytes": min(len(data), LOG_LIMIT),
                "truncated": len(data) > LOG_LIMIT, "sha256": hashlib.sha256(data).hexdigest()}

    def run(self, label, command, cwd, env, *, input_data=None, timeout=7200, accepted_returncodes=(0,)):
        number = len(self.receipt["stages"]) + 1
        stem = str(number).zfill(3) + "-" + re.sub(r"[^a-zA-Z0-9_-]", "_", label)
        row = {"stage": label, "argv": [str(x) for x in command], "cwd": str(cwd),
               "state": "RUNNING", "returncode": None,
               "started_utc": datetime.now(timezone.utc).isoformat(),
               "accepted_returncodes": list(accepted_returncodes)}
        self.receipt["stages"].append(row)
        self.receipt["active_stage"] = label
        _write(self.directory / "receipt.json", self.receipt)
        print("[XP Editor] " + label + ": " + json.dumps(row["argv"]), flush=True)
        started = time.monotonic_ns()
        try:
            result = self.runner(row["argv"], cwd=str(cwd), env=env, input=input_data,
                                 text=True, encoding="utf-8", errors="replace",
                                 capture_output=True, timeout=timeout, check=False,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            row["stdout"] = self._log(self.directory / (stem + ".stdout.log"), result.stdout)
            row["stderr"] = self._log(self.directory / (stem + ".stderr.log"), result.stderr)
            row["returncode"] = result.returncode
            if result.returncode not in accepted_returncodes:
                raise subprocess.CalledProcessError(result.returncode, row["argv"],
                                                    output=result.stdout, stderr=result.stderr)
            row["state"] = "SUCCEEDED"
            return result
        except BaseException as error:
            row["state"] = "FAILED"
            if "stdout" not in row:
                row["stdout"] = self._log(self.directory / (stem + ".stdout.log"),
                                           getattr(error, "stdout", None))
                row["stderr"] = self._log(self.directory / (stem + ".stderr.log"),
                                           getattr(error, "stderr", None) or str(error))
            row["error"] = str(error)[:4096]
            raise
        finally:
            row["elapsed_ns"] = str(time.monotonic_ns() - started)
            _write(self.directory / "receipt.json", self.receipt)


def _preflight(toolchain_root, arch, stages, repo, env):
    # Load a private instance: recording the existing owner's launches must not
    # replace another caller's xp_compat._execute while builds run concurrently.
    xp = _module("xp_compat")
    xp._execute = lambda command, cwd=None, input_data=None: stages.run(
        "preflight-" + arch, command, cwd or repo, env, input_data=input_data, timeout=600)
    return xp.preflight(toolchain_root, arch)


def _configured_storage(home):
    """Read Conan's effective storage setting without opening its cache."""
    from conan.internal.model.conf import ConfDefinition
    conf = ConfDefinition()
    path = home / "global.conf"
    if path.is_file():
        with path.open("rb") as stream:
            raw = stream.read(1024 * 1024 + 1)
        if len(raw) > 1024 * 1024:
            raise ValueError("Conan global configuration exceeds the small-file budget")
        conf.loads(raw.decode("utf-8"))
    configured = conf.get("core.cache:storage_path", default=None)
    return _ordinary(Path(configured) if configured else home / "p")


def _separate_paths(first, second):
    return first != second and first not in second.parents and second not in first.parents


def _xp_store(repo, store):
    store = _owned_child(store, repo / "build/compat")
    marker = store / ".jce-xp-editor-store.json"
    owner = {"schema": "jce.xp.editor-store.v1", "source_root": str(repo),
             "scope": "independent XP GNU/MSVCRT packages; no copied upstream cache"}
    if marker.is_file():
        if _read_json(marker) != owner:
            raise ValueError("XP Conan package-store ownership differs")
    elif store.exists() and any(store.iterdir()):
        raise ValueError("refusing a nonempty unowned XP Conan package store")
    else:
        store.mkdir(parents=True, exist_ok=True)
        _write(marker, owner)
    return store


def _verified_clang_major(info):
    match = re.search(r"\bclang version ([1-9][0-9]*)\.", info.get("clang_version", ""))
    if not match or match[1] != "22" or info.get("target") not in (
            "x86_64-w64-windows-gnu", "i686-w64-windows-gnu"):
        raise ValueError("verified native XP Clang major/target is unavailable")
    return match[1]


def _prepare_conan_home(repo, home, store, info, *, base_conan_home=None,
                        explicit_home=False, allow_prepare=True):
    """Prepare small owned configuration, sharing only this XP-owned store."""
    major = _verified_clang_major(info)
    home = _owned_child(home, repo / "build")
    marker = home / ".jce-toolchain-overlay.json"
    if home.exists() and any(home.iterdir()) and not marker.is_file():
        raise ValueError("refusing a nonempty unowned Conan configuration overlay")
    configuration = _module("conan_config")
    existing = _read_json(marker) if marker.is_file() else None
    base = None
    source_digest = None
    # An existing explicit home is a caller-selected configuration. Refresh it
    # only when the caller also explicitly selects its source configuration.
    if not explicit_home or base_conan_home is not None:
        base = _ordinary(base_conan_home if base_conan_home is not None else Path.home() / ".conan2")
        original_store = _configured_storage(base)
        if (not _separate_paths(store, original_store) or not _separate_paths(store, base)
                or not _separate_paths(home, base)):
            raise ValueError("XP Conan store/overlay overlaps the original home or package store")
        source_digest = configuration.configuration_digest(base)
    if existing is not None:
        if (Path(existing.get("home", "")).resolve() != home
                or existing.get("clang_major") != major
                or Path(existing.get("storage", "")).resolve() != store
                or _configured_storage(home) != store):
            raise ValueError("owned Conan overlay does not select the independent XP store/actual Clang major")
    _xp_store(repo, store)
    refresh = existing is None or (base is not None and existing.get("base_configuration_sha256") != source_digest)
    if refresh:
        if not allow_prepare:
            raise ValueError("audit requires the already prepared owned Conan configuration overlay")
        if explicit_home and base_conan_home is None:
            raise ValueError("preparing an explicit --conan-home requires --base-conan-home")
        if base is None:
            raise ValueError("original Conan configuration home is required")
        metadata = configuration.prepare_compiler_home(base, home, major, package_store=store)
        # A concurrent source edit must not admit a mixed configuration snapshot.
        if configuration.configuration_digest(base) != source_digest:
            raise ValueError("original Conan configuration changed while preparing its overlay")
        if (Path(metadata.get("home", "")).resolve() != home or metadata.get("clang_major") != major
                or Path(metadata.get("storage", "")).resolve() != store
                or _configured_storage(home) != store):
            raise ValueError("Conan overlay factory did not select the independent XP configuration")
        metadata = dict(metadata, base_home=str(base), base_configuration_sha256=source_digest)
        _write(marker, metadata)
    else:
        metadata = existing
    return {"metadata": metadata, "configuration_sha256": configuration.configuration_digest(home),
            "base_configuration_sha256": source_digest, "refreshed": refresh}


def _conan_environment(repo, conan_home, toolchain_root, jobs, env):
    home = _owned_child(conan_home, repo / "build")
    marker = home / ".jce-toolchain-overlay.json"
    if not marker.is_file() or not (home / "settings.yml").is_file():
        raise ValueError("--conan-home must select an existing owned configuration overlay")
    overlay = _read_json(marker)
    if Path(overlay.get("home", "")).resolve() != home or overlay.get("clang_major") != "22":
        raise ValueError("Conan overlay must record the actual native Clang major 22")
    result = dict(os.environ if env is None else env)
    for key in ("CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS", "INCLUDE", "LIB", "LIBPATH",
                "CMAKE_TOOLCHAIN_FILE", "CMAKE_PREFIX_PATH", "CMAKE_GENERATOR_PLATFORM",
                "CL", "_CL_", "LINK", "_LINK_", "VSCMD_ARG_TGT_ARCH", "VSCMD_ARG_HOST_ARCH"):
        result.pop(key, None)
    result.update({"CONAN_HOME": str(home), "CMAKE_BUILD_PARALLEL_LEVEL": str(jobs),
                   "JCE_XP_TOOLCHAIN_ROOT": str(toolchain_root),
                   "JCE_GRAPHICS_API_TIER": "compatibility", "JCE_BGFX_CPU_PROFILER": "off",
                   "JCE_BGFX_DEBUG_ANNOTATION": "off"})
    result["PATH"] = str(toolchain_root / "bin") + os.pathsep + result.get("PATH", "")
    return result


def _recipe_commands(repo):
    for recipe in sorted((repo / "conan/recipes").glob("*/conanfile.py")):
        command = ["conan", "export", str(recipe.parent)]
        data = recipe.parent / "conandata.yml"
        if data.is_file():
            versions = re.findall(r'^\s+"([^"\n]+)":\s*$', data.read_text(encoding="utf-8"), re.M)
            if recipe.parent.name == "quickjs-ng":
                if "0.4.1" not in versions:
                    raise ValueError("XP QuickJS provider source pin is missing")
                command += ["--version", "0.4.1"]
            elif versions:
                command += ["--version", versions[0]]
        yield command


def _snapshot_platform_headers(repo, directory, *, consumer="mimalloc"):
    """Freeze one named first-party adapter cohort, never a dependency tree."""
    cohorts = {
        "mimalloc": ("jce_windows_xp_sync_internal.h", "jce_windows_xp_mimalloc_internal.h"),
        "codec": ("jce_windows_xp_sync_internal.h", "jce_windows_xp_codec_sync_internal.h",
                  "jce_windows_xp_codecs_internal.h"),
        "stdio": ("jce_windows_xp_stdio_internal.h",),
        "bx_memory": ("windows-xp-bgfx-isa.cmake", "windows-xp-bgfx-simd.hpp",
                      "windows-xp-bgfx-vulkan.cpp", "jce_windows_xp_bx_memory_internal.h"),
    }
    if consumer not in cohorts:
        raise ValueError("unknown private XP consumer header cohort")
    names = cohorts[consumer]
    source = _owned_child(repo / "engine/src/os/platform/compatibility", repo / "engine/src/os/platform")
    payloads, hashes = {}, {}
    for name in names:
        root = repo / "cmake/toolchains" if name.startswith("windows-xp-bgfx-") else source
        path = _owned_child(root / name, root)
        with path.open("rb") as stream:
            before = os.fstat(stream.fileno())
            if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= 65536:
                raise ValueError("invalid or oversized first-party platform header")
            content = stream.read(65537)
            held = os.fstat(stream.fileno())
        after = path.stat()
        identity = lambda item: (item.st_size, item.st_mtime_ns, item.st_ino)
        if identity(before) != identity(held) or identity(held) != identity(after) or len(content) != before.st_size:
            raise ValueError("first-party platform header changed during admission")
        if b"\r" in content:
            raise ValueError("first-party platform headers must use LF")
        payloads[name] = content
        hashes[name] = hashlib.sha256(content).hexdigest()
    manifest = {"schema": "jce.xp.platform-headers.v1", "headers": hashes}
    digest = _digest(manifest)
    directory = _owned_child(directory, repo / "build/compat")
    directory.mkdir(parents=True, exist_ok=True)
    cohort = _owned_child(directory / digest, directory)
    if not cohort.exists():
        temporary = _owned_child(directory / (".pending-" + uuid.uuid4().hex), directory)
        temporary.mkdir()
        for name, content in payloads.items():
            (temporary / name).write_bytes(content)
        _write(temporary / "owner.json", manifest)
        os.replace(temporary, cohort)
    entries = []
    for entry in cohort.iterdir():
        if len(entries) >= len(names) + 1:
            raise ValueError("platform header cohort entry budget exceeded")
        entries.append(entry)
    if {entry.name for entry in entries} != set(names) | {"owner.json"}:
        raise ValueError("platform header cohort has unknown or missing entries")
    if _read_json(_ordinary(cohort / "owner.json"), 65536) != manifest:
        raise ValueError("platform header cohort identity mismatch; refusing repair")
    for name in names:
        if _sha(_ordinary(cohort / name)) != hashes[name]:
            raise ValueError("platform header snapshot changed; refusing repair")
    return {"headers_sha256": digest, "header": (cohort / names[-1]).as_posix(),
            "snapshot": str(cohort), "source_headers": hashes,
            "scope": ("two" if len(names) == 2 else str(len(names))) +
                     " first-party adapter headers; no upstream copies"}


def _conan_profile_arguments(profile, build_profile, jobs, abi_digest, platform_port=None,
                             consumer_ports=None, consumer_toolchain=None, *, bgfx_bx_debug="auto"):
    """One argument owner for graph-only resolution and dependency installation."""
    arguments = ["-pr:h", str(profile), "-pr:b", str(build_profile),
        "-o:h", "*:shared=False", "-o:h", "bgfx/*:tools=False", "-c:a", "tools.build:jobs=" + str(jobs),
        "-c:h", "user.jce:windows_xp=True", "-c:h", "user.jce:ai_dispatch=False",
        "-c:h", "user.jce:windows_xp_abi=" + abi_digest,
        "-c:h", 'tools.info.package_id:confs=["user.jce:windows_xp","user.jce:windows_xp_abi"]',
        "-c:h", "user.jce:bgfx_graphics_tier=compatibility", "-c:h", "user.jce:bgfx_cpu_profiler=off",
        "-c:h", "user.jce:bgfx_debug_annotation=off"]
    if bgfx_bx_debug != "auto":
        # BX public C++ headers require their macro even when a custom
        # toolchain keeps provider inference unknown. This is an explicit
        # consumer assertion; actual provider evidence remains separate.
        arguments += ["-c:h", "user.jce:bgfx_bx_debug=" + bgfx_bx_debug,
                      "-c:h", "user.jce:bgfx_bx_debug_consumer=" + bgfx_bx_debug]

    if platform_port is not None:
        arguments += ["-c:h", "user.jce:mimalloc_xp_header=" + platform_port["header"],
                      "-c:h", "user.jce:mimalloc_xp_headers_sha256=" + platform_port["headers_sha256"]]
    if consumer_ports is not None:
        if set(consumer_ports) != {"codec", "stdio", "bx_memory"} or consumer_toolchain is None:
            raise ValueError("complete private XP consumer cohorts and toolchain are required")
        for kind in ("codec", "stdio", "bx_memory"):
            port = consumer_ports[kind]
            arguments += ["-c:h", "user.jce:xp_" + kind + "_header=" + port["header"],
                          "-c:h", "user.jce:xp_" + kind + "_headers_sha256=" + port["headers_sha256"]]
        arguments += ["-c:h", "user.jce:xp_consumer_toolchain=" + Path(consumer_toolchain).as_posix()]
    return arguments


def _default_build_profile(repo, cache, driver, *, allow_create=True):
    """Pin the installed native toolset without changing target package inputs."""
    installation = driver.find_vs_install("x86_64")
    if installation is None:
        raise ValueError("A native MSVC installation is required; supply --build-profile for another native toolchain")
    return _module("msvc_build_profile").prepare(
        repo / "conan/profiles/windows-x64", installation,
        cache / "native-build-profiles", allow_create=allow_create)


def _package_observation(node):
    return {"ref": node["ref"], "package_id": node["package_id"],
            "package_folder": node.get("package_folder"), "context": node.get("context"),
            "recipe_status": node.get("recipe"), "binary_status": node.get("binary"),
            "settings": node.get("settings", {})}


def _resolved_graph_admission(graph_path, arch):
    targets = _admit_graph(graph_path, arch, allow_missing=True)
    nodes = _read_json(graph_path)["graph"]["nodes"]
    build_packages = []
    for key, node in nodes.items():
        if key == "0" or node.get("context") != "build":
            continue
        if not node.get("ref") or not node.get("package_id"):
            raise ValueError("resolved native build-host package identity is incomplete")
        build_packages.append(_package_observation(node))
    packages = targets + build_packages
    return {"state": "DEPENDENCY_GRAPH_RESOLVED_NOT_BUILT", "target_packages": targets,
            "build_packages": build_packages,
            "build_context_scope": "separate native build-host graph; execution NOT_EXECUTED",
            "missing_binaries": [row["ref"] for row in packages if row["binary_status"] == "Missing"],
            "unavailable_binary_status": [row["ref"] for row in packages if row["binary_status"] is None],
            "recipe_refs": sorted({node["ref"] for node in nodes.values() if node.get("ref")}),
            "consumer_recipe": {"ref": nodes.get("0", {}).get("ref"),
                                "recipe_status": nodes.get("0", {}).get("recipe")}}


def _generation_key(repo, command, env, profile, build_profile):
    files = {str(profile): _sha(profile), str(build_profile): _sha(build_profile)}
    for pattern in ("conanfile.py", "conan.lock", "conan/profiles/*", "conan/hooks/*.py",
                    "conan/recipes/**/*", "contracts/conan-source-policy.json"):
        for path in sorted(repo.glob(pattern)):
            if path.is_file():
                files[path.relative_to(repo).as_posix()] = _sha(path)
    configuration = _module("conan_config").configuration_digest(env["CONAN_HOME"])
    return _digest({"command": command, "files": files, "configuration": configuration,
                    "environment": {name: env.get(name) for name in (
                        "CONAN_HOME", "PATH", "CMAKE_BUILD_PARALLEL_LEVEL",
                        "JCE_GRAPHICS_API_TIER", "JCE_BGFX_CPU_PROFILER", "JCE_BGFX_DEBUG_ANNOTATION")}})


def _admit_graph(graph_path, arch, *, allow_missing=False):
    graph = _read_json(graph_path).get("graph", {})
    nodes = graph.get("nodes")
    if not isinstance(nodes, dict) or not nodes:
        raise ValueError("Conan did not produce a complete dependency graph")
    admitted = []
    for key, node in nodes.items():
        if key == "0" or node.get("context") == "build":
            continue
        if node.get("context") != "host":
            raise ValueError("Conan dependency has an unknown context")
        settings, options = node.get("settings", {}), node.get("options", {})
        compiler = settings.get("compiler")
        headers = node.get("package_type") == "header-library"
        # C-only recipes legitimately remove compiler.libcxx/cppstd. A package
        # declaring no compiler settings is admissible only as header-only.
        if not headers and (settings.get("os") != "Windows" or settings.get("arch") != ("x86_64" if arch == "x64" else "x86")
                or compiler != "clang" or str(settings.get("compiler.version")) != "22"
                or settings.get("compiler.libcxx") not in (None, "libc++")
                or any(name in settings for name in ("compiler.runtime", "compiler.runtime_type", "compiler.runtime_version"))):
            raise ValueError("target graph contains a non-XP GNU ABI dependency: " + str(node.get("ref")))
        if str(options.get("shared", "False")).lower() != "false":
            raise ValueError("target graph contains a shared dependency: " + str(node.get("ref")))
        if not node.get("package_id") or not node.get("ref"):
            raise ValueError("Conan graph is missing resolved package identity")
        if not allow_missing and node.get("binary") == "Missing":
            raise ValueError("target dependency binary is missing: " + node["ref"])
        admitted.append(_package_observation(node))
    if not admitted:
        raise ValueError("Conan graph has no target dependencies")
    return admitted


def _cache_values(path):
    values = {}
    for line in path.read_text(encoding="utf-8", errors="strict").splitlines():
        match = re.match(r"([^#/:][^:=]*):[^=]+=(.*)$", line)
        if match:
            values[match[1]] = match[2]
    return values


def _admit_stock_shaders(cache, values, repo):
    keys = ("JCE_SHADER_STOCK_POLICY_NAME", "JCE_SHADER_STOCK_POLICY_SHA256", "JCE_SHADER_OUTPUT_DIR")
    if any(not isinstance(values.get(key), str) or not values[key] for key in keys):
        raise ValueError("configured stock shader policy metadata is incomplete")
    if values[keys[0]] != "compatibility" or not re.fullmatch(r"[0-9a-f]{64}", values[keys[1]]):
        raise ValueError("configured stock shader policy identity is invalid")
    catalog_path = _owned_child(repo / "engine/shaders/stock_profiles.json", repo / "engine/shaders")
    evidence = _module("xp_native_probe")
    raw, _identity = evidence._read(catalog_path, 1024 * 1024)
    catalog = evidence._json(raw)
    profiles = catalog.get("profiles")
    if (type(catalog.get("schema")) is not int or catalog["schema"] != 1
            or catalog.get("source_authority") != "contracts/shader-keywords.json"
            or not isinstance(profiles, dict)
            or any(not isinstance(profiles.get(name), dict) or type(profiles[name].get("id")) is not int
                   or profiles[name]["id"] != identifier for name, identifier in (("full", 0), ("compatibility", 1)))):
        raise ValueError("stock shader catalog identity is invalid")
    digest = hashlib.sha256(raw).hexdigest()
    if values[keys[1]] != digest:
        raise ValueError("configured stock shader policy hash does not match the current catalog")
    declared = Path(values[keys[2]])
    if not declared.is_absolute() or ".." in declared.parts:
        raise ValueError("stock shader policy output directory is not canonical")
    directory = _owned_child(declared, cache / "generated")
    expected = cache / "generated" / ("engine_assets-compat-" + digest[:32]) / "shaders"
    if directory != expected or not directory.is_dir():
        raise ValueError("stock shader policy output directory is missing or does not match the current policy")
    return {"name": "compatibility", "sha256": digest, "catalog": str(catalog_path),
            "directory": str(directory), "scope": "configured source policy; shader bytes and device execution are separate"}


def _configured_executable(cache, candidate, *, allow_legacy=False):
    configuration = cache / "CMakeCache.txt"
    configured = _cache_values(configuration).get("JCE_EDITOR_ARTIFACT_DIR") if configuration.is_file() else None
    if configured:
        directory = Path(configured)
        if not directory.is_absolute() or _ordinary(directory) != candidate.parent:
            raise ValueError("configured XP Editor output is outside its desktop candidate directory")
        return _ordinary(candidate)
    if candidate.exists():
        raise ValueError("desktop XP Editor candidate already exists outside the admitted build")
    if not allow_legacy:
        raise ValueError("XP Editor build has no configured desktop candidate")
    return _owned_child(cache / "dist/jce_editor.exe", cache)


def _admit_configured(cache, profile, toolchain_root, arch, *, repo=ROOT):
    values = _cache_values(cache / "CMakeCache.txt")
    if values.get("MSVC", "FALSE").upper() in ("1", "ON", "TRUE", "YES"):
        raise ValueError("XP Editor cannot use the MSVC ABI")
    expected = {
        "JCE_WINXP_LEGACY": "ON", "JCE_EDITOR_STANDALONE": "ON",
        "JCE_BUILD_EDITOR": "ON", "JCE_ENABLE_SDK_INSTALL": "OFF",
        "JCE_DELIVERY_PROFILE": "embedded", "JCE_BUILD_VARIANT": "dist",
        "JCE_BUILD_SCRIPT_VM_PYTHON": "OFF", "JCE_BUILD_SCRIPT_CSHARP": "OFF",
        "JCE_BUILD_SCRIPT_JAVA": "OFF", "JCE_ENABLE_AI_DISPATCH": "OFF",
        "JCE_EMBED_ENGINE_SHADERS": "ON", "JCE_BUILD_TESTS": "OFF",
        "JCE_SHADER_PROFILES": ";".join(XP_SHADER_PROFILES),
    }
    for name, wanted in expected.items():
        if values.get(name) != wanted:
            raise ValueError("configured Editor capability mismatch: " + name)
    xp = _module("xp_compat")
    for variable, suffix in (("CMAKE_C_COMPILER", "-clang.exe"),
                             ("CMAKE_CXX_COMPILER", "-clang++.exe")):
        wanted = toolchain_root / "bin" / (xp.ARCH[arch]["triple"] + suffix)
        if Path(values.get(variable, "")).resolve() != wanted.resolve():
            raise ValueError("configured compiler is not the admitted XP compiler: " + variable)
    return {"requested_languages": ["lua", "c", "cpp", "js"],
            "gui": "CONFIGURED_NOT_BUILT", "headless": "NOT_EXECUTED",
            "scripting_execution": "NOT_EXECUTED", "stock_shaders": "configured ON",
            "shader_profiles": list(XP_SHADER_PROFILES),
            "profile_sha256": _sha(profile),
            "stock_shader_policy": _admit_stock_shaders(cache, values, repo)}


def audit_pe(exe, arch, export_inventory=None, *, require_gui=True):
    """Validate all normal/delay symbols against an explicit XP guest inventory."""
    if type(require_gui) is not bool:
        raise ValueError("require_gui must be a strict boolean")
    xp, standalone = _module("xp_compat"), _module("standalone")
    blob = exe.read_bytes()
    records = standalone._pe_import_records(blob)
    pe = struct.unpack_from("<I", blob, 0x3c)[0]
    machine = struct.unpack_from("<H", blob, pe + 4)[0]
    optional = pe + 24
    os_version = struct.unpack_from("<HH", blob, optional + 40)
    subsystem_version = struct.unpack_from("<HH", blob, optional + 48)
    subsystem = struct.unpack_from("<H", blob, optional + 68)[0]
    magic = struct.unpack_from("<H", blob, optional)[0]
    directory_count = struct.unpack_from("<I", blob, optional + (108 if magic == 0x20b else 92))[0]
    if directory_count < 14:
        raise ValueError("XP Editor delay-import coverage is unavailable")
    if machine != xp.ARCH[arch]["machine"]:
        raise ValueError("XP Editor PE architecture mismatch")
    if os_version != xp.ARCH[arch]["os"] or subsystem_version != xp.ARCH[arch]["os"]:
        raise ValueError("XP Editor PE OS/subsystem version must be " + str(xp.ARCH[arch]["os"]))
    if require_gui and subsystem != 2:
        raise ValueError("XP Editor must be a native GUI subsystem executable")
    if not require_gui and subsystem not in (2, 3):
        raise ValueError("XP native probe requires GUI or console subsystem")
    dlls = {row["dll"] for row in records}
    if not records or "msvcrt.dll" not in dlls:
        raise ValueError("XP Editor import graph must contain MSVCRT")
    for row in records:
        dll = row["dll"]
        if not row["symbols"]:
            raise ValueError("empty XP Editor import lookup table")
        if not dll.endswith(".dll") or dll[:-4] not in XP_SYSTEM_DLLS:
            raise ValueError("non-XP or external DLL dependency: " + dll)
        for symbol in row["symbols"]:
            if isinstance(symbol, str) and (symbol in xp.POST_XP_IMPORTS or
                    dll == "kernel32.dll" and symbol.startswith("K32")):
                raise ValueError("known post-XP imported API: " + dll + "!" + symbol)
    result = {"machine": hex(machine), "os_version": list(os_version),
              "subsystem_version": list(subsystem_version), "subsystem": "GUI" if subsystem == 2 else "CONSOLE",
              "imports": records, "sha256": hashlib.sha256(blob).hexdigest(), "exe_bytes": len(blob),
              "runtime_execution": "NOT_EXECUTED", "state": "STATIC_UNVERIFIED"}
    if export_inventory is None:
        result["unavailable"] = "complete guest export inventory is required for import admission"
        return result
    inventory = _read_json(Path(export_inventory))
    guest = inventory.get("guest", {})
    if (inventory.get("schema") != "jce.xp.guest-exports.v1" or guest.get("arch") != arch
            or guest.get("os_version") != list(xp.ARCH[arch]["os"])):
        raise ValueError("export inventory does not identify the exact XP guest architecture/OS")
    exports = inventory.get("dlls")
    if not isinstance(exports, dict) or not exports or len(exports) > 512:
        raise ValueError("invalid guest DLL export inventory")

    def admit(dll, symbol, seen):
        token = dll + "!" + str(symbol)
        if token in seen or len(seen) >= 32:
            raise ValueError("cyclic/unbounded guest export forwarder: " + token)
        entry = exports.get(dll)
        if (dll[:-4] not in XP_SYSTEM_DLLS or not isinstance(entry, dict)
                or not re.fullmatch(r"[0-9a-f]{64}", entry.get("sha256", ""))):
            raise ValueError("guest system DLL provenance is missing: " + dll)
        names, ordinals = entry.get("names"), entry.get("ordinals")
        if (not isinstance(names, list) or not isinstance(ordinals, list)
                or any(not isinstance(name, str) for name in names)
                or any(type(ordinal) is not int or not 1 <= ordinal <= 65535 for ordinal in ordinals)):
            raise ValueError("invalid named/ordinal guest export inventory: " + dll)
        if symbol not in (ordinals if type(symbol) is int else names):
            raise ValueError("import absent from exact XP guest exports: " + token)
        forwarders = entry.get("forwarders")
        if not isinstance(forwarders, dict):
            raise ValueError("guest forwarder coverage must be explicit: " + dll)
        forward = forwarders.get(str(symbol))
        if forward is not None:
            if not isinstance(forward, dict) or set(forward) != {"dll", "symbol"}:
                raise ValueError("malformed guest forwarder: " + token)
            if type(forward["symbol"]) not in (str, int):
                raise ValueError("invalid guest forwarder symbol: " + token)
            admit(forward["dll"], forward["symbol"], seen | {token})

    for row in records:
        for symbol in row["symbols"]:
            admit(row["dll"], symbol, set())
    result.update({"state": "STATIC_IMPORT_AUDIT_PASSED", "inventory_sha256": _sha(export_inventory),
                   "import_scope": "all ordinary/delayed names, ordinals and declared forwarders"})
    return result


def _verify_payload(exe, cache, repo):
    standalone = _module("standalone")
    assets = standalone.verify_assets(exe, cache)
    inventory = _read_json(cache / "reports/editor_assets_bom.json")
    paths = {entry["path"].replace("\\", "/") for entry in inventory["entries"]}
    required_paths = (
        "i18n/en.json", "fonts/aileron-regular.ttf", "fonts/jce-cjk-fallback.ttf",
        "fonts/jce-cjk-fallback-ofl.txt", "fonts/jce-cjk-fallback-provenance.json",
        "engine_settings.rml",
    )
    for required in required_paths:
        if required not in paths:
            raise ValueError("required stock Editor payload missing: " + required)
    for profile in XP_SHADER_PROFILES:
        for shader in ("vs_imgui", "fs_imgui", "vs_pbr", "fs_pbr"):
            required = f"shaders/{shader}_{profile}.bin"
            if required not in paths:
                raise ValueError("required stock Editor shader missing: " + required)
    notices = standalone.verify_notices(exe, repo)
    return {"embedded_assets": assets, "embedded_notices": notices}


def _publish(exe, output, audit):
    if audit.get("state") != "STATIC_AUDIT_PASSED":
        raise ValueError("refusing to publish an unqualified XP Editor")
    if not audit.get("embedded_assets") or not audit.get("embedded_notices"):
        raise ValueError("single-file XP Editor requires verified assets and notices")
    if _sha(exe) != audit.get("sha256"):
        raise ValueError("XP Editor changed after audit")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + "." + uuid.uuid4().hex + ".tmp")
    shutil.copyfile(exe, temporary)
    if _sha(temporary) != audit["sha256"]:
        raise ValueError("copied XP Editor differs from audited input")
    os.replace(temporary, output)


def _built_identity(exe, cache, abi_digest, profile, build_profile, platform_port=None, consumer_ports=None,
                    *, bgfx_bx_debug="auto"):
    identity = {"abi_sha256": abi_digest, "sha256": _sha(exe),
            "platform_headers_sha256": platform_port["headers_sha256"] if platform_port else None,
            "consumer_headers_sha256": {kind: port["headers_sha256"] for kind, port in
                                        (consumer_ports or {}).items()},
            "cmake_cache_sha256": _sha(cache / "CMakeCache.txt"),
            "graph_sha256": _sha(cache / "conan/xp-graph.json"),
            "generation_sha256": _sha(cache / "conan/xp-generation.json"),
            "target_profile_sha256": _sha(profile), "build_profile_sha256": _sha(build_profile)}
    # Default identity stays compatible with existing admitted caches. Explicit
    # diagnostics must be selected again at audit, never relabeled by omission.
    if bgfx_bx_debug != "auto":
        identity["bgfx_bx_debug_request"] = bgfx_bx_debug
    return identity


def build(repo, arch, toolchain_root, jobs=1, *, phase="build", conan_home=None, base_conan_home=None,
          build_profile=None, shaderc=None, host_pak=None, host_bin2obj=None,
          export_inventory=None, bgfx_bx_debug="auto", driver=None, runner=None, env=None):
    """Run one explicit phase; only audit may publish the final executable.

    Dependencies/configure/build are cumulative and reuse matching generated
    dependencies. Audit does not compile; unavailable import coverage returns
    None and preserves STATIC_UNVERIFIED, never a published artifact.
    runner is a subprocess.run-compatible test/embedding port, never a CLI flag.
    """
    if type(bgfx_bx_debug) is not str or bgfx_bx_debug not in ("auto", "off", "on"):
        raise ValueError("bgfx BX debug must be auto, off or on")
    if arch not in ("x86", "x64") or phase not in PHASES:
        raise ValueError("unsupported XP Editor architecture/phase")
    if type(jobs) is not int or not 1 <= jobs <= 256:
        raise ValueError("jobs must be an integer from 1 through 256")
    repo, toolchain_root = _ordinary(repo), _ordinary(toolchain_root)
    desktop = repo / "build/desktop"
    compat = repo / "build/compat"
    desktop_target = _owned_child(desktop / ("windows-" + arch + "-xp"), desktop)
    desktop_cache = _owned_child(desktop_target / "obj", desktop_target)
    legacy_cache = _owned_child(compat / ("windows-" + arch + "-xp-editor-llvm-msvcrt"), compat)
    candidate = _owned_child(desktop_target / "dist/jce_editor.exe", desktop_target)
    validation = _owned_child(repo / "build/validation/xp-editor" / arch, repo / "build/validation/xp-editor")
    run = validation / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:12])
    run.mkdir(parents=True)
    receipt = {"schema": "jce.xp.editor-build.v1", "state": "FAILED", "phase": phase,
               "arch": arch, "cache": str(desktop_cache), "bgfx_bx_debug_request": bgfx_bx_debug,
               "canonical_candidate": str(candidate),
               "stages": [], "active_stage": "prepare",
               "runtime_execution": "NOT_EXECUTED", "final_artifact": None}
    stages = _Stages(run, receipt, runner)
    try:
        lock = _module("cache_lock")
        output = _owned_child(repo / "dist/editor" / ("JCE-editor-windows-" + arch + "-xp.exe"), repo / "dist/editor")
        caches = (desktop_cache, legacy_cache)
        homes = {path: Path(conan_home).resolve() if conan_home else _default_conan_home(repo, path)
                 for path in caches}
        store = _owned_child(compat / "conan-xp-llvm-msvcrt-store", compat)
        port_root = _owned_child(compat / "xp-platform-ports", compat)
        resources = [desktop_target, *caches, *homes.values(), store, output, port_root]
        with lock.cache_locks(resources, repo / "build/.locks"):
            selected_env = dict(os.environ if env is None else env)
            selected_env["CMAKE_BUILD_PARALLEL_LEVEL"] = str(jobs)
            info = _preflight(toolchain_root, arch, stages, repo, selected_env)
            _verified_clang_major(info)
            receipt["active_stage"] = "compiler-runtime-identity"
            xp = _module("xp_compat")
            tc = repo / "cmake/toolchains" / xp.ARCH[arch]["header"]
            profile_text = target_profile(toolchain_root, arch, tc)
            abi = {"toolchain": info, "runtime_inputs": runtime_identity(toolchain_root, arch),
                   "arch": arch, "minimum_os": list(xp.ARCH[arch]["os"]),
                        "crt": "system-msvcrt", "cxx": "libc++", "support_linkage": "static",
                   "tls": "emulated", "profile_sha256": hashlib.sha256(profile_text.encode()).hexdigest(),
                   "toolchain_sha256": _sha(tc),
                   "common_toolchain_sha256": _sha(tc.with_name("windows-xp-llvm-common.cmake"))}
            abi_digest = _digest(abi)
            identity = {"schema": "jce.xp.editor-cache.v1", "source_root": str(repo),
                        "product": "editor", "delivery": "native-single-exe", "abi": abi,
                        "abi_sha256": abi_digest}
            receipt["active_stage"] = "cache-admission"
            cache = desktop_cache
            desktop_owner = _owned_child(desktop_cache / XP_CACHE_OWNER, desktop_cache)
            legacy_owner = _owned_child(legacy_cache / XP_CACHE_OWNER, legacy_cache)
            if not desktop_owner.exists() and legacy_owner.is_file() and _read_json(legacy_owner) == identity:
                cache = legacy_cache
            receipt["cache"] = str(cache)
            home = homes[cache]
            if phase in ("configure", "build") and candidate.exists():
                _configured_executable(cache, candidate)
            _guard_cache(cache, identity)
            profile = cache / "xp-host.profile"
            if profile.exists() and profile.read_text(encoding="utf-8") != profile_text:
                raise ValueError("generated target profile changed outside its owner")
            if not profile.exists():
                profile.write_text(profile_text, encoding="utf-8", newline="\n")
            receipt.update({"tuple": identity, "state": "PREPARED"})
            if phase == "prepare":
                return cache
            receipt["platform_port"] = _snapshot_platform_headers(repo, port_root)
            receipt["consumer_ports"] = {kind: _snapshot_platform_headers(repo, port_root, consumer=kind)
                                         for kind in ("codec", "stdio", "bx_memory")}
            receipt["active_stage"] = "conan-home-admission"
            receipt["conan_overlay"] = _prepare_conan_home(repo, home, store, info,
                base_conan_home=base_conan_home, explicit_home=conan_home is not None,
                allow_prepare=phase != "audit")
            selected_env = _conan_environment(repo, home, toolchain_root, jobs, env)
            if build_profile is None:
                if driver is None:
                    driver = _module("jce")
                selected_profile = _default_build_profile(repo, cache, driver, allow_create=phase != "audit")
                build_profile = Path(selected_profile["profile"])
                receipt["native_build_profile"] = dict(selected_profile, source="installed-msvc-toolset")
            else:
                build_profile = Path(build_profile).resolve()
                receipt["native_build_profile"] = {"profile": str(build_profile), "source": "explicit"}
            if not build_profile.is_file():
                raise ValueError("native build-host Conan profile is missing")
            dependency_root = cache / "conan"
            profile_arguments = _conan_profile_arguments(profile, build_profile, jobs, abi_digest,
                receipt["platform_port"], receipt["consumer_ports"], tc, bgfx_bx_debug=bgfx_bx_debug)
            command = ["conan", "install", str(repo), *profile_arguments, "--build=missing",
                       "--output-folder", str(dependency_root), "--format=json"]
            graph_path = dependency_root / "xp-graph.json"
            stamp = dependency_root / "xp-generation.json"
            if phase != "audit":
                if driver is None:
                    driver = _module("jce")
                receipt["active_stage"] = "sync-owned-conan-hooks"
                driver.sync_conan_hooks(selected_env)
                if conan_home is None and _default_conan_home(repo, cache) != home:
                    raise ValueError("first-party Conan hooks changed during synchronization")
                if phase == "resolve":
                    for index, export in enumerate(_recipe_commands(repo)):
                        stages.run("conan-export-" + str(index), export, repo, selected_env)
                    graph_command = ["conan", "graph", "info", str(repo), *profile_arguments, "--format=json"]
                    result = stages.run("resolve", graph_command, repo, selected_env)
                    if len(result.stdout.encode("utf-8")) > INVENTORY_LIMIT:
                        raise ValueError("resolved Conan graph exceeds bounded JSON budget")
                    resolved_path = run / "resolved-graph.json"
                    resolved_path.write_text(result.stdout, encoding="utf-8", newline="\n")
                    receipt["graph_admission"] = _resolved_graph_admission(resolved_path, arch)
                    receipt["resolved_graph"] = {"path": str(resolved_path), "sha256": _sha(resolved_path),
                        "inputs_sha256": _generation_key(repo, graph_command, selected_env, profile, build_profile)}
                    receipt["target_packages"] = receipt["graph_admission"]["target_packages"]
                    receipt["state"] = "DEPENDENCY_GRAPH_RESOLVED_NOT_BUILT"
                    return resolved_path
                fingerprint = _generation_key(repo, command, selected_env, profile, build_profile)
                generated = dependency_root / "build/Release/generators/conan_toolchain.cmake"
                current = _read_json(stamp) if stamp.is_file() else None
                if current != {"fingerprint": fingerprint} or not generated.is_file() or not graph_path.is_file():
                    stamp.unlink(missing_ok=True)
                    for index, export in enumerate(_recipe_commands(repo)):
                        stages.run("conan-export-" + str(index), export, repo, selected_env)
                    result = stages.run("dependencies", command, repo, selected_env)
                    dependency_root.mkdir(parents=True, exist_ok=True)
                    if len(result.stdout.encode("utf-8")) > INVENTORY_LIMIT:
                        raise ValueError("Conan dependency graph exceeds bounded JSON budget")
                    graph_path.write_text(result.stdout, encoding="utf-8", newline="\n")
                    receipt["target_packages"] = _admit_graph(graph_path, arch)
                    if not generated.is_file():
                        raise ValueError("Conan succeeded without generating its target CMake toolchain")
                    _write(stamp, {"fingerprint": fingerprint})
                else:
                    receipt["target_packages"] = _admit_graph(graph_path, arch)
                    receipt["dependencies_reused"] = True
                receipt["state"] = "DEPENDENCIES_READY"
                if phase == "dependencies":
                    return cache
                tools = {"shaderc": shaderc, "host-pak": host_pak, "host-bin2obj": host_bin2obj}
                receipt["build_host_tools"] = {}
                for label, raw in tools.items():
                    if raw is None or not Path(raw).is_file():
                        raise ValueError("explicit runnable --" + label + " is required")
                    tools[label] = str(Path(raw).resolve())
                    stages.run("verify-" + label, [tools[label], "--version" if label == "shaderc" else "--help"],
                               repo, selected_env, timeout=30,
                               accepted_returncodes=(0,) if label == "shaderc" else (0, 1))
                    # Existing JCE cookers deliberately return usage error 1
                    # for --help. This probe establishes launch, not operation.
                    receipt["build_host_tools"][label] = {"path": tools[label], "sha256": _sha(tools[label]),
                                                           "scope": "launch only"}
                # compile_shaders.cmake gives its environment override priority
                # over -D. Publish exactly the compiler verified above.
                selected_env["JCE_SHADERC_EXECUTABLE"] = tools["shaderc"]
                configure = ["cmake", "-S", str(repo), "-B", str(cache), "-G", "Ninja",
                    "-DCMAKE_TOOLCHAIN_FILE=" + str(generated), "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DJCE_XP_TOOLCHAIN_ROOT=" + str(toolchain_root),
                    "-DJCE_WINXP_LEGACY=ON", "-DJCE_BUILD_VARIANT=dist", "-DJCE_EDITOR_STANDALONE=ON",
                    "-DJCE_BUILD_EDITOR=ON", "-DJCE_DELIVERY_PROFILE=embedded", "-DJCE_ENABLE_SDK_INSTALL=OFF",
                    "-DJCE_BUILD_SCRIPT_VM_PYTHON=OFF", "-DJCE_BUILD_SCRIPT_CSHARP=OFF", "-DJCE_BUILD_SCRIPT_JAVA=OFF",
                    "-DJCE_ENABLE_AI_DISPATCH=OFF", "-DJCE_ENABLE_PROFILING=OFF", "-DJCE_ENABLE_RENDERDOC=OFF",
                    "-DJCE_ENABLE_CPPCHECK=OFF", "-DJCE_BUILD_TESTS=OFF", "-DJCE_ENABLE_PATENTED_CODECS=OFF",
                    "-DJCE_EMBED_ENGINE_SHADERS=ON",
                    "-DJCE_SHADER_PROFILES=" + ";".join(XP_SHADER_PROFILES),
                    "-DJCE_EDITOR_ARTIFACT_DIR=" + str(candidate.parent), "-DJCE_EMIT_LINK_MAP=ON",
                    "-DJCE_SHADERC_EXECUTABLE=" + tools["shaderc"],
                    "-DJCE_PAK_EXECUTABLE=" + tools["host-pak"], "-DJCE_BIN2OBJ_EXECUTABLE=" + tools["host-bin2obj"]]
                stages.run("configure", configure, repo, selected_env)
                receipt["capabilities"] = _admit_configured(cache, profile, toolchain_root, arch, repo=repo)
                if _configured_executable(cache, candidate) != candidate:
                    raise ValueError("configure did not select the desktop XP Editor candidate")
                receipt["state"] = "CONFIGURED"
                if phase == "configure":
                    return cache
                stages.run("build", ["cmake", "--build", str(cache), "--target", "JCE_Editor", "--parallel", str(jobs)],
                           repo, selected_env)
                exe = _owned_child(candidate, desktop_target)
                if not exe.is_file():
                    raise ValueError("Editor build succeeded without its exact target executable")
                receipt.update(state="BUILT_STATIC_UNVERIFIED", candidate=str(exe), built_sha256=_sha(exe))
                receipt["capabilities"]["gui"] = "BUILT_NOT_EXECUTED"
                _write(cache / "xp-built.json", _built_identity(exe, cache, abi_digest, profile, build_profile,
                    receipt["platform_port"], receipt["consumer_ports"], bgfx_bx_debug=bgfx_bx_debug))
                return exe
            # Auditing cannot bless an arbitrary/pre-existing executable.
            exe = _configured_executable(cache, candidate, allow_legacy=cache == legacy_cache)
            receipt["audited_candidate"] = str(exe)
            built = _read_json(cache / "xp-built.json")
            if built != _built_identity(exe, cache, abi_digest, profile, build_profile,
                    receipt["platform_port"], receipt["consumer_ports"], bgfx_bx_debug=bgfx_bx_debug):
                raise ValueError("built Editor identity does not match this admitted cache")
            receipt["capabilities"] = _admit_configured(cache, profile, toolchain_root, arch, repo=repo)
            receipt["capabilities"]["gui"] = "BUILT_NOT_EXECUTED"
            receipt["target_packages"] = _admit_graph(graph_path, arch)
            receipt["active_stage"] = "static-audit"
            audit = audit_pe(exe, arch, export_inventory)
            audit.update(_verify_payload(exe, cache, repo))
            receipt["audit"] = audit
            if audit["state"] != "STATIC_IMPORT_AUDIT_PASSED":
                receipt["state"] = "STATIC_UNVERIFIED"
                return None
            audit["state"] = "STATIC_AUDIT_PASSED"
            if bgfx_bx_debug != "auto":
                receipt.update(state="DIAGNOSTIC_STATIC_AUDIT_PASSED", diagnostic_artifact=str(exe))
                return exe
            _publish(exe, output, audit)
            receipt.update({"state": "STATIC_AUDIT_PASSED", "final_artifact": str(output)})
            return output
    except BaseException as error:
        receipt.update({"state": "FAILED", "error": str(error)[:4096],
                        "error_type": type(error).__name__})
        raise
    finally:
        _write(run / "receipt.json", receipt)
        _write(validation / "latest.json", {"receipt": str(run / "receipt.json"),
                                             "state": receipt["state"], "phase": phase})
        print("[XP Editor] " + receipt["state"] + "; receipt: " + str(run / "receipt.json"), flush=True)


def add_arguments(parser):
    parser.add_argument("--arch", choices=("x86", "x64"), default="x64")
    parser.add_argument("--toolchain-root", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--phase", choices=PHASES, default="build")
    parser.add_argument("--bgfx-bx-debug", choices=("auto", "off", "on"), default="auto",
                        help="upstream BX diagnostics/assertions; auto preserves the build default")
    parser.add_argument("--conan-home", type=Path, help="owned configuration overlay with actual Clang 22 settings")
    parser.add_argument("--base-conan-home", type=Path,
                        help="original configuration source; defaults to ~/.conan2 for the automatic overlay")
    parser.add_argument("--build-profile", type=Path, help="explicit native build-host profile; default pins the installed MSVC update")
    parser.add_argument("--shaderc", type=Path)
    parser.add_argument("--host-pak", type=Path)
    parser.add_argument("--host-bin2obj", type=Path)
    parser.add_argument("--export-inventory", type=Path, help="jce.xp.guest-exports.v1 inventory from the exact target guest")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    add_arguments(parser)
    args = parser.parse_args(argv)
    try:
        result = build(ROOT, args.arch, args.toolchain_root, args.jobs, phase=args.phase,
              conan_home=args.conan_home, base_conan_home=args.base_conan_home,
              build_profile=args.build_profile, shaderc=args.shaderc,
              host_pak=args.host_pak, host_bin2obj=args.host_bin2obj, export_inventory=args.export_inventory,
              bgfx_bx_debug=args.bgfx_bx_debug)
        return 2 if args.phase == "audit" and result is None else 0
    except (OSError, ValueError, subprocess.SubprocessError, struct.error) as error:
        print("[XP Editor] FAILED: " + str(error), flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
