#!/usr/bin/env python3
"""
run_differential.py — the cross-language differential's comparator.

It runs two processes over ONE recording mock host and compares their streams:

    jce_script_java_lua_reference <file>   the Lua bindings, in the engine
    java ... JceDifferential <file>        the Java surface, through the
                                           production JNI shim and the C ABI

and fails on the FIRST difference, naming the case, the line and both sides.

WHAT A DIFFERENCE MEANS.  The two streams carry, per case, the arity of the
answer, the TYPE and VALUE of every slot (so nil / 0 / false cannot pass for
each other), and the host-call trace — which member fired, in what order, with
which arguments.  A binding that pushes the right values by calling the wrong
member is invisible to a result comparison and visible in the trace.

WHAT IT CANNOT SEE, stated rather than left to be discovered:
  * a manifest decision that is wrong in the same way on both sides. Both
    emitters read one manifest, so a mis-declared shape or out_capacity is
    invisible here. The design says the same thing about `shape` and it is
    the differential's stated limit, not an oversight.
  * the seven hand-written entries. They are not on the Java surface at all.
  * an entry the manifest omits entirely: neither side has it.

-Xcheck:jni IS PART OF THE ACCEPTANCE, FOR WHAT IT ACTUALLY CHECKS.  Any
WARNING or FATAL ERROR from it fails the run — a checker whose output is not
read is a checker that cannot fail.

But it is NOT the local-reference oracle, and this docstring used to say it
was.  Measured on JDK 21.0.9: a shim mutated to leak 200,000 local references
inside ONE native frame runs all 151 cases here GREEN under -Xcheck:jni, with
no warning of any kind.  What it does catch is reference MISUSE — a double
DeleteLocalRef aborts the JVM with "FATAL ERROR in native method: Bad global
or local ref passed to JNI", which this script sees as a non-zero exit and a
stack naming the native.  Also measured.

The local-reference discipline is therefore enforced STATICALLY, over the
emitted shim, by tools/audit/tests/test_script_java_gate.py:
test_no_native_creates_more_than_one_object,
test_no_native_creates_a_reference_that_outlives_the_call, and
test_the_shim_contains_no_loop.

Numbers in the stream are IEEE-754 bit patterns, because C's %g and Java's %g
disagree about trailing zeros and that disagreement is about printf and nothing
else. This script decodes them when it prints a diff.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
JAVA_SRC_ROOT = REPO_ROOT / "scripting/java/src/main/java"
DIFF_SRC_ROOT = HERE / "src"

# What the JNI checker prints when it is unhappy. Matched case-sensitively on
# purpose: "warning" appears in ordinary JVM chatter, "WARNING:" does not.
_JNI_COMPLAINT = re.compile(r"^(WARNING|FATAL ERROR) ", re.M)


def find_tool(name: str) -> Path:
    exe = name + (".exe" if os.name == "nt" else "")
    home = os.environ.get("JAVA_HOME")
    if home and (Path(home) / "bin" / exe).is_file():
        return Path(home) / "bin" / exe
    found = shutil.which(name)
    if not found:
        raise SystemExit(f"error: {name} not found (set JAVA_HOME or PATH)")
    return Path(found)


def compile_java(work: Path) -> Path:
    classes = work / "classes"
    if classes.exists():
        shutil.rmtree(classes)
    classes.mkdir(parents=True)
    srcs = ([str(p) for p in sorted(JAVA_SRC_ROOT.rglob("*.java"))] +
            [str(p) for p in sorted(DIFF_SRC_ROOT.rglob("*.java"))])
    if not srcs:
        raise SystemExit("error: no Java sources found — regenerate with "
                         "python tools/scriptgen/gen_script_bindings.py --write")
    rc = subprocess.call([str(find_tool("javac")), "--release", "11",
                          "-Xlint:-cast", "-d", str(classes)] + srcs)
    if rc != 0:
        raise SystemExit("error: javac failed")
    return classes


def decode(tok: str) -> str:
    """A stream token, made readable. `f:` carries a double's raw bits."""
    if tok.startswith("f:"):
        try:
            bits = int(tok[2:])
            return f"{tok}  (= {struct.unpack('<d', struct.pack('<q', bits))[0]!r})"
        except (ValueError, struct.error):
            return tok
    return tok


def explain(line: str) -> str:
    return " ".join(decode(t) for t in line.split("|"))


