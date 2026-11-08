#!/usr/bin/env python3
"""Build isolated C99 Flecs callback/atomic controls using unchanged headers."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

if __package__:
    from . import xp_embedding as shared
    from . import cache_lock
else:
    import xp_embedding as shared
    import cache_lock
ROOT = Path(__file__).resolve().parents[2]
BRIDGE_CASES = ("smoke", "install", "heap", "thread", "thread-fast", "rollback", "sync",
                "clock", "atomic", "foreign", "foreign-empty", "late-defaults", "refused-set", "contended")
PLAN = {"xp_flecs_bridge_control": BRIDGE_CASES,
        "xp_flecs_bridge_modern_control": ("modern",),
        "xp_flecs_bridge_track_control": ("track",),
        "xp_flecs_atomic_control": (None,)}


def _compat():
    spec = importlib.util.spec_from_file_location("jce_flecs_private_compat",
                                                 Path(__file__).with_name("xp_compat.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Attempt:
    execute = shared.Attempt.execute
    save = shared.Attempt.save

    def __init__(self, repo, arch):
        nonce = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ") + "-" + uuid.uuid4().hex[:8]
        self.path = repo / "build/validation/xp-flecs" / arch / nonce
        self.path.mkdir(parents=True, exist_ok=False)
        self.commands = []
        self.receipt = {"schema": "jce.xp.flecs-controls.v1", "arch": arch, "state": "STARTED",
                        "xp_execution_verified": False, "editor_or_graphics_verified": False,
                        "local_execution_verified": False}


def _headers(directory):
    rows, total = [], 0
    if not (directory / "flecs.h").is_file():
        raise ValueError("the explicit unchanged Flecs include directory is required")
    for index, path in enumerate(directory.rglob("*"), 1):
        if index > 4096:
            raise ValueError("Flecs header directory exceeds its entry budget")
        shared._ordinary(path)
        if not path.is_file():
            continue
        if len(rows) >= 4096 or path.stat().st_size > 16 * 1024 * 1024:
            raise ValueError("Flecs header inventory exceeds its small-input budget")
        row = shared._identity(path)
        total += row["size"]
        if total > 64 * 1024 * 1024:
            raise ValueError("Flecs headers exceed the total input budget")
        rows.append({"path": path.relative_to(directory).as_posix(),
                     "size": row["size"], "sha256": row["sha256"]})
    return sorted(rows, key=lambda row: row["path"])


def _source_inputs(repo):
    fixture = repo / "tests/compatibility/xp_flecs"
    required = [fixture / name for name in ("CMakeLists.txt", "main.c", "bridge_control.c", "atomic_control.c")]
    sources = required + [path for path in sorted(fixture.glob("*")) if path.is_file() and path not in required]
    sources += [repo / name for name in (
        "engine/src/middleware/scene/jce_scene_flecs_os.c",
        "engine/src/middleware/scene/jce_scene_flecs_os.h",
        "engine/src/os/platform/jce_native_atomic_integer_internal.h",
        "engine/src/os/platform/jce_native_atomic_word_internal.h",
        "engine/src/os/platform/jce_entropy.c", "engine/src/os/core/jce_memory.h",
        "engine/include/jce/os/core/jce_defs.h", "engine/include/jce/jce_compat.h",
        "engine/include/jce/os/core/jce_alloc.h", "engine/include/jce/os/core/jce_str.h",
        "engine/include/jce/os/core/jce_thread.h", "engine/include/jce/os/core/jce_timer.h",
        "engine/include/jce/os/platform/jce_entropy.h", "tools/build/xp_flecs.py",
        "tools/build/xp_embedding.py", "tools/build/xp_compat.py")]
    return [shared._identity(shared._ordinary(path)) for path in sources]


def build(repo, arch, toolchain_root, flecs_include, jobs=1, run_local=False, negative_controls=False):
    if arch not in ("x86", "x64") or type(jobs) is not int or not 1 <= jobs <= 256:
        raise ValueError("explicit x86/x64 and jobs in 1..256 are required")
    repo = shared._ordinary(Path(repo))
    attempt = Attempt(repo, arch)
    try:
        toolchain_root = shared._ordinary(Path(toolchain_root))
        flecs_include = shared._ordinary(Path(flecs_include))
        xp = _compat()
        xp._execute = attempt.execute
        info = xp.preflight(toolchain_root, arch)
        cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
        if not cmake or not ninja:
            raise ValueError("CMake and Ninja are required")
        source = repo / "tests/compatibility/xp_flecs"
        output = shared._ordinary(repo / "build/compat" / ("windows-" + arch + "-xp-flecs-llvm-msvcrt"))
        toolchain = repo / "cmake/toolchains" / xp.ARCH[arch]["header"]
        headers, inputs = _headers(flecs_include), _source_inputs(repo)
        mutants, mutant_directory = None, ""
        if negative_controls:
            if not run_local:
                raise ValueError("ordinary negative controls require explicit --run-local")
            writer_path = repo / "tools/tests/test_xp_flecs_os.py"
            spec = importlib.util.spec_from_file_location("jce_flecs_mutation_writer", writer_path)
            writer = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(writer)
            directory = attempt.path / "mutants"
            mutants = writer.write_mutants(directory, repo=repo)
            mutant_directory = directory.as_posix()
            attempt.receipt["negative_manifest"] = shared._identity(directory / "manifest.json")
        request = {"schema": "jce.xp.flecs-cache.v1", "arch": arch, "source": source.as_posix(),
                   "toolchain": info, "cmake_toolchain": shared._identity(toolchain),
                   "common_toolchain": shared._identity(toolchain.with_name("windows-xp-llvm-common.cmake")),
                   "flecs_include": flecs_include.as_posix(), "headers": headers}
        attempt.receipt.update(toolchain=info, sources=inputs, headers=headers, plan=PLAN)
        with cache_lock.cache_locks([output], repo / "build/.locks"):
            marker = output / "xp-flecs-owner.json"
            if marker.is_file():
                if json.loads(marker.read_text(encoding="utf-8")) != request:
                    raise ValueError("XP Flecs cache ownership mismatch; refusing implicit reuse")
            elif output.exists() and any(output.iterdir()):
                raise ValueError("XP Flecs cache is nonempty and unowned")
            output.mkdir(parents=True, exist_ok=True)
            if not marker.exists():
                shared._write_json(marker, request)
            attempt.execute([Path(cmake).as_posix(), "-S", source.as_posix(), "-B", output.as_posix(),
                "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                "-DCMAKE_MAKE_PROGRAM:FILEPATH=" + Path(ninja).as_posix(),
                "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=" + toolchain.as_posix(),
                "-DJCE_XP_TOOLCHAIN_ROOT:PATH=" + toolchain_root.as_posix(),
                "-DJCE_XP_FLECS_INCLUDE:PATH=" + flecs_include.as_posix(),
                "-DJCE_XP_FLECS_MUTANT_DIR:PATH=" + mutant_directory, "-DJCE_WINXP_LEGACY=ON"], cwd=repo)
            attempt.execute([Path(cmake).as_posix(), "--build", output.as_posix(), "--parallel", str(jobs)], cwd=repo)
            executables, results = {}, []
            for target, cases in PLAN.items():
                exe = output / "out" / (target + ".exe")
                observation = shared._identity(exe)
                observation["pe"] = xp.audit_pe(exe, arch)
                if xp.sha256_file(exe) != observation["sha256"]:
                    raise ValueError("fixture changed during static audit")
                executables[target] = observation
                if not run_local:
                    continue
                for case in cases:
                    command = [str(exe)] + ([case] if case is not None else [])
                    result = attempt.execute(command, cwd=output)
                    expected = ("JCE_XP_FLECS_ATOMICS_OK" if case is None else
                                "JCE_XP_FLECS_BRIDGE_OK case=" + case)
                    if expected not in result.stdout.splitlines():
                        raise ValueError("fixture exited without its exact scenario marker")
                    if xp.sha256_file(exe) != observation["sha256"]:
                        raise ValueError("fixture changed during local execution")
                    results.append({"target": target, "case": case, "exit_code": 0, "marker": expected})
            negative_results = []
            if mutants is not None:
                for name, control in mutants["mutants"].items():
                    target = "xp_flecs_mutant_" + name
                    exe = output / "out" / (target + ".exe")
                    observation = shared._identity(exe)
                    observation["pe"] = xp.audit_pe(exe, arch)
                    try:
                        attempt.execute([str(exe), control["case"]], cwd=output)
                    except subprocess.CalledProcessError as error:
                        lines = ((error.stdout or "") + "\n" + (error.stderr or "")).splitlines()
                        if (error.returncode != control["expected_returncode"] or control["marker"] not in lines):
                            raise ValueError("negative control failed at an unexpected exit/assertion") from error
                    else:
                        raise ValueError("negative control unexpectedly passed")
                    if xp.sha256_file(exe) != observation["sha256"]:
                        raise ValueError("negative control changed during execution")
                    executables[target] = observation
                    negative_results.append({"target": target, "case": control["case"],
                        "expected_returncode": control["expected_returncode"], "marker": control["marker"],
                        "state": "EXPECTED_ORDINARY_ASSERTION_FAILURE"})
            attempt.receipt["negative_results"] = negative_results
            if _source_inputs(repo) != inputs or _headers(flecs_include) != headers:
                raise ValueError("fixture/header input changed during validation")
            attempt.receipt.update(state="BUILD_AND_STATIC_PE_AUDIT_PASSED", executables=executables,
                                   local_execution_verified=bool(run_local), local_results=results)
            return output
    except BaseException as error:
        attempt.receipt.update(state="FAILED", error={"type": type(error).__name__, "message": str(error)})
        raise
    finally:
        attempt.save()
        print("[XP Flecs controls] Receipt: " + str(attempt.path / "receipt.json"))


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--arch", choices=("x86", "x64"), required=True)
    parser.add_argument("--toolchain-root", type=Path, required=True)
    parser.add_argument("--flecs-include", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--run-local", action="store_true")
    parser.add_argument("--negative-controls", action="store_true")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        build(args.repo, args.arch, args.toolchain_root, args.flecs_include, args.jobs, args.run_local, args.negative_controls)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print("[XP Flecs controls] FAILURE: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
