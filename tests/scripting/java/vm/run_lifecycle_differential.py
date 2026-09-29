#!/usr/bin/env python3
"""
run_lifecycle_differential.py — Lua is the reference lifecycle; Java must match
it.

Runs ONE driver binary twice — `--language lua`, then `--language java` — over
the SAME recording mock host, and compares the two streams line by line.  Two
processes rather than two passes: a JVM that aborts must not be able to take
the reference stream with it, and the exit codes have to be readable
separately.

WHAT A DIFFERENCE MEANS.  Each stream carries, in order: the slot being
exercised, the engine-side observations (the return of every lifecycle call,
typed, so nil / 0 / false cannot pass for one another) and the host-call trace —
which member fired, with which arguments, in which order.  A VM that produced
the right return value by calling the wrong host member is invisible to the
first half and visible in the second.

FOUR THINGS ARE CHECKED BEFORE ANY COMPARISON, because two streams that both
stopped producing answers compare equal.  This repository shipped a
differential that was 150 cases of 151 dead and looked complete:

  1. neither stream carries R VMFAIL;
  2. both end in DONE, and DONE's observation and host-call counts are
     non-zero — asserted on EACH SIDE INDEPENDENTLY, because comparing the two
     counts to each other is satisfied by two zeroes;
  3. both exercised EVERY slot of struct JceScriptVM, and the slot list is read
     out of engine/include/jce/middleware/script/jce_script_vm.h rather than
     spelled here — so a nineteenth slot added tomorrow reddens this by name
     instead of being silently untested;
  4. the driver's own exit code is 0 (it refuses its own dead run first).

WHAT IT CANNOT SEE, stated rather than left to be discovered:

  * a behaviour that is wrong in the SAME way on both sides.  The two scripts
    are written to be equivalent by a human, and that equivalence is the
    premise, not the finding.
  * error message TEXT.  Lua reports `chunk:12: boom`, Java reports
    `java.lang.IllegalStateException: boom`; the mock records the method and
    whether a detail was present, and nothing else.  See
    jce_vm_lifecycle_host.h.
  * the watchdog.  Lua aborts a runaway dispatch with an instruction-count
    hook; Java has no equivalent and this backend says so rather than
    pretending.  No case here loops forever, so no case here reveals it.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
JAVA_SRC_ROOT = REPO_ROOT / "scripting/java/src/main/java"
VM_HEADER = REPO_ROOT / "engine/include/jce/middleware/script/jce_script_vm.h"

_SLOT = re.compile(r"\(\*(\w+)\)\s*\(")

# What the JNI checker prints when it is unhappy.  Case-sensitive on purpose:
# "warning" appears in ordinary JVM chatter, "WARNING:" does not.
_JNI_COMPLAINT = re.compile(r"^(WARNING|FATAL ERROR) ", re.M)


def vtable_slots() -> list[str]:
    """The slot names, read out of the struct the backend must fill.

    Derived, never spelled: a slot appended to JceScriptVM that no case
    exercises fails condition 3 by name, which is the whole reason this
    function exists instead of a list literal."""
    text = VM_HEADER.read_text(encoding="utf-8")
    start = text.index("struct JceScriptVM {")
    end = text.index("\n};", start)
    body = text[start:end]
    slots = _SLOT.findall(body)
    if len(slots) < 10:
        raise SystemExit(
            f"error: only {len(slots)} slot(s) parsed out of {VM_HEADER} — the "
            f"struct's shape changed and this parser did not")
    return slots


def find_tool(name: str) -> Path:
    exe = name + (".exe" if os.name == "nt" else "")
    home = os.environ.get("JAVA_HOME")
    if home and (Path(home) / "bin" / exe).is_file():
        return Path(home) / "bin" / exe
    found = shutil.which(name)
    if not found:
        raise SystemExit(f"error: {name} not found (set JAVA_HOME or PATH)")
    return Path(found)


def compile_vm_classes(work: Path) -> Path:
    classes = work / "classes"
    if classes.exists():
        shutil.rmtree(classes)
    classes.mkdir(parents=True)
    srcs = [str(p) for p in sorted(JAVA_SRC_ROOT.rglob("*.java"))]
    if not srcs:
        raise SystemExit(f"error: no Java sources under {JAVA_SRC_ROOT}")
    rc = subprocess.call([str(find_tool("javac")), "--release", "11",
                          "-Xlint:-cast", "-d", str(classes)] + srcs)
    if rc != 0:
        raise SystemExit("error: javac failed on the scripting classes")
    return classes


def jvm_library() -> Path:
    home = os.environ.get("JAVA_HOME")
    if not home:
        raise SystemExit("error: JAVA_HOME is not set")
    home_path = Path(home)
    for rel in ("bin/server/jvm.dll", "lib/server/libjvm.so",
                "lib/server/libjvm.dylib"):
        cand = home_path / rel
        if cand.is_file():
            return cand
    raise SystemExit(f"error: no JVM library under {home}")


def decode(tok: str) -> str:
    if tok.startswith("f:"):
        try:
            bits = int(tok[2:])
            return f"{tok}  (= {struct.unpack('<d', struct.pack('<Q', bits))[0]!r})"
        except (ValueError, struct.error):
            return tok
    return tok


def explain(line: str) -> str:
    return " ".join(decode(t) for t in line.split("|"))


def run_side(driver: Path, language: str, out: Path, scripts: Path,
             classes: Path | None, jvm: Path | None,
             env: dict, jvm_options: list[str] | None = None) -> tuple[int, str]:
    cmd = [str(driver), "--language", language, "--out", str(out),
           "--scripts", str(scripts)]
    if classes is not None:
        cmd += ["--classpath", str(classes)]
    if jvm is not None:
        cmd += ["--jvm", str(jvm)]
    for opt in (jvm_options or []):
        cmd += ["--jvm-option", opt]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def liveness(name: str, lines: list[str], slots: list[str]) -> int:
    dead = [ln for ln in lines if ln.startswith("R VMFAIL")]
    if dead:
        print(f"LIFECYCLE DIFFERENTIAL FAILED: the {name} side never made a "
              f"VM — a side that does not RUN is not a side that agrees")
        return 1

    done = [ln for ln in lines if ln.startswith("DONE ")]
    if not done:
        print(f"LIFECYCLE DIFFERENTIAL FAILED: the {name} stream has no DONE "
              f"line — it did not finish, and two truncated streams compare "
              f"equal")
        return 1
    parts = done[-1].split()
    cases, observations, host_calls = (int(parts[1]), int(parts[2]),
                                       int(parts[3]))
    # Absolute, on THIS side alone. Comparing the two sides' counts to each
    # other is satisfied by two zeroes.
    if cases == 0 or observations == 0 or host_calls == 0:
        print(f"LIFECYCLE DIFFERENTIAL FAILED: the {name} side produced "
              f"{cases} case(s), {observations} observation(s) and "
              f"{host_calls} host call(s). Nothing compares equal to nothing.")
        return 1

    exercised = {ln.split(None, 1)[1] for ln in lines if ln.startswith("SLOT ")}
    missing = [s for s in slots if s not in exercised]
    if missing:
        print(f"LIFECYCLE DIFFERENTIAL FAILED: the {name} side never exercised "
              f"{len(missing)} of the {len(slots)} JceScriptVM slot(s): "
              f"{', '.join(missing)}")
        return 1
    unknown = sorted(exercised - set(slots))
    if unknown:
        print(f"LIFECYCLE DIFFERENTIAL FAILED: the {name} side reported "
              f"slot(s) that are not in struct JceScriptVM: "
              f"{', '.join(unknown)}")
        return 1
    return 0


def compare(a: list[str], b: list[str]) -> int:
    case = "(before the first case)"
    for i in range(max(len(a), len(b))):
        la = a[i] if i < len(a) else "<end of stream>"
        lb = b[i] if i < len(b) else "<end of stream>"
        if la.startswith("CASE "):
            case = la
        if la != lb:
            print("LIFECYCLE DIFFERENTIAL FAILED")
            print(f"    case  {case}")
            print(f"    line  {i + 1}")
            print(f"    lua   {explain(la)}")
            print(f"    java  {explain(lb)}")
            return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--driver", required=True)
    ap.add_argument("--scripts", required=True)
    ap.add_argument("--work-dir", required=True)
    args = ap.parse_args()

    driver = Path(args.driver).resolve()
    scripts = Path(args.scripts).resolve()
    work = Path(args.work_dir).resolve()
    work.mkdir(parents=True, exist_ok=True)

    lua_stream = work / "lua.stream"
    java_stream = work / "java.stream"
    for p in (lua_stream, java_stream):
        if p.exists():
            p.unlink()

    slots = vtable_slots()
    classes = compile_vm_classes(work)
    jvm = jvm_library()

    env = dict(os.environ)
    # The JVM library is opened by ABSOLUTE path, but its own dependencies live
    # beside it and are found by the OS loader's rules, not by ours.
    for extra in (jvm.parent, jvm.parent.parent):
        env["PATH"] = str(extra) + os.pathsep + env.get("PATH", "")
        env["LD_LIBRARY_PATH"] = (str(extra) + os.pathsep
                                  + env.get("LD_LIBRARY_PATH", ""))

    rc, output = run_side(driver, "lua", lua_stream, scripts, None, None, env)
    if rc != 0 or not lua_stream.is_file():
        print(f"LIFECYCLE DIFFERENTIAL FAILED: the Lua side exited {rc}")
        print(output)
        return 1

    rc, output = run_side(driver, "java", java_stream, scripts, classes, jvm,
                          env, ["-Xcheck:jni"])
    if rc != 0 or not java_stream.is_file():
        print(f"LIFECYCLE DIFFERENTIAL FAILED: the Java side exited {rc}")
        print(output)
        return 1

    # -Xcheck:jni IS PART OF THE ACCEPTANCE, FOR WHAT IT ACTUALLY CHECKS.  It is
    # not the local-reference oracle — measured on JDK 21.0.9, 200,000 leaked
    # references inside one native frame produce no warning at all — but it does
    # catch reference MISUSE (a stale ref, a double delete) and argument-type
    # mistakes, and it aborts the JVM when it finds one.  Reading its output is
    # what makes it a checker rather than a decoration.
    complaints = _JNI_COMPLAINT.findall(output)
    if complaints:
        print(f"LIFECYCLE DIFFERENTIAL FAILED: -Xcheck:jni reported "
              f"{len(complaints)} problem(s) in the Java backend")
        print(output)
        return 1

    a = lua_stream.read_text(encoding="utf-8").splitlines()
    b = java_stream.read_text(encoding="utf-8").splitlines()

    for name, stream in (("lua", a), ("java", b)):
        problem = liveness(name, stream, slots)
        if problem:
            return problem

    if compare(a, b):
        return 1

    cases = sum(1 for ln in a if ln.startswith("CASE "))
    traced = sum(1 for ln in a if ln.startswith("T "))
    observed = sum(1 for ln in a if ln.startswith("R "))
    print(f"java lifecycle differential: OK — {cases} cases identical across "
          f"Lua and Java ({observed} typed observations, {traced} host-call "
          f"trace lines, all {len(slots)} JceScriptVM slots exercised on both "
          f"sides)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
