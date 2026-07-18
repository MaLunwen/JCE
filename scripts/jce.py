#!/usr/bin/env python3
"""
jce.py — unified cross-platform build driver for the JCE engine.

One entry point replaces the parallel scripts/*.bat + scripts/{linux,macos}/*.sh
families.  Two tracks:

  * Track A (producer):  `jce.py sdk`  builds + installs the redistributable SDK
                         (fat libs + host tools + cmake package + headers + share).
  * Track B (consumer):  `jce.py app <dir>`  builds a project as a pure SDK
                         consumer via find_package(JCE) — identical to an
                         external user.

The desktop target matrix lives in ONE table (TARGETS, below).  Adding a desktop
target = one row here + one conan profile under conan/profiles/.

Scope (2026-06-04 spec): desktop only.  web/android/iOS consumer builds are a
follow-on.  Use --dry-run to print the exact command sequence without running it.

Subcommands:
    jce.py sdk        [--arch …] [--variant release|dist|both] [--no-debug]
    jce.py app  <dir> [--arch …] [--variant …] [--sdk DIR] [--target T] [--exe N]
    jce.py editor     [--arch …] [--variant …]
    jce.py host-tools [--arch …]
    jce.py cook  <dir>
    jce.py serve
    jce.py lint
    jce.py targets

Shorthand: --dist == --variant dist (on every variant-aware subcommand).
Global: --dry-run, --clean, -v/--verbose
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Repo root = parent of this scripts/ directory.
ROOT = Path(__file__).resolve().parent.parent

# ── Host detection ────────────────────────────────────────────────────────
_SYS = platform.system().lower()
HOST = {"windows": "windows", "linux": "linux", "darwin": "darwin"}.get(_SYS, _SYS)

# SDK install-tree platform tag (matches dist/sdk/<tag>-<arch>).
SDK_TAG = {"windows": "win32", "linux": "linux", "darwin": "darwin"}

# ── The desktop target matrix — ADD A PLATFORM = ADD A ROW ────────────────
# arch    : GNU-triplet style (x86_64 / aarch64 / i686) used across the tree.
# eng_arch: short name used in build-dir / SDK-install paths (x64 / arm64 / x86).
# profile : conan/profiles/<profile>.
# preset_stem : CMakePresets.json preset stem; full name = <stem>-<kind> via
#               preset_name() (kind ∈ release|debug|release-sdk|dist-sdk).
TARGETS = {
    "windows-x64":   dict(host="windows", arch="x86_64",  bits=64, eng_arch="x64",   profile="windows-x64",   preset_stem="windows-x64"),
    "windows-arm64": dict(host="windows", arch="aarch64", bits=64, eng_arch="arm64", profile="windows-arm64", preset_stem="windows-arm64"),
    "windows-x86":   dict(host="windows", arch="i686",    bits=32, eng_arch="x86",   profile="windows-x86",   preset_stem="windows-x86"),
    "linux-x64":     dict(host="linux",   arch="x86_64",  bits=64, eng_arch="x64",   profile="linux-x64",     preset_stem="linux-x64"),
    "linux-arm64":   dict(host="linux",   arch="aarch64", bits=64, eng_arch="arm64", profile="linux-arm64",   preset_stem="linux-arm64"),
    "macos-x64":     dict(host="darwin",  arch="x86_64",  bits=64, eng_arch="x64",   profile="macos-x64",     preset_stem="macos-x64"),
    "macos-arm64":   dict(host="darwin",  arch="aarch64", bits=64, eng_arch="arm64", profile="macos-arm64",   preset_stem="macos-arm64"),
}

# Web (Emscripten) is a cross target buildable from any host with emsdk.
# Not part of the desktop matrix above (no native preset / MSVC env); _sdk_one
# special-cases t["emscripten"] with an explicit -S/-B Ninja configure.
TARGETS["wasm"] = dict(host=HOST, arch="wasm32", bits=32, eng_arch="wasm",
                       profile="wasm", preset_stem="wasm", emscripten=True)

# Accept short aliases on the CLI; normalise to GNU-triplet arch.
ARCH_ALIAS = {
    "x64": "x86_64", "amd64": "x86_64", "x86_64": "x86_64",
    "x86": "i686",   "i686": "i686",
    "arm64": "aarch64", "aarch64": "aarch64", "arm64e": "aarch64",
    "wasm": "wasm32", "wasm32": "wasm32", "web": "wasm32",
}

DRY_RUN = False
VERBOSE = False


# ── Small helpers ─────────────────────────────────────────────────────────
def log(msg: str) -> None:
    print(f"[jce] {msg}", flush=True)


def die(msg: str, code: int = 1) -> "None":
    print(f"[jce] ERROR: {msg}", file=sys.stderr, flush=True)
    sys.exit(code)


def run(cmd, env=None, cwd=None) -> None:
    """Print, then (unless --dry-run) execute a command, aborting on failure."""
    shown = cmd if isinstance(cmd, str) else " ".join(str(c) for c in cmd)
    log(f"$ {shown}")
    if DRY_RUN:
        return
    rc = subprocess.run(
        cmd, env=env, cwd=str(cwd) if cwd else None,
        shell=isinstance(cmd, str),
    ).returncode
    if rc != 0:
        die(f"command failed (exit {rc}): {shown}", rc)


def host_arch() -> str:
    m = platform.machine().lower()
    if m in ("amd64", "x86_64"):
        return "x86_64"
    if m in ("arm64", "aarch64"):
        return "aarch64"
    if m in ("x86", "i686", "i386"):
        return "i686"
    return m


def preset_name(t: dict, kind: str) -> str:
    """Full CMakePresets name. kind in {release, debug, release-sdk, dist-sdk}."""
    return f"{t['preset_stem']}-{kind}"


def preset_binary_dir(t: dict, kind: str) -> Path:
    """Build dir matching each preset's binaryDir (legacy flat layout).

    editor/game release & dist SHARE build/desktop/<stem> (the exe lands in
    <stem>/<variant> via the engine's RUNTIME_OUTPUT/${JCE_BUILD_VARIANT});
    debug and the SDK trees get their own dirs.
    """
    sub = {"release": "", "dist": "", "debug": "-debug",
           "release-sdk": "-sdk", "dist-sdk": "-dist-sdk"}[kind]
    return ROOT / "build" / "desktop" / f"{t['preset_stem']}{sub}"


def host_build_profile() -> str:
    """Conan profile for the native build machine (matrix-driven)."""
    ha = host_arch()
    for t in TARGETS.values():
        if t["host"] == HOST and t["arch"] == ha:
            return t["profile"]
    die(f"no host build profile for {HOST}-{ha}")
    return ""  # unreachable; die() exits


def resolve_target(arch_cli: str | None) -> dict:
    """Map --arch (default: host arch) to a TARGETS row for the current host."""
    arch = ARCH_ALIAS.get((arch_cli or host_arch()).lower())
    if not arch:
        die(f"unsupported --arch '{arch_cli}' (expected x64|x86|arm64)")
    key = None
    for k, t in TARGETS.items():
        if t["host"] == HOST and t["arch"] == arch:
            key = k
            break
    if not key:
        die(f"no desktop target for host={HOST} arch={arch}. "
            f"Run on the matching host, or see `jce.py targets`.")
    t = dict(TARGETS[key])
    t["key"] = key
    t["cross"] = (t["arch"] != host_arch())
    return t


def conan_dir(t: dict) -> Path:
    # Web (Emscripten) lives under build/web/ to match build-web.bat.
    if t.get("emscripten"):
        return ROOT / "build" / "web" / "wasm-conan"
    # One shared Conan output dir per target (matches the legacy .bat layout).
    return ROOT / "build" / "desktop" / f"{t['key']}-conan"


def toolchain_path(t: dict, config: str = "Release") -> Path:
    return conan_dir(t) / "build" / config / "generators" / "conan_toolchain.cmake"


def sdk_install_dir(t: dict, variant: str) -> Path:
    if t.get("emscripten"):
        return ROOT / "dist" / "sdk" / ("wasm-dist" if variant == "dist" else "wasm")
    suffix = "-dist" if variant == "dist" else ""
    return ROOT / "dist" / "sdk" / f"{SDK_TAG[t['host']]}-{t['arch']}{suffix}"


def git_short_sha() -> str:
    """Short HEAD commit, or 'unknown' outside a git checkout."""
    try:
        out = subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
            text=True, stderr=subprocess.DEVNULL).strip()
        return out or "unknown"
    except Exception:  # noqa: BLE001
        return "unknown"


# ── MSVC environment (Windows) ────────────────────────────────────────────
_MSVC_ENV_CACHE: dict[str, dict] = {}


# MSVC toolset prefix matching the conan profiles' compiler.version (msvc 194
# = VS2022 17.4+, toolset 14.4x). On a machine with multiple VS lines (e.g. a
# 14.5x preview alongside 14.4x), pick the one whose toolset matches so the
# build AND conan's own vcvars (vcvars_ver=14.4) both resolve.
_VC_TOOLSET_PREFIX = "14.4"
_ARCH_CLDIR = {"x86_64": "x64", "aarch64": "arm64", "armv8": "arm64", "i686": "x86"}


def _vs_install_paths() -> list:
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    paths = []
    if vswhere.exists():
        try:
            out = subprocess.check_output(
                [str(vswhere), "-all", "-prerelease", "-products", "*",
                 "-property", "installationPath"], text=True)
            paths = [Path(p) for p in out.splitlines() if p.strip()]
        except Exception:
            paths = []
    if not paths:
        for base in (os.environ.get("ProgramFiles", r"C:\Program Files"), pf86):
            for ed in ("2022", "2019"):
                for sku in ("Enterprise", "Professional", "Community", "BuildTools"):
                    p = Path(base) / "Microsoft Visual Studio" / ed / sku
                    if p.is_dir():
                        paths.append(p)
    return paths


def _vs_has_arch_cl(vs: Path, target_arch: str | None, want_toolset: bool) -> bool:
    tgt = _ARCH_CLDIR.get(target_arch or host_arch(), "x64")
    msvc = vs / "VC" / "Tools" / "MSVC"
    if not msvc.is_dir():
        return False
    try:
        for ts in msvc.iterdir():
            if want_toolset and not ts.name.startswith(_VC_TOOLSET_PREFIX):
                continue
            if (ts / "bin" / "Hostx64" / tgt / "cl.exe").exists():
                return True
    except OSError:
        return False
    return False


def find_vs_install(target_arch: str | None = None) -> Path | None:
    """VS install with the target arch's cl AND a toolset matching the project's
    msvc version (14.4x); falls back to any VS with the arch cl."""
    installs = _vs_install_paths()
    for vs in installs:
        if _vs_has_arch_cl(vs, target_arch, want_toolset=True):
            return vs
    for vs in installs:
        if _vs_has_arch_cl(vs, target_arch, want_toolset=False):
            return vs
    return installs[0] if installs else None


def find_vcvars(target_arch: str | None = None) -> Path | None:
    vs = find_vs_install(target_arch)
    if vs:
        cand = vs / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
        if cand.exists():
            return cand
    return None


def vcvars_arch(t: dict) -> str:
    ha = host_arch()
    prefix = {"x86_64": "x64", "aarch64": "arm64"}.get(ha, "x64")
    tgt = {"x86_64": "x64", "aarch64": "arm64", "i686": "x86"}[t["arch"]]
    return tgt if prefix == tgt else f"{prefix}_{tgt}"


def msvc_env(t: dict) -> dict:
    """Return an environment dict with MSVC activated for target arch (Windows)."""
    if HOST != "windows":
        return dict(os.environ)
    arch = vcvars_arch(t)
    if arch in _MSVC_ENV_CACHE:
        return _MSVC_ENV_CACHE[arch]
    if shutil.which("cl") and not t["cross"]:
        _MSVC_ENV_CACHE[arch] = dict(os.environ)
        return _MSVC_ENV_CACHE[arch]
    vc = find_vcvars(t["arch"])
    if not vc:
        log("WARN: vcvarsall.bat not found; relying on current PATH for cl/link.")
        _MSVC_ENV_CACHE[arch] = dict(os.environ)
        return _MSVC_ENV_CACHE[arch]
    log(f"activating MSVC env: {vc} {arch}")
    if DRY_RUN:
        return dict(os.environ)
    # Run vcvarsall from a PRISTINE base env — like the fresh cmd session the
    # legacy .bat scripts relied on. The launching shell may carry a DIFFERENT
    # VS on PATH/LIB (e.g. a preview install with x64 but no ARM64 tools);
    # inheriting it leaks the wrong link.exe (LNK1112) / libs (LNK1104) into
    # cross builds. Keep only system vars + the non-VS build tools and let
    # vcvarsall lay down the complete, correct target toolchain.
    keep = {"SYSTEMROOT", "SYSTEMDRIVE", "WINDIR", "TEMP", "TMP", "USERPROFILE",
            "USERNAME", "HOMEDRIVE", "HOMEPATH", "APPDATA", "LOCALAPPDATA",
            "PROGRAMFILES", "PROGRAMFILES(X86)", "PROGRAMW6432", "PROGRAMDATA",
            "COMSPEC", "PATHEXT", "NUMBER_OF_PROCESSORS",
            "PROCESSOR_ARCHITECTURE", "COMPUTERNAME", "OS", "CONAN_HOME"}
    base = {k: v for k, v in os.environ.items() if k.upper() in keep}
    tool_dirs = []
    for _tool in ("cmake", "ninja", "conan", "python", "git"):
        _p = shutil.which(_tool)
        if _p:
            _d = str(Path(_p).resolve().parent)
            if _d not in tool_dirs:
                tool_dirs.append(_d)
    _sr = base.get("SystemRoot") or os.environ.get("SystemRoot", r"C:\Windows")
    base["PATH"] = os.pathsep.join(
        tool_dirs + [fr"{_sr}\System32", _sr, fr"{_sr}\System32\Wbem"])
    out = subprocess.check_output(
        f'"{vc}" {arch} >nul 2>&1 && set', shell=True, text=True, env=base)
    env = {}
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    _MSVC_ENV_CACHE[arch] = env or dict(os.environ)
    return _MSVC_ENV_CACHE[arch]


# ── Conan ─────────────────────────────────────────────────────────────────
def sync_conan_hooks() -> None:
    src = ROOT / "conan" / "hooks"
    if not src.is_dir():
        return
    dst = Path(os.path.expanduser("~")) / ".conan2" / "extensions" / "hooks"
    hooks = sorted(src.glob("hook_*.py"))
    if not hooks:
        return
    log(f"syncing {len(hooks)} conan hook(s) -> {dst}")
    if DRY_RUN:
        return
    dst.mkdir(parents=True, exist_ok=True)
    for h in hooks:
        shutil.copy2(h, dst / h.name)


def conan_install(t: dict, config: str, env: dict) -> Path:
    """Ensure the conan toolchain for (target, config) exists; return its path."""
    tc = toolchain_path(t, config)
    # Web (Emscripten): cross-compile profile + emcc/em++ toolchain (mirrors
    # build-web.bat's conan invocation). Done before the desktop tc.exists()
    # short-circuit so the wasm conan args never leak into a desktop install.
    if t.get("emscripten"):
        if tc.exists():
            log(f"conan toolchain present (wasm/{config}): {tc} (skip; --clean to force)")
            return tc
        sync_conan_hooks()
        em = emscripten_paths()
        run(["conan", "install", ".",
             "-pr:h", "conan/profiles/wasm",
             "-pr:b", f"conan/profiles/{host_build_profile()}",
             "-c", f"tools.cmake.cmaketoolchain:user_toolchain=['{em['toolchain']}']",
             "-c", ("tools.build:compiler_executables="
                    f"{{'c': '{em['cc']}', 'cpp': '{em['cxx']}'}}"),
             "--output-folder", str(conan_dir(t)),
             "--build=missing"],
            env=env, cwd=ROOT)
        if not tc.exists() and not DRY_RUN:
            die(f"conan toolchain not generated: {tc}")
        return tc
    if tc.exists():
        log(f"conan toolchain present ({config}): {tc} (skip; --clean to force)")
        return tc
    sync_conan_hooks()
    # Debug: also force the DEBUG CRT runtime (/MDd) for deps so they match the
    # editor's /MDd debug code (the shared profile pins runtime_type=Release,
    # which makes a /MDd editor unlinkable against /MD deps — LNK2038).
    extra = (["-s", "build_type=Debug", "-s", "compiler.runtime_type=Debug"]
             if config == "Debug" else [])
    # Point conan's own vcvars at the SAME VS we use (toolset 14.4x matching
    # msvc 194). Without this, conan may pick another VS line for from-source
    # dep builds (e.g. a 14.5x preview) → "vcvars_ver=14.4 toolset not found".
    vs_conf = []
    if HOST == "windows":
        vs = find_vs_install(t["arch"])
        if vs:
            vs_conf = ["-c", f"tools.microsoft.msbuild:installation_path={vs}"]
    run(["conan", "install", ".",
         "-pr:h", f"conan/profiles/{t['profile']}",
         "-pr:b", f"conan/profiles/{host_build_profile()}",
         "--output-folder", str(conan_dir(t)),
         "--build=missing", *vs_conf, *extra],
        env=env, cwd=ROOT)
    if not tc.exists() and not DRY_RUN:
        die(f"conan toolchain not generated: {tc}")
    return tc


# ── shaderc + host tools (cross prerequisites) ────────────────────────────
def find_host_shaderc() -> Path | None:
    exe_name = "shaderc.exe" if HOST == "windows" else "shaderc"
    # 1) The host-conan bgfx package's bin/ (present when bgfx built tools=True).
    gen = ROOT / "build" / "host-conan" / "build" / "Release" / "generators"
    for data in gen.glob("bgfx-release-*-data.cmake"):
        try:
            for line in data.read_text(encoding="utf-8", errors="ignore").splitlines():
                if line.startswith("set(bgfx_PACKAGE_FOLDER_RELEASE"):
                    folder = Path(line.split('"')[1])
                    # bin/shaderc (packaged) OR the recipe build-tree location
                    # (mirrors tools/compile_shaders.cmake's candidate probe).
                    for cand in (folder / "bin" / exe_name,
                                 folder.parent / "b" / "build" / "Release"
                                       / "cmake" / "bgfx" / exe_name,
                                 folder.parent / "b" / "build" / "Debug"
                                       / "cmake" / "bgfx" / exe_name):
                        if cand.exists():
                            return cand
        except Exception:
            pass
    # 2) Cross builds (wasm/android/ios) deliberately build bgfx tools=False, so
    #    the host-conan package above has NO shaderc.  Fall back to ANY tools=True
    #    bgfx shaderc already in the conan cache (a desktop build populates one).
    #    Without this the wasm SDK compiled ZERO engine shaders → empty shader PAK
    #    → "not found in PAK: vs_color_essl.bin" → black WebGL canvas.
    try:
        import os as _os
        home = _os.environ.get("CONAN_HOME") or str(Path.home() / ".conan2")
        cache = Path(home) / "p" / "b"
        if cache.is_dir():
            cands: list[Path] = []
            for pkg in cache.glob("bgfx*"):
                for sub in (pkg / "p" / "bin" / exe_name,
                            pkg / "b" / "build" / "Release" / "cmake" / "bgfx" / exe_name):
                    if sub.exists():
                        cands.append(sub)
            if cands:
                # newest first — most likely to match the current bgfx version
                cands.sort(key=lambda p: p.stat().st_mtime, reverse=True)
                log(f"host shaderc from conan cache fallback: {cands[0]}")
                return cands[0]
    except Exception:
        pass
    return None


def emscripten_paths() -> dict:
    """Resolve the Emscripten toolchain + compiler wrappers (mirrors build-web.bat).

    EMSDK comes from $EMSDK or the well-known checkout. All paths use forward
    slashes so they survive conan's -c args and a CMake cache on Windows.
    """
    emsdk = os.environ.get("EMSDK") or "D:/Code/C_CPP/cross_platform/emsdk"
    emsdk = str(Path(emsdk)).replace("\\", "/")
    ext = ".bat" if HOST == "windows" else ""
    em = f"{emsdk}/upstream/emscripten"
    return dict(
        toolchain=f"{em}/cmake/Modules/Platform/Emscripten.cmake",
        cc=f"{em}/emcc{ext}",
        cxx=f"{em}/em++{ext}",
    )


def host_tool(name: str) -> Path:
    ext = ".exe" if HOST == "windows" else ""
    return ROOT / "build" / "host" / "tools" / f"{name}{ext}"


def ensure_host_tools(env: dict, tools=("jce_pak",)) -> dict[str, Path]:
    """Build host (native) tools into build/host/tools; return {name: path}.

    Always builds with the NATIVE host target's env — NOT the caller's `env`,
    which on a cross build (e.g. arm64) targets the other arch and would give
    the host tools the wrong LIB/cl (LNK1104 'cannot open LIBCMT.lib').
    """
    need = [n for n in tools if not host_tool(n).exists()]
    if need:
        log(f"building host tools: {', '.join(need)}")
        # Native host target. Use resolve_target (searches by host field) rather
        # than the key f"{HOST}-x64": on macOS HOST=='darwin' but the TARGETS key
        # is 'macos-x64', and the host may not even be x64 (Apple Silicon).
        ht = resolve_target(None); ht["cross"] = False
        host_env = msvc_env(ht)   # native x64 env; caller `env` may be cross-arch
        host_conan = ROOT / "build" / "host-conan"
        tc = host_conan / "build" / "Release" / "generators" / "conan_toolchain.cmake"
        if not tc.exists():
            sync_conan_hooks()
            run(["conan", "install", ".",
                 "-pr:h", f"conan/profiles/{host_build_profile()}",
                 "-pr:b", f"conan/profiles/{host_build_profile()}",
                 "--output-folder", str(host_conan), "--build=missing"],
                env=host_env, cwd=ROOT)
        run(["cmake", "-S", ".", "-B", "build/host", "-G", "Ninja",
             f"-DCMAKE_TOOLCHAIN_FILE={tc}", "-DCMAKE_BUILD_TYPE=Release",
             "-DJCE_ENABLE_CPPCHECK=OFF", "-DJCE_ENABLE_SDK_INSTALL=ON"],
            env=host_env, cwd=ROOT)
        run(["cmake", "--build", "build/host", "--target", *need],
            env=host_env, cwd=ROOT)
    return {n: host_tool(n) for n in tools}


# ── Subcommand: targets ───────────────────────────────────────────────────
def cmd_targets(_args) -> None:
    print(f"host = {HOST}-{host_arch()}\n")
    print(f"{'target':16} {'arch':9} {'bits':4} {'profile':16} preset_stem")
    print("-" * 72)
    for k, t in TARGETS.items():
        mark = "  <- buildable here" if t["host"] == HOST else ""
        print(f"{k:16} {t['arch']:9} {t['bits']:<4} {t['profile']:16} {t['preset_stem']}{mark}")


# ── Subcommand: host-tools ────────────────────────────────────────────────
def cmd_host_tools(args) -> None:
    t = resolve_target(args.arch)
    env = msvc_env(t)
    paths = ensure_host_tools(env, ("jce_pak", "jce_cook", "jce_bin2obj"))
    for n, p in paths.items():
        log(f"{n}: {p}{'' if p.exists() or DRY_RUN else '  (MISSING)'}")


# ── Subcommand: sdk (Track A) ─────────────────────────────────────────────
def _tool_runs(path: Path) -> bool:
    """True when the tool binary at `path` EXECUTES on this host (exit code is
    irrelevant — usage text returns nonzero). Catches wrong-arch / missing-DLL
    breakage at SDK-producer time instead of consumer time."""
    try:
        subprocess.run([str(path), "--help"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30)
        return True
    except Exception:
        return False


def _sdk_one(t: dict, variant: str, config: str, do_clean: bool,
             tolerate_missing_tools: bool = False) -> None:
    is_wasm = t.get("emscripten", False)
    kind = "debug" if config == "Debug" else f"{variant}-sdk"   # release-sdk|dist-sdk
    preset  = preset_name(t, kind)
    bdir    = preset_binary_dir(t, kind)
    install = sdk_install_dir(t, variant)
    if is_wasm:
        # Web SDK lives under build/web (matches build-web.bat) and configures
        # via explicit -S/-B Ninja, NOT a preset (there is no wasm preset).
        bdir = ROOT / "build" / "web" / (
            "wasm-dist-sdk" if variant == "dist" else "wasm-sdk")
    if config == "Debug":
        # Debug SDK is release-flavoured (no -dist-debug preset) and installs to
        # its own tree so it never clobbers the Release SDK at the same path.
        install = install.parent / f"{install.name}-debug"
    # MSVC env would inject the wrong toolchain into a wasm build; use a clean env.
    env = dict(os.environ) if is_wasm else msvc_env(t)

    if do_clean:
        for d in (bdir, conan_dir(t)):
            if d.exists():
                log(f"clean: {d}")
                if not DRY_RUN:
                    shutil.rmtree(d, ignore_errors=True)

    conan_install(t, config, env)
    overrides = []
    if config == "Debug":
        # No debug-sdk preset by design; enable SDK install via overrides.
        overrides += ["-DJCE_ENABLE_SDK_INSTALL=ON",
                      f"-DCMAKE_INSTALL_PREFIX={install}"]
    if t["cross"]:
        ht = ensure_host_tools(env, ("jce_pak",))
        overrides += [f"-DJCE_PAK_EXECUTABLE={ht['jce_pak']}"]
        sc = find_host_shaderc()
        if sc:
            overrides += [f"-DJCE_SHADERC_EXECUTABLE={sc}"]
    # Conan regenerates a root CMakeUserPresets.json that breaks `cmake --preset`
    # (duplicate conan-release across targets). We use our own presets, so drop it.
    if not DRY_RUN:
        (ROOT / "CMakeUserPresets.json").unlink(missing_ok=True)
    if is_wasm:
        # No wasm preset: configure explicitly through the Emscripten toolchain.
        tc = toolchain_path(t, config)
        run(["cmake", "-S", str(ROOT), "-B", str(bdir), "-G", "Ninja",
             f"-DCMAKE_TOOLCHAIN_FILE={tc}",
             f"-DCMAKE_BUILD_TYPE={config}",
             "-DJCE_ENABLE_SDK_INSTALL=ON",
             f"-DCMAKE_INSTALL_PREFIX={install}",
             "-DJCE_ENABLE_CPPCHECK=OFF",
             f"-DJCE_BUILD_VARIANT={variant}",
             *overrides], env=env, cwd=ROOT)
        # emscripten has no MSVC STL shims; build only the fat lib.
        run(["cmake", "--build", str(bdir), "--target",
             "jce_sdk_fat_lib", "-j", "8"], env=env, cwd=ROOT)
    else:
        run(["cmake", "--preset", preset, *overrides], env=env, cwd=ROOT)
        # Build by binary dir (configured by the preset above). Build presets are
        # named "build-<preset>"; building by dir avoids that name dependency.
        run(["cmake", "--build", str(bdir), "--target",
             "jce_sdk_fat_lib", "jce_msvc_stl_shims", "-j", "8"], env=env, cwd=ROOT)
    # Host tools to ship with the SDK. A turnkey SDK (jce_add_pak cooking real
    # assets without the editor) REQUIRES jce_cook/jce_pak/jce_bin2obj, so a
    # missing tool is a hard error by default; --tolerate-missing-tools
    # restores the old WARN behavior and stamps the SDK as degraded.
    tools_status = "ok"
    if not (t["cross"] or is_wasm):
        # Native tree: the three targets exist here — build them or die.
        run(["cmake", "--build", str(bdir), "--target",
             "jce_cook", "jce_pak", "jce_bin2obj", "-j", "8"], env=env, cwd=ROOT)
    run(["cmake", "--install", str(bdir)], env=env, cwd=ROOT)
    if t["cross"] or is_wasm:
        # Cross-arch SDK: tools built in-tree would be TARGET-arch and can't
        # run on the build host. Ship the BUILD-HOST tools under bin/host/ so
        # consumers still get a real (non-stub) PAK; JCEConfig probes bin/
        # then bin/host/. (See jce-sdk-host-tools-tension.)
        log("cross SDK: copying build-host cook/pack tools into <sdk>/bin/host")
        ht = ensure_host_tools(env, ("jce_pak", "jce_cook", "jce_bin2obj"))
        bindir = install / "bin" / "host"
        if not DRY_RUN:
            bindir.mkdir(parents=True, exist_ok=True)
        for name, path in ht.items():
            if DRY_RUN:
                log(f"copy host tool {path} -> {bindir}")
            elif Path(path).exists():
                shutil.copy2(path, bindir / Path(path).name)
            elif tolerate_missing_tools:
                tools_status = "MISSING (degraded SDK — jce_add_pak packs raw)"
                log(f"WARN: host tool missing, SDK ships without {name}: {path}")
            else:
                die(f"host tool missing: {path} — the SDK would be unable to "
                    "cook/pack assets. Pass --tolerate-missing-tools to ship "
                    "a degraded SDK anyway.")
    # Producer-side run check: every shipped tool must at least execute here.
    if not DRY_RUN and tools_status == "ok":
        for sub_dir in ("bin", os.path.join("bin", "host")):
            tdir = install / sub_dir
            if not tdir.is_dir():
                continue
            for tool in ("jce_cook", "jce_pak", "jce_bin2obj"):
                cand = tdir / (tool + (".exe" if HOST == "windows" else ""))
                if cand.exists() and not _tool_runs(cand):
                    die(f"shipped host tool does not run on this host: {cand}")
    # VERSION.txt — matches package-sdk.bat so consumers can identify the tree.
    if not DRY_RUN:
        _host_tag = "wasm" if is_wasm else f"{SDK_TAG[t['host']]}-{t['arch']}"
        (install / "VERSION.txt").write_text(
            "JCE SDK build\n"
            f"commit:  {git_short_sha()}\n"
            f"host:    {_host_tag}\n"
            f"variant: {variant}\n"
            f"tools:   {tools_status}\n"
            "note:    bin/ tools need the MSVC C++ Redistributable on Windows\n",
            encoding="utf-8")
    log(f"SDK ({variant}/{config}) -> {install}")


def cmd_sdk(args) -> None:
    t = resolve_target(args.arch)
    if t["host"] != HOST:
        die(f"target {t['key']} must be built on host '{t['host']}'.")
    variants = {"release": ["release"], "dist": ["dist"],
                "both": ["release", "dist"]}[args.variant]
    tolerate = getattr(args, "tolerate_missing_tools", False)
    for v in variants:
        _sdk_one(t, v, "Release", args.clean, tolerate)
    # Debug SDK is release-flavoured only: there is no -dist-debug preset, and
    # `dist` is a royalty-free *ship* build. Build Debug once, for release, and
    # only when a release SDK was requested.  Web ships Release-only (the wasm
    # fat lib is Release; a Debug consumer maps to it via MAP_IMPORTED_CONFIG_DEBUG).
    if not args.no_debug and "release" in variants and not t.get("emscripten"):
        _sdk_one(t, "release", "Debug", args.clean, tolerate)
    # Gate: build + run the plain-C99 smoke consumer against each fresh SDK.
    if getattr(args, "smoke", False):
        for v in variants:
            _smoke_one(t, sdk_install_dir(t, v), v)
    log("sdk: done")


# ── Subcommand: smoke (C ABI gate against a built SDK) ────────────────────
def _smoke_one(t: dict, sdk: Path, variant: str) -> None:
    """Build tests/sdk_smoke OUT-OF-TREE against the SDK at `sdk`, verify the
    cook→pack→embed artifacts offline, then (native arch only) run the
    consumer headless and require its `JCE_SMOKE: OK` marker."""
    jce_cmake = sdk / "lib" / "cmake" / "JCE"
    if not DRY_RUN and not (jce_cmake / "JCEConfig.cmake").exists():
        die(f"smoke: no JCEConfig.cmake under {jce_cmake} — build the SDK first.")

    if not DRY_RUN:
        # Tripwire: target binaries must never leak into the SDK bin/ again
        # (the stale win32-aarch64 SDK shipped a caged_kingdom.exe once).
        bin_dir = sdk / "bin"
        leaked = list(bin_dir.glob("caged_kingdom*")) if bin_dir.is_dir() else []
        if leaked:
            die(f"smoke: sample executable leaked into the SDK: {leaked[0]}")
        # Producer-side check: every shipped host tool must execute here.
        ext = ".exe" if HOST == "windows" else ""
        tool_found = False
        for sub_dir in ("bin", os.path.join("bin", "host")):
            for tool in ("jce_cook", "jce_pak", "jce_bin2obj"):
                cand = sdk / sub_dir / f"{tool}{ext}"
                if cand.exists():
                    tool_found = True
                    if not _tool_runs(cand):
                        die(f"smoke: shipped tool does not run on this host: {cand}")
        if not tool_found:
            die(f"smoke: SDK at {sdk} ships no host tools (degraded SDK) — "
                "nothing to gate.")

    env = dict(os.environ) if t.get("emscripten") else msvc_env(t)
    bdir = ROOT / "build" / "desktop" / f"sdk-smoke-{t['key']}-{variant}"
    cfg = ["cmake", "-S", str(ROOT / "tests" / "sdk_smoke"), "-B", str(bdir),
           "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
           "-U", "JCE_PAK_EXECUTABLE",
           "-U", "JCE_COOK_EXECUTABLE",
           "-U", "JCE_BIN2OBJ_EXECUTABLE",
           f"-DJCE_DIR={jce_cmake}"]
    if HOST == "windows" and not t.get("emscripten"):
        cfg += ["-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"]
    run(cfg, env=env, cwd=ROOT)
    run(["cmake", "--build", str(bdir)], env=env, cwd=ROOT)
    if DRY_RUN:
        return

    # Offline artifact gates — these run even for SDKs whose target arch
    # cannot execute on this host (cross/wasm).
    cooked = bdir / "JceSdkSmoke_cooked" / "smoke.png"
    if not cooked.exists():
        die(f"smoke: cooked asset missing: {cooked}")
    with open(cooked, "rb") as f:
        magic = f.read(4)
    if magic != b"JCEA":
        die(f"smoke: {cooked} is not a cooked .jceasset (magic={magic!r}) — "
            "the cook path produced raw bytes.")
    pak = bdir / "JceSdkSmoke_assets.pak"
    exe = bdir / ("jce_sdk_smoke.exe" if HOST == "windows" else "jce_sdk_smoke")
    if not pak.exists():
        die(f"smoke: pak missing: {pak}")
    if not exe.exists():
        die(f"smoke: exe missing: {exe}")
    if exe.stat().st_size <= pak.stat().st_size:
        die("smoke: exe is not larger than its pak — embed step suspect "
            f"(exe={exe.stat().st_size} pak={pak.stat().st_size})")

    if t["cross"] or t.get("emscripten"):
        log("smoke: link + artifact gates passed (target arch not runnable "
            "on this host; run gate skipped)")
        return

    # Run gate: headless boot (noop renderer), one frame, clean exit. The
    # stdout marker is the primary signal — the engine's own error path
    # decides the exit code when init fails.
    renv = dict(env)
    renv["JCE_BACKEND"] = "noop"
    if HOST == "linux":
        renv.setdefault("SDL_VIDEODRIVER", "dummy")
    log(f"smoke: running {exe.name} (JCE_BACKEND=noop)")
    try:
        proc = subprocess.run([str(exe)], env=renv, cwd=str(bdir),
                              capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        die("smoke: consumer hung (>120 s) — should_quit never fired?")
    marker_ok = "JCE_SMOKE: OK" in (proc.stdout or "")
    if not marker_ok or proc.returncode != 0:
        sys.stderr.write(proc.stdout or "")
        sys.stderr.write(proc.stderr or "")
        die(f"smoke: FAILED (exit={proc.returncode}, "
            f"marker={'yes' if marker_ok else 'MISSING'})")
    for line in (proc.stdout or "").splitlines():
        if "JCE_SMOKE:" in line:
            log(line.strip())
    log(f"smoke: PASS ({variant} SDK at {sdk})")


def cmd_smoke(args) -> None:
    t = resolve_target(args.arch)
    sdk = Path(args.sdk).resolve() if args.sdk else sdk_install_dir(t, args.variant)
    _smoke_one(t, sdk, args.variant)


# ── Subcommand: editor (Track A first-party desktop app) ──────────────────
def _editor_asan(t: dict, env: dict) -> None:
    """x64-only AddressSanitizer editor (Release + /Zi + /fsanitize=address),
    configured directly into a dedicated -asan tree.  Invoked via
    scripts/<os>/build-editor.{bat,sh} --variant asan."""
    if t["arch"] != "x86_64":
        die("asan editor build is x64-only")
    bdir = ROOT / "build" / "desktop" / f"{t['key']}-asan"
    conan_install(t, "Release", env)
    flags = ("/MD /O1 /Ob1 /Zi /DNDEBUG /fsanitize=address /Oy- /FS "
             "/D_DISABLE_STRING_ANNOTATION=1 /D_DISABLE_VECTOR_ANNOTATION=1")
    if not DRY_RUN:
        (ROOT / "CMakeUserPresets.json").unlink(missing_ok=True)
    run(["cmake", "-S", str(ROOT), "-B", str(bdir), "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={toolchain_path(t, 'Release')}",
         "-DCMAKE_BUILD_TYPE=Release", "-DJCE_BUILD_VARIANT=asan",
         f"-DCMAKE_C_FLAGS_RELEASE={flags}",
         f"-DCMAKE_CXX_FLAGS_RELEASE={flags}"], env=env, cwd=ROOT)
    run(["cmake", "--build", str(bdir), "--target", "JCE_Editor"], env=env, cwd=ROOT)
    log(f"editor (asan) -> {bdir}/asan")


def _git_short() -> str:
    try:
        r = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                           cwd=str(ROOT), capture_output=True, text=True, timeout=5)
        return r.stdout.strip() if r.returncode == 0 else ""
    except Exception:
        return ""


def _release_versions() -> list:
    """Release version tokens from recent git commit messages, newest first
    (e.g. ['v-0.9.8', 'v-0.9.6', ...]). A release commit's message starts with a
    'v' followed by a version number — that is the project's tagging convention."""
    try:
        r = subprocess.run(["git", "log", "--format=%s", "-n", "80"],
                           cwd=str(ROOT), capture_output=True, text=True, timeout=8)
        if r.returncode != 0:
            return []
        out: list = []
        for line in r.stdout.splitlines():
            m = re.match(r"^(v[-\s]?\d+(?:\.\d+)*)", line.strip())
            if m:
                v = m.group(1).replace(" ", "-")
                if v not in out:
                    out.append(v)
        return out
    except Exception:
        return []


def attach_binsize_reports(t: dict, variant: str, bdir: Path) -> None:
    """After an editor build, emit the binary-size composition report AND a diff
    report on EVERY build. Two snapshots persist per target+variant under
    <ROOT>/.binsize_history/ (gitignored, survive build-dir cleans):
      <key>.last.*  the previous build's snapshot (updated every build)
      <key>.base.*  the previous DISTINCT commit's snapshot (promoted from .last
                    when the commit advances)
    The diff is taken vs .base (previous commit) when available, else vs .last
    (previous build) — so a diff report is always produced after the first build.
    Non-fatal: any failure here is logged, never breaks the build."""
    if DRY_RUN:
        return
    try:
        script = ROOT / "tools" / "gen_binsize_report.py"
        # The linker map is emitted under <build>/reports/ (see jce_emit_link_map
        # in CMakeLists.txt), not next to the exe.
        mp = bdir / "reports" / "JCE_Editor.map"
        if not script.exists() or not mp.exists():
            log("binsize: map/script missing — report skipped")
            return
        reports = bdir / "reports"
        reports.mkdir(parents=True, exist_ok=True)
        cur_html = reports / "binsize_report.html"
        cur_json = reports / "binsize_report.json"
        diff_html = reports / "binsize_diff.html"
        commit = _git_short() or "local"
        hist = ROOT / ".binsize_history"
        hist.mkdir(exist_ok=True)
        key = f"{t['key']}-{variant}"
        last_json, last_commit_f = hist / f"{key}.last.json", hist / f"{key}.last.commit"

        def _read(p: Path) -> str:
            return p.read_text(encoding="utf-8").strip() if p.exists() else ""

        # Migrate the older single-snapshot layout (<key>.json/.commit) -> .last.
        old_json = hist / f"{key}.json"
        if old_json.exists() and not last_json.exists():
            shutil.move(str(old_json), str(last_json))
            old_commit = hist / f"{key}.commit"
            if old_commit.exists():
                shutil.move(str(old_commit), str(last_commit_f))

        last_commit = _read(last_commit_f)
        # Diff baseline = the PREVIOUS RELEASE (by git commit message, e.g. the
        # 'v-0.9.6' commit) so the diff shows real release-over-release deltas;
        # fall back to the previous build's snapshot if that release wasn't built
        # here yet.
        vers = _release_versions()
        cur_ver = vers[0] if vers else commit
        prev_ver = next((v for v in vers[1:] if v != cur_ver), "")

        def _vkey(v: str) -> str:
            return f"{key}@" + re.sub(r"[^\w.\-]", "_", v)

        prev_snap = (hist / f"{_vkey(prev_ver)}.json") if prev_ver else None
        if prev_snap and prev_snap.exists():
            diff_src, diff_label = prev_snap, prev_ver
        elif last_json.exists():
            diff_src, diff_label = last_json, (last_commit or "prev-build")
        else:
            diff_src, diff_label = None, ""

        title = f"JCE editor {t['key']} {variant}"
        cmd = [sys.executable, str(script), str(mp), str(cur_html),
               "--json", str(cur_json), "--title", title, "--commit", cur_ver]
        if diff_src:
            cmd += ["--baseline", str(diff_src), "--base-commit", diff_label]
        subprocess.run(cmd, cwd=str(ROOT))

        if diff_src:
            subprocess.run(
                [sys.executable, str(script), "--diff", str(diff_src), str(cur_json),
                 str(diff_html), "--title", f"{title}: {diff_label} -> {cur_ver}",
                 "--base-commit", diff_label, "--commit", cur_ver], cwd=str(ROOT))
            log(f"binsize: report + diff ({diff_label} -> {cur_ver}) -> {reports}")
        else:
            log(f"binsize: report -> {cur_html} (no prior release/build baseline yet)")

        # Snapshot this build under its release version (for future diffs) + .last.
        if cur_json.exists():
            shutil.copy2(cur_json, hist / f"{_vkey(cur_ver)}.json")
            shutil.copy2(cur_json, last_json)
            last_commit_f.write_text(commit, encoding="utf-8")
    except Exception as e:  # noqa: BLE001 — reports must never fail the build
        log(f"binsize: report generation failed (non-fatal): {e}")


def cmd_editor(args) -> None:
    t = resolve_target(args.arch)
    if t["host"] != HOST:
        die(f"editor target {t['key']} must be built on host '{t['host']}'.")
    env = msvc_env(t)
    if args.variant == "asan":
        _editor_asan(t, env)
        return
    # dist = royalty-free ship editor (Release build type, no patented codecs / Tracy).
    config = "Debug" if args.variant == "debug" else "Release"
    kind = {"debug": "debug", "dist": "dist"}.get(args.variant, "release")
    preset = preset_name(t, kind)        # windows-x64-{release|debug|dist}
    bdir = preset_binary_dir(t, kind)
    conan_install(t, config, env)
    # Pin the build variant on EVERY configure.  release & dist share one
    # build dir (build/desktop/<stem>; exe lands in <stem>/<variant>) and the
    # *-release presets do NOT set JCE_BUILD_VARIANT, so a prior dist/debug
    # configure left it cached as 'dist' — making a later `--variant release`
    # build silently emit into <stem>/dist/ and leave <stem>/release/ stale.
    # An explicit -D always overrides the cached value.
    overrides = [f"-DJCE_BUILD_VARIANT={args.variant}"]
    # Same shared-build-dir hazard as JCE_BUILD_VARIANT (above): the *-dist
    # preset caches JCE_ENABLE_PATENTED_CODECS=OFF, and the *-release presets
    # rely on the option() default (ON), which CANNOT override an existing
    # cache entry — so a release configure after a dist one in the same
    # build/desktop/<stem> dir silently keeps patented codecs OFF.  Pin the
    # variant-appropriate value on every configure (dist = OFF, else ON).
    overrides.append(
        f"-DJCE_ENABLE_PATENTED_CODECS={'OFF' if args.variant == 'dist' else 'ON'}")
    # Emit a linker map so the build can attach a binary-composition report.
    overrides.append("-DJCE_EMIT_LINK_MAP=ON")
    if t["cross"]:
        ht = ensure_host_tools(env, ("jce_pak",))
        overrides.append(f"-DJCE_PAK_EXECUTABLE={ht['jce_pak']}")
        sc = find_host_shaderc()
        if sc:
            overrides.append(f"-DJCE_SHADERC_EXECUTABLE={sc}")
    if not DRY_RUN:
        (ROOT / "CMakeUserPresets.json").unlink(missing_ok=True)
    run(["cmake", "--preset", preset, *overrides], env=env, cwd=ROOT)
    run(["cmake", "--build", str(bdir), "--target", "JCE_Editor"], env=env, cwd=ROOT)
    attach_binsize_reports(t, args.variant, bdir)
    # Run cppcheck for the x64 Windows RELEASE variant only — that is the only
    # config where the target is created (see CMakeLists.txt). It is on-demand
    # (no ALL), so invoke it explicitly here. Non-fatal; the target is absent for
    # dist/debug/arm64/non-Windows and the run then reports "unavailable".
    if not DRY_RUN and args.variant == "release":
        rc = subprocess.run(["cmake", "--build", str(bdir), "--target", "cppcheck"],
                            env=env, cwd=str(ROOT))
        log(f"cppcheck report -> {bdir}\\reports\\cppcheck-report.txt"
            if rc.returncode == 0
            else "cppcheck skipped (not installed / target unavailable)")
    log(f"editor ({args.variant}) -> {bdir}")


# ── Subcommand: app (Track B — build via SDK as a real consumer) ──────────
def resolve_sdk(args, t: dict) -> Path:
    if args.sdk:
        sdk = Path(args.sdk).resolve()
    elif os.environ.get("JCE_SDK_DIR"):
        sdk = Path(os.environ["JCE_SDK_DIR"]).resolve()
    else:
        variant = getattr(args, "variant", "release")
        sdk = sdk_install_dir(t, "dist" if variant == "dist" else "release")
    if not (sdk / "lib" / "cmake" / "JCE" / "JCEConfig.cmake").exists():
        die(f"no valid SDK at {sdk} (missing lib/cmake/JCE/JCEConfig.cmake). "
            f"Run `jce.py sdk` first, pass --sdk DIR, or set JCE_SDK_DIR.")
    return sdk


def read_manifest(project: Path) -> dict:
    """Parse jce_project.json (proper JSON; unlike build-project.bat's scrape)."""
    m = project / "jce_project.json"
    if not m.exists():
        return {}
    try:
        return json.loads(m.read_text(encoding="utf-8"))
    except Exception as e:  # noqa: BLE001
        log(f"WARN: could not parse {m}: {e}")
        return {}


def cmd_app(args) -> None:
    project = Path(args.project).resolve()
    if not project.is_dir():
        die(f"project dir not found: {project}")
    manifest = read_manifest(project)
    t = resolve_target(args.arch)
    if t["host"] != HOST:
        die(f"app target {t['key']} must be built on host '{t['host']}'.")
    sdk = resolve_sdk(args, t)
    jce_cmake = sdk / "lib" / "cmake" / "JCE"
    if not (jce_cmake / "JCEConfig.cmake").exists():
        die(f"JCEConfig.cmake not under {jce_cmake}")
    # MSVC env would inject the wrong toolchain into a wasm consumer build;
    # the SDK fat lib carries all deps, so a clean env + Emscripten.cmake is
    # the whole toolchain story (mirrors _sdk_one).
    env = dict(os.environ) if t.get("emscripten") else msvc_env(t)
    config = "Debug" if args.variant == "debug" else "Release"
    bdir = project / "build" / f"{t['host']}-{t['arch']}-{args.variant}"
    if args.clean and bdir.exists():
        log(f"clean: {bdir}")
        if not DRY_RUN:
            shutil.rmtree(bdir, ignore_errors=True)
    cfg = ["cmake", "-S", str(project), "-B", str(bdir), "-G", "Ninja",
           f"-DCMAKE_BUILD_TYPE={config}",
           # Tool paths are find_program cache entries. A build directory may
           # be reused with --sdk pointing at a different installation, so
           # make the selected SDK authoritative while preserving all other
           # project cache state.
           "-U", "JCE_PAK_EXECUTABLE",
           "-U", "JCE_COOK_EXECUTABLE",
           "-U", "JCE_BIN2OBJ_EXECUTABLE",
           f"-DJCE_DIR={jce_cmake}"]
    if t.get("emscripten"):
        cfg.append(f"-DCMAKE_TOOLCHAIN_FILE={emscripten_paths()['toolchain']}")
    elif HOST == "windows":
        cfg.append("-DCMAKE_C_COMPILER=cl")
    run(cfg, env=env, cwd=ROOT)
    if args.configure_only:
        log(f"app: configured {project.name} (via SDK at {sdk}) [--configure-only]")
        return
    # CMake target name comes from the manifest (CK: "CagedKingdom"), not the
    # dir name; fall back to the dir name for manifest-less projects.
    target = args.target or manifest.get("target") or manifest.get("name") or project.name
    run(["cmake", "--build", str(bdir), "--target", target], env=env, cwd=ROOT)
    if t.get("emscripten"):
        # Web artifact is <name>.html (+ .js/.wasm/.data); the manifest "exe"
        # field is the DESKTOP artifact name and does not apply here.
        exe = args.exe or f"{manifest.get('name', project.name)}.html"
    else:
        exe = args.exe or manifest.get("exe") or ("caged_kingdom" + (".exe" if HOST == "windows" else ""))
    found = next((p for p in (bdir / exe, bdir / config / exe) if p.exists()), None)
    if not found and not DRY_RUN:
        die(f"build succeeded but artifact missing: {exe} (under {bdir})")
    log(f"app: {found if found else exe} (via SDK at {sdk})")


# ── Subcommand: package (stage redistributable bundles) ───────────────────
_EDITOR_BUNDLE_README = """\
JCE Editor bundle ({platform}-{arch}, {variant})
=================================================

Contents:
  jce_editor.exe   The editor. Double-click to launch.
  sdk/             Engine SDK (headers, libs, CMake config, resources).

Prerequisites on this machine (NOT bundled):
  * Microsoft Visual C++ Build Tools (MSVC) - the editor compiles your game
    project locally and links it against the SDK libs.
  * CMake 3.20 or newer, on PATH.
  * Ninja, on PATH.

The editor builds/cooks/packages projects natively. Point your project at
this bundled sdk/ (set sdk_path in jce_project.json or the JCE_SDK_DIR
environment variable). Keep jce_editor.exe and sdk/ together.
"""


def _host_exe(base: str) -> str:
    """Executable filename for the build HOST — '.exe' only on Windows."""
    return base + ".exe" if HOST == "windows" else base


def cmd_package_editor(args) -> None:
    """Stage a redistributable editor bundle (exe + sdk/ + VERSION + README)
    into dist/editor/<tag>-<arch>[-dist].  Replaces package-editor.bat."""
    t = resolve_target(args.arch)
    if t["host"] != HOST:
        die(f"editor package target {t['key']} must be built on host '{t['host']}'.")
    variant = args.variant                       # release | dist
    suffix  = "-dist" if variant == "dist" else ""
    src     = preset_binary_dir(t, "dist" if variant == "dist" else "release") / variant
    editor_exe = src / _host_exe("jce_editor")
    sdk_dir = sdk_install_dir(t, variant)
    out = (Path(args.out).resolve() if args.out
           else ROOT / "dist" / "editor" / f"{SDK_TAG[t['host']]}-{t['arch']}{suffix}")

    if not args.skip_build:
        cmd_editor(argparse.Namespace(arch=args.arch, variant=variant, clean=False))
        cmd_sdk(argparse.Namespace(arch=args.arch, variant=variant,
                                   no_debug=True, clean=False))

    if not DRY_RUN:
        if not editor_exe.exists():
            die(f"editor exe not found: {editor_exe} (build first, or drop --skip-build)")
        if not (sdk_dir / "lib" / "cmake" / "JCE" / "JCEConfig.cmake").exists():
            die(f"SDK not found at {sdk_dir}; run `jce.py sdk --arch {t['arch']} --variant {variant}`")

    log(f"stage editor bundle -> {out}")
    if not DRY_RUN:
        if out.exists():
            shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True, exist_ok=True)
        shutil.copy2(editor_exe, out / editor_exe.name)
        for extra in ("jce_editor_sha256.txt",):
            if (src / extra).exists():
                shutil.copy2(src / extra, out / extra)
        for dll in src.glob("*.dll"):
            shutil.copy2(dll, out / dll.name)
        shutil.copytree(sdk_dir, out / "sdk", dirs_exist_ok=True)
        (out / "VERSION.txt").write_text(
            "product:  JCE Editor bundle\n"
            f"platform: {SDK_TAG[t['host']]}\narch:     {t['arch']}\n"
            f"variant:  {variant}\ncommit:   {git_short_sha()}\n", encoding="utf-8")
        (out / "README.txt").write_text(
            _EDITOR_BUNDLE_README.format(platform=SDK_TAG[t["host"]],
                                         arch=t["arch"], variant=variant), encoding="utf-8")
    log(f"package editor: {out}")


