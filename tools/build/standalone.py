"""Native single-EXE editor build: static CRT, embedded assets, no SDK payload."""
from __future__ import annotations

import ctypes
import hashlib
import os
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

# Windows-owned DLLs are the only dependencies allowed in the delivered EXE.
SYSTEM_DLLS = set("""advapi32 bcrypt cabinet cfgmgr32 comctl32 comdlg32 crypt32
    cryptbase d3d11 d3d12 d3dcompiler_47 dbghelp dnsapi dwmapi dwrite dxgi gdi32
    gdiplus hid imm32 iphlpapi kernel32 kernelbase mpr msimg32 ncrypt netapi32
    normaliz ntdll ole32 oleaut32 opengl32 powrprof psapi rpcrt4 secur32 setupapi
    shell32 shlwapi user32 userenv usp10 uxtheme version winhttp wininet winmm
    winspool wintrust wldap32 ws2_32 wsock32 wtsapi32""".split())


def pe_imports(exe: Path) -> list[str]:
    """Read ordinary and delayed PE imports without executing the artifact."""
    blob = exe.read_bytes()
    def unpack(fmt, offset):
        try:
            return struct.unpack_from(fmt, blob, offset)
        except struct.error as exc:
            raise ValueError("truncated PE image") from exc
    if blob[:2] != b"MZ":
        raise ValueError("not a PE executable")
    pe, = unpack("<I", 0x3c)
    if blob[pe:pe + 4] != b"PE\0\0":
        raise ValueError("invalid PE signature")
    machine, sections = unpack("<HH", pe + 4)
    optional_size, = unpack("<H", pe + 20)
    optional = pe + 24
    magic, = unpack("<H", optional)
    if (machine, magic) not in {(0x8664, 0x20b), (0xaa64, 0x20b), (0x14c, 0x10b)}:
        raise ValueError("unsupported PE architecture")
    data_dir = optional + (112 if magic == 0x20b else 96)
    image_base, = unpack("<Q" if magic == 0x20b else "<I", optional + (24 if magic == 0x20b else 28))
    directory_count, = unpack("<I", data_dir - 4)
    section_table = optional + optional_size
    ranges = []
    for index in range(sections):
        virtual_size, address, raw_size, raw = unpack("<IIII", section_table + index * 40 + 8)
        ranges.append((address, max(virtual_size, raw_size), raw, raw_size))
    def rva_offset(address):
        for virtual, size, raw, raw_size in ranges:
            if virtual <= address < virtual + size and address - virtual < raw_size:
                offset = raw + address - virtual
                if offset < len(blob):
                    return offset
        raise ValueError("PE import RVA outside mapped file sections")
    names = []
    for directory, width in ((1, 20), (13, 32)):
        if directory_count <= directory:
            continue
        address, size = unpack("<II", data_dir + directory * 8)
        if not address:
            continue
        pos = rva_offset(address)
        end = min(pos + size, len(blob))
        while pos + width <= end:
            row = unpack("<" + "I" * (width // 4), pos)
            if not any(row):
                break
            name_rva = row[3] if directory == 1 else row[1]
            if directory == 13 and not row[0] & 1:
                name_rva -= image_base
            name_offset = rva_offset(name_rva)
            zero = blob.find(b"\0", name_offset, name_offset + 260)
            if zero < 0:
                raise ValueError("unterminated PE import name")
            names.append(blob[name_offset:zero].decode("ascii").lower())
            pos += width
        else:
            raise ValueError("unterminated PE import table")
    return sorted(set(names))


def verify(exe: Path) -> dict:
    imports = pe_imports(exe)
    if not imports:
        raise ValueError("PE artifact has no imports; cannot verify delivery")
    forbidden = [name for name in imports if not
                 (name.endswith(".dll") and name[:-4] in SYSTEM_DLLS or
                  name.startswith(("api-ms-win-core-", "ext-ms-win-")))]
    if forbidden:
        raise ValueError("single EXE still requires external DLLs: " + ", ".join(forbidden))
    return {"delivery": "native-single-exe", "exe_bytes": exe.stat().st_size,
            "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "imports": imports,
            "languages": ["lua", "c", "cpp", "js"],
            "embedded_vms": ["lua", "js"],
            "native_script_abi": ["c", "cpp"],
            "project_modules_embedded": False,
            "external_language_build": ["python", "java", "csharp"],
            "sdk_embedded": False, "self_extracting": False}


def verify_assets(exe: Path, build: Path) -> dict:
    """Prove that the verified editor asset pack is byte-for-byte in the EXE."""
    pack = build / "editor_assets.pak"
    bom = build / "reports/editor_assets_bom.json"
    if not pack.is_file() or not bom.is_file():
        raise ValueError("standalone editor asset pack or inventory is missing")
    payload = pack.read_bytes()
    inventory = json.loads(bom.read_text(encoding="utf-8"))
    entries = inventory.get("entries", [])
    totals = inventory.get("totals", {})
    if (not entries or inventory.get("file_size") != len(payload)
            or totals.get("entries") != len(entries)
            or totals.get("verified_count") != len(entries)
            or totals.get("corrupt_count") != 0
            or totals.get("original_size") != sum(entry.get("original_size", 0) for entry in entries)
            or totals.get("stored_size") != sum(entry.get("stored_size", 0) for entry in entries)
            or any(not entry.get("verified") for entry in entries)):
        raise ValueError("standalone editor asset inventory is incomplete")
    if exe.read_bytes().find(payload) < 0:
        raise ValueError("verified editor asset pack is not embedded in the EXE")
    groups = {}
    for entry in entries:
        group = entry["path"].split("/", 1)[0] if "/" in entry["path"] else "root"
        item = groups.setdefault(group, {"entries": 0, "original_bytes": 0,
                                         "stored_bytes": 0})
        item["entries"] += 1
        item["original_bytes"] += entry["original_size"]
        item["stored_bytes"] += entry["stored_size"]
    return {"entries": len(entries), "pack_bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "groups": groups}


def verify_notices(exe: Path, source_root: Path) -> dict:
    """Read RCDATA as data only; never execute the editor during validation."""
    if os.name != "nt":
        raise ValueError("native resource verification requires Windows")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    signatures = {
        "LoadLibraryExW": ([ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32], ctypes.c_void_p),
        "FindResourceW": ([ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_void_p], ctypes.c_void_p),
        "SizeofResource": ([ctypes.c_void_p, ctypes.c_void_p], ctypes.c_uint32),
        "LoadResource": ([ctypes.c_void_p, ctypes.c_void_p], ctypes.c_void_p),
        "LockResource": ([ctypes.c_void_p], ctypes.c_void_p),
        "FreeLibrary": ([ctypes.c_void_p], ctypes.c_int),
    }
    for name, (args, result) in signatures.items():
        function = getattr(kernel, name)
        function.argtypes, function.restype = args, result
    module = kernel.LoadLibraryExW(str(exe.resolve()), None, 0x22)
    if not module:
        raise ValueError("cannot read native editor resources")
    notices = {}
    try:
        for name, file in (("JCE_LICENSE", "LICENSE"),
                           ("JCE_THIRD_PARTY_LICENSES", "THIRD_PARTY_LICENSES.md")):
            resource = kernel.FindResourceW(module, name, ctypes.c_void_p(10))
            size = kernel.SizeofResource(module, resource) if resource else 0
            data = kernel.LoadResource(module, resource) if size else None
            pointer = kernel.LockResource(data) if data else None
            if not pointer:
                raise ValueError("single EXE is missing embedded notice: " + file)
            contents = ctypes.string_at(pointer, size)
            if contents != (source_root / file).read_bytes():
                raise ValueError("embedded notice differs from source: " + file)
            notices[file] = hashlib.sha256(contents).hexdigest()
    finally:
        kernel.FreeLibrary(module)
    return notices


def source_identity(driver) -> dict:
    cmake = (driver.ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(JCE VERSION (\d+\.\d+\.\d+) LANGUAGES C\)", cmake)
    if not match:
        raise ValueError("standalone release has no authoritative JCE version")
    result = subprocess.run(["git", "-C", str(driver.ROOT), "diff", "--quiet", "HEAD", "--"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return {"jce_version": match.group(1),
            "source_commit": driver.git_short_sha(),
            "source_modified": result.returncode == 1,
            "source_state_known": result.returncode in (0, 1)}


def build_editor(driver, args, target: dict, env: dict) -> Path:
    """Isolated static dependency closure; never relabel a dynamic-CRT build."""
    if target["host"] != "windows" or driver.HOST != "windows":
        driver.die("native standalone editor currently requires Windows/MSVC")
    if args.variant != "dist":
        driver.die("standalone editor uses --variant dist (Release, no Tracy)")
    stem = target["key"] + "-standalone"
    build = driver.ROOT / "build/desktop" / stem
    conan = driver.ROOT / "build/desktop" / (stem + "-conan")
    toolchain = conan / "build/Release/generators/conan_toolchain.cmake"
    if getattr(args, "clean", False):
        for path in (build, conan):
            if not path.resolve().is_relative_to((driver.ROOT / "build/desktop").resolve()):
                raise ValueError("standalone clean target escaped build/desktop")
            if path.exists() and not driver.DRY_RUN:
                shutil.rmtree(path)
    driver.sync_conan_hooks()
    driver.export_conan_recipes(env)
    vs = driver.find_vs_install(target["arch"])
    flags = ["-c", f"tools.microsoft.msbuild:installation_path={vs}"] if vs else []
    driver.run(["conan", "install", ".", "-pr:h", f"conan/profiles/{target['profile']}",
                "-pr:b", f"conan/profiles/{driver.host_build_profile()}",
                "-s:h", "compiler.runtime=static", "-o:h", "*:shared=False",
                "--build=missing", "--output-folder", str(conan),
                *driver.bgfx_graphics_conan_args(env), *flags], cwd=driver.ROOT, env=env)
    if not driver.DRY_RUN:
        (driver.ROOT / "CMakeUserPresets.json").unlink(missing_ok=True)
    driver.run(["cmake", "-S", str(driver.ROOT), "-B", str(build), "-G", "Ninja",
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
                "-DJCE_BUILD_VARIANT=dist", "-DJCE_EDITOR_STANDALONE=ON",
                "-DJCE_BUILD_SCRIPT_VM_PYTHON=OFF", "-DJCE_BUILD_SCRIPT_CSHARP=OFF",
                "-DJCE_BUILD_SCRIPT_JAVA=OFF", "-DJCE_ENABLE_SDK_INSTALL=OFF",
                "-DJCE_ENABLE_AI_DISPATCH=OFF", "-DJCE_ENABLE_RENDERDOC=OFF",
                "-DJCE_ENABLE_CPPCHECK=OFF", "-DJCE_BUILD_TESTS=OFF", "-DJCE_EMIT_LINK_MAP=ON"],
               cwd=driver.ROOT, env=env)
    driver.run(["cmake", "--build", str(build), "--target", "JCE_Editor", "-j", driver.build_jobs()],
               cwd=driver.ROOT, env=env)
    exe = build / "dist/jce_editor.exe"
    if not driver.DRY_RUN:
        driver._assert_no_profiler_listener(exe, "dist")
        report = verify(exe)
        report["embedded_assets"] = verify_assets(exe, build)
        report["embedded_notices"] = verify_notices(exe, driver.ROOT)
        report.update(source_identity(driver))
        (build / "standalone.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    driver.log(f"native standalone editor -> {exe}")
    return exe


def package_editor(driver, args) -> None:
    target = driver.resolve_target(args.arch)
    if target["host"] != "windows" or driver.HOST != "windows" or args.variant != "dist":
        driver.die("standalone editor packaging requires Windows --variant dist")
    if getattr(args, "python_runtime", None):
        driver.die("--python-runtime is for the full language bundle, not the native standalone editor")
    env = driver.msvc_env(target)
    exe = driver.ROOT / "build/desktop" / (target["key"] + "-standalone") / "dist/jce_editor.exe"
    if not args.skip_build:
        exe = build_editor(driver, args, target, env)
    out = Path(args.out).resolve() if args.out else driver.ROOT / "dist/editor" / ("JCE-editor-" + target["key"] + ".exe")
    if out.suffix.lower() != ".exe":
        driver.die("--standalone --out must name an EXE, not a bundle directory")
    if driver.DRY_RUN:
        driver.log(f"native single EXE -> {out}")
        return
    report = verify(exe)
    report["embedded_assets"] = verify_assets(exe, exe.parents[1])
    report["embedded_notices"] = verify_notices(exe, driver.ROOT)
    driver._assert_no_profiler_listener(exe, "dist")
    report.update(source_identity(driver))
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(exe, out)
    out.with_suffix(".exe.sha256").write_text(report["sha256"] + "  " + out.name + "\n", encoding="ascii")
    out.with_suffix(".manifest.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    driver.log(f"native single EXE -> {out}; {report['exe_bytes']} bytes; SDK separate")
