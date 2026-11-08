#!/usr/bin/env python3
"""A1 XP probe owner: isolated LLVM-MinGW-XP build, PE/import audit and receipt.

This deliberately does not configure Conan, JCE's root CMake, the public SDK,
Editor or graphics.  A successful probe establishes only XP OS/C++ runtime
capability, including a real first-party JCE entropy source translation unit.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[2]
ARCH = {
    "x64": {"triple": "x86_64-w64-mingw32", "machine": 0x8664,
            "os": (5, 2), "header": "windows-x64-xp-llvm.cmake"},
    "x86": {"triple": "i686-w64-mingw32", "machine": 0x014c,
            "os": (5, 1), "header": "windows-x86-xp-llvm.cmake"},
}
# An explicit small *probe* allowlist, not an assertion these DLLs are always
# present or that all their imported functions existed on XP.
PROBE_DLLS = frozenset({
    "msvcrt.dll", "kernel32.dll", "advapi32.dll", "ntdll.dll", "user32.dll",
    "gdi32.dll", "shell32.dll", "ole32.dll", "ws2_32.dll", "winmm.dll",
    "secur32.dll", "oleaut32.dll", "psapi.dll",
})
# Known post-XP Kernel32 exports; the list is NOT a complete API database.
POST_XP_IMPORTS = frozenset({
    "GetTickCount64", "InitializeCriticalSectionEx", "GetLargePageMinimum",
    "GetThreadGroupAffinity", "GetLogicalProcessorInformationEx",
    "GetSystemTimePreciseAsFileTime", "SetThreadDescription",
    "GetThreadDescription", "GetThreadInformation", "CreateFile2",
    "GetCurrentProcessorNumber", "InitializeConditionVariable",
    "SleepConditionVariableCS", "SleepConditionVariableSRW",
    "InitializeSRWLock", "AcquireSRWLockExclusive", "ReleaseSRWLockExclusive",
    "CancelIoEx", "SetFileInformationByHandle", "GetFinalPathNameByHandleW",
})


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _u16(raw: bytes, off: int) -> int:
    return struct.unpack_from("<H", raw, off)[0]


def _u32(raw: bytes, off: int) -> int:
    return struct.unpack_from("<I", raw, off)[0]


def _bounded(raw: bytes, off: int, size: int) -> None:
    if off < 0 or size < 0 or off + size > len(raw):
        raise ValueError("PE structure extends beyond file")


def audit_pe(path: Path, arch: str) -> dict:
    """Read PE headers, normal imports and delay directory; reject unproved imports.

    No external packages or dumpbin are required.  It is a *static* gate;
    actual XP execution is the only runtime compatibility verification.
    """
    if arch not in ARCH:
        raise ValueError(f"unknown arch {arch!r}")
    raw = path.read_bytes()
    _bounded(raw, 0, 64)
    if raw[:2] != b"MZ":
        raise ValueError("not a DOS/PE executable")
    pe = _u32(raw, 0x3c)
    _bounded(raw, pe, 24)
    if raw[pe:pe + 4] != b"PE\x00\x00":
        raise ValueError("missing PE signature")
    machine, nsec = struct.unpack_from("<HH", raw, pe + 4)
    if machine != ARCH[arch]["machine"]:
        raise ValueError(f"PE machine 0x{machine:04x} not expected for {arch}")
    opt_size = _u16(raw, pe + 20)
    opt = pe + 24
    _bounded(raw, opt, opt_size)
    magic = _u16(raw, opt)
    if magic not in (0x10b, 0x20b):
        raise ValueError("unknown PE optional header type")
    if (arch == "x64") != (magic == 0x20b):
        raise ValueError("PE optional header / CPU arch mismatch")
    os_version = struct.unpack_from("<HH", raw, opt + 40)
    subsystem_version = struct.unpack_from("<HH", raw, opt + 48)
    expected = ARCH[arch]["os"]
    if os_version != expected or subsystem_version != expected:
        raise ValueError(f"PE OS {os_version}, subsystem {subsystem_version}; expected {expected}")
    data_start = opt + (112 if magic == 0x20b else 96)
    nrva_offset = opt + (108 if magic == 0x20b else 92)
    if _u32(raw, nrva_offset) < 14 or data_start + 14 * 8 > opt + opt_size:
        raise ValueError("PE data directories incomplete (delay imports unauditable)")
    sections = []
    shoff = opt + opt_size
    _bounded(raw, shoff, nsec * 40)
    for s in range(nsec):
        so = shoff + s * 40
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", raw, so + 8)
        sections.append((va, max(vsize, rsize), rptr, rsize))

    def rva_off(rva: int, count: int = 1) -> int:
        for va, span, ptr, rsize in sections:
            if va <= rva and rva + count <= va + span:
                delta = rva - va
                if delta + count > rsize:
                    break
                result = ptr + delta
                _bounded(raw, result, count)
                return result
        raise ValueError(f"PE RVA 0x{rva:x} not backed by raw section data")

    def cstring(rva: int) -> str:
        p = rva_off(rva)
        end = raw.find(b"\x00", p, min(p + 1024, len(raw)))
        if end < 0:
            raise ValueError("unterminated PE import string")
        return raw[p:end].decode("ascii", "strict")

    import_rva, import_size = struct.unpack_from("<II", raw, data_start + 8)
    delay_rva, delay_size = struct.unpack_from("<II", raw, data_start + 13 * 8)
    if delay_rva or delay_size:
        raise ValueError("delay imports exist and are not audited by the A1 probe")
    if not import_rva or not import_size:
        raise ValueError("no normal PE imports; cannot verify MSVCRT")
    imports = {}
    max_desc = min(import_size // 20 + 2, 4096)
    for i in range(max_desc):
        desc = rva_off(import_rva + i * 20, 20)
        first_thunk, _time, _fwd, name_rva, iat_rva = struct.unpack_from("<IIIII", raw, desc)
        if first_thunk == 0 and name_rva == 0 and iat_rva == 0:
            break
        if not name_rva or not (first_thunk or iat_rva):
            raise ValueError("malformed PE import descriptor")
        dll = cstring(name_rva).lower()
        if dll not in PROBE_DLLS:
            raise ValueError(f"unexpected XP probe DLL {dll!r} (not approved for this limited gate)")
        items = imports.setdefault(dll, [])
        thunk_rva = first_thunk or iat_rva
        width = 8 if magic == 0x20b else 4
        mask = (1 << (width * 8 - 1))
        for j in range(8192):
            toff = rva_off(thunk_rva + j * width, width)
            value = struct.unpack_from("<Q" if width == 8 else "<I", raw, toff)[0]
            if value == 0:
                break
            if value & mask:
                raise ValueError(f"ordinal import from {dll} is unverified")
            sym = cstring(value + 2)
            if sym in POST_XP_IMPORTS or (dll == "kernel32.dll" and sym.startswith("K32")):
                raise ValueError(f"known post-XP API import {dll}!{sym}")
            # The x86 static unwind runtime enumerates its modules. This named
            # PSAPI entry exists on XP; accepting its DLL does not admit newer
            # exports such as EnumProcessModulesEx (Vista), or K32 aliases (7).
            if dll == "psapi.dll" and sym != "EnumProcessModules":
                raise ValueError(f"unverified XP probe PSAPI import {sym}")
            items.append(sym)
        else:
            raise ValueError("unbounded PE thunk list")
    else:
        raise ValueError("unbounded PE import directory")
    if "msvcrt.dll" not in imports:
        raise ValueError("MSVCRT not present; XP CRT target is not verified")
    if "advapi32.dll" not in imports or "CryptGenRandom" not in imports["advapi32.dll"]:
        raise ValueError("JCE XP entropy adapter must import CryptoAPI from advapi32.dll")
    return {
        "machine": f"0x{machine:04x}", "os_version": ".".join(map(str, os_version)),
        "subsystem_version": ".".join(map(str, subsystem_version)),
        "imports": {k: sorted(v) for k, v in sorted(imports.items())},
        "import_audit_scope": "Known post-XP symbols + restricted DLLs, not a full XP export database",
    }


def _execute(command: list[str], cwd: Path | None = None, input_data: str | None = None):
    print("[A1]", " ".join('"' + x + '"' if ' ' in x else x for x in command), flush=True)
    return subprocess.run(command, cwd=str(cwd) if cwd else None,
                          text=True, input=input_data, capture_output=True,
                          check=True, timeout=600,
                          creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


def preflight(root: Path, arch: str) -> dict:
    """Hard fail on unexpected compiler/target/config. Never fall back to MSVC."""
    expected = ARCH[arch]
    if "msvcrt" not in root.name.lower():
        raise ValueError("toolchain directory does not identify MSVCRT; refusing ambiguous CRT")
    binpath = root / "bin"
    cc = binpath / (expected["triple"] + "-clang.exe")
    cxx = binpath / (expected["triple"] + "-clang++.exe")
    cfg = binpath / ("x86_64-w64-windows-gnu.cfg" if arch == "x64"
                     else "i686-w64-windows-gnu.cfg")
    if not (cc.is_file() and cxx.is_file() and cfg.is_file()):
        raise ValueError(f"missing XP compiler or target configuration in {binpath}")
    ver = _execute([str(cxx), "--version"]).stdout
    if not re.search(r"clang version 22\.1\.7(?:\D|$)", ver):
        raise ValueError("expected verified clang 22.1.7, got: " + ver[:250])
    target = "x86_64-w64-windows-gnu" if arch == "x64" else "i686-w64-windows-gnu"
    if f"Target: {target}" not in ver:
        raise ValueError("compiler targets wrong ABI/arch: " + ver[:250])
    macros = _execute([str(cc), "-dM", "-E", "-x", "c", "-"], input_data="\n").stdout
    for macro in ("__clang__", "__MINGW32__", "_WIN32"):
        if not re.search(rf"^#define {macro}(?:\s|$)", macros, re.M):
            raise ValueError(f"compiler missing required macro {macro}")
    if arch == "x64" and "#define _WIN64" not in macros:
        raise ValueError("x64 compiler does not define _WIN64")
    if arch == "x86" and "#define _WIN64" in macros:
        raise ValueError("x86 compiler unexpectedly defines _WIN64")
    return {"clang_version": ver.splitlines()[0], "target": target,
            "clang_c_sha256": sha256_file(cc),
            "clang_cxx_sha256": sha256_file(cxx),
            "target_config_sha256": sha256_file(cfg),
            "toolchain_root": str(root)}


def build(repo: Path, arch: str, toolchain_root: Path, jobs: int = 1) -> Path:
    if not isinstance(jobs, int) or isinstance(jobs, bool) or not 1 <= jobs <= 256:
        raise ValueError("jobs must be an integer from 1 through 256")
    info = preflight(toolchain_root, arch)
    if not shutil.which("cmake") or not shutil.which("ninja"):
        raise ValueError("cmake and ninja must both be present on PATH")
    out = repo / "build" / "compat" / f"windows-{arch}-xp-llvm-msvcrt"
    source = repo / "tests" / "compatibility" / "xp_core"
    tc = repo / "cmake" / "toolchains" / ARCH[arch]["header"]
    out.mkdir(parents=True, exist_ok=True)
    # Existing CMake cache belongs to one compiler/toolchain/CRT only.
    # Source tree can be reused; stale build tree never silently switched.
    cache = out / "CMakeCache.txt"
    if cache.exists():
        body = cache.read_text(encoding="utf-8", errors="replace")
        expected_path = str(toolchain_root).replace("\\", "/")
        if expected_path.lower() not in body.replace("\\", "/").lower():
            raise ValueError(f"cached toolchain differs at {out}; refuse implicit reuse")
    _execute(["cmake", "-S", str(source), "-B", str(out), "-G", "Ninja",
              "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_TOOLCHAIN_FILE={tc}",
              f"-DJCE_XP_TOOLCHAIN_ROOT={toolchain_root}"])
    _execute(["cmake", "--build", str(out), "--config", "Release", "--parallel", str(jobs)])
    exe = out / "out" / "jce_xp_a1_probe.exe"
    if not exe.is_file():
        raise ValueError("compiler returned success but probe executable is missing")
    pe = audit_pe(exe, arch)
    receipt = {
        "schema": "jce.xp.a1.build-receipt.v1", "state": "LOCAL_BUILD_AND_STATIC_PE_AUDIT_PASSED",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "arch": arch, "min_nt": ".".join(map(str, ARCH[arch]["os"])),
        "toolchain": info,
        "executable": str(exe), "sha256": sha256_file(exe),
        "pe": pe,
        "not_verified": ["execution on XP guest", "complete JCE runtime", "GPU/Editor",
                         "same SHA256 across Windows OS versions"],
    }
    receipt_path = out / "jce_xp_a1_build_receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"[A1] PE static audit PASS: {exe}")
    print(f"[A1] SHA-256: {receipt['sha256']}")
    print(f"[A1] Receipt: {receipt_path}")
    print("[A1] XP VM execution remains UNVERIFIED until run on the guest.")
    return exe


def resolve_toolchain_root(repo: Path, configured: Path | None = None) -> Path:
    selected = configured or os.environ.get("JCE_XP_TOOLCHAIN_ROOT")
    return (Path(selected) if selected else
            repo.parent / "llvm-mingw-xp-22.1.7-msvcrt-x86_64").resolve()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--arch", choices=ARCH, default="x64")
    parser.add_argument("--toolchain-root", type=Path)
    parser.add_argument("--jobs", type=int, default=1, help="compiler budget (standalone default: 1)")
    args = parser.parse_args(argv)
    root = args.repo.resolve()
    if not (root / "engine/src/os/platform/jce_entropy.c").is_file():
        parser.error("not a JCE source tree")
    try:
        build(root, args.arch, resolve_toolchain_root(root, args.toolchain_root), args.jobs)
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        if isinstance(exc, subprocess.CalledProcessError):
            if exc.stdout: print(exc.stdout[-16000:])
            if exc.stderr: print(exc.stderr[-16000:], file=sys.stderr)
        print(f"[A1] FAILURE: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