def cmd_package_game(args) -> None:
    """Cook -> build (via SDK) -> stage a game bundle (exe + dlls + cooked
    assets + VERSION) into dist/games/<name>-<ver>-<tag>-<arch>.  Replaces
    package-game.bat."""
    project = Path(args.project).resolve()
    if not project.is_dir():
        die(f"project dir not found: {project}")
    manifest = read_manifest(project)
    t = resolve_target(args.arch)
    variant = args.variant
    name    = args.name or manifest.get("name") or project.name
    version = args.version or manifest.get("version") or "0.0.0"

    cmd_cook(argparse.Namespace(project=str(project)))
    cmd_app(argparse.Namespace(project=str(project), arch=args.arch, variant=variant,
                               sdk=args.sdk, target=args.target, exe=args.exe,
                               configure_only=False, clean=args.clean))

    bdir   = project / "build" / f"{t['host']}-{t['arch']}-{variant}"
    config = "Debug" if variant == "debug" else "Release"
    exe    = args.exe or manifest.get("exe") or _host_exe(name)
    if HOST == "windows" and not exe.lower().endswith(".exe"):
        exe += ".exe"
    built = next((p for p in (bdir / exe, bdir / config / exe) if p.exists()), None)
    if not DRY_RUN and not built:
        die(f"game exe not found under {bdir}: {exe}")
    out = (Path(args.out).resolve() if args.out
           else project / "dist" / "games"
           / f"{name}-{version}-{SDK_TAG[t['host']]}-{t['arch']}")

    log(f"stage game bundle -> {out}")
    if not DRY_RUN:
        if out.exists():
            shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True, exist_ok=True)
        shutil.copy2(built, out / built.name)
        for dll in built.parent.glob("*.dll"):
            shutil.copy2(dll, out / dll.name)
        # Single-exe by default: the game embeds its PAK into the executable
        # (jce_add_pak) and the runtime mounts only that blob, so the loose
        # cooked tree beside the exe is redundant and would leak plaintext
        # assets.  --with-loose (or manifest "stage_loose": true) re-stages it
        # for dev / projects that genuinely load loose assets.
        stage_loose = bool(getattr(args, "with_loose", False)) or \
                      bool(manifest.get("stage_loose", False))
        if stage_loose:
            cooked_rel = (manifest.get("cooked_assets") or "resources/_cooked")
            cooked_src = project / cooked_rel.replace("/", os.sep)
            if cooked_src.is_dir():
                shutil.copytree(cooked_src, out / cooked_rel.replace("/", os.sep),
                                dirs_exist_ok=True)
            else:
                log(f"WARN: cooked assets dir missing: {cooked_src} (game ships without assets)")
            # The runtime reads jce_project.json from the CWD to find
            # startup_scene; without it the game boots to an empty default
            # scene.  Loose deployments therefore always need the manifest.
            proj_json = project / "jce_project.json"
            if proj_json.is_file():
                shutil.copy2(proj_json, out / "jce_project.json")
            # Mirror extra loose runtime dirs the project's POST_BUILD staged
            # beside the exe (e.g. particles/ — emitter descs are read
            # CWD-relative by jce_particles_desc_load_json, not from the PAK).
            for extra in ("particles",):
                extra_src = built.parent / extra
                if extra_src.is_dir():
                    shutil.copytree(extra_src, out / extra, dirs_exist_ok=True)
        else:
            log("single-file: loose cooked tree NOT staged (assets embedded in "
                "exe; pass --with-loose for dev / non-embedded projects)")
        (out / "VERSION.txt").write_text(
            f"name:     {name}\nversion:  {version}\n"
            f"platform: {SDK_TAG[t['host']]}\narch:     {t['arch']}\n"
            f"variant:  {variant}\ncommit:   {git_short_sha()}\n", encoding="utf-8")
    log(f"package game: {out}")


