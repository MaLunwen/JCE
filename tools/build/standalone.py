"""Native single-EXE editor build: static CRT, embedded assets, no SDK payload."""
from __future__ import annotations

import ctypes
import hashlib
import importlib.util
import os
import json
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess

# Windows-owned DLLs are the only dependencies allowed in the delivered EXE.
# LLVM-MinGW may use OS MSVCRT; compiler redistributable runtimes stay excluded.
SYSTEM_DLLS = set("""advapi32 bcrypt cabinet cfgmgr32 comctl32 comdlg32 crypt32
    cryptbase d3d11 d3d12 d3dcompiler_47 dbghelp dnsapi dwmapi dwrite dxgi gdi32
    gdiplus hid imm32 iphlpapi kernel32 kernelbase mpr msimg32 msvcrt ncrypt netapi32
    normaliz ntdll ole32 oleaut32 opengl32 powrprof psapi rpcrt4 secur32 setupapi
    shell32 shlwapi user32 userenv usp10 uxtheme version winhttp wininet winmm
    winspool wintrust wldap32 ws2_32 wsock32 wtsapi32""".split())


# A bounded regression policy, not a complete Windows API-version catalog.
POST_WINDOWS7_IMPORTS = frozenset({"GetThreadDescription", "SetThreadDescription",
                                   "CreateFile2", "CopyFile2"})


