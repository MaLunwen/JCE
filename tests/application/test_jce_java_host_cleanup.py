#!/usr/bin/env python3
"""Run complete shipping Java host sources with typed native/bootstrap ports.

No production lifecycle method is extracted or copied. Native loading is the
only replaced static block; six native declarations become counted Java ports.
The real JNI adapter has an independent complete-source C fixture. Outputs stay
under the caller's reusable build cache; missing JDK returns explicit skip 77.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def replace_one(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise ValueError(f"expected exactly one actual host wire: {old[:100]!r}")
    return source.replace(old, new)


def source_pair(variant: str) -> tuple[str, str]:
    runtime = (ROOT / "engine/java/com/jce/JceRuntime.java").read_text(encoding="utf-8")
    main = (ROOT / "engine/java/com/jce/Main.java").read_text(encoding="utf-8")
    runtime = replace_one(runtime, """        loadNativeLibrary();
        verifyApiVersion();""", "        /* CPU control: native load/ABI handshake observed separately. */")
    declarations = {
        "int nativeApiVersion()": "return 0x000D0400;",
        "String nativeApiVersionString()": 'return "0.13.4";',
        "long nativeCreate()": "return NativePort.create();",
        "int nativeIterate(long handle)": "return NativePort.iterate(handle);",
        "int nativeLastResult(long handle)": "return NativePort.last(handle);",
        "void nativeDestroy(long handle)": "NativePort.destroy(handle);",
    }
    for signature, body in declarations.items():
        runtime = replace_one(runtime, f"private static native {signature};",
                              f"private static {signature} {{ {body} }}")
    if variant == "old_java_clear":
        runtime = replace_one(runtime, """            nativeDestroy(nativeHandle);
            nativeHandle = 0L;""", """            long pending = nativeHandle;
            nativeHandle = 0L;
            nativeDestroy(pending);""")
    elif variant == "old_main_abandon":
        main = replace_one(main, """        JceRuntime runtime = new JceRuntime();
        try {""", "        try (JceRuntime runtime = new JceRuntime()) {")
        main = replace_one(main, """        } finally {
            closeOwned(runtime);
        }""", "        }")
    return runtime, main


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--variant", choices=("current", "old_java_clear", "old_main_abandon"), default="current")
    parser.add_argument("--expect-assertion-failure", action="store_true")
    args = parser.parse_args()
    suffix = ".exe" if sys.platform == "win32" else ""
    if args.java_home:
        javac = args.java_home / "bin" / f"javac{suffix}"
        java = args.java_home / "bin" / f"java{suffix}"
    else:
        found = shutil.which(f"javac{suffix}")
        javac = Path(found) if found else Path("__unavailable_javac__")
        java = javac.parent / f"java{suffix}"
    if not javac.is_file() or not java.is_file():
        print("UNAVAILABLE: Java host ownership controls require one JDK with javac and java")
        return 77
    runtime, host_main = source_pair(args.variant)
    work = args.work_dir.resolve() / args.variant
    sources = work / "src/com/jce"
    classes = work / "classes"
    sources.mkdir(parents=True, exist_ok=True)
    classes.mkdir(parents=True, exist_ok=True)
    for name, text in (("JceRuntime.java", runtime), ("Main.java", host_main),
                       ("HostCleanupControl.java", (Path(__file__).parent / "host_cleanup_ports/HostCleanupControl.java").read_text(encoding="utf-8"))):
        output = sources / name
        if not output.exists() or output.read_text(encoding="utf-8") != text:
            output.write_text(text, encoding="utf-8")
    compile_run = subprocess.run([str(javac), "--release", "11", "-encoding", "UTF-8", "-d", str(classes),
                                  *map(str, sorted(sources.glob("*.java")))],
                                 capture_output=True, text=True, timeout=60, check=False)
    print(compile_run.stdout + compile_run.stderr, end="")
    if compile_run.returncode:
        print("FAIL: Java control did not compile; not an accepted negative result")
        return 2
    run = subprocess.run([str(java), "-cp", str(classes), "com.jce.HostCleanupControl"],
                         capture_output=True, text=True, timeout=20, check=False)
    output = run.stdout + run.stderr
    print(output, end="")
    if args.expect_assertion_failure:
        if run.returncode != 0 and "java.lang.AssertionError:" in output:
            print(f"PASS: {args.variant} rejected by ordinary ownership assertion")
            return 0
        print("FAIL: negative control did not produce an ordinary assertion failure")
        return 1
    return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
