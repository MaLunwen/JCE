#!/usr/bin/env python3
"""
build_java.py — the Java half of the Java binding's build.

DELIBERATELY NOT CMAKE.  scripting/java/CMakeLists.txt builds the JNI shim and
stops there; javac and jar are run from here.  The reason is the one the owner
gave when scripting/ was made a sibling of engine/: a JDK must not become a
configure-time input of the engine build.  A machine with a C toolchain and no
JDK can build every native target the project declares, and the Java classes
are produced by whoever wants them.

    python scripting/java/build_java.py --out build/java
        -> <out>/classes/com/jce/script/*.class
        -> <out>/jce-script-<version>.jar

The version in the jar's name is the manifest's script_api_version read from
contracts/script-api.json, not a constant typed here: a jar whose name
disagrees with the surface it carries is the two-sided contract this whole
effort exists to remove.

javac is found through JAVA_HOME first and PATH second, and the search order is
reported, because "which JDK compiled this" is the first question when a
UnsatisfiedLinkError shows up on someone else's machine.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
JAVA_SRC_ROOT = REPO_ROOT / "scripting/java/src/main/java"
API_JSON = REPO_ROOT / "contracts/script-api.json"


def find_tool(name: str) -> tuple[Path, str]:
    """(path, how it was found).  JAVA_HOME wins, PATH is the fallback."""
    exe = name + (".exe" if os.name == "nt" else "")
    home = os.environ.get("JAVA_HOME")
    if home:
        cand = Path(home) / "bin" / exe
        if cand.is_file():
            return cand, f"JAVA_HOME ({home})"
    found = shutil.which(name)
    if found:
        return Path(found), "PATH"
    raise SystemExit(
        f"error: {name} not found. Set JAVA_HOME to a JDK, or put it on PATH.")


def script_api_version() -> int:
    return int(json.loads(API_JSON.read_text(encoding="utf-8"))
               ["script_api_version"])


def java_sources() -> list[Path]:
    files = sorted(JAVA_SRC_ROOT.rglob("*.java"))
    if not files:
        raise SystemExit(
            f"error: no .java under {JAVA_SRC_ROOT.relative_to(REPO_ROOT)} — "
            f"regenerate with:\n"
            f"  python tools/scriptgen/gen_script_bindings.py --write")
    return files


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(REPO_ROOT / "build/java"),
                    help="output directory (default build/java)")
    ap.add_argument("--no-jar", action="store_true",
                    help="compile only; do not package")
    ap.add_argument("--release", default="11",
                    help="javac --release (default 11: engine/java is Java 11 and "
                         "the scripting surface must not raise the minimum)")
    args = ap.parse_args()

    out = Path(args.out).resolve()
    classes = out / "classes"
    classes.mkdir(parents=True, exist_ok=True)

    javac, how = find_tool("javac")
    print(f"javac: {javac}  (found via {how})")

    srcs = [str(p) for p in java_sources()]
    cmd = [str(javac), "--release", args.release, "-Xlint:-cast",
           "-d", str(classes)] + srcs
    rc = subprocess.call(cmd)
    if rc != 0:
        return rc
    print(f"compiled {len(srcs)} source file(s) -> {classes}")

    if args.no_jar:
        return 0

    jar, _ = find_tool("jar")
    name = f"jce-script-{script_api_version()}.jar"
    rc = subprocess.call([str(jar), "--create", "--file", str(out / name),
                          "-C", str(classes), "."])
    if rc != 0:
        return rc
    print(f"packaged {out / name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