# ── Subcommand: cook / serve / lint (folded wrappers) ─────────────────────
def cmd_cook(args) -> None:
    project = Path(args.project).resolve()
    cooker = host_tool("jce_cook")
    if not cooker.exists():
        env = msvc_env(resolve_target(None))
        cooker = ensure_host_tools(env, ("jce_cook",))["jce_cook"]
    manifest = read_manifest(project)
    src = project / (manifest.get("source_assets") or "assets")
    out = project / (manifest.get("cooked_assets") or "resources/_cooked")
    # Target platform drives GPU texture compression (desktop -> BC, mobile ->
    # ASTC). Default to the host so a plain `cook` produces compressed textures.
    plat = getattr(args, "platform", None) or resolve_target(None)["host"]
    run([str(cooker), "--batch", str(src), str(out),
         "--preserve-names", "--level", "0", "--max-texture-size", "2048",
         "--platform", plat], cwd=ROOT)


def cmd_serve(args) -> None:
    cmd = [sys.executable, str(ROOT / "scripts" / "serve-web.py")]
    if args.port:
        cmd += ["--port", str(args.port)]
    if args.host:
        cmd += ["--host", args.host]
    if args.directory:
        cmd += ["--dir", args.directory]
    if args.entry:
        cmd += ["--entry", args.entry]
    if args.no_open:
        cmd.append("--no-open")
    run(cmd, cwd=ROOT)


