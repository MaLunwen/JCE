#!/usr/bin/env python3
"""Compile and inspect one C99 object for a named CI target.

Scope: actual compiler/object machine ID, not a JCE native build or execution.
The source is intentionally freestanding: no C runtime header or link is needed.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).with_suffix(".c")
# (compiler kind, output format, expected machine, pointer width)
TARGETS = {
    "windows-x86": ("msvc", "coff", 0x014C, 4),
    "windows-x64": ("msvc", "coff", 0x8664, 8),
    "windows-arm64": ("msvc", "coff", 0xAA64, 8),
    "linux-x86": ("linux32", "elf", 3, 4),
    "linux-x64": ("linux", "elf", 62, 8),
    "linux-arm64": ("linux", "elf", 183, 8),
    "macos-x64": ("macos-x64", "macho", 0x01000007, 8),
    "macos-arm64": ("macos-arm64", "macho", 0x0100000C, 8),
    "ios-arm64-device": ("ios", "macho", 0x0100000C, 8),
    "android-armv7": ("android-armv7", "elf", 40, 4),
    "android-arm64": ("android-arm64", "elf", 183, 8),
    "wasm32": ("wasm", "wasm", 1, 4),
}


def inspect_object(data: bytes) -> tuple[str, int, int]:
    """Return (format, machine, pointer-width); reject unknown/invalid headers."""
    if len(data) >= 20 and data[:4] == b"\x7fELF":
        if data[5] != 1 or data[4] not in (1, 2):
            raise ValueError("ELF byte order or class is unsupported")
        return "elf", struct.unpack_from("<H", data, 18)[0], (4 if data[4] == 1 else 8)
    if len(data) >= 8 and data[:4] == b"\x00asm":
        if struct.unpack_from("<I", data, 4)[0] != 1:
            raise ValueError("unexpected WebAssembly binary version")
        return "wasm", 1, 4
    if len(data) >= 8 and data[:4] in (b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe"):
        width = 8 if data[:4] == b"\xcf\xfa\xed\xfe" else 4
        return "macho", struct.unpack_from("<I", data, 4)[0], width
    # Regular COFF .obj: IMAGE_FILE_HEADER starts with 16-bit Machine.
    if len(data) >= 20:
        machine, nsections, _, symptr, _, optsize, _ = struct.unpack_from("<HHIIIHH", data, 0)
        if machine in (0x014C, 0x8664, 0xAA64) and nsections >= 1 and optsize == 0:
            return "coff", machine, (4 if machine == 0x014C else 8)
    raise ValueError("not a recognized ELF, Mach-O, COFF or wasm object")


def ndk_clang() -> Path:
    roots = [os.environ.get(name, "") for name in
             ("ANDROID_NDK_LATEST_HOME", "ANDROID_NDK_HOME", "ANDROID_NDK_ROOT")]
    sdk = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if sdk:
        ndks = Path(sdk) / "ndk"
        if ndks.is_dir():
            roots.extend(str(p) for p in sorted(ndks.iterdir(), reverse=True) if p.is_dir())
    for root in roots:
        if root:
            for host in ("linux-x86_64", "darwin-x86_64", "darwin-arm64"):
                binary = Path(root) / "toolchains" / "llvm" / "prebuilt" / host / "bin" / "clang"
                if binary.is_file():
                    return binary
    raise RuntimeError("Android NDK clang not found; install NDK in ANDROID_HOME/ndk")


def compile_command(target: str, output: Path) -> list[str]:
    kind, _, _, width = TARGETS[target]
    c = str(SOURCE)
    define = f"JCE_CI_EXPECT_POINTER_BYTES={width}"
    if kind == "msvc":
        return ["cl", "/nologo", "/W4", "/TC", "/c", "/D" + define,
                "/Fo" + str(output), c]
    if kind.startswith("linux"):
        return ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-c",
                "-m32" if kind == "linux32" else "-m64", "-D" + define,
                "-o", str(output), c] if kind != "linux" or target == "linux-x64" else [
                "cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-c",
                "-D" + define, "-o", str(output), c]
    if kind.startswith("macos"):
        arch = "x86_64" if kind == "macos-x64" else "arm64"
        return ["clang", "-std=c99", "-Wall", "-Wextra", "-Werror", "-arch", arch,
                "-D" + define, "-c", "-o", str(output), c]
    if kind == "ios":
        return ["xcrun", "--sdk", "iphoneos", "clang", "-std=c99", "-arch", "arm64",
                "-miphoneos-version-min=15.0", "-D" + define,
                "-c", "-o", str(output), c]
    if kind.startswith("android-"):
        triples = {"android-armv7": "armv7a-linux-androideabi21",
                   "android-arm64": "aarch64-linux-android21",
                   }
        return [str(ndk_clang()), "--target=" + triples[kind], "-std=c99", "-Wall",
                "-Wextra", "-Werror", "-D" + define,
                "-c", "-o", str(output), c]
    if kind == "wasm":
        return ["emcc", "-std=c99", "-D" + define, "-c", "-o", str(output), c]
    raise AssertionError("unreachable compiler selector")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=sorted(TARGETS), required=True)
    args = parser.parse_args(argv)
    if not SOURCE.is_file():
        parser.error("missing checked-in C99 probe source")
    with tempfile.TemporaryDirectory(prefix="jce-ci-arch-") as tmp:
        suffix = ".obj" if TARGETS[args.target][0] == "msvc" else ".o"
        output = Path(tmp) / ("ci_probe" + suffix)
        command = compile_command(args.target, output)
        print("toolchain:", args.target, "=>", command[0], flush=True)
        subprocess.run(command, check=True)
        if not output.is_file():
            raise RuntimeError("compiler exited successfully but created no object")
        actual = inspect_object(output.read_bytes())
        expected = TARGETS[args.target][1:]
        if actual != expected:
            raise RuntimeError(f"incorrect object machine: actual={actual!r}, expected={expected!r}")
        print(f"PASS object-only: {args.target}, {actual[0]}, machine=0x{actual[1]:x}, "
              f"pointer={actual[2]} bytes", flush=True)
        print("NOT VALIDATED: full JCE build, SDK smoke, emulator, hardware rendering", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as exc:
        print("FAIL:", exc, file=sys.stderr)
        sys.exit(1)
