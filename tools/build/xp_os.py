#!/usr/bin/env python3
"""Build isolated actual XP OS adapter controls; optional local CPU execution."""
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
    from . import xp_embedding as embedding
else:
    import xp_compat as compat
    import xp_embedding as embedding
ROOT = Path(__file__).resolve().parents[2]
TARGETS = {"jce_xp_executable_path": "JCE_XP_EXECUTABLE_PATH_OK",
           "jce_xp_window_adapter": "JCE_XP_WINDOW_ADAPTER_OK",
           "jce_xp_executable_path_omitted": None}


class Attempt:
    execute = embedding.Attempt.execute
    save = embedding.Attempt.save

    def __init__(self, repo, arch):
        nonce = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ") + "-" + uuid.uuid4().hex[:8]
        self.path = repo / "build/validation/xp-os" / arch / nonce
        self.path.mkdir(parents=True, exist_ok=False)
        self.commands = []
        self.receipt = {"schema": "jce.xp.os-controls.v1", "arch": arch, "state": "STARTED",
                        "xp_execution_verified": False, "runtime_verified": False,
                        "runtime_plan": {target: "exit0-and-marker" if marker else
                            "nonzero-and-surrogate-rejection-assertion" for target, marker in TARGETS.items()}}


def build(repo, arch, toolchain_root, jobs=1, run_local=False):
    if arch not in compat.ARCH or isinstance(jobs, bool) or not isinstance(jobs, int) or not 1 <= jobs <= 256:
        raise ValueError("valid x86/x64 and jobs 1..256 are required")
    repo = embedding._ordinary(Path(repo))
    attempt = Attempt(repo, arch)
    old_execute, lock = compat._execute, None
    try:
        toolchain_root = embedding._ordinary(Path(toolchain_root))
        source = repo / "tests/compatibility/xp_os"
        output = embedding._ordinary(repo / "build/compat" / f"windows-{arch}-xp-os-llvm-msvcrt")
        toolchain = repo / "cmake/toolchains" / compat.ARCH[arch]["header"]
        inputs = [source / name for name in ("CMakeLists.txt", "executable_path.c", "window_adapter.c")]
        inputs += [repo / name for name in (
            "engine/src/os/platform/jce_executable_path_native_internal.h",
            "engine/src/os/platform/jce_window_adapter.c", "engine/src/os/platform/jce_window_adapter_internal.h",
            "engine/src/os/platform/jce_entropy.c", "engine/include/jce/os/core/jce_alloc.h",
            "engine/include/jce/os/core/jce_defs.h", "engine/include/jce/os/platform/jce_window.h",
            "engine/include/jce/os/platform/jce_entropy.h", "cmake/JCEWindowsTarget.cmake",
            "cmake/toolchains/windows-xp-llvm-common.cmake", "tools/build/xp_os.py",
            "tools/build/xp_embedding.py", "tools/build/xp_compat.py",
            "tests/cmake/JceGeneratedFixture.cmake")]
        inputs.append(toolchain)
        attempt.receipt["sources"] = [embedding._identity(embedding._ordinary(path)) for path in inputs]
        cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
        if not cmake or not ninja:
            raise ValueError("cmake and ninja are required")
        attempt.receipt["cmake"] = embedding._identity(Path(cmake))
        attempt.receipt["ninja"] = embedding._identity(Path(ninja))
        compat._execute = attempt.execute
        info = compat.preflight(toolchain_root, arch)
        attempt.receipt["toolchain"] = info
        prefix = toolchain_root / "bin" / compat.ARCH[arch]["triple"]
        cache = {"CMAKE_HOME_DIRECTORY": source.as_posix(), "CMAKE_GENERATOR": "Ninja",
                 "CMAKE_BUILD_TYPE": "Release", "CMAKE_TOOLCHAIN_FILE": toolchain.as_posix(),
                 "JCE_XP_TOOLCHAIN_ROOT": toolchain_root.as_posix(), "JCE_WINXP_LEGACY": "ON",
                 "CMAKE_C_COMPILER": str(prefix).replace(chr(92), "/") + "-clang.exe"}
        request = {"schema": "jce.xp.os-controls.v1", "arch": arch, "cache": cache,
                   "toolchain": info, "targets": TARGETS,
                   "windows_target": embedding._identity(repo / "cmake/JCEWindowsTarget.cmake")}
        output.mkdir(parents=True, exist_ok=True)
        candidate = output / ".xp-embedding.lock"
        fd = os.open(candidate, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.close(fd)
        lock = candidate
        embedding._admit_owner(output, request)
        cmake = Path(cmake).as_posix()
        attempt.execute([cmake, "-S", source.as_posix(), "-B", output.as_posix(), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            f"-DCMAKE_MAKE_PROGRAM:FILEPATH={Path(ninja).as_posix()}",
            f"-DCMAKE_TOOLCHAIN_FILE:FILEPATH={toolchain.as_posix()}",
            f"-DJCE_XP_TOOLCHAIN_ROOT:PATH={toolchain_root.as_posix()}", "-DJCE_WINXP_LEGACY=ON"], cwd=repo)
        attempt.execute([cmake, "--build", output.as_posix(), "--config", "Release", "--verbose",
            "--parallel", str(jobs)], cwd=repo)
        executables = {}
        for target in TARGETS:
            exe = output / "out" / (target + ".exe")
            item = embedding._identity(exe)
            item["pe"] = compat.audit_pe(exe, arch)
            if compat.sha256_file(exe) != item["sha256"]:
                raise ValueError("OS fixture changed during static audit")
            executables[target] = item
        attempt.receipt.update(state="BUILD_AND_STATIC_PE_AUDIT_PASSED", executables=executables)
        if run_local:
            results = {}
            for target, marker in TARGETS.items():
                exe = output / "out" / (target + ".exe")
                try:
                    result = attempt.execute([str(exe)], cwd=output)
                except subprocess.CalledProcessError as error:
                    if (marker is not None or "XP_PATH_CHECK_FAILED:" not in (error.stdout or "")
                            or "JCE_XP_PATH_SURROGATE_REJECT_FAILED" not in (error.stdout or "").splitlines()):
                        raise
                    results[target] = {"expected_failure_observed": True, "exit_code": error.returncode}
                else:
                    if marker is None or marker not in result.stdout.splitlines():
                        raise ValueError("OS control runtime result did not satisfy its actual assertion plan")
                    results[target] = {"marker": marker, "exit_code": 0}
                if compat.sha256_file(exe) != executables[target]["sha256"]:
                    raise ValueError("OS fixture changed during local execution")
            attempt.receipt.update(runtime_verified=True, local_results=results)
        return output
    except BaseException as error:
        attempt.receipt.update(state="FAILED", error={"type": type(error).__name__, "message": str(error)})
        raise
    finally:
        compat._execute = old_execute
        if lock is not None and lock.exists(): lock.unlink()
        attempt.save()
        print("[XP OS controls] Receipt: " + str(attempt.path / "receipt.json"))


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--arch", choices=compat.ARCH, required=True)
    parser.add_argument("--toolchain-root", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--run-local", action="store_true")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        build(args.repo, args.arch, args.toolchain_root, args.jobs, args.run_local)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print("[XP OS controls] FAILURE: " + str(error), file=sys.stderr)
        return 1
if __name__ == "__main__": sys.exit(main())