def cmd_lint(_args) -> None:
    run([sys.executable, str(ROOT / "scripts" / "lint" / "run_all.py")], cwd=ROOT)


# ── Subcommand: checksums (aggregate dist artifacts into one SHA256SUMS) ────
def _sha256(path: Path) -> str:
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _write_sha256sums(entries, out_file: Path) -> None:
    """entries: list of (display_name, file_path).  Writes a GNU coreutils
    `sha256sum -c`-compatible SHA256SUMS (one `<hash>  <name>` line each)."""
    lines = []
    for name, path in sorted(entries):
        digest = _sha256(path)
        lines.append(f"{digest}  {name}")
        log(f"  {digest}  {name}")
    out_file.parent.mkdir(parents=True, exist_ok=True)
    if not DRY_RUN:
        # Write bytes with explicit LF: a CRLF SHA256SUMS breaks `sha256sum -c`
        # (the trailing \r gets glued onto each filename) on Linux/macOS.
        out_file.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
    log(f"wrote {out_file} ({len(entries)} artifact(s)) "
        f"-- verify with: sha256sum -c {out_file.name}")


def cmd_checksums(args) -> None:
    """Aggregate dist editor binaries into one verifiable SHA256SUMS.

    Cross-arch / cross-platform binaries are intentionally different (different
    machine code / OS / toolchain), so this is NOT a single shared hash — it is
    one file listing each artifact's own hash, the industry-standard release
    verification model (`sha256sum -c SHA256SUMS`).

    Default: collect THIS machine's per-arch dist editors into a staging dir
    (arch-tagged names) and hash them.  --dir: hash an already-collected release
    folder in place (use this to produce the global file after gathering every
    platform's artifacts)."""
    import glob as _glob
    import shutil

    # Mode 1 — hash a folder of already-collected (renamed) artifacts in place.
    if args.dir:
        base = Path(args.dir).resolve()
        if not base.is_dir():
            die(f"--dir not found: {base}")
        entries = [(p.name, p) for p in sorted(base.iterdir())
                   if p.is_file() and p.name != "SHA256SUMS"]
        if not entries:
            die(f"no files to hash in {base}")
        _write_sha256sums(entries, base / "SHA256SUMS")
        return

    # Mode 2 (default) — collect this machine's dist editor binaries.
    out_dir = Path(args.out).resolve() if args.out else (ROOT / "dist" / "release")
    patterns = [
        ROOT / "build" / "desktop" / "*" / "dist" / "jce_editor",
        ROOT / "build" / "desktop" / "*" / "dist" / "jce_editor.exe",
        ROOT / "build" / "desktop" / "macos-universal" / "jce_editor",
    ]
    found = []
    for pat in patterns:
        found += [Path(m) for m in _glob.glob(str(pat)) if Path(m).is_file()]
    if not found:
        die("no dist editor binaries under build/desktop/*/dist "
            "(build with --dist first), or pass --dir <collected-folder>")

    if not DRY_RUN:
        out_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    seen = set()
    for src in sorted(found):
        # stem = the build/desktop/<stem> dir (e.g. windows-x64, macos-universal)
        stem = src.parent.parent.name if src.parent.name == "dist" else src.parent.name
        name = f"jce_editor-{stem}{src.suffix}"
        if name in seen:
            continue
        seen.add(name)
        dst = out_dir / name
        if not DRY_RUN:
            shutil.copy2(src, dst)
        # Hash the source: byte-identical to the copy, and works under --dry-run
        # (where the copy hasn't happened).
        entries.append((name, src))
    _write_sha256sums(entries, out_dir / "SHA256SUMS")


