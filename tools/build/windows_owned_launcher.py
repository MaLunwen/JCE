#!/usr/bin/env python3
"""Build the tooling-only XP owned-child launcher; native build gate defaults closed.

The launcher is QA transport, never an Editor runtime companion. This producer
never runs the helper. Compiler/CMake stages use the shared XP stage owner
for hidden launches, timeout handling and retained stdout/stderr logs.
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
    from . import cache_lock, xp_editor
else:
    import cache_lock
    import xp_editor

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path("tools/build/native")
SCHEMA = "jce.windows.owned-launch.v1"


def make_plan(repo, arch, toolchain_root, out, *, cmake="cmake", ninja="ninja"):
    """Construct a serial command plan without writing or launching anything."""
    if arch not in ("x86", "x64"):
        raise ValueError("explicit x86/x64 architecture is required")
    for path in (repo, toolchain_root, out):
        if not Path(path).is_absolute():
            raise ValueError("repository, toolchain and output paths must be absolute")
    repo, toolchain_root, out = map(xp_editor._ordinary, (repo, toolchain_root, out))
    if out == repo or repo.is_relative_to(out) or out.is_relative_to(repo) and not out.is_relative_to(repo / "build"):
        raise ValueError("output must be external or within the repository build directory")
    toolchain = repo / "cmake/toolchains" / ("windows-" + arch + "-xp-llvm.cmake")
    return {"schema": "jce.windows.owned-launch-build.v1", "arch": arch,
            "source": str(repo / SOURCE), "toolchain_root": str(toolchain_root), "out": str(out),
            "helper": str(out / "out/jce_windows_owned_launcher.exe"), "jobs": 1,
            "commands": [[str(cmake), "-S", str(repo / SOURCE), "-B", str(out), "-G", "Ninja",
                          "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                          "-DCMAKE_MAKE_PROGRAM:FILEPATH=" + str(ninja),
                          "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=" + str(toolchain),
                          "-DJCE_XP_TOOLCHAIN_ROOT:PATH=" + str(toolchain_root)],
                         [str(cmake), "--build", str(out), "--parallel", "1"]],
            "runtime_execution": "NOT_EXECUTED", "graphics_execution": "NOT_EXECUTED"}


def _source_identity(repo):
    paths = [repo / SOURCE / name for name in ("windows_owned_launcher.c", "CMakeLists.txt")]
    paths += [repo / "tools/build/windows_owned_launcher.py", repo / "tools/build/xp_editor.py",
              repo / "tools/build/xp_compat.py", repo / "cmake/toolchains/windows-xp-llvm-common.cmake"]
    paths += [repo / "cmake/toolchains" / ("windows-" + arch + "-xp-llvm.cmake") for arch in ("x86", "x64")]
    return [{"path": path.relative_to(repo).as_posix(), "sha256": xp_editor._sha(xp_editor._ordinary(path))}
            for path in paths]


def _cache_owner(repo, arch, toolchain, runtime, sources):
    return {"schema": "jce.windows.owned-launch-cache.v2", "arch": arch,
            "source_root": str(repo / SOURCE), "toolchain": toolchain, "runtime": runtime,
            "toolchain_files": [{"path": row["path"].replace("\\", "/"), "sha256": row["sha256"]}
                                for row in sources if row["path"].replace("\\", "/").startswith("cmake/toolchains/")],
            "jobs": 1}


def _legacy_owner(out, stored, expected):
    """Admit the previous first-party marker only with its configured source root."""
    if set(stored) != {"schema", "arch", "toolchain", "runtime", "sources", "jobs"}:
        raise ValueError("legacy launcher cache ownership is incomplete")
    with (out / "CMakeCache.txt").open("rb") as stream:
        cache = stream.read(1024 * 1024 + 1)
    if len(cache) > 1024 * 1024:
        raise ValueError("legacy launcher CMake cache exceeds its read budget")
    prefix = "CMAKE_HOME_DIRECTORY:INTERNAL="
    roots = [line[len(prefix):] for line in cache.decode("utf-8").splitlines() if line.startswith(prefix)]
    if (len(roots) != 1 or not Path(roots[0]).is_absolute() or
            xp_editor._ordinary(roots[0]) != Path(expected["source_root"])):
        raise ValueError("legacy launcher cache source root differs")
    if not isinstance(stored["sources"], list) or any(
            not isinstance(row, dict) or set(row) != {"path", "sha256"} or
            not isinstance(row["path"], str) or not isinstance(row["sha256"], str)
            for row in stored["sources"]):
        raise ValueError("legacy launcher source inventory is invalid")
    normalized = _cache_owner(Path(expected["source_root"]).parents[2], stored["arch"],
                              stored["toolchain"], stored["runtime"], stored["sources"])
    normalized["jobs"] = stored["jobs"]
    return normalized


def _guard_cache(out, owner):
    marker = out / "windows-owned-launcher-owner.json"
    legacy = False
    if marker.exists():
        stored = xp_editor._read_json(marker)
        if not isinstance(stored, dict):
            raise ValueError("launcher cache ownership must be an object")
        if stored.get("schema") == "jce.windows.owned-launch-cache.v1":
            stored = _legacy_owner(out, stored, owner)
            legacy = True
        if stored != owner:
            raise ValueError("owned launcher cache identity differs; choose a fresh --out")
    elif out.exists() and any(out.iterdir()):
        raise ValueError("refusing a nonempty unowned launcher cache")
    out.mkdir(parents=True, exist_ok=True)
    if legacy or not marker.exists():
        xp_editor._write(marker, owner)


def build(repo, arch, toolchain_root, out, *, native_go=False, export_inventory=None, runner=None):
    """Build/audit only with native_go; runner is a Python-only counted-test port."""
    plan = make_plan(repo, arch, toolchain_root, out)
    if type(native_go) is not bool:
        raise ValueError("native_go must be a strict boolean")
    if not native_go:
        return {**plan, "state": "PLANNED_NATIVE_GATE_CLOSED"}
    if os.name != "nt":
        raise ValueError("the explicit native build gate requires a Windows host")
    repo, toolchain_root, out = map(xp_editor._ordinary, (repo, toolchain_root, out))
    cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
    if not cmake or not ninja:
        raise ValueError("CMake and Ninja are required")
    plan = make_plan(repo, arch, toolchain_root, out, cmake=cmake, ninja=ninja)
    sources = _source_identity(repo)
    nonce = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ") + "-" + uuid.uuid4().hex[:8]
    directory = repo / "build/validation/windows-owned-launcher" / arch / nonce
    receipt = {**plan, "state": "STARTED", "stages": [], "sources": sources,
               "protocol_schema": SCHEMA}
    directory.mkdir(parents=True, exist_ok=False)
    stages = xp_editor._Stages(directory, receipt, runner)
    env = dict(os.environ, CMAKE_BUILD_PARALLEL_LEVEL="1")
    try:
        info = xp_editor._preflight(toolchain_root, arch, stages, repo, env)
        runtime = xp_editor.runtime_identity(toolchain_root, arch)
        owner = _cache_owner(repo, arch, info, runtime, sources)
        receipt.update(toolchain=info, runtime_inputs=runtime)
        with cache_lock.cache_locks([out], repo / "build/.locks"):
            _guard_cache(out, owner)
            for index, command in enumerate(plan["commands"]):
                stages.run("configure" if index == 0 else "build", command, repo, env, timeout=600)
            helper = Path(plan["helper"])
            audit = xp_editor.audit_pe(helper, arch, export_inventory, require_gui=False)
            if _source_identity(repo) != sources:
                raise ValueError("owned launcher inputs changed during build")
            if xp_editor._sha(helper) != audit["sha256"]:
                raise ValueError("owned launcher changed during static audit")
            receipt.update(state="BUILT_AND_STATICALLY_INSPECTED", pe=audit,
                           helper_sha256=audit["sha256"], helper_bytes=helper.stat().st_size)
            return receipt
    except BaseException as error:
        receipt.update(state="FAILED", error={"type": type(error).__name__, "message": str(error)[:4096]})
        raise
    finally:
        xp_editor._write(directory / "receipt.json", receipt)
        print("[Owned launcher] Receipt: " + str(directory / "receipt.json"), flush=True)


def add_arguments(parser):
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--arch", choices=("x86", "x64"), required=True)
    parser.add_argument("--toolchain-root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--export-inventory", type=Path)
    parser.add_argument("--native-go", action="store_true", help="open the native compile gate; never run the helper")


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    add_arguments(parser)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        result = build(args.repo, args.arch, args.toolchain_root, args.out, native_go=args.native_go,
                       export_inventory=args.export_inventory)
        print(json.dumps(result, indent=2, ensure_ascii=True))
        return 0
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print("[Owned launcher] " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