def compare(a_name: str, a: list[str], b_name: str, b: list[str]) -> int:
    case = "(before the first case)"
    for i in range(max(len(a), len(b))):
        la = a[i] if i < len(a) else "<end of stream>"
        lb = b[i] if i < len(b) else "<end of stream>"
        if la.startswith("CASE "):
            case = la
        if la != lb:
            print("DIFFERENTIAL FAILED")
            print(f"    case  {case}")
            print(f"    line  {i + 1}")
            print(f"    {a_name:<6} {explain(la)}")
            print(f"    {b_name:<6} {explain(lb)}")
            return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lua-exe", required=True)
    ap.add_argument("--script-lib", required=True)
    ap.add_argument("--diff-lib", required=True)
    ap.add_argument("--work-dir", required=True)
    args = ap.parse_args()

    work = Path(args.work_dir).resolve()
    work.mkdir(parents=True, exist_ok=True)
    lua_stream = work / "lua.stream"
    java_stream = work / "java.stream"
    for p in (lua_stream, java_stream):
        if p.exists():
            p.unlink()

    classes = compile_java(work)

    rc = subprocess.call([args.lua_exe, str(lua_stream)])
    if rc != 0 or not lua_stream.is_file():
        print(f"DIFFERENTIAL FAILED: the Lua reference exited {rc} and wrote "
              f"{'no' if not lua_stream.is_file() else 'a'} stream")
        return 1

    env = dict(os.environ)
    # The shim's own dependency (jce_script_api) lives beside it; on Windows
    # that directory has to be reachable when the JVM loads the shim.
    libdir = str(Path(args.script_lib).parent)
    env["PATH"] = libdir + os.pathsep + env.get("PATH", "")
    env["LD_LIBRARY_PATH"] = libdir + os.pathsep + env.get("LD_LIBRARY_PATH", "")

    proc = subprocess.run(
        [str(find_tool("java")), "-Xcheck:jni",
         f"-Djce.script.library={args.script_lib}",
         f"-Djce.diff.library={args.diff_lib}",
         "-cp", str(classes),
         "com.jce.script.diff.JceDifferential", str(java_stream)],
        env=env, capture_output=True, text=True)
    jvm_output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0 or not java_stream.is_file():
        print(f"DIFFERENTIAL FAILED: the Java driver exited "
              f"{proc.returncode}")
        print(jvm_output)
        return 1

    complaints = _JNI_COMPLAINT.findall(jvm_output)
    if complaints:
        print("DIFFERENTIAL FAILED: -Xcheck:jni reported "
              f"{len(complaints)} problem(s) in the generated shim")
        print(jvm_output)
        return 1

    a = lua_stream.read_text(encoding="utf-8").split("\n")
    b = java_stream.read_text(encoding="utf-8").split("\n")

    # THE CHUNK MUST RUN — checked BEFORE the comparison, and on both streams.
    #
    # Measured, not feared. The first build of this harness emitted a Lua chunk
    # that ended at its `jce.log(probe(...))` line; jce_script.c's run_chunk()
    # hands whatever the chunk returned to build_instance(), which refuses
    # anything that is not a table, so 150 of 151 cases reported R CHUNKFAIL
    # while the binding under test had already run and its answer been thrown
    # away. Comparing first would have blamed the FIRST case's values for a
    # fault that applied to all of them; worse, a Java driver that also stopped
    # producing answers would make two useless streams compare equal.
    #
    # No case may legitimately report either state: `strict` cases are recorded
    # as R SKIPPED (they are a compile error in Java, not a runtime raise), and
    # a VM that will not open is a broken harness, never a result.
    for who, stream in (("lua", a), ("java", b)):
        dead = [ln for ln in stream
                if ln.startswith("R CHUNKFAIL") or ln.startswith("R VMFAIL")]
        if dead:
            print(f"DIFFERENTIAL FAILED: {len(dead)} of the {who} stream's "
                  f"cases never produced an answer — a case that does not RUN "
                  f"is not a case that agrees")
            print(f"    first: {dead[0]}")
            return 1

    rc = compare("lua", a, "java", b)
    if rc:
        return rc

    cases = sum(1 for ln in a if ln.startswith("CASE "))
    skipped = sum(1 for ln in a if ln.startswith("R SKIPPED"))
    traced = sum(1 for ln in a if ln.startswith("T "))
    if not a or not any(ln.startswith("DONE ") for ln in a):
        # A gate must prove it ran. Two empty streams compare equal.
        print("DIFFERENTIAL FAILED: the reference stream has no DONE line — "
              "it did not finish, and two truncated streams compare equal")
        return 1
    if traced == 0:
        # The trace is half the evidence: a binding that pushes the right
        # values by calling the wrong member is invisible to the result
        # comparison. Zero trace lines means that half was never collected,
        # and zero on both sides compares equal.
        print("DIFFERENTIAL FAILED: not one host-call trace line was "
              "recorded — the result halves may agree, but nothing checked "
              "WHICH host member each surface called")
        return 1
    print(f"java differential: OK — {cases} cases identical across Lua and "
          f"Java ({skipped} recorded as not-applicable-in-Java, "
          f"{traced} host-call trace lines matched), -Xcheck:jni clean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