# ── argparse ──────────────────────────────────────────────────────────────
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="jce.py", description="JCE unified build driver")
    p.add_argument("--dry-run", action="store_true", help="print commands, do not run")
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_arch(sp):
        sp.add_argument("--arch", help="x64|x86|arm64 (default: host)")

    def add_variant(sp, choices, default):
        # --variant is the general selector; --dist is a quick shorthand for
        # --variant dist (resolved in main()).  Keeps the common distribution
        # build a single flag, matching build-web.bat's existing --dist.
        sp.add_argument("--variant", choices=choices, default=default)
        sp.add_argument("--dist", action="store_true",
                        help="shorthand for --variant dist")

    sp = sub.add_parser("sdk", help="build + install the redistributable SDK")
    add_arch(sp)
    add_variant(sp, ["release", "dist", "both"], "both")
    sp.add_argument("--no-debug", action="store_true", help="skip the Debug SDK")
    sp.add_argument("--clean", action="store_true")
    sp.add_argument("--smoke", action="store_true",
                    help="after install, build + run the C99 smoke consumer "
                         "against the fresh SDK (tests/sdk_smoke)")
    sp.add_argument("--tolerate-missing-tools", action="store_true",
                    help="ship a DEGRADED SDK without cook/pack host tools "
                         "instead of failing (stamped in VERSION.txt)")
    sp.set_defaults(func=cmd_sdk)

    sp = sub.add_parser("smoke",
                        help="build + run the C99 SDK smoke consumer against "
                             "an existing SDK (C ABI regression gate)")
    add_arch(sp)
    sp.add_argument("--sdk", help="SDK install dir "
                                  "(default: dist/sdk/<host-arch tag>)")
    sp.add_argument("--variant", choices=["release", "dist"], default="release")
    sp.set_defaults(func=cmd_smoke)

    sp = sub.add_parser("app", help="build a project via the SDK (consumer)")
    sp.add_argument("project")
    add_arch(sp)
    add_variant(sp, ["release", "debug", "dist"], "release")
    sp.add_argument("--sdk", help="SDK root (else JCE_SDK_DIR / dist/sdk/<host>-<arch>)")
    sp.add_argument("--target", help="CMake target (default: manifest 'target' or dir name)")
    sp.add_argument("--exe", help="expected artifact filename")
    sp.add_argument("--configure-only", action="store_true", help="configure, skip build")
    sp.add_argument("--clean", action="store_true")
    sp.set_defaults(func=cmd_app)

    sp = sub.add_parser("editor", help="build the first-party editor")
    add_arch(sp)
    add_variant(sp, ["release", "debug", "dist", "asan"], "release")
    sp.add_argument("--clean", action="store_true")
    sp.set_defaults(func=cmd_editor)

    sp = sub.add_parser("host-tools", help="build host jce_pak/jce_cook/jce_bin2obj")
    add_arch(sp)
    sp.set_defaults(func=cmd_host_tools)

    sp = sub.add_parser("cook", help="cook a project's raw assets")
    sp.add_argument("project")
    sp.set_defaults(func=cmd_cook)

    sp = sub.add_parser("serve", help="serve the web build")
    sp.add_argument("--port", type=int, default=None)
    sp.add_argument("--host", default=None)
    sp.add_argument("--dir", "--directory", dest="directory", default=None)
    sp.add_argument("--entry", default=None)
    sp.add_argument("--no-open", action="store_true")
    sp.set_defaults(func=cmd_serve)

    sp = sub.add_parser("lint", help="run the lint suite")
    sp.set_defaults(func=cmd_lint)

    sp = sub.add_parser("targets", help="print the desktop target matrix")
    sp.set_defaults(func=cmd_targets)

    sp = sub.add_parser("checksums",
                        help="aggregate dist editor binaries into one verifiable SHA256SUMS")
    sp.add_argument("--dir",
                    help="hash all files in this already-collected release folder "
                         "(writes <dir>/SHA256SUMS); use after gathering every "
                         "platform's artifacts. Default: scan build/desktop/*/dist.")
    sp.add_argument("--out",
                    help="staging/output dir for default mode (default: dist/release)")
    sp.set_defaults(func=cmd_checksums)

    # package: stage redistributable bundles (replaces package-*.bat)
    pkg = sub.add_parser("package", help="stage redistributable bundles (editor/game/sdk)")
    pkg_sub = pkg.add_subparsers(dest="what", required=True)

    pe = pkg_sub.add_parser("editor", help="stage editor bundle (exe + sdk/ + README)")
    add_arch(pe)
    add_variant(pe, ["release", "dist"], "release")
    pe.add_argument("--skip-build", action="store_true",
                    help="reuse existing editor exe + SDK tree")
    pe.add_argument("--out", help="override output dir")
    pe.set_defaults(func=cmd_package_editor)

    pg = pkg_sub.add_parser("game", help="cook + build + stage a game bundle")
    pg.add_argument("project")
    add_arch(pg)
    add_variant(pg, ["release", "dist"], "release")
    pg.add_argument("--sdk", help="SDK root (else JCE_SDK_DIR / dist/sdk/...)")
    pg.add_argument("--target", help="CMake target (default: manifest 'target')")
    pg.add_argument("--exe", help="built artifact filename")
    pg.add_argument("--name", help="bundle name (default: manifest 'name' / dir)")
    pg.add_argument("--version", help="bundle version (default: manifest 'version')")
    pg.add_argument("--out", help="override output dir")
    pg.add_argument("--clean", action="store_true")
    pg.add_argument("--with-loose", action="store_true",
                    help="also stage the loose cooked tree beside the exe "
                         "(default: single-file, assets embedded in the exe)")
    pg.set_defaults(func=cmd_package_game)

    ps = pkg_sub.add_parser("sdk", help="build + install the SDK (alias of `sdk`)")
    add_arch(ps)
    add_variant(ps, ["release", "dist", "both"], "both")
    ps.add_argument("--no-debug", action="store_true")
    ps.add_argument("--clean", action="store_true")
    ps.set_defaults(func=cmd_sdk)

    return p


def main(argv=None) -> int:
    global DRY_RUN, VERBOSE
    args = build_parser().parse_args(argv)
    DRY_RUN = getattr(args, "dry_run", False)
    VERBOSE = getattr(args, "verbose", False)
    if not hasattr(args, "clean"):
        args.clean = False
    # --dist is a shorthand: force the distribution variant when requested.
    if getattr(args, "dist", False):
        args.variant = "dist"
    args.func(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