def _pe_import_records(blob: bytes) -> list[dict]:
    """Read mapped ordinary/delayed DLL and function imports without execution."""
    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(blob):
            raise ValueError("truncated PE image")
        return struct.unpack_from(fmt, blob, offset)

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
    header_size = 112 if magic == 0x20b else 96
    if optional_size < header_size or optional + optional_size > len(blob):
        raise ValueError("truncated PE optional header")
    data_dir = optional + header_size
    image_base, = unpack("<Q" if magic == 0x20b else "<I", optional + (24 if magic == 0x20b else 28))
    directory_count, = unpack("<I", data_dir - 4)
    if directory_count > (optional_size - header_size) // 8:
        raise ValueError("PE data directories exceed optional header")
    section_table = optional + optional_size
    ranges = []
    for index in range(sections):
        virtual_size, address, raw_size, raw = unpack("<IIII", section_table + index * 40 + 8)
        ranges.append((address, max(virtual_size, raw_size), raw, raw_size))

    def rva_span(address, needed=1):
        for virtual, size, raw, raw_size in ranges:
            delta = address - virtual
            if 0 <= delta < size and delta + needed <= raw_size:
                offset, limit = raw + delta, min(raw + raw_size, len(blob))
                if needed >= 0 and offset >= 0 and offset + needed <= limit:
                    return offset, limit
        raise ValueError("PE import RVA outside mapped file sections")

    def text(address, bound):
        offset, limit = rva_span(address)
        zero = blob.find(b"\0", offset, min(offset + bound, limit))
        if zero < 0:
            raise ValueError("unterminated PE import name")
        value = blob[offset:zero].decode("ascii")
        if not value:
            raise ValueError("empty PE import name")
        return value

    records, total_entries = [], 0
    pointer_size = 8 if magic == 0x20b else 4
    pointer_format = "<Q" if pointer_size == 8 else "<I"
    ordinal_flag = 1 << (pointer_size * 8 - 1)
    for directory, width in ((1, 20), (13, 32)):
        if directory_count <= directory:
            continue
        address, size = unpack("<II", data_dir + directory * 8)
        if not address:
            continue
        if size < width:
            raise ValueError("truncated PE import table")
        # Descriptors and all referenced thunk/name ranges must be file-backed.
        rva_span(address, size)
        for index in range(min(size // width, 4097)):
            pos, _ = rva_span(address + index * width, width)
            row = unpack("<" + "I" * (width // 4), pos)
            if not any(row):
                break
            delayed = directory == 13
            if delayed and row[0] & ~1:
                raise ValueError("reserved PE delay-import attributes")
            bias = image_base if delayed and not row[0] & 1 else 0
            name_rva = (row[1] if delayed else row[3]) - bias
            thunk_rva = (row[4] if delayed else row[0] or row[4]) - bias
            if thunk_rva <= 0:
                raise ValueError("PE import descriptor has no lookup table")
            record = {"dll": text(name_rva, 260).lower(),
                      "kind": "delayed" if delayed else "ordinary", "symbols": []}
            for entry in range(65537):
                offset, _ = rva_span(thunk_rva + entry * pointer_size, pointer_size)
                value, = unpack(pointer_format, offset)
                if not value:
                    break
                total_entries += 1
                if total_entries > 65536:
                    raise ValueError("PE import entry budget exceeded")
                if value & ordinal_flag:
                    if value & ~(ordinal_flag | 0xffff):
                        raise ValueError("reserved PE ordinal-import bits")
                    record["symbols"].append(value & 0xffff)
                else:
                    symbol_rva = value - bias
                    rva_span(symbol_rva, 2)  # IMAGE_IMPORT_BY_NAME's hint.
                    record["symbols"].append(text(symbol_rva + 2, 4096))
            else:
                raise ValueError("unterminated PE import lookup table")
            records.append(record)
            if len(records) > 4096:
                raise ValueError("PE import descriptor budget exceeded")
        else:
            raise ValueError("unterminated PE import table")
    return records


def pe_imports(exe: Path) -> list[str]:
    """Retain the existing DLL-list interface over the shared PE parser."""
    return sorted({row["dll"] for row in _pe_import_records(exe.read_bytes())})


def verify(exe: Path, *, require_gui: bool = False) -> dict:
    blob = exe.read_bytes()
    records = _pe_import_records(blob)
    pe_offset, = struct.unpack_from("<I", blob, 0x3c)
    subsystem, = struct.unpack_from("<H", blob, pe_offset + 24 + 68)
    if require_gui and subsystem != 2:
        raise ValueError("editor delivery requires a Windows GUI executable (PE subsystem 2)")
    imports = sorted({row["dll"] for row in records})
    if not imports:
        raise ValueError("PE artifact has no imports; cannot verify delivery")
    forbidden = [name for name in imports if not
                 (name.endswith(".dll") and name[:-4] in SYSTEM_DLLS or
                  name.startswith(("api-ms-win-core-", "ext-ms-win-")))]
    if forbidden:
        raise ValueError("single EXE still requires external DLLs: " + ", ".join(forbidden))
    unsupported = sorted({row["dll"] + "!" + symbol
                          for row in records for symbol in row["symbols"]
                          if isinstance(symbol, str) and symbol in POST_WINDOWS7_IMPORTS})
    if unsupported:
        raise ValueError("Windows 7 target requires unavailable imported APIs: " + ", ".join(unsupported))
    ordinals = [{"dll": row["dll"], "kind": row["kind"], "ordinal": symbol}
                for row in records for symbol in row["symbols"] if isinstance(symbol, int)]
    return {"delivery": "native-single-exe", "exe_bytes": len(blob), "pe_subsystem": subsystem,
            "sha256": hashlib.sha256(blob).hexdigest(), "imports": imports,
            "windows_api_admission": {
                "minimum_target": "Windows 7", "coverage": "known post-Windows-7 names only",
                "checked_names": sorted(POST_WINDOWS7_IMPORTS),
                "named_import_count": sum(isinstance(symbol, str) for row in records for symbol in row["symbols"]),
                "ordinal_imports": ordinals, "ordinal_api_floor": "unknown" if ordinals else "not applicable"},
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


def sdk_outputs():
    spec = importlib.util.spec_from_file_location("jce_sdk_outputs", Path(__file__).with_name("sdk_outputs.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _cache_text(path: Path) -> str:
    with path.open("rb") as stream:
        encoded = stream.read(1048577)
    if len(encoded) > 1048576:
        raise ValueError("native cache identity exceeds 1 MiB: " + str(path))
    return encoded.decode("utf-8")


def _admit_cache(plan: dict, target: dict, *, exact_variant: bool = False) -> None:
    """Qualify existing objects before selecting or mutating their cache owner."""
    cache = plan["build"] / "CMakeCache.txt"
    if not cache.is_file():
        if plan["build"].exists() and any(plan["build"].iterdir()):
            raise ValueError("native cache is nonempty without a complete CMake identity: " + str(plan["build"]))
        if exact_variant:
            raise ValueError("native editor cache is missing: " + str(cache))
        return
    actual = {}
    for line in _cache_text(cache).splitlines():
        if line.startswith(("//", "#")) or ":" not in line or "=" not in line:
            continue
        typed, value = line.split("=", 1)
        key = typed.partition(":")[0]
        if key in actual:
            raise ValueError("duplicate native cache identity key: " + key)
        actual[key] = value
    # A retired option can leave an experimental payload in an otherwise valid cache.
    if actual.get("JCE_BUILD_CSM_PCF_DIAGNOSTIC", "OFF").upper() != "OFF":
        raise ValueError("native cache contains a retired experiment; use a public build cache")
    for key, expected in (("CMAKE_HOME_DIRECTORY", plan["source"]),
            ("CMAKE_CACHEFILE_DIR", plan["build"]), ("CMAKE_TOOLCHAIN_FILE", plan["toolchain"])):
        if not actual.get(key) or Path(actual[key]).resolve() != expected:
            raise ValueError("native cache identity mismatch: " + key)
    expected = {"CMAKE_BUILD_TYPE": "Release", "JCE_DELIVERY_PROFILE": "embedded",
        "JCE_EDITOR_STANDALONE": "ON", "JCE_ENABLE_PROFILING": "OFF",
        "JCE_BUILD_SCRIPT_VM_PYTHON": "OFF", "JCE_BUILD_SCRIPT_JAVA": "OFF",
        "JCE_BUILD_SCRIPT_CSHARP": "OFF", "JCE_EMBED_ENGINE_SHADERS": "ON"}
    if exact_variant:
        expected.update(JCE_BUILD_VARIANT=plan["variant"], JCE_BUILD_EDITOR="ON")
    elif actual.get("JCE_BUILD_VARIANT") not in ("release", "dist"):
        raise ValueError("native cache identity mismatch: JCE_BUILD_VARIANT")
    for key, value in expected.items():
        if actual.get(key) != value:
            raise ValueError("native cache identity mismatch: " + key)
    toolchain = _cache_text(plan["toolchain"])
    if target["host"] == "windows":
        runtimes = re.findall(r'^\s*set\(CMAKE_MSVC_RUNTIME_LIBRARY\s+(.+?)\)\s*$',
                              toolchain, re.MULTILINE)
        if len(runtimes) != 1 or runtimes[0].strip('"') not in ("MultiThreaded", "$<$<CONFIG:Release>:MultiThreaded>"):
            raise ValueError("native cache identity mismatch: static MSVC runtime")
    version = [actual.get("CMAKE_CACHE_" + part + "_VERSION") for part in ("MAJOR", "MINOR", "PATCH")]
    if all(value is not None and re.fullmatch(r"[0-9]+", value) for value in version):
        compiler_files = [plan["build"] / "CMakeFiles" / ".".join(version) / "CMakeCCompiler.cmake"]
        if not compiler_files[0].is_file():
            raise ValueError("native cache active compiler metadata is missing")
    elif any(value is not None for value in version):
        raise ValueError("native cache CMake version identity is incomplete")
    else:
        compiler_files = list((plan["build"] / "CMakeFiles").glob("*/CMakeCCompiler.cmake"))
    if len(compiler_files) != 1:
        raise ValueError("native cache requires one compiler-generated architecture identity")
    compiler = _cache_text(compiler_files[0])
    if target["host"] == "windows":
        architecture = {"x86_64": "x64", "i686": "X86", "aarch64": "ARM64"}.get(target["arch"])
        if (not architecture or not re.search(r'^set\(CMAKE_C_COMPILER_ARCHITECTURE_ID\s+"?' +
                re.escape(architecture) + r'"?\)', compiler, re.MULTILINE) or
                'set(CMAKE_C_COMPILER_ID "MSVC")' not in compiler):
            raise ValueError("native cache identity mismatch: compiler/architecture")
    else:
        system = _cache_text(compiler_files[0].with_name("CMakeSystem.cmake"))
        aliases = {"x86_64": ("x86_64", "amd64"), "i686": ("i386", "i686", "x86"),
                   "aarch64": ("aarch64", "arm64"), "armv7": ("armv7", "armv7l")}
        architecture = re.findall(r'^set\(CMAKE_SYSTEM_PROCESSOR\s+"?([^"\s)]+)"?\)', system, re.MULTILINE)
        if (not re.search(r'^set\(CMAKE_C_COMPILER_ID\s+"(?:GNU|Clang|AppleClang)"\)', compiler, re.MULTILINE)
                or len(architecture) != 1 or architecture[0].lower() not in aliases.get(target["arch"], ())):
            raise ValueError("native cache identity mismatch: compiler/architecture")


def native_cache_layout(driver, args, target: dict) -> dict:
    """Select stable paths without admitting objects an isolated SDK never uses."""
    root = driver.ROOT.resolve()
    desktop = root / "build/desktop"
    target_dir = desktop / target["key"]
    canonical = target_dir / "obj"
    legacy = (desktop / (target["key"] + "-release"),
              desktop / (target["key"] + "-standalone"))
    remembered = getattr(args, "_native_cache_plan", None)
    build = remembered["build"] if remembered else canonical
    if not remembered and not canonical.exists():
        build = next((path for path in legacy if (path / "CMakeCache.txt").is_file()), canonical)
    if build not in (canonical, *legacy):
        raise ValueError("native cache owner differs from the requested target/variant")
    conan = target_dir / "conan" if build == canonical else build.with_name(build.name + "-conan")
    plan = {"source": root, "build": build, "conan": conan,
            "toolchain": conan / "build/Release/generators/conan_toolchain.cmake",
            "variant": args.variant}
    if remembered and remembered != plan:
        raise ValueError("native cache request identity changed after ownership selection")
    return plan


def native_cache_plan(driver, args, target: dict, *, exact_variant: bool = False) -> dict:
    """Keep admitted legacy absolute paths; new defaults use the Release config."""
    plan = native_cache_layout(driver, args, target)
    _admit_cache(plan, target, exact_variant=exact_variant)
    return plan


def editor_artifact_dir(driver, args, target: dict) -> Path:
    override = getattr(args, "artifact_dir", None)
    return (Path(override).resolve() if override is not None else
            driver.ROOT.resolve() / "build/desktop" / target["key"] / args.variant)


def guard_cache_clean(paths, allowed_root: Path, *, entry_budget: int = 100000,
                      depth_budget: int = 64) -> None:
    """Refuse cache cleanup that contains declared project sources; never follow links."""
    if type(entry_budget) is not int or entry_budget < 1 or type(depth_budget) is not int or depth_budget < 0:
        raise ValueError("cache clean scan budgets must be nonnegative (entries must be positive)")
    original_root = Path(allowed_root).absolute()
    pending = []

    def ordinary_directory(path):
        info = path.lstat()
        if (stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400 or
                not stat.S_ISDIR(info.st_mode)):
            raise ValueError("cache clean refuses an aliased/non-directory traversal: " + str(path))
        return info

    try:
        if ".." in original_root.parts:
            raise ValueError("cache clean allowed root has noncanonical spelling")
        for ancestor in reversed((original_root, *original_root.parents)):
            try:
                ordinary_directory(ancestor)
            except FileNotFoundError:
                break
        root = original_root.resolve()
        if root != original_root:
            raise ValueError("cache clean allowed root traverses an alias: " + str(original_root))
        for value in paths:
            path = Path(value).absolute()
            if ".." in path.parts or path == root or not path.is_relative_to(root):
                raise ValueError("cache clean target must be a strict child of " + str(root))
            current = root
            present = True
            # Inspect original spelling before resolving aliases away.
            for part in path.relative_to(root).parts:
                current /= part
                try:
                    ordinary_directory(current)
                except FileNotFoundError:
                    present = False
                    break
            resolved = path.resolve()
            if resolved == root or not resolved.is_relative_to(root):
                raise ValueError("cache clean target must be a strict child of " + str(root))
            if resolved != path:
                raise ValueError("cache clean target traverses an alias: " + str(path))
            if present:
                pending.append((path, 0))
        entries = 0
        while pending:
            directory, depth = pending.pop()
            before = ordinary_directory(directory)
            with os.scandir(directory) as children:
                for entry in children:
                    entries += 1
                    if entries > entry_budget:
                        raise ValueError("cache clean entry scan budget exhausted")
                    if entry.name.casefold() == "jce_project.json":
                        raise ValueError("cache clean refuses nested project source: " + str(Path(entry.path)))
                    info = entry.stat(follow_symlinks=False)
                    attributes = getattr(info, "st_file_attributes", 0)
                    if attributes & 0x400:
                        tag = getattr(info, "st_reparse_tag", 0)
                        if (attributes & 0x10 or not stat.S_ISLNK(info.st_mode) or
                                tag not in (0, getattr(stat, "IO_REPARSE_TAG_SYMLINK", 0xA000000C))):
                            raise ValueError("cache clean refuses directory/unknown reparse traversal: " + str(Path(entry.path)))
                        continue  # Known file symlink: rmtree unlinks it, never its target.
                    if stat.S_ISLNK(info.st_mode):
                        continue  # POSIX file/directory symlinks are never traversed.
                    if stat.S_ISDIR(info.st_mode):
                        if depth >= depth_budget:
                            raise ValueError("cache clean directory scan budget exhausted")
                        pending.append((Path(entry.path), depth + 1))
                    # Other non-directory entries (including FIFOs) need no reads.
            after = ordinary_directory(directory)
            if (before.st_dev, before.st_ino, before.st_mtime_ns) != (after.st_dev, after.st_ino, after.st_mtime_ns):
                raise ValueError("cache clean directory changed during inspection: " + str(directory))
    except OSError as error:
        raise ValueError("cache clean cannot inspect source ownership: " + str(error)) from error


def _configure(driver, args, target: dict, env: dict, sdk: Path | None = None, plan=None) -> Path:
    """Isolated static dependency closure; never relabel a dynamic-CRT build."""
    # An x86 Windows target is runnable on an x64 Windows build host through
    # WOW64. Keep rejecting cross targets that cannot execute their generated
    # build-time tools on the host (notably Windows ARM64).
    wow64_runnable_x86 = (
        target["host"] == driver.HOST == "windows"
        and driver.host_arch() == "x86_64"
        and target["arch"] == "i686"
    )
    if target["host"] != driver.HOST or (
        target.get("cross") and not wow64_runnable_x86
    ):
        driver.die("embedded delivery needs the matching native host toolchain")
    if args.variant not in ("release", "dist"):
        driver.die("embedded delivery supports release/dist; use authoring for debug")
    if getattr(args, "script_java", False):
        driver.die("embedded delivery cannot require an external JVM; choose authoring")
    if getattr(args, "profiling", None) == "on":
        driver.die("embedded delivery excludes profiler listeners; choose authoring for profiling")
    bgfx_args = driver.bgfx_graphics_conan_args(env)
    native = plan if plan else native_cache_plan(driver, args, target)
    build = plan["build"] if plan else native["build"]
    conan = plan["conan"] if plan else native["conan"]
    toolchain = conan / "build/Release/generators/conan_toolchain.cmake"
    if getattr(args, "clean", False):
        guard_cache_clean((build, conan), driver.ROOT / "build/desktop")
        for path in (build, conan):
            if not path.resolve().is_relative_to((driver.ROOT / "build/desktop").resolve()):
                raise ValueError("standalone clean target escaped build/desktop")
            if path.exists() and not driver.DRY_RUN:
                shutil.rmtree(path)
    options = sdk_outputs()
    if plan and not driver.DRY_RUN:
        options.state(plan, "requested")
    try:
        driver.sync_conan_hooks(env)
        driver.export_conan_recipes(env)
        vs = driver.find_vs_install(target["arch"]) if target["host"] == "windows" else None
        flags = ["-c", f"tools.microsoft.msbuild:installation_path={vs}"] if vs else []
        driver.run(["conan", "install", ".", "-pr:h", f"conan/profiles/{target['profile']}",
                    "-pr:b", f"conan/profiles/{driver.host_build_profile()}",
                    *(["-s:h", "compiler.runtime=static"] if target["host"] == "windows" else []),
                    "-o:h", "*:shared=False",
                    "-c:a", "tools.build:jobs=" + driver.build_jobs(env),
                    "--build=missing", "--output-folder", str(conan),
                    *bgfx_args, *flags], cwd=driver.ROOT, env=env)
        # shaderc runs on the *build* host, even when the editor targets x86.
        # Without an explicit -D below CMake may silently create empty
        # CompileShaders targets, followed by a late PAK enumeration failure.
        raw_shaderc = (env.get("JCE_SHADERC_EXECUTABLE") or
                       env.get("BGFX_SHADERC") or "").strip()
        if driver.DRY_RUN:
            shaderc = Path(raw_shaderc) if raw_shaderc else None
        elif raw_shaderc:
            shaderc = Path(raw_shaderc)
            if not shaderc.is_file() or not driver._shaderc_runs(shaderc):
                driver.die(f"explicit host shaderc cannot execute: {shaderc}")
        else:
            # Keep this admitted native tree's runnable tool before a fresh
            # automatic search can select an unrelated SDK's compiler.
            shaderc = None
            cache = build / "CMakeCache.txt"
            if cache.is_file():
                configured = [line.split("=", 1)[1] for line in _cache_text(cache).splitlines()
                              if line.startswith("JCE_SHADERC_EXECUTABLE:") and "=" in line]
                if len(configured) == 1:
                    candidate = Path(configured[0])
                    if candidate.is_absolute() and candidate.is_file() and driver._shaderc_runs(candidate):
                        shaderc = candidate
            if shaderc is None:
                shaderc = driver.find_host_shaderc()
        shaders_required = plan is None or plan.get("shaders") != "OFF"
        if not shaderc and shaders_required and not driver.DRY_RUN:
            driver.die("embedded shader packaging requires a runnable native "
                       "host shaderc; set JCE_SHADERC_EXECUTABLE")
        if shaderc:
            driver.log(f"embedded shaderc (build host): {shaderc}")
        if not driver.DRY_RUN:
            (driver.ROOT / "CMakeUserPresets.json").unlink(missing_ok=True)
        driver.run(["cmake", "-S", str(driver.ROOT), "-B", str(build), "-G", "Ninja",
                f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
                f"-DJCE_BUILD_VARIANT={args.variant}", "-DJCE_EDITOR_STANDALONE=ON",
                "-DJCE_BUILD_EDITOR=" + ("OFF" if sdk else "ON"),
                "-DJCE_DELIVERY_PROFILE=embedded", "-DJCE_ENABLE_PROFILING=OFF",
                "-DJCE_EDITOR_ARTIFACT_DIR=" + (str(editor_artifact_dir(driver, args, target)) if sdk is None else ""),
                "-DJCE_BUILD_SCRIPT_VM_PYTHON=OFF", "-DJCE_BUILD_SCRIPT_CSHARP=OFF",
                "-DJCE_BUILD_SCRIPT_JAVA=OFF",
                *driver.codec_overrides(args),
                "-DJCE_ENABLE_SDK_INSTALL=" + ("ON" if sdk else "OFF"),
                *([f"-DCMAKE_INSTALL_PREFIX={sdk}"] if sdk else []),
                *(["-DJCE_EMBED_ENGINE_SHADERS=" + plan["shaders"]] if plan else []),
                "-DJCE_ENABLE_AI_DISPATCH=OFF", "-DJCE_ENABLE_RENDERDOC=OFF",
                "-DJCE_ENABLE_CPPCHECK=OFF", "-DJCE_BUILD_TESTS=OFF", "-DJCE_EMIT_LINK_MAP=ON",
                *([f"-DJCE_SHADERC_EXECUTABLE={shaderc}"] if shaderc else [])],
               cwd=driver.ROOT, env=env)
        # Catch CMake's disabled-shader case before Ninja tries packing a
        # non-existent directory.  Merely creating it would hide a broken PAK.
        if shaders_required and not driver.DRY_RUN:
            shader_dir = build / "generated" / "engine_assets" / "shaders"
            if not shader_dir.is_dir():
                driver.die("embedded shaders were not configured: missing "
                           f"{shader_dir}; verify shaderc in CMake output")
    except BaseException:
        if plan and not driver.DRY_RUN:
            options.state(plan, "configure_failed")
        raise
    if plan and not driver.DRY_RUN:
        options.state(plan, "configured")
    return build


def build_sdk(driver, args, target: dict, env: dict) -> Path:
    """Install a static dependency SDK from the same closure as the editor."""
    options = sdk_outputs()
    plan = options.resolve(driver, args, target)
    driver.log("SDK stock shader embedding: " + plan["shaders"] +
        (" (explicit isolated capability; supply required shader assets)" if plan["shaders"] == "OFF" else ""))
    sdk = plan["install"]
    build = _configure(driver, args, target, env, sdk, plan)
    try:
        driver.run(["cmake", "--build", str(build), "--target", "jce_sdk_fat_lib",
                "jce_scripting_sdk_artifacts",
                *(["jce_msvc_stl_shims"] if target["host"] == "windows" else []),
                "jce_pak", "jce_cook", "jce_bin2obj", "jce_bundle_pack", "-j", driver.build_jobs(env)],
                   cwd=driver.ROOT, env=env)
    except BaseException:
        if not driver.DRY_RUN:
            options.state(plan, "build_failed")
        raise
    if not driver.DRY_RUN:
        options.state(plan, "built")
    try:
        driver.run(["cmake", "--install", str(build), "--config", "Release"],
                   cwd=driver.ROOT, env=env)
        if not driver.DRY_RUN:
            driver._prune_install_prefix(build, sdk)
            options.verify_install(driver, plan)
    except BaseException:
        if not driver.DRY_RUN:
            options.state(plan, "install_failed")
        raise
    if not driver.DRY_RUN:
        languages = ["lua", *driver.sdk_shipped_script_backends(sdk, target)]
        (sdk / "delivery-profile.json").write_text(json.dumps({"schema": 1,
            "profile": "embedded", "languages": languages, "external_vm_allowed": False}, indent=2)+"\n", encoding="utf-8")
        (sdk / "VERSION.txt").write_text("JCE SDK build\n"+
            f"commit: {driver.git_short_sha()}\nvariant: {args.variant}\n"+
            "delivery: embedded\nscripts: "+", ".join(languages)+"\n", encoding="utf-8")
    if getattr(args, "smoke", False):
        native_run = not (driver.DRY_RUN or target["cross"] or target.get("emscripten"))
        driver._smoke_one(target, sdk, args.variant,
                          build_dir=options.smoke_directory(plan),
                          require_run=native_run, delivery_profile="embedded")
        driver._smoke_scripting_one(target, sdk, args.variant,
                                    build_dir=options.smoke_directory(plan, True),
                                    require_run=native_run, delivery_profile="embedded")
    if not driver.DRY_RUN:
        try:
            driver._publish_sdk_product(sdk, target, "Release", args.variant, "embedded", build)
        except BaseException:
            options.state(plan, "install_failed")
            raise
    driver.log(f"static dependency SDK -> {sdk}")
    return sdk


def build_editor(driver, args, target: dict, env: dict) -> Path:
    build = _configure(driver, args, target, env)
    driver.run(["cmake", "--build", str(build), "--target", "JCE_Editor", "-j", driver.build_jobs(env)],
               cwd=driver.ROOT, env=env)
    output = editor_artifact_dir(driver, args, target)
    exe = output / ("jce_editor.exe" if target["host"] == "windows" else "jce_editor")
    if not driver.DRY_RUN:
        driver._assert_no_profiler_listener(exe, args.variant)
        if target["host"] == "windows":
            report = verify(exe, require_gui=args.variant == "dist")
            report["embedded_assets"] = verify_assets(exe, build)
            report["embedded_notices"] = verify_notices(exe, driver.ROOT)
        else:
            report = {"sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
                      "relocation_audit": "UNVERIFIED", "system_dependency_audit": "UNVERIFIED"}
        report.update(source_identity(driver))
        (build / "standalone.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    driver.log(f"native single-executable editor -> {exe}")
    return exe


def cached_editor_artifact(build: Path, variant: str, host: str) -> Path:
    """Resolve the configured output; a missing override never falls back."""
    output = build / variant
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        lines = cache.read_text(encoding="utf-8").splitlines()
        for line in lines:
            if line.startswith("JCE_BUILD_VARIANT:"):
                configured = line.partition("=")[2]
                if configured and configured != variant:
                    raise ValueError("cached editor variant differs from requested variant")
        for line in lines:
            if line.startswith("JCE_EDITOR_ARTIFACT_DIR:"):
                value = line.partition("=")[2]
                if value:
                    output = Path(value)
                    if not output.is_absolute():
                        output = build / output
                break
    return output / ("jce_editor.exe" if host == "windows" else "jce_editor")


def editor_package_output(driver, args, target: dict) -> Path:
    return (Path(args.out).resolve() if args.out else driver.ROOT / "dist/editor" /
            ("JCE-editor-" + target["key"] + (".exe" if target["host"] == "windows" else "")))


def editor_package_cache_paths(driver, args, target: dict) -> list[Path]:
    plan = native_cache_plan(driver, args, target)
    output = (cached_editor_artifact(plan["build"], args.variant, target["host"]) if args.skip_build else
              editor_artifact_dir(driver, args, target) /
              ("jce_editor.exe" if target["host"] == "windows" else "jce_editor"))
    args._native_package_artifact = output.resolve()
    args._native_package_output = editor_package_output(driver, args, target).resolve()
    return [output.parent, args._native_package_output.parent, args._native_package_output]


def package_editor(driver, args) -> None:
    target = driver.resolve_target(args.arch)
    if target["host"] != driver.HOST or args.variant not in ("release", "dist"):
        driver.die("embedded packaging needs a matching native release/dist build")
    if getattr(args, "python_runtime", None):
        driver.die("--python-runtime is for the full language bundle, not the native standalone editor")
    plan = native_cache_plan(driver, args, target, exact_variant=args.skip_build)
    args._native_cache_plan = plan
    build = plan["build"]
    if args.skip_build:
        exe = cached_editor_artifact(build, args.variant, target["host"])
    else:
        exe = build_editor(driver, args, target, driver.msvc_env(target))
    out = editor_package_output(driver, args, target)
    if ((getattr(args, "_native_package_artifact", exe.resolve()) != exe.resolve()) or
            (getattr(args, "_native_package_output", out.resolve()) != out.resolve())):
        raise ValueError("configured editor output changed after ownership selection")
    if target["host"] == "windows" and out.suffix.lower() != ".exe":
        driver.die("editor --out must name an EXE, not a bundle directory")
    if driver.DRY_RUN:
        driver.log(f"native single EXE -> {out}")
        return
    if target["host"] != "windows":
        driver.die("embedded artifact built, but non-Windows dependency/relocation audit is not implemented; refusing unverified shipment")
    report = verify(exe, require_gui=args.variant == "dist")
    report["embedded_assets"] = verify_assets(exe, build)
    report["embedded_notices"] = verify_notices(exe, driver.ROOT)
    driver._assert_no_profiler_listener(exe, args.variant)
    report.update(source_identity(driver))
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(exe, out)
    receipts = driver.ROOT / "build/reports/delivery" / target["key"] / args.variant / out.stem
    receipts.mkdir(parents=True, exist_ok=True)
    (receipts / "SHA256SUMS").write_text(report["sha256"] + "  " + out.name + "\n",
                                      encoding="ascii", newline="\n")
    (receipts / "manifest.json").write_text(json.dumps(report, indent=2) + "\n",
                                           encoding="utf-8", newline="\n")
    driver.log(f"native single EXE -> {out}; {report['exe_bytes']} bytes; receipts -> {receipts}")
