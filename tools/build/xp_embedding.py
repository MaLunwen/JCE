#!/usr/bin/env python3
"""Isolated XP binary-symbol/RC link probe; static PE checks and optional host run.

This configures only tests/compatibility/xp_embedding. It never resolves Conan,
configures the Editor/SDK, or certifies XP execution or graphical behavior.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

if __package__:
    from . import xp_compat as compat
else:
    import xp_compat as compat

ROOT = Path(__file__).resolve().parents[2]
PAYLOAD = bytes.fromhex("317ca5e21963bd4f")
MARKER = "JCE_XP_EMBEDDING_PROBE_OK"
SCHEMA = "jce.xp.embedding.v1"
TARGETS = {"console": {"target": "jce_xp_embedding_probe", "subsystem": 3},
           "gui": {"target": "jce_xp_embedding_gui_probe", "subsystem": 2}}
CMAKE_PATH_KEYS = frozenset({"CMAKE_HOME_DIRECTORY", "CMAKE_TOOLCHAIN_FILE",
    "JCE_XP_TOOLCHAIN_ROOT", "JCE_BIN2OBJ_EXECUTABLE", "CMAKE_C_COMPILER",
    "CMAKE_CXX_COMPILER", "CMAKE_ASM_COMPILER", "CMAKE_RC_COMPILER"})


def _ordinary(path: Path) -> Path:
    path = path.absolute()
    for parent in (path, *path.parents):
        if parent.exists():
            status = parent.lstat()
            if parent.is_symlink() or getattr(status, "st_file_attributes", 0) & 0x400:
                raise ValueError("redirected probe path is not admitted: " + str(parent))
    return path.resolve()


def _identity(path: Path) -> dict:
    if not path.is_file():
        raise ValueError("required probe input is missing: " + str(path))
    return {"path": str(path), "size": path.stat().st_size, "sha256": compat.sha256_file(path)}


def _write_json(path: Path, data: dict) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, ensure_ascii=True) + "\n", encoding="utf-8", newline="\n")
    temporary.replace(path)


class Attempt:
    def __init__(self, repo: Path, arch: str):
        identifier = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ") + "-" + uuid.uuid4().hex[:8]
        self.path = repo / "build/validation/xp-embedding" / arch / identifier
        self.path.mkdir(parents=True, exist_ok=False)
        self.commands = []
        self.receipt = {"schema": SCHEMA, "arch": arch, "state": "STARTED",
                        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                        "xp_execution_verified": False, "editor_or_gpu_verified": False,
                        "local_execution": {"requested": False, "verified": False}}

    def execute(self, command, cwd=None, input_data=None):
        index = len(self.commands)
        record = {"argv": [str(item) for item in command],
                  "cwd": str(cwd) if cwd else None, "stdin": input_data,
                  "returncode": None}
        self.commands.append(record)
        stdout = stderr = ""
        try:
            completed = subprocess.run(record["argv"], cwd=record["cwd"], input=input_data,
                text=True, encoding="utf-8", errors="replace", capture_output=True,
                check=False, timeout=600,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            stdout, stderr = completed.stdout or "", completed.stderr or ""
            record["returncode"] = completed.returncode
            if completed.returncode:
                raise subprocess.CalledProcessError(completed.returncode, record["argv"], stdout, stderr)
            return completed
        except subprocess.TimeoutExpired as error:
            stdout = error.stdout or ""
            stderr = error.stderr or ""
            record["timeout"] = True
            raise
        except OSError as error:
            record["error"] = str(error)
            raise
        finally:
            for name, value in (("stdout", stdout), ("stderr", stderr)):
                if isinstance(value, bytes):
                    value = value.decode("utf-8", "replace")
                (self.path / f"{index:02d}-{name}.log").write_text(value, encoding="utf-8", newline="\n")
            _write_json(self.path / f"{index:02d}-command.json", record)
            _write_json(self.path / "commands.json", {"commands": self.commands})

    def save(self):
        self.receipt["commands"] = self.commands
        _write_json(self.path / "receipt.json", self.receipt)


def _canonical_owner(request):
    # Earlier attempts recorded native Windows separators in this metadata.
    # Compare only the known path fields canonically; keep the original marker.
    if not isinstance(request, dict) or not isinstance(request.get("cache"), dict):
        return request
    normalized = dict(request)
    normalized["cache"] = dict(request["cache"])
    for key in CMAKE_PATH_KEYS:
        value = normalized["cache"].get(key)
        if isinstance(value, str):
            normalized["cache"][key] = value.replace(chr(92), "/")
    return normalized


def _admit_owner(output: Path, request: dict) -> None:
    owner = output / "jce-xp-embedding-owner.json"
    if owner.exists():
        if _canonical_owner(json.loads(owner.read_text(encoding="utf-8"))) != _canonical_owner(request):
            raise ValueError("embedding cache ownership tuple differs; refusing reuse")
    else:
        occupied = [item for item in output.iterdir() if item.name != ".xp-embedding.lock"]
        if occupied:
            raise ValueError("embedding cache is nonempty without ownership metadata")
        _write_json(owner, request)
    cache = output / "CMakeCache.txt"
    if not cache.exists():
        return
    entries = {}
    for line in cache.read_text(encoding="utf-8", errors="strict").splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line and ":" in line.split("=", 1)[0]:
            left, value = line.split("=", 1)
            entries[left.split(":", 1)[0]] = value
    for key, expected in request["cache"].items():
        actual = entries.get(key)
        if actual is None or actual.replace("\\", "/").lower() != expected.replace("\\", "/").lower():
            raise ValueError("embedding CMake cache mismatch: " + key)


def _prepare_payload(path: Path) -> None:
    if path.exists():
        if path.read_bytes() != PAYLOAD:
            raise ValueError("embedding payload differs from the fixed eight-byte fixture")
    else:
        path.write_bytes(PAYLOAD)


def build(repo: Path, arch: str, toolchain_root: Path, host_bin2obj: Path,
          jobs: int = 1, run_local: bool = False, gui: bool = False) -> Path:
    if arch not in compat.ARCH:
        raise ValueError("arch must be x86 or x64")
    if not isinstance(jobs, int) or isinstance(jobs, bool) or not 1 <= jobs <= 256:
        raise ValueError("jobs must be an integer from 1 through 256")
    repo = _ordinary(repo)
    selection = TARGETS["gui" if gui else "console"]
    attempt = Attempt(repo, arch)
    attempt.receipt.update(requested_target=selection["target"], requested_subsystem=selection["subsystem"])
    attempt.receipt["local_execution"]["requested"] = bool(run_local)
    lock = None
    original_execute = compat._execute
    try:
        toolchain_root = _ordinary(toolchain_root)
        host_bin2obj = _ordinary(host_bin2obj)
        source = _ordinary(repo / "tests/compatibility/xp_embedding")
        output = _ordinary(repo / "build/compat" / f"windows-{arch}-xp-embedding-llvm-msvcrt")
        toolchain = _ordinary(repo / "cmake/toolchains" / compat.ARCH[arch]["header"])
        compiler = toolchain_root / "bin" / (compat.ARCH[arch]["triple"] + "-clang.exe")
        cxx = toolchain_root / "bin" / (compat.ARCH[arch]["triple"] + "-clang++.exe")
        windres = toolchain_root / "bin" / (compat.ARCH[arch]["triple"] + "-windres.exe")
        inputs = [source / name for name in ("CMakeLists.txt", "main.cpp", "resource.rc.in")]
        inputs += [repo / relative for relative in (
            "tests/compatibility/xp_core/main.cpp", "engine/src/os/platform/jce_entropy.c",
            "engine/include/jce/os/platform/jce_entropy.h", "engine/include/jce/jce_compat.h",
            "cmake/JCESDKHelpers.cmake", "cmake/JCESDKTarget.cmake", "cmake/JCEWindowsTarget.cmake",
            "cmake/toolchains/windows-xp-llvm-common.cmake", "tools/jce_bin2obj.c",
            "tools/build/xp_embedding.py", "tools/build/xp_compat.py")]
        inputs.append(toolchain)
        attempt.receipt["sources"] = [_identity(_ordinary(path)) for path in inputs]
        attempt.receipt["host_bin2obj"] = _identity(host_bin2obj)
        attempt.receipt["resource_compiler"] = _identity(_ordinary(windres))
        cmake = shutil.which("cmake")
        ninja = shutil.which("ninja")
        if not cmake or not ninja:
            raise ValueError("cmake and ninja must both be present on PATH")
        attempt.receipt["cmake"] = _identity(_ordinary(Path(cmake)))
        attempt.receipt["ninja"] = _identity(_ordinary(Path(ninja)))
        # Reuse the existing preflight while retaining its exact commands/output.
        compat._execute = attempt.execute
        info = compat.preflight(toolchain_root, arch)
        attempt.receipt["toolchain"] = info
        cache = {"CMAKE_HOME_DIRECTORY": source.as_posix(), "CMAKE_GENERATOR": "Ninja",
                 "CMAKE_BUILD_TYPE": "Release", "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
                 "CMAKE_TOOLCHAIN_FILE": toolchain.as_posix(),
                 "JCE_XP_TOOLCHAIN_ROOT": toolchain_root.as_posix(),
                 "JCE_BIN2OBJ_EXECUTABLE": host_bin2obj.as_posix(),
                 "CMAKE_C_COMPILER": compiler.as_posix(), "CMAKE_CXX_COMPILER": cxx.as_posix(),
                 "CMAKE_ASM_COMPILER": compiler.as_posix(), "CMAKE_RC_COMPILER": windres.as_posix(),
                 "JCE_XP_EXPECTED_ARCH": arch, "JCE_WINXP_LEGACY": "ON",
                 "JCE_XP_EMBED_SUBSYSTEMS": "console3-gui2"}
        request = {"schema": SCHEMA, "arch": arch, "cache": cache, "toolchain": info,
                   "host_bin2obj": attempt.receipt["host_bin2obj"],
                   "resource_compiler": attempt.receipt["resource_compiler"], "targets": TARGETS,
                   "windows_target": _identity(repo / "cmake/JCEWindowsTarget.cmake")}
        output.mkdir(parents=True, exist_ok=True)
        candidate = output / ".xp-embedding.lock"
        descriptor = os.open(candidate, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.close(descriptor)
        lock = candidate
        _admit_owner(output, request)
        payload = output / "payload.bin"
        _prepare_payload(payload)
        cmake = Path(cmake).as_posix()
        configure = [cmake, "-S", source.as_posix(), "-B", output.as_posix(), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            f"-DCMAKE_MAKE_PROGRAM:FILEPATH={Path(ninja).as_posix()}",
            f"-DCMAKE_TOOLCHAIN_FILE:FILEPATH={toolchain.as_posix()}",
            f"-DJCE_XP_TOOLCHAIN_ROOT:PATH={toolchain_root.as_posix()}",
            f"-DJCE_BIN2OBJ_EXECUTABLE:FILEPATH={host_bin2obj.as_posix()}",
            f"-DCMAKE_ASM_COMPILER:FILEPATH={compiler.as_posix()}",
            f"-DCMAKE_RC_COMPILER:FILEPATH={windres.as_posix()}",
            f"-DJCE_XP_EXPECTED_ARCH={arch}",
            "-DJCE_WINXP_LEGACY=ON", "-DJCE_XP_EMBED_SUBSYSTEMS=console3-gui2",
            f"-DJCE_XP_EMBED_PAYLOAD:FILEPATH={payload.as_posix()}"]
        attempt.execute(configure, cwd=repo)
        attempt.execute([cmake, "--build", output.as_posix(), "--config", "Release",
            "--target", selection["target"], "--verbose", "--parallel", str(jobs)], cwd=repo)
        exe = output / "out" / (selection["target"] + ".exe")
        attempt.receipt["executable"] = _identity(exe)
        pe = dict(compat.audit_pe(exe, arch))
        raw = exe.read_bytes()
        subsystem = compat._u16(raw, compat._u32(raw, 0x3c) + 24 + 68)
        if subsystem != selection["subsystem"]:
            raise ValueError(f"PE subsystem {subsystem} differs from requested {selection['subsystem']}")
        pe["subsystem"] = subsystem
        if compat.sha256_file(exe) != attempt.receipt["executable"]["sha256"]:
            raise ValueError("probe executable changed during static audit")
        crypto = set(pe["imports"].get("advapi32.dll", []))
        if not {"CryptAcquireContextW", "CryptGenRandom", "CryptReleaseContext"} <= crypto:
            raise ValueError("embedding probe is missing the real entropy CryptoAPI imports")
        attempt.receipt["pe"] = pe
        attempt.receipt["generated"] = [_identity(output / name) for name in
            ("payload.bin", "payload.S", "probe.rc", "CMakeCache.txt", "compile_commands.json")]
        attempt.receipt["state"] = "BUILD_AND_STATIC_PE_AUDIT_PASSED"
        if run_local:
            attempt.receipt["local_execution"]["requested"] = True
            completed = attempt.execute([str(exe)], cwd=output)
            if MARKER not in completed.stdout.splitlines():
                raise ValueError("local execution did not emit the final embedding marker")
            if compat.sha256_file(exe) != attempt.receipt["executable"]["sha256"]:
                raise ValueError("probe executable changed during local execution")
            attempt.receipt["local_execution"]["verified"] = True
        print("[XP embedding] Static PE scope passed: " + str(exe))
        return exe
    except BaseException as error:
        attempt.receipt["state"] = "FAILED"
        attempt.receipt["error"] = {"type": type(error).__name__, "message": str(error)}
        raise
    finally:
        compat._execute = original_execute
        if lock is not None and lock.exists():
            lock.unlink()
        attempt.save()
        print("[XP embedding] Attempt receipt: " + str(attempt.path / "receipt.json"))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--arch", choices=compat.ARCH, default="x64")
    parser.add_argument("--toolchain-root", type=Path, required=True)
    parser.add_argument("--host-bin2obj", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--gui", action="store_true",
                        help="select the shared-cache GUI subsystem target; creates no window")
    parser.add_argument("--run-local", action="store_true",
                        help="run on this host separately; never XP certification")
    args = parser.parse_args(argv)
    try:
        build(args.repo, args.arch, args.toolchain_root, args.host_bin2obj, args.jobs, args.run_local, args.gui)
        return 0
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print("[XP embedding] FAILURE: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
