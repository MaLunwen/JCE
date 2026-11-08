#!/usr/bin/env python3
"""Build isolated C99 XP mimalloc controls; local execution is explicit."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

if __package__:
    from . import xp_embedding as shared, cache_lock, xp_native_probe as native
else:
    import xp_embedding as shared
    import cache_lock
    import xp_native_probe as native

ROOT = Path(__file__).resolve().parents[2]
CASES = ("locks", "concurrency", "exports-present", "exports-missing",
         "kernel-missing", "ordinals", "forwarding")
FATAL_CASES = ("fail-current", "fail-lock", "fail-release")
TARGET = "xp_mimalloc_port_control"
MUTANT = "xp_mimalloc_port_omitted_try_control"
INPUTS = ("tests/compatibility/xp_mimalloc/CMakeLists.txt",
          "tests/compatibility/xp_mimalloc/main.c",
          "engine/src/os/platform/compatibility/jce_windows_xp_sync_internal.h",
          "engine/src/os/platform/compatibility/jce_windows_xp_mimalloc_internal.h",
          "tools/build/xp_mimalloc.py", "tools/build/xp_embedding.py",
          "tools/build/xp_compat.py", "tools/build/xp_native_probe.py",
          "tools/build/cache_lock.py", "tools/tests/test_xp_mimalloc_port.py",
          "cmake/JCEWindowsTarget.cmake")
MAX_INPUT_BYTES = 16 * 1024 * 1024
MAX_TOTAL_BYTES = 64 * 1024 * 1024


def _module(path):
    spec = importlib.util.spec_from_file_location("jce_mimalloc_private_" + uuid.uuid4().hex, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _compat():
    return _module(Path(__file__).with_name("xp_compat.py"))


class Attempt:
    execute = shared.Attempt.execute
    save = shared.Attempt.save

    def __init__(self, repo, arch):
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ") + "-" + uuid.uuid4().hex[:8]
        self.path = repo / "build/validation/xp-mimalloc" / arch / stamp
        self.path.mkdir(parents=True, exist_ok=False)
        self.commands = []
        self.receipt = {"schema": "jce.xp.mimalloc-controls.v1", "arch": arch, "state": "STARTED",
                        "xp_execution_verified": False, "editor_or_graphics_verified": False,
                        "local_execution_verified": False,
                        "timestamp_utc": datetime.now(timezone.utc).isoformat()}


def _ordinary(path):
    # Native policy additionally rejects dangling aliases before resolution.
    return shared._ordinary(native._ordinary(Path(path)))


def _identity(path, limit=MAX_INPUT_BYTES):
    path = _ordinary(path)
    before = path.stat()
    if not path.is_file() or not 0 < before.st_size <= limit:
        raise ValueError("mimalloc input type/size budget refusal: " + str(path))
    row = shared._identity(path)
    after = path.stat()
    fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")
    if any(getattr(before, key) != getattr(after, key) for key in fields):
        raise ValueError("mimalloc input changed during identity read")
    shared._ordinary(path)
    return row


def _inputs(repo, include, toolchain):
    paths = [repo / value for value in INPUTS] + [toolchain,
             toolchain.with_name("windows-xp-llvm-common.cmake"), include / "mimalloc/atomic.h"]
    if len(paths) > 32:
        raise ValueError("mimalloc input entry budget refusal")
    rows = [_identity(path) for path in paths]
    if sum(row["size"] for row in rows) > MAX_TOTAL_BYTES:
        raise ValueError("mimalloc input total budget refusal")
    return rows


def _compiler_inputs(root, xp, arch):
    triple = xp.ARCH[arch]["triple"]
    stem = "x86_64" if arch == "x64" else "i686"
    paths = [root / "bin" / (triple + suffix) for suffix in ("-clang.exe", "-clang++.exe")]
    paths.append(root / "bin" / (stem + "-w64-windows-gnu.cfg"))
    return [_identity(path, 512 * 1024 * 1024) for path in paths]


def _admit_cache(output, request, source, toolchain, compiler):
    marker = output / "xp-mimalloc-owner.json"
    if marker.exists():
        _identity(marker, 64 * 1024)
        if json.loads(marker.read_text(encoding="utf-8")) != request:
            raise ValueError("XP mimalloc cache ownership mismatch")
    elif output.exists() and any(output.iterdir()):
        raise ValueError("XP mimalloc cache is nonempty and unowned")
    output.mkdir(parents=True, exist_ok=True)
    if not marker.exists():
        shared._write_json(marker, request)
    cache = output / "CMakeCache.txt"
    if cache.exists():
        _identity(cache, 1024 * 1024)
        entries = {}
        for line in cache.read_text(encoding="utf-8").splitlines():
            if not line.startswith(("#", "//")) and ":" in line and "=" in line:
                left, value = line.split("=", 1)
                entries[left.split(":", 1)[0]] = value
        expected = {"CMAKE_HOME_DIRECTORY": source, "CMAKE_TOOLCHAIN_FILE": toolchain,
                    "CMAKE_C_COMPILER": compiler}
        for key, value in expected.items():
            if entries.get(key, "").replace(chr(92), "/").lower() != value.as_posix().lower():
                raise ValueError("XP mimalloc CMake cache mismatch: " + key)


def _run(attempt, exe, case, code, marker, kind, expected_sha):
    if _identity(exe)["sha256"] != expected_sha:
        raise ValueError("mimalloc executable changed before local execution")
    try:
        completed = attempt.execute([str(exe), case], cwd=exe.parent)
        actual, stdout, stderr = 0, completed.stdout or "", completed.stderr or ""
    except subprocess.CalledProcessError as error:
        actual, stdout, stderr = error.returncode, error.stdout or "", error.stderr or ""
    if _identity(exe)["sha256"] != expected_sha:
        raise ValueError("mimalloc executable changed during local execution")
    if actual != code or (stdout + "\n" + stderr).splitlines().count(marker) != 1:
        raise ValueError("mimalloc scenario unexpected exit or exact marker: " + case)
    return {"case": case, "exit_code": actual, "marker": marker, "kind": kind}


def build(repo, arch, toolchain_root, mimalloc_include, export_receipt,
          jobs=1, run_local=False, negative_controls=False):
    if arch not in ("x86", "x64") or type(jobs) is not int or not 1 <= jobs <= 256:
        raise ValueError("explicit x86/x64 and integer jobs in 1..256 are required")
    repo = _ordinary(repo)
    attempt = Attempt(repo, arch)
    try:
        if negative_controls and not run_local:
            raise ValueError("negative controls require explicit --run-local")
        root = _ordinary(toolchain_root)
        include = _ordinary(mimalloc_include)
        receipt = _ordinary(export_receipt)
        target = native.declared_target(receipt, arch)
        xp = _compat()
        xp._execute = attempt.execute
        compiler_inputs = _compiler_inputs(root, xp, arch)
        info = xp.preflight(root, arch)
        cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
        if not cmake or not ninja:
            raise ValueError("CMake and Ninja are required")
        source = repo / "tests/compatibility/xp_mimalloc"
        output = _ordinary(repo / "build/compat" / ("windows-" + arch + "-xp-mimalloc-llvm-msvcrt"))
        toolchain = repo / "cmake/toolchains" / xp.ARCH[arch]["header"]
        inputs = _inputs(repo, include, toolchain)
        mutant_dir = ""
        if negative_controls:
            directory = attempt.path / "mutants"
            writer = _module(repo / "tools/tests/test_xp_mimalloc_port.py")
            writer.write_mutants(directory, repo=repo)
            mutant_dir = directory.as_posix()
            attempt.receipt["negative_manifest"] = _identity(directory / "manifest.json")
        # Source/header content remains an ordinary incremental dependency.
        # Only the actual toolchain identity and architecture own this cache.
        owner = {"schema": "jce.xp.mimalloc-cache.v1", "arch": arch, "toolchain": info}
        attempt.receipt.update(toolchain=info, inputs=inputs, compiler_inputs=compiler_inputs,
                               source_scope="Only listed first-party inputs and complete upstream atomic.h")
        with cache_lock.cache_locks([output], repo / "build/.locks"):
            _ordinary(output)  # Revalidate after waiting for the target lease.
            _admit_cache(output, owner, source, toolchain, Path(compiler_inputs[0]["path"]))
            attempt.execute([Path(cmake).as_posix(), "-S", source.as_posix(), "-B", output.as_posix(),
                "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                "-DCMAKE_MAKE_PROGRAM:FILEPATH=" + Path(ninja).as_posix(),
                "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=" + toolchain.as_posix(),
                "-DJCE_XP_TOOLCHAIN_ROOT:PATH=" + root.as_posix(),
                "-DJCE_XP_MIMALLOC_INCLUDE:PATH=" + include.as_posix(),
                "-DJCE_XP_MIMALLOC_OUTPUT:PATH=" + (output / "out").as_posix(),
                "-DJCE_XP_MIMALLOC_MUTANT_DIR:PATH=" + mutant_dir, "-DJCE_WINXP_LEGACY=ON"], cwd=repo)
            attempt.execute([Path(cmake).as_posix(), "--build", output.as_posix(),
                             "--parallel", str(jobs)], cwd=repo)
            if _inputs(repo, include, toolchain) != inputs or _compiler_inputs(root, xp, arch) != compiler_inputs:
                raise ValueError("mimalloc source/header/toolchain input changed during build")
            executables, results, negative_results, fatal_results = {}, [], [], []
            for name in (TARGET, MUTANT) if negative_controls else (TARGET,):
                exe = output / "out" / (name + ".exe")
                observation = _identity(exe)
                observation["pe"] = native.audit_pe(exe, arch, receipt, **target)
                if _identity(exe)["sha256"] != observation["sha256"]:
                    raise ValueError("mimalloc executable changed during static audit")
                executables[name] = observation
                if run_local and name == TARGET:
                    results = [_run(attempt, exe, case, 0, "JCE_XP_MIMALLOC_OK:" + case, "POSITIVE", observation["sha256"])
                               for case in CASES]
                    if negative_controls:
                        fatal_results = [_run(attempt, exe, case, 91,
                            "JCE_XP_MIMALLOC_FAIL_STOP:" + case, "EXPECTED_FAIL_STOP",
                            observation["sha256"]) for case in FATAL_CASES]
                elif run_local:
                    negative_results.append(_run(attempt, exe, "locks", 1,
                        "JCE_XP_MIMALLOC_ASSERT_FAILED:try-publishes-held-state",
                        "EXPECTED_ORDINARY_ASSERTION_FAILURE", observation["sha256"]))
                if _identity(exe)["sha256"] != observation["sha256"]:
                    raise ValueError("mimalloc executable changed during local execution")
            if _inputs(repo, include, toolchain) != inputs or _compiler_inputs(root, xp, arch) != compiler_inputs:
                raise ValueError("mimalloc source/header/toolchain input changed during validation")
            attempt.receipt.update(state="BUILD_AND_STATIC_PE_AUDIT_PASSED", executables=executables,
                local_execution_verified=bool(run_local), local_results=results,
                negative_results=negative_results, fail_stop_results=fatal_results)
            return output
    except BaseException as error:
        attempt.receipt.update(state="FAILED", error={"type": type(error).__name__, "message": str(error)})
        raise
    finally:
        attempt.save()
        print("[XP mimalloc controls] Receipt: " + str(attempt.path / "receipt.json"))


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--arch", choices=("x86", "x64"), required=True)
    parser.add_argument("--toolchain-root", type=Path, required=True)
    parser.add_argument("--mimalloc-include", type=Path, required=True)
    parser.add_argument("--export-receipt", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--run-local", action="store_true")
    parser.add_argument("--negative-controls", action="store_true")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        build(args.repo, args.arch, args.toolchain_root, args.mimalloc_include, args.export_receipt,
              args.jobs, args.run_local, args.negative_controls)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print("[XP mimalloc controls] FAILURE: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
