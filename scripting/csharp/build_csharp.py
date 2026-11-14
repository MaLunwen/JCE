#!/usr/bin/env python3
"""
build_csharp.py — build the managed half of the "csharp" scripting backend.

WHY NOT CMAKE.  `dotnet build` is not run from CMake, for the same reason
scripting/java/build_java.py exists: a machine with a C toolchain and no .NET
SDK must still be able to build every native target the tree declares.  Wiring
the managed build into the native one would make the .NET SDK a hard
dependency of `cmake --build`, which it is not.

WHAT IT PRODUCES.  managed/artifacts/ containing JceScript.dll and
JceScript.runtimeconfig.json.  The runtimeconfig is what hostfxr reads to pick
a runtime, and hostfxr finds it BY NAME beside the assembly — so the two must
stay in one directory, which is why this copies both rather than pointing the
engine at bin/Release/net8.0/.

Usage:
    python scripting/csharp/build_csharp.py            # Release
    python scripting/csharp/build_csharp.py --debug
    python scripting/csharp/build_csharp.py --check    # is it up to date?
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE / "managed" / "JceScript" / "JceScript.csproj"
ARTIFACTS = HERE / "managed" / "artifacts"
OUTPUTS = ("JceScript.dll", "JceScript.runtimeconfig.json")


def log(msg: str) -> None:
    print("[csharp] " + msg, flush=True)


def have_dotnet() -> bool:
    return shutil.which("dotnet") is not None


def newest_source_mtime() -> float:
    newest = 0.0
    for p in (HERE / "managed").rglob("*"):
        if p.is_dir() or "artifacts" in p.parts or "obj" in p.parts \
           or "bin" in p.parts:
            continue
        newest = max(newest, p.stat().st_mtime)
    return newest


def up_to_date() -> bool:
    """Every output present AND newer than every source.

    Presence alone is not enough and that is the whole point of this check:
    an artifacts/ directory left over from an earlier source tree is exactly
    the state where the engine loads an assembly whose Bootstrap does not
    match the bridge it was built against."""
    src = newest_source_mtime()
    for name in OUTPUTS:
        f = ARTIFACTS / name
        if not f.is_file() or f.stat().st_mtime < src:
            return False
    return True


def build(config: str) -> int:
    if not have_dotnet():
        log("SKIPPED — no `dotnet` on PATH. The C# backend needs the .NET SDK "
            "to BUILD its managed half; a player needs only the runtime.")
        return 0

    # MSBuild may place outputs in bin/x64/Release/net8.0 on Windows but
    # bin/Release/net8.0 on Unix. Do not guess the Platform dimension.
    # Stage under the managed project's existing obj/ owner; publish only
    # once BOTH required outputs exist. A failed build leaves old artifacts
    # untouched, and the unique temporary output is deleted on exit.
    staging_root = PROJECT.parent / "obj"
    staging_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="jce-csharp-output-", dir=staging_root) as temp:
        built = Path(temp)
        cmd = ["dotnet", "build", str(PROJECT), "-c", config, "--nologo",
               "-v", "minimal", "--output", str(built)]
        log(" ".join(cmd))
        r = subprocess.run(cmd, cwd=str(HERE))
        if r.returncode != 0:
            return r.returncode

        missing = [name for name in OUTPUTS if not (built / name).is_file()]
        if missing:
            log("ERROR: dotnet build did not publish: " + ", ".join(missing)
                + " in " + str(built)
                + ". Verify <EnableDynamicLoading>true</EnableDynamicLoading> "
                  "for the class-library runtimeconfig.")
            return 1
        ARTIFACTS.mkdir(parents=True, exist_ok=True)
        for name in OUTPUTS:
            shutil.copy2(built / name, ARTIFACTS / name)
    log("-> " + str(ARTIFACTS))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--check", action="store_true",
                    help="exit 0 if the artifacts are newer than the sources")
    a = ap.parse_args()
    if a.check:
        ok = up_to_date()
        log("up to date" if ok else "STALE or missing")
        return 0 if ok else 1
    return build("Debug" if a.debug else "Release")


if __name__ == "__main__":
    sys.exit(main())
