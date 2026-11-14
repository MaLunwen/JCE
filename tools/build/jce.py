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
# JCE_BUILD_CONSISTENCY_XP_V3: patched driver

import hashlib
import argparse
from contextlib import nullcontext
import datetime as _dt
import json
import os
import platform
import re
import secrets
import shutil
import subprocess
import time
import threading
import sys
import tempfile
from pathlib import Path

import functools
import importlib.util
import inspect

# Repo root = parent of this scripts/ directory.
ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

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
    "linux-x86":     dict(host="linux",   arch="i686",    bits=32, eng_arch="x86",   profile="linux-x86",     preset_stem="linux-x86"),
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


def build_jobs(env: dict | None = None) -> str:
    """Shared compiler/Conan job budget; defaults to eight.

    An explicit -j on the command line WINS over CMAKE_BUILD_PARALLEL_LEVEL,
    so a call site that hard-codes one silently ignores the standard knob.
    Every such call site goes through here instead.

    The override exists because a machine that cannot afford 8 gets no warning
    when it tries: on Windows the failure is a commit-limit exhaustion that
    surfaces as `D8027 cannot execute c2.dll`, `LNK1102 out of memory` and
    `LNK1171 ... error code 1455` -- three messages, one cause, and only one
    of them says anything about memory.
    """
    raw = (os.environ if env is None else env).get("CMAKE_BUILD_PARALLEL_LEVEL", "").strip()
    if raw.isdigit() and int(raw) > 0:
        return raw
    return "8"


def conan_jobs_args(env: dict) -> list[str]:
    """Apply the shared job budget to both Conan graph contexts."""
    return ["-c:a", "tools.build:jobs=" + build_jobs(env)]


def ctest_jobs(explicit=None) -> str:
    """The -j value for a `ctest` we pass -j to explicitly.

    THE SAME BLIND SPOT ONE TOOL OVER, and a more expensive one.  ctest does
    not read CMAKE_BUILD_PARALLEL_LEVEL at all -- its knob is
    CTEST_PARALLEL_LEVEL -- and an explicit -j on the command line wins over
    that too.  So every site here that hard-coded 8 was silently ignoring the
    only standard way to ask for fewer.

    Why it costs more than the compiler's: -j 8 here runs EIGHT TEST
    EXECUTABLES at once, and this repository's suite includes tests that hold
    over a gigabyte.  On Windows the binding constraint is the commit limit
    rather than physical memory, so the eviction arrives as MemoryError,
    0xC0000142, or a process that simply is not there any more -- none of
    which mentions memory or tests.

    An argument the caller actually passed still wins: a person typing -j 4
    means it, and an environment variable must not override a request.
    """
    if explicit is not None:
        return str(explicit)
    raw = os.environ.get("CTEST_PARALLEL_LEVEL", "").strip()
    if raw.isdigit() and int(raw) > 0:
        return raw
    return "8"


def _jce_progress_interval() -> float:
    # JCE_BUILD_CONSISTENCY_XP_V3: all jce.py commands share a live heartbeat.
    raw = os.environ.get("JCE_BUILD_PROGRESS_INTERVAL", "12")
    try:
        value = float(raw)
    except ValueError:
        return 12.0
    return value if 0 < value <= 3600 else 12.0


def run(cmd, env=None, cwd=None) -> None:
    """Unified live step: preserve child output, show timing and idle heartbeat.

    Native Conan/CMake/Ninja output is inherited, never buffered or stripped.
    XP's dedicated stage executor additionally owns detailed JSON receipts.
    """
    shown = cmd if isinstance(cmd, str) else " ".join(str(c) for c in cmd)
    log(f"$ {shown}")
    if DRY_RUN:
        log("STEP SKIP status=DRY_RUN")
        return
    words = shown.split()
    label = " ".join(words[:2])[:110] if words else "command"
    started = time.monotonic()
    completed = threading.Event()
    def heartbeat():
        while not completed.wait(_jce_progress_interval()):
            log(f"STEP RUNNING {label}: elapsed={time.monotonic() - started:.0f}s")
    watcher = threading.Thread(target=heartbeat, name="jce-build-heartbeat", daemon=True)
    watcher.start()
    log(f"STEP BEGIN {label}")
    try:
        rc = subprocess.run(cmd, env=env, cwd=str(cwd) if cwd else None,
                            shell=isinstance(cmd, str)).returncode
    except BaseException:
        log(f"STEP ABORTED {label}: elapsed={time.monotonic() - started:.1f}s")
        raise
    finally:
        completed.set()
        watcher.join(timeout=2)
    elapsed = time.monotonic() - started
    log(f"STEP END {label}: status={'OK' if rc == 0 else 'FAILED'} "
        f"exit={rc} elapsed={elapsed:.1f}s")
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
    """Variant-isolated desktop preset CMake tree (not the embedded Editor)."""
    from tools.build.layout_contract import preset_cmake_dir
    return preset_cmake_dir(ROOT, t["preset_stem"], kind)


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
        return ROOT / "build" / "web" / "wasm" / "conan" / "sdk"
    # Preset generators are separated from embedded-editor Release/Dist.
    from tools.build.layout_contract import preset_conan_dir
    return preset_conan_dir(ROOT, t["key"])


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
                 "-property", "installationPath"], text=True,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
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
    return None if target_arch else (installs[0] if installs else None)


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


def _msvc_env_ready(env: dict) -> bool:
    """True only for a complete MSVC command-line environment."""
    path = env.get("PATH") or env.get("Path") or ""
    return bool(path and shutil.which("cl", path=path) and
                env.get("INCLUDE") and env.get("LIB"))


def msvc_env(t: dict) -> dict:
    """Return an environment dict with MSVC activated for target arch (Windows)."""
    if HOST != "windows":
        return dict(os.environ)
    arch = vcvars_arch(t)
    if arch in _MSVC_ENV_CACHE:
        return _MSVC_ENV_CACHE[arch]
    current = dict(os.environ)
    if not t["cross"] and _msvc_env_ready(current):
        _MSVC_ENV_CACHE[arch] = current
        return _MSVC_ENV_CACHE[arch]
    vc = find_vcvars(t["arch"])
    if not vc:
        if t["cross"]:
            die("cross-compile refused: target MSVC vcvarsall.bat is missing; "
                "install matching ARM64 build tools and Windows SDK")
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
            "PROCESSOR_ARCHITECTURE", "COMPUTERNAME", "OS", "CONAN_HOME",
            "JAVA_HOME", "CMAKE_BUILD_PARALLEL_LEVEL",
            "CMAKE_POLICY_VERSION_MINIMUM", "JCE_SYSTEM_PACKAGE_MODE",
            "JCE_SYSTEM_PACKAGE_SUDO",
            "JCE_GRAPHICS_API_TIER", "JCE_BGFX_CPU_PROFILER",
            "JCE_BGFX_DEBUG_ANNOTATION", "JCE_SHADERC_EXECUTABLE", "BGFX_SHADERC"}
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
        f'"{vc}" {arch} -vcvars_ver={_VC_TOOLSET_PREFIX} >nul 2>&1 && set', shell=True, text=True, env=base)
    env = {}
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    if not _msvc_env_ready(env):
        die(f"vcvarsall.bat did not produce a complete MSVC environment for "
            f"'{arch}' (cl/INCLUDE/LIB required).")
    if t["arch"] == "aarch64":
        # Target name, active tools and linker must all agree. This guard does
        # NOT certify package ABIs or a successful ARM64 compilation.
        target_arch = env.get("VSCMD_ARG_TGT_ARCH", "").lower()
        resolved_cl = shutil.which("cl.exe", path=env.get("PATH", ""))
        resolved_link = shutil.which("link.exe", path=env.get("PATH", ""))
        def _arm64_tool(value, executable):
            return bool(value and
                str(value).replace("\\", "/").lower().endswith("/arm64/" + executable))
        if (target_arch != "arm64" or
            not _arm64_tool(resolved_cl, "cl.exe") or
            not _arm64_tool(resolved_link, "link.exe")):
            die("ARM64 MSVC environment refused: VSCMD target and cl/link paths "
                "must be ARM64; x64 fallback is not permitted")
    _MSVC_ENV_CACHE[arch] = env
    return _MSVC_ENV_CACHE[arch]


# ── Conan ─────────────────────────────────────────────────────────────────
GRAPHICS_API_TIERS = ("stable", "modern", "current", "compatibility")


def graphics_api_tier(env: dict | None = None) -> str:
    source = env if env is not None else os.environ
    tier = source.get("JCE_GRAPHICS_API_TIER", "stable").strip().lower()
    if tier not in GRAPHICS_API_TIERS:
        die("JCE_GRAPHICS_API_TIER must be stable, modern, current, or compatibility "
            f"(got {tier!r})")
    return tier


def bgfx_graphics_conan_args(env: dict) -> list[str]:
    profiler = env.get("JCE_BGFX_CPU_PROFILER", "off").strip().lower()
    if profiler not in ("off", "on"):
        die("JCE_BGFX_CPU_PROFILER must be off or on " + f"(got {profiler!r})")
    annotation = env.get("JCE_BGFX_DEBUG_ANNOTATION", "auto").strip().lower()
    if annotation not in ("auto", "off", "on"):
        die("JCE_BGFX_DEBUG_ANNOTATION must be auto, off or on " + f"(got {annotation!r})")
    return ["-c", "user.jce:bgfx_graphics_tier=" + graphics_api_tier(env),
            "-c", "user.jce:bgfx_cpu_profiler=" + profiler,
            "-c", "user.jce:bgfx_debug_annotation=" + annotation]


def system_package_conan_args(env: dict) -> list[str]:
    """Explicit dependency-manager policy; never install host packages by default."""
    mode = env.get("JCE_SYSTEM_PACKAGE_MODE", "check").strip().lower()
    if mode not in ("check", "install", "report"):
        die("JCE_SYSTEM_PACKAGE_MODE must be check, install, or report")
    sudo = env.get("JCE_SYSTEM_PACKAGE_SUDO", "0").strip()
    if sudo not in ("0", "1"):
        die("JCE_SYSTEM_PACKAGE_SUDO must be 0 or 1")
    return ["-c", "tools.system.package_manager:mode=" + mode,
            "-c", "tools.system.package_manager:sudo=" + ("True" if sudo == "1" else "False")]


def graphics_tier_stamp(toolchain: Path) -> Path:
    return toolchain.parent / ".jce-bgfx-graphics-tier"


def graphics_tier_matches(toolchain: Path, tier: str) -> bool:
    stamp = graphics_tier_stamp(toolchain)
    if not toolchain.exists() or not stamp.is_file():
        return False
    try:
        return stamp.read_text(encoding="ascii").strip() == tier + ";pristine-v1"
    except OSError:
        return False


def stamp_graphics_tier(toolchain: Path, tier: str) -> None:
    if DRY_RUN:
        return
    graphics_tier_stamp(toolchain).write_text(tier + ";pristine-v1\n", encoding="ascii")


def sync_conan_hooks(env: dict | None = None) -> None:
    """Synchronize owned configuration hooks; preserve all upstream files."""
    from tools.build.conan_config import sync_hooks
    try:
        sync_hooks(ROOT, dict(os.environ) if env is None else env,
                   dry_run=DRY_RUN, log=log)
    except ValueError as error:
        die(str(error))


def export_conan_recipes(env: dict) -> None:
    """Export conan/recipes/*/ into the local cache before an install.

    A LOCAL recipe packages an upstream that Conan Center cannot supply.
    Hooks configure toolchains; they never repair dependency code. An example
    is quickjs-ng: conan-center's `quickjs` is Bellard's fork and its recipe
    raises ConanInvalidConfiguration on msvc, and that refusal is honest
    (Makefile-only, no MSVC support), so no hook could fix it.

    Exported EVERY time rather than only when absent: an export is cheap and
    idempotent, and a stale recipe in the cache silently building an old
    dependency is the failure this ordering exists to prevent.
    """
    src = ROOT / "conan" / "recipes"
    if not src.is_dir():
        return
    recipes = sorted(p for p in src.iterdir() if (p / "conanfile.py").is_file())
    if not recipes:
        return
    log(f"exporting {len(recipes)} local conan recipe(s)")
    if DRY_RUN:
        return
    for r in recipes:
        # The version comes from conandata.yml's single `sources` key, so the
        # recipe and its pinned source cannot disagree about which one this is.
        ver = None
        cd = r / "conandata.yml"
        if cd.is_file():
            for line in cd.read_text(encoding="utf-8").splitlines():
                t2 = line.strip()
                if t2.startswith('"') and t2.endswith('":'):
                    ver = t2.strip('":')
                    break
        args = ["conan", "export", str(r)]
        if ver:
            args += ["--version", ver]
        run(args, env=env, cwd=ROOT)


def conan_generation_fingerprint(command: list[str], env: dict) -> str:
    """Hash owned graph inputs and the exact install configuration, not packages."""
    spec = importlib.util.spec_from_file_location(
        "jce_conan_configuration_digest", Path(__file__).with_name("conan_config.py"))
    configuration = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(configuration)
    try:
        configuration_sha256 = configuration.configuration_digest(
            env.get("CONAN_HOME") or str(Path.home() / ".conan2"))
    except (OSError, ValueError) as error:
        die(f"cannot fingerprint selected Conan configuration: {error}")
    inputs = [ROOT / "conanfile.py", ROOT / "conan.lock",
              ROOT / "contracts/conan-source-policy.json"]
    inputs += sorted(p for p in (ROOT / "conan").rglob("*")
                     if p.is_file() and "__pycache__" not in p.parts)
    files = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
             if p.is_file() else None for p in inputs}
    relevant_env = {key: value for key, value in env.items()
                    if key.startswith(("CONAN_", "CMAKE_", "ANDROID_", "EMSDK", "EMSCRIPTEN"))
                    or key in ("CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS",
                               "JCE_WEB_CONAN_CONFIG_SHA256")}
    payload = {"schema": 1, "command": command, "files": files,
               "configuration_sha256": configuration_sha256,
               "environment": relevant_env}
    return hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()


def _cache_scope(paths):
    # Load by owned module path: the public entry point uses exec, not a package.
    module_name = "jce_build_cache_lock"
    module = sys.modules.get(module_name)
    if module is None:
        spec = importlib.util.spec_from_file_location(
            module_name, Path(__file__).with_name("cache_lock.py"))
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
    return module.cache_locks(paths, ROOT / "build/.locks", notify=log)


def _host_cache_paths():
    return [ROOT / "build/host", ROOT / "build/host-conan"]


def _cache_owned(resources):
    """Hold ownership across the entire configure/build/install/test lifecycle."""
    def decorate(function):
        signature = inspect.signature(function)
        @functools.wraps(function)
        def owned(*args, **kwargs):
            if DRY_RUN:
                return function(*args, **kwargs)
            bound = signature.bind(*args, **kwargs)
            bound.apply_defaults()
            with _cache_scope(resources(bound.arguments)):
                return function(*args, **kwargs)
        return owned
    return decorate


def _sdk_cache_paths(values):
    target, variant, config = values["t"], values["variant"], values["config"]
    kind = "debug" if config == "Debug" else f"{variant}-sdk"
    tree = preset_binary_dir(target, kind)
    if target.get("emscripten"):
        tree = ROOT / "build/web/wasm/obj/sdk" / kind
    prefix = sdk_install_dir(target, variant)
    if config == "Debug":
        prefix = prefix.parent / (prefix.name + "-debug")
    host_tools = _host_cache_paths() if target.get("cross") or target.get("emscripten") else []
    return [tree, conan_dir(target), prefix, *host_tools]


def _authoring_editor_paths(target, variant):
    root = ROOT / "build/desktop" / target["key"] / "authoring"
    return root / "obj" / ("release" if variant == "dist" else variant), root / variant


def _editor_cache_paths(values):
    args = values["args"]
    target = resolve_target(args.arch)
    if _delivery_module().profile(args) == "embedded":
        owner = _standalone_module()
        driver = argparse.Namespace(**globals())
        plan = owner.native_cache_plan(driver, args, target)
        args._native_cache_plan = plan
        return [
            plan["build"],
            plan["conan"],
            owner.editor_artifact_dir(driver, args, target),
            *(_host_cache_paths() if target.get("cross") else []),
        ]
    tree, output = _authoring_editor_paths(target, args.variant)
    override = getattr(args, "artifact_dir", None)
    host_tools = _host_cache_paths() if target.get("cross") or target.get("emscripten") else []
    return [tree, conan_dir(target), Path(override).resolve() if override else output, *host_tools]


def _test_ai_dispatch_flags(args) -> list[str]:
    """Only an explicit configure request may change this optional module."""
    selected = getattr(args, "ai_dispatch", None)
    if selected is None:
        return []
    if selected not in ("on", "off"):
        die("test: invalid --ai-dispatch (expected on or off)")
    if not getattr(args, "configure", False):
        die("test: --ai-dispatch requires --configure")
    return ["-DJCE_ENABLE_AI_DISPATCH=" + selected.upper()]


def _test_cache_paths(values):
    args = values["args"]
    # Refuse invalid scope changes before even acquiring build/cache ownership.
    _test_ai_dispatch_flags(args)
    target = resolve_target(getattr(args, "arch", None))
    return [preset_binary_dir(target, getattr(args, "variant", "release")),
            conan_dir(target)]


def _sdk_host_tools(sdk: Path, target: dict) -> dict[str, Path]:
    """Select executable build-host tools from this SDK, never another install."""
    ext = ".exe" if HOST == "windows" else ""
    directories = ("bin/host", "bin") if target.get("cross") or target.get("emscripten") else ("bin", "bin/host")
    selected = {}
    for name in ("jce_cook", "jce_pak", "jce_bin2obj"):
        for directory in directories:
            candidate = Path(sdk) / directory / (name + ext)
            if not candidate.is_file():
                continue
            try:
                probe = subprocess.run([str(candidate), "--help"],
                                       stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL, timeout=30)
            except (OSError, subprocess.SubprocessError):
                continue
            # Existing tools may return a conventional usage error. Signals,
            # Windows loader failures and other abnormal exits are not probes.
            if probe.returncode in (0, 1, 2):
                selected[name] = candidate
                break
    return selected


def _confirmed_app_sdk_tools(args, sdk: Path, target: dict) -> dict[str, Path]:
    """Recheck under SDK ownership; never introduce an unowned host fallback."""
    previous = getattr(args, "_sdk_host_tools", None)
    current = _sdk_host_tools(sdk, target)
    if previous is not None and len(previous) == 3 and current != previous:
        die("SDK host tools changed while waiting for ownership; retry app build")
    return current


def _app_build_dir(project: Path, target: dict, variant: str) -> Path:
    if target.get("emscripten"):
        return project / "build" / "web" / "wasm" / "obj" / "app" / variant
    return project / "build/desktop" / target["key"] / variant


def _app_cache_paths(values):
    args = values["args"]
    target = resolve_target(args.arch)
    sdk = resolve_sdk(args, target)
    args._sdk_host_tools = _sdk_host_tools(sdk, target)
    host_paths = [] if len(args._sdk_host_tools) == 3 else _host_cache_paths()
    project = Path(args.project).resolve()
    return [_app_build_dir(project, target, args.variant),
            project / "build/delivery-admission", sdk, *host_paths]


def _smoke_build_dir(t, sdk, variant, build_dir=None, scripting=False):
    family = "web" if t.get("emscripten") else "desktop"
    role = "scripting" if scripting else "c99"
    directory = (Path(build_dir).resolve() if build_dir else
                 ROOT / "build" / family / t["key"] / "smoke" / role / variant)
    sdk = Path(sdk).resolve()
    if (directory == ROOT or directory in ROOT.parents or
            (directory.is_relative_to(ROOT) and
             not directory.is_relative_to(ROOT / "build")) or
            directory == sdk or directory.is_relative_to(sdk) or
            sdk.is_relative_to(directory)):
        die(f"smoke: unsafe consumer build directory {directory}")
    return directory


def _smoke_cache_paths(values, scripting=False):
    return [_smoke_build_dir(values["t"], values["sdk"], values["variant"],
                             values.get("build_dir"), scripting), values["sdk"]]


def web_toolchain_conan_args(em: dict, env: dict) -> list[str]:
    """Probe actual compiler identity before constructing reusable graph inputs."""
    web_path = Path(__file__).with_name("web_toolchain.py")
    web_spec = importlib.util.spec_from_file_location("jce_web_toolchain", web_path)
    web = importlib.util.module_from_spec(web_spec)
    web_spec.loader.exec_module(web)
    try:
        identity = web.probe_toolchain(em, env)
        host_args = web.conan_host_args(identity, ROOT / "conan/profiles/wasm")
        if not DRY_RUN:
            overlay_spec = importlib.util.spec_from_file_location(
                "jce_conan_config", Path(__file__).with_name("conan_config.py"))
            overlay = importlib.util.module_from_spec(overlay_spec)
            overlay_spec.loader.exec_module(overlay)
            config = overlay.prepare_toolchain_home(
                env.get("CONAN_HOME", str(Path.home() / ".conan2")),
                ROOT / "build/conan-config/web", identity["clang_major"])
            env["CONAN_HOME"] = config["home"]
            env["JCE_WEB_CONAN_CONFIG_SHA256"] = hashlib.sha256(
                json.dumps(config, sort_keys=True).encode()).hexdigest()
            log(f"Web Conan configuration overlay: {config['home']}; shared store: {config['storage']}")
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        die(f"invalid Web toolchain identity: {error}")
    log(f"Web compiler emcc {identity['emcc_version']}, ABI {identity['abi_sha256']}")
    return host_args


def conan_rebuild_names(names) -> tuple[str, ...]:
    """Admit explicit package names without accepting Conan reference patterns."""
    if isinstance(names, (str, bytes)):
        raise ValueError("dependency rebuild names must be a sequence of package names")
    result = []
    for name in names:
        if not isinstance(name, str) or re.fullmatch(r"[a-z0-9][a-z0-9_.+-]{0,99}", name) is None:
            raise ValueError("dependency rebuild requires a simple lowercase Conan package name "
                             "(1..100 characters), not a reference or pattern")
        if name not in result:
            result.append(name)
    return tuple(result)


@_cache_owned(lambda values: [conan_dir(values["t"])])
def conan_install(t: dict, config: str, env: dict, *, rebuild_dependencies=()) -> Path:
    """Reuse generated Conan metadata only while all owned graph inputs match."""
    rebuild_dependencies = conan_rebuild_names(rebuild_dependencies)
    env = dict(env)
    env.setdefault("CMAKE_POLICY_VERSION_MINIMUM", "3.5")
    tc = toolchain_path(t, config)
    graphics_tier = graphics_api_tier(env)
    if t.get("emscripten") and graphics_tier != "stable":
        die("WebGL 2 maps to the stable GLES 3.0 tier; "
            f"--graphics-api-tier {graphics_tier} is not a Web target")
    # Build the actual configuration before deciding whether it is reusable.
    if t.get("emscripten"):
        em = emscripten_paths()
        env["EMSCRIPTEN"] = str(Path(em["cc"]).parent)
        host_args = web_toolchain_conan_args(em, env)
        command = ["conan", "install", ".",
                   *host_args,
                   "-pr:b", f"conan/profiles/{host_build_profile()}",
                   "-c", f"tools.cmake.cmaketoolchain:user_toolchain=['{em['toolchain']}']",
                   "-c", ("tools.build:compiler_executables="
                          f"{{'c': '{em['cc']}', 'cpp': '{em['cxx']}'}}"),
                   *bgfx_graphics_conan_args(env), *system_package_conan_args(env),
                   *conan_jobs_args(env),
                   "--output-folder", str(conan_dir(t)), "--build=missing"]
    else:
        extra = ["-s", "build_type=Debug"] if config == "Debug" else []
        if config == "Debug" and t["host"] == "windows":
            extra += ["-s", "compiler.runtime_type=Debug"]
        from tools.build.conan_msvc import configuration as msvc_configuration
        vs_conf = msvc_configuration(t, env, host=HOST, arch_dirs=_ARCH_CLDIR,
                                     toolset_prefix=_VC_TOOLSET_PREFIX, dry_run=DRY_RUN)
        command = ["conan", "install", ".",
                   "-pr:h", f"conan/profiles/{t['profile']}",
                   "-pr:b", f"conan/profiles/{host_build_profile()}",
                   "--output-folder", str(conan_dir(t)), "--build=missing",
                   *bgfx_graphics_conan_args(env),
                   *conan_jobs_args(env),
                   *system_package_conan_args(env), *vs_conf, *extra]
    sync_conan_hooks(env)
    fingerprint = conan_generation_fingerprint(command, env)
    stamp = tc.parent / ".jce-conan-generation"
    try:
        current = stamp.read_text(encoding="ascii").strip()
    except OSError:
        current = None
    if not rebuild_dependencies and graphics_tier_matches(tc, graphics_tier) and current == fingerprint:
        log(f"Conan metadata inputs unchanged ({config}): {tc} (reuse)")
        return tc
    if rebuild_dependencies:
        log("Conan explicit dependency rebuild requested: " + ", ".join(rebuild_dependencies))
    else:
        log(f"Conan metadata changed, missing or unstamped ({config}); regenerating")
    # Failed installs must never bless partially rewritten metadata. Preserve
    # build trees and packages; invalidate only our own success marker.
    if not DRY_RUN:
        stamp.unlink(missing_ok=True)
    export_conan_recipes(env)
    # Rebuilding is a one-invocation action, not a different dependency graph.
    # Keep the normal fingerprint so the next ordinary preparation can reuse it.
    run([*command, *("--build=" + name + "/*" for name in rebuild_dependencies)], env=env, cwd=ROOT)
    if not tc.exists() and not DRY_RUN:
        die(f"conan toolchain not generated: {tc}")
    stamp_graphics_tier(tc, graphics_tier)
    if not DRY_RUN:
        stamp.write_text(fingerprint + "\n", encoding="ascii")
    return tc


# ── shaderc + host tools (cross prerequisites) ────────────────────────────
def _tool_runs(path: Path) -> bool:
    """Return whether a generic build tool can start on this host.

    JCE cookers intentionally return a usage error for ``--help``; launch
    success, not the tool-specific exit code, is the generic contract.
    """
    try:
        subprocess.run([str(path), "--help"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30)
        return True
    except (OSError, subprocess.SubprocessError):
        return False


def _shaderc_runs(path: Path) -> bool:
    """Return whether bgfx shaderc is native to and healthy on this host."""
    try:
        proc = subprocess.run([str(path), "--version"],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=30)
        return proc.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def find_host_shaderc() -> Path | None:
    exe_name = "shaderc.exe" if HOST == "windows" else "shaderc"
    for variable in ("JCE_SHADERC_EXECUTABLE", "BGFX_SHADERC"):
        explicit = os.environ.get(variable)
        if not explicit:
            continue
        candidate = Path(explicit)
        if candidate.is_file() and _shaderc_runs(candidate):
            return candidate
        log(f"ignoring non-runnable {variable}: {candidate}")

    # Search the live Conan cache, never generator files: generated package
    # paths can outlive the package directory after Conan garbage collection.
    # A cache may contain x64 and ARM64 shaderc.exe side by side, so every
    # candidate is execution-probed before it can enter a cross build.
    try:
        home = os.environ.get("CONAN_HOME") or str(Path.home() / ".conan2")
        cache = Path(home) / "p" / "b"
        if cache.is_dir():
            cands: list[Path] = []
            for pkg in cache.glob("bgfx*"):
                for sub in (pkg / "b" / "build" / "Debug" / "cmake" / "bgfx" / exe_name,
                            pkg / "b" / "build" / "Release" / "cmake" / "bgfx" / exe_name,
                            pkg / "p" / "bin" / exe_name):
                    if sub.is_file() and _shaderc_runs(sub):
                        cands.append(sub)
            if cands:
                # newest first — most likely to match the current bgfx version
                cands.sort(key=lambda p: p.stat().st_mtime, reverse=True)
                log(f"runnable host shaderc from Conan cache: {cands[0]}")
                return cands[0]
    except Exception:
        pass

    for base in (ROOT / "dist" / "sdk", ROOT / "build" / "host" / "tools"):
        pattern = f"*/bin/{exe_name}" if base.name == "sdk" else exe_name
        for candidate in base.glob(pattern):
            if candidate.is_file() and _shaderc_runs(candidate):
                return candidate
    return None


def find_emsdk() -> Path | None:
    """Locate an emsdk checkout WITHOUT naming one machine's disk.

    In order: $EMSDK (what `emsdk_env` exports); the emcc wrapper already on
    PATH, walked back to its checkout; then a sibling or in-tree checkout
    named relative to THIS repository.  A literal absolute path used to sit
    at the end of that list, and it was one developer's D: drive -- on every
    other machine it turned "you have not activated emsdk" into "the
    toolchain is at a path that does not exist", which is the same failure
    wearing a confusing message.
    """
    env = os.environ.get("EMSDK")
    if env and (Path(env) / "upstream" / "emscripten").is_dir():
        return Path(env)

    # emsdk_env puts <emsdk>/upstream/emscripten on PATH, so the wrapper
    # names the checkout: <emsdk>/upstream/emscripten/emcc[.bat]
    for exe in ("emcc.bat", "emcc"):
        found = shutil.which(exe)
        if found:
            root = Path(found).resolve().parents[2]
            if (root / "upstream" / "emscripten").is_dir():
                return root

    # Conventional checkouts, stated RELATIVE to this repo so they mean the
    # same thing on every machine.
    for rel in (ROOT.parent / "emsdk",
                ROOT.parent / "cross_platform" / "emsdk",
                ROOT / "third_party" / "emsdk"):
        if (rel / "upstream" / "emscripten").is_dir():
            return rel
    return None


def emscripten_paths() -> dict:
    """Resolve the Emscripten toolchain + compiler wrappers.

    All paths use forward slashes so they survive conan's -c args and a CMake
    cache on Windows.
    """
    found = find_emsdk()
    if not found:
        die("emsdk not found. Activate it (`emsdk_env`), set $EMSDK, or put a "
            "checkout beside this repo as ../emsdk. Looked at: $EMSDK, emcc "
            "on PATH, ../emsdk, ../cross_platform/emsdk, third_party/emsdk.")
    emsdk = str(found).replace("\\", "/")
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


@_cache_owned(lambda values: _host_cache_paths())
def ensure_host_tools(env: dict, tools=("jce_pak",)) -> dict[str, Path]:
    """Build host (native) tools into build/host/tools; return {name: path}.

    Always builds with the NATIVE host target's env — NOT the caller's `env`,
    which on a cross build (e.g. arm64) targets the other arch and would give
    the host tools the wrong LIB/cl (LNK1104 'cannot open LIBCMT.lib').
    """
    # ALWAYS build; do not gate on the file existing.
    #
    # This asked `not host_tool(n).exists()`, which answers "is there a file"
    # and not "is it current".  Once a tool had been built, a change to its
    # source never rebuilt it: `jce.py host-tools` printed the path and
    # returned, and `jce.py cook` went on using a binary from whenever it was
    # first produced.  Measured: a fix to tools/jce_cook.c had no effect
    # across three cook runs, and the tool on disk was two days old while the
    # source was minutes old -- and every command reported success.
    #
    # Ninja already answers the real question in milliseconds when there is
    # nothing to do, so the cheap wrong predicate was not buying anything.
    missing = [n for n in tools if not host_tool(n).exists()]
    need = list(tools)
    if need:
        if missing:
            log(f"building host tools: {', '.join(missing)}")
        # Native host target. Use resolve_target (searches by host field) rather
        # than the key f"{HOST}-x64": on macOS HOST=='darwin' but the TARGETS key
        # is 'macos-x64', and the host may not even be x64 (Apple Silicon).
        ht = resolve_target(None); ht["cross"] = False
        host_env = msvc_env(ht)   # native x64 env; caller `env` may be cross-arch
        host_conan = ROOT / "build" / "host-conan"
        tc = host_conan / "build" / "Release" / "generators" / "conan_toolchain.cmake"
        if not tc.exists():
            sync_conan_hooks(host_env)
            export_conan_recipes(host_env)
            run(["conan", "install", ".",
                 "-pr:h", f"conan/profiles/{host_build_profile()}",
                 "-pr:b", f"conan/profiles/{host_build_profile()}",
                 *conan_jobs_args(host_env),
                 "--output-folder", str(host_conan), "--build=missing"],
                env=host_env, cwd=ROOT)
        run(["cmake", "-S", ".", "-B", "build/host", "-G", "Ninja",
             f"-DCMAKE_TOOLCHAIN_FILE={tc}", "-DCMAKE_BUILD_TYPE=Release",
             "-DJCE_BUILD_EDITOR=OFF", "-DJCE_BUILD_TESTS=OFF",
             "-DJCE_BUILD_SCRIPT_VM_PYTHON=OFF", "-DJCE_BUILD_SCRIPT_CSHARP=OFF",
             "-DJCE_BUILD_SCRIPT_JAVA=OFF",
             
             "-DJCE_ENABLE_CPPCHECK=OFF", "-DJCE_ENABLE_SDK_INSTALL=ON",
             "-DJCE_DELIVERY_PROFILE=authoring", "-DJCE_EDITOR_STANDALONE=OFF"],
            env=host_env, cwd=ROOT)
        run(["cmake", "--build", "build/host", "--target", *need,
             "-j", build_jobs(host_env)],
            env=host_env, cwd=ROOT)
    return {n: host_tool(n) for n in tools}


def cross_asset_tool_overrides(env: dict) -> list[str]:
    """Target builds always execute the same refreshed native asset tools."""
    tools = ensure_host_tools(env, ("jce_pak", "jce_bin2obj"))
    return [f"-DJCE_PAK_EXECUTABLE={tools['jce_pak']}",
            f"-DJCE_BIN2OBJ_EXECUTABLE={tools['jce_bin2obj']}"]



# ── Subcommand: shader-inspect ────────────────────────────────────────────
#
# The Shader Inspector panel's question, asked from a terminal: compile a .sc
# for one or more backends and report what came out.  Same two programs the
# panel uses -- bgfx's shaderc, then jce_shader_inspect, which reflects the
# blob with the engine's own jce_shader_reflect() -- so a number here and a
# number in the panel cannot disagree.
#
# It exists because the panel cannot be scripted: a CI job asking "did this
# shader's instruction count jump" and an assistant asking "which uniforms
# does fs_pbr actually declare" both need an answer without a window.

_SHADER_PROFILES = {
    # backend      platform   profile   disasm available (DirectX only)
    "d3d11":   ("windows", "s_5_0",  True),
    "vulkan":  ("linux",   "spirv",  False),
    "opengl":  ("linux",   "120",    False),
    "gles":    ("android", "300_es", False),
    "metal":   ("osx",     "metal",  False),
}


def _host_sdk_roots() -> list[Path]:
    """Installed SDKs that could run on THIS machine, best first.

    An SDK directory is named <os>-<arch>[-debug|-dist].  Sorting the glob
    and taking the first was wrong twice over on an x64 Windows box: it
    picked win32-aarch64 (CreateProcess: WinError 216) and then paired it
    with the wasm SDK's include directory, so the second wrong answer was
    hidden behind the first.  Rank by host arch, then prefer the plain
    release build over -debug / -dist.
    """
    mach = platform.machine().lower()
    if mach in ("amd64", "x86_64"):
        arch = "x86_64"
    elif mach in ("arm64", "aarch64"):
        arch = "aarch64"
    else:
        arch = mach

    roots = [p for p in (ROOT / "dist" / "sdk").glob("*") if p.is_dir()]

    def rank(p: Path) -> tuple:
        n = p.name
        return (0 if arch in n else 1,
                0 if n.count("-") == 1 else 1,   # plain before -debug/-dist
                n)

    return sorted((p for p in roots if arch in p.name or "-" not in p.name),
                  key=rank)


def _resolve_shaderc() -> Path | None:
    """Use the same execution-probed host tool as SDK and cross builds."""
    return find_host_shaderc()


def _resolve_shader_include(shaderc: Path | None = None) -> Path | None:
    """Prefer the include directory of the SAME SDK the shaderc came from:
    the two are a matched pair, and mixing them compiles a shader against
    another target's headers."""
    for var in ("JCE_SHADERC_INCLUDE_DIR", "BGFX_SHADER_INCLUDE_PATH"):
        v = os.environ.get(var)
        if v and Path(v).is_dir():
            return Path(v)
    if shaderc is not None:
        sibling = shaderc.parent.parent / "share" / "jce" / "shader_include"
        if sibling.is_dir():
            return sibling
    for root in _host_sdk_roots():
        p = root / "share" / "jce" / "shader_include"
        if p.is_dir():
            return p
    return None


def _find_shader_inspect() -> Path | None:
    """Prefer an already-built one over a fresh build: this subcommand is
    meant to be cheap enough to put in a loop."""
    # The build dir comes from the target matrix, not from a literal: this
    # used to say build/desktop/windows-x64/tools, so on linux-x64, on
    # macos-arm64 and on a windows-arm64 cross it silently skipped a build
    # that was sitting right there.
    t = resolve_target(None)
    dirs = [preset_binary_dir(t, k) / "tools" for k in ("release", "debug")]
    dirs.append(ROOT / "build" / "host" / "tools")
    for d in dirs:
        for name in ("jce_shader_inspect.exe", "jce_shader_inspect"):
            p = d / name
            if p.is_file():
                return p
    return None


def cmd_shader_inspect(args) -> None:
    src = Path(args.source)
    if not src.is_file():
        die(f"shader source not found: {src}")

    shaderc = _resolve_shaderc()
    if not shaderc:
        die("shaderc not found. Set JCE_SHADERC_EXECUTABLE, or run "
            "`python scripts/jce.py sdk` (the SDK ships one in bin/).")

    include = _resolve_shader_include(shaderc)
    if not include:
        die("shader include directory not found. Set JCE_SHADERC_INCLUDE_DIR "
            "to the directory containing bgfx_shader.sh.")

    varying = args.varying
    if not varying:
        varying = os.environ.get("JCE_SHADER_VARYING_DEF") \
                  or str(ROOT / "engine/shaders/pbr/varying_pbr.def.sc")
    if not Path(varying).is_file():
        die(f"varying.def.sc not found: {varying}  (pass --varying)")

    tool = _find_shader_inspect()
    if not tool:
        env = msvc_env(resolve_target(None))
        tool = ensure_host_tools(env, ("jce_shader_inspect",))["jce_shader_inspect"]

    backends = args.backend or ["d3d11"]
    unknown = [b for b in backends if b not in _SHADER_PROFILES]
    if unknown:
        die("unknown backend(s): " + ", ".join(unknown)
            + "\nknown: " + ", ".join(sorted(_SHADER_PROFILES)))

    with tempfile.TemporaryDirectory(prefix="jce_shader_inspect_") as tmp:
        for b in backends:
            platform, profile, can_disasm = _SHADER_PROFILES[b]
            out = Path(tmp) / f"{src.stem}_{b}.bin"
            cmd = [str(shaderc), "-f", str(src), "-o", str(out),
                   "--type", args.type, "--platform", platform,
                   "-p", profile, "--varyingdef", str(varying),
                   # ORDER MATTERS, and it used to be backwards.  The SDK's
                   # flat share/jce/shader_include carries a COPY of every
                   # engine .sh, so listing it first meant inspecting a shader
                   # in the working tree compiled dist/'s snapshot of its
                   # includes: an `#error` added to engine/shaders/pbr/
                   # fs_pbr_main.sh did not reach the compiler and the tool
                   # still reported 8311 instructions, happily.  Every number
                   # it gave for an uninstalled edit was about dist/.
                   # The source's OWN directory first, then the shader tree,
                   # then the SDK -- which is now only reached for bgfx's own
                   # headers, the one thing the tree does not carry.
                   "-i", str(src.parent.resolve()),
                   "-i", str(ROOT / "engine" / "shaders"),
                   "-i", str(ROOT / "engine" / "shaders" / "ssr"),
                   "-i", str(include),
                   "-O", str(args.optimise)]
            if can_disasm:
                cmd.append("--disasm")
            log(f"[{b}] {' '.join(cmd)}")
            if DRY_RUN:
                continue
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0 or not out.is_file():
                # shaderc says why on stderr; passing it through beats a
                # "compilation failed" with the reason thrown away.
                sys.stderr.write(r.stderr or r.stdout)
                die(f"[{b}] shaderc failed (exit {r.returncode})")

            inspect = [str(tool), str(out)]
            if args.json: inspect.append("--json")
            if args.code: inspect.append("--code")
            disasm = out.with_suffix(".bin.disasm")
            if can_disasm and disasm.is_file():
                inspect += ["--disasm", str(disasm)]
            subprocess.run(inspect, check=True)


# ── Subcommand: targets ───────────────────────────────────────────────────
def cmd_design(args) -> None:
    """Author or validate a scene with a language model.

    Forwards to private/tools/ai/jce_design.py with argv untouched, so the two entry
    points cannot drift.  Two properties worth repeating at this layer,
    because this is the one people type:

      * WITHOUT --send NOTHING LEAVES THIS MACHINE.  The default prints the
        exact request instead.  A scene brief carries the shape of an
        unreleased game and the tool must not make that call for anyone.
      * --check needs no key and no network at all.  It runs the same
        validation on a scene somebody else wrote, which is the half that says
        whether the JSON is real.
    """
    tool = ROOT / "private/tools/ai/jce_design.py"
    if not tool.is_file():
        die("missing %s" % tool)

    # Taken from sys.argv rather than from a parsed argument.  argparse.REMAINDER
    # does not hold when the first thing after the subcommand is an OPTION: the
    # `design` subparser claims `--check` as its own and fails with
    # "unrecognized arguments" before the forwarding ever runs.  Slicing after
    # the subcommand token is unambiguous and keeps every flag defined in ONE
    # place -- private/tools/ai/jce_design.py -- so the two entry points cannot come to
    # mean different things.
    argv = list(sys.argv[1:])
    rest = argv[argv.index("design") + 1:] if "design" in argv else []
    rc = subprocess.call([sys.executable, str(tool)] + rest)
    if rc:
        raise SystemExit(rc)


def cmd_automation(args) -> None:
    """Drive JCE through the Automation API -- the surface agents call.

    Forwards to private/tools/automation/automation_cli.py with argv untouched, for the
    same reason `design` does: one place defines the flags, so the two entry
    points cannot come to mean different things.  Everything real lives there --
    the tool registry, the permission tiers, the changeset store -- and this
    function exists only because `jce.py` is where people already look.

    Three properties worth stating at the layer people type:

      * EVERY PROJECT WRITE HAPPENS INSIDE A CHANGESET.  Not a convention: a
        write without one is refused, because the changeset is what snapshots
        the previous bytes, and without those there is nothing to undo.
      * PACKAGE, PUBLISH, GIT PUSH AND NETWORK ARE REFUSED BY DEFAULT.  They
        need --allow, or an interactive confirmation.
      * `automation list` needs no build, no SDK and no configuration, which is
        the state anything meeting a project for the first time is in.
    """
    tool = ROOT / "private/tools/automation/automation_cli.py"
    if not tool.is_file():
        die("missing %s" % tool)
    # Sliced out of sys.argv rather than taken from a parsed argument: the
    # subparser would otherwise claim --json, --project and --allow as its own
    # and fail with "unrecognized arguments" before forwarding ever ran.  Same
    # reasoning as cmd_design, and the same single definition site.
    argv = list(sys.argv[1:])
    rest = argv[argv.index("automation") + 1:] if "automation" in argv else []
    rc = subprocess.call([sys.executable, str(tool)] + rest)
    if rc:
        raise SystemExit(rc)


def agent_bridge_paths(t: dict, variant: str = "release"):
    """Where the agent bridge builds to, and where its exe lands.

    Its OWN build directory, deliberately: several sessions work in this
    checkout at once and two concurrent builds into one directory corrupt each
    other rather than queueing.  Nothing here touches build/desktop/<key>.
    """
    bdir = ROOT / "build" / "desktop" / f"agent-bridge-{t['key']}-{variant}"
    exe = bdir / ("jce_agent_bridge.exe" if HOST == "windows"
                  else "jce_agent_bridge")
    return bdir, exe


def cmd_agent_bridge(args) -> None:
    """Build the engine-side bridge the agent layer calls.

    It answers the three questions only the engine can answer -- does this
    recipe compile and to what plan, does this scene behave physically, what
    does the engine accept -- and it is built as an SDK CONSUMER so that the
    answer comes through the packaged headers and library rather than from an
    in-tree build that proves less.
    """
    t = resolve_target(args.arch)
    sdk = Path(args.sdk).resolve() if args.sdk else sdk_install_dir(t, args.variant)
    jce_cmake = sdk / "lib" / "cmake" / "JCE"
    if not DRY_RUN and not (jce_cmake / "JCEConfig.cmake").exists():
        die(f"agent-bridge: no JCEConfig.cmake under {jce_cmake} — "
            "run `python scripts/jce.py sdk` first.")
    env = msvc_env(t)
    bdir, exe = agent_bridge_paths(t, args.variant)
    run(["cmake", "-S", str(ROOT / "tools" / "jce_agent_bridge"),
         "-B", str(bdir), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DJCE_DIR={jce_cmake}",
           *_delivery_module().cmake_flags(_delivery_module().profile(args))]
        + (["-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"]
           if HOST == "windows" else []),
        env=env, cwd=ROOT)
    run(["cmake", "--build", str(bdir)], env=env, cwd=ROOT)
    if DRY_RUN:
        return
    if not exe.exists():
        die(f"agent-bridge: build produced no executable at {exe}")
    log(f"agent-bridge: {exe}")
    print(exe)


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
_ROSTER_SET_RE = re.compile(
    r'^\s*set\s*\(\s*(JCE_BACKEND_[A-Z_]+|JCE_SCRIPT_API_STATIC)\s+"?([^")]*)"?\s*\)', re.M)


def _read_roster(path: Path) -> dict:
    """The set() values out of one jce_backend.cmake.

    A regex and not a CMake run, because this is a REPORT about an installed
    tree and must not need a configure to produce.  The rosters are plain
    set(NAME "value") by contract -- JCEScriptEnable.cmake says so, and
    include()s them, so a roster that grew logic would break there first and
    loudly, not here and silently.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}
    return {m.group(1): m.group(2).strip() for m in _ROSTER_SET_RE.finditer(text)}


def sdk_shipped_script_backends(install: Path, target: dict | None = None,
                                config: str | None = None) -> list:
    """Which language backends the INSTALLED SDK actually contains.

    Derived from the files on disk and not from the options this run passed: an
    option says what was asked for, and the question a consumer has is what
    arrived.

    AND FROM THE ROSTERS, not from a list written here.  This was a hardcoded
    tuple of three languages, and it silently omitted "c" -- jce_script_vm_c.lib
    ships and the report never said so, which put VERSION.txt ("backends
    shipped: cpp, python, java") in direct contradiction with the SDK's own
    config ("this SDK can run lua plus c;cpp;java;python").  The stamped
    artefact was the wrong one.

    A backend counts only when every piece it cannot run without is there.  The
    rosters declare those pieces, because they are per-language facts: java
    needs the JNI shim and the compiled com.jce.script classes as well as the
    archive, and an SDK with the archive alone produces UnsatisfiedLinkError or
    ClassNotFoundException inside the consumer's JVM, a long way from here.

    Necessary, not sufficient: the consumer still supplies the CPython or the
    JVM, and lib/cmake/JCE/JCEScripting.cmake is what decides that on their
    machine at their configure time.
    """
    libdir = install / "lib"
    library_pattern = (config + "/") if config else "*/"
    backends = install / "lib" / "cmake" / "JCE" / "backends"
    shipped = []
    config = install / 'lib/cmake/JCE/JCEScripting.cmake'
    api_static = (config.is_file() and
                  _read_roster(config).get('JCE_SCRIPT_API_STATIC', '').upper() in
                  ('1', 'ON', 'TRUE', 'YES'))

    def has_link_library(stem: str) -> bool:
        return (any(libdir.glob(library_pattern + stem + ".lib")) or
                any(libdir.glob(library_pattern + "lib" + stem + ".a")) or
                any(libdir.glob(library_pattern + stem + ".a")))

    def has_shared_library(stem: str) -> bool:
        return (any(libdir.glob(library_pattern + stem + ".dll")) or
                any(libdir.glob(library_pattern + "lib" + stem + ".so")) or
                any(libdir.glob(library_pattern + "lib" + stem + ".dylib")))

    for roster in sorted(backends.glob("*/jce_backend.cmake")):
        r = _read_roster(roster)
        lang = r.get("JCE_BACKEND_LANGUAGE", "")
        stem = r.get("JCE_BACKEND_VM_TARGET", "")
        if not lang or not stem:
            # Not silent: a roster the SDK installed and this cannot read is a
            # language the report will omit, which is the exact defect above.
            log(f"WARN: unreadable scripting roster {roster} — the shipped "
                "report will omit whatever language it declares")
            continue
        found = has_link_library(stem)
        # Extra pieces this backend cannot run without, named by its roster.
        if (found and
                r.get("JCE_BACKEND_NEEDS_SCRIPT_API", "").upper() in
                ("1", "ON", "TRUE", "YES")):
            # Native class backends may use a genuinely installed static API.
            # Foreign interpreter bindings still need the loadable transport;
            # an import archive alone is not a runnable shared library.
            static_eligible = api_static and lang in ('c', 'cpp')
            api_found = (has_link_library("jce_script_api") if static_eligible
                         else has_shared_library("jce_script_api"))
            if not api_found:
                found = False
        if found:
            for linked in filter(None, r.get("JCE_BACKEND_SDK_REQUIRES_LINK",
                                             "").split(";")):
                if not has_link_library(linked):
                    found = False
        if found:
            for shared in filter(None, r.get("JCE_BACKEND_SDK_REQUIRES_SHARED",
                                             "").split(";")):
                if not has_shared_library(shared):
                    found = False
        if found:
            for rel in filter(None, r.get("JCE_BACKEND_SDK_REQUIRES_DIR",
                                          "").split(";")):
                if not (install / rel).is_dir():
                    found = False
        if found:
            for rel in filter(None, r.get("JCE_BACKEND_SDK_REQUIRES_FILE",
                                          "").split(";")):
                if not (install / rel).is_file():
                    found = False
        if found:
            shipped.append(lang)
    return shipped


def _prune_install_prefix(bdir: Path, install: Path) -> None:
    """Delete files under `install` that this build did not install.

    `cmake --install` only ever ADDS.  Without this, an SDK prefix accumulates
    every artefact any previous build with any previous options ever put there,
    and the result is a redistributable whose halves come from different
    builds -- the failure this repository hit with the Java backend, where a
    plain `sdk` run reported "shipped: cpp, python, java" and warned in the
    same line that java was not included.

    Uses CMake's install_manifest.txt: its own record of what this install
    wrote.  Every refusal below is a guard on a DELETING loop, so each one
    reports and returns rather than guessing.
    """
    sdk_root = (ROOT / "dist" / "sdk").resolve()
    inst = install.resolve()
    # 1. Never outside dist/sdk.  A prefix is a generated output; anything else
    #    is somebody's directory.
    if sdk_root not in inst.parents and inst != sdk_root:
        log(f"prune: SKIPPED — {inst} is not under {sdk_root}")
        return
    manifest = bdir / "install_manifest.txt"
    if not manifest.exists():
        log(f"prune: SKIPPED — no install_manifest.txt in {bdir}; the prefix "
            "may still hold artefacts from an earlier build")
        return
    try:
        lines = [ln.strip() for ln in
                 manifest.read_text(encoding="utf-8", errors="replace").splitlines()]
    except OSError as exc:
        log(f"prune: SKIPPED — cannot read {manifest}: {exc}")
        return
    kept = set()
    for ln in lines:
        if ln:
            try:
                kept.add(Path(ln).resolve())
            except OSError:
                pass
    if not kept:
        log(f"prune: SKIPPED — {manifest} is empty")
        return
    # 2. THE MISMATCH GUARD.  Release and Debug SDKs have separate build dirs
    #    and separate prefixes, so a manifest that names nothing inside this
    #    prefix belongs to a different build -- and pruning against it would
    #    delete the entire prefix.  Refuse instead.
    if not any(inst == p or inst in p.parents for p in kept):
        log(f"prune: SKIPPED — {manifest} names no file under {inst}; it "
            "belongs to a different build. Pruning against it would empty "
            "this prefix.")
        return

    removed = 0
    for p in sorted(inst.rglob("*"), key=lambda q: len(str(q)), reverse=True):
        try:
            if p.is_file() and p.resolve() not in kept:
                if not DRY_RUN:
                    p.unlink()
                log(f"prune: removed stale {p.relative_to(inst)}")
                removed += 1
        except OSError as exc:
            log(f"prune: could not remove {p}: {exc}")
    # Directories the removals emptied.  Deepest-first, and only if empty, so
    # nothing that still holds an installed file can go.
    for p in sorted(inst.rglob("*"), key=lambda q: len(str(q)), reverse=True):
        try:
            if p.is_dir() and not any(p.iterdir()) and not DRY_RUN:
                p.rmdir()
        except OSError:
            pass
    if removed:
        log(f"prune: {removed} stale file(s) removed from {inst} — they were "
            "left by an earlier install with different options")


@_cache_owned(_sdk_cache_paths)
def _sdk_one(t: dict, variant: str, config: str, do_clean: bool,
             tolerate_missing_tools: bool = False,
             codec_flags: list = None, script_java: str = "auto",
             delivery_profile: str = "authoring") -> None:
    is_wasm = t.get("emscripten", False)
    kind = "debug" if config == "Debug" else f"{variant}-sdk"   # release-sdk|dist-sdk
    preset  = preset_name(t, kind)
    bdir    = preset_binary_dir(t, kind)
    install = sdk_install_dir(t, variant)
    if is_wasm:
        # Web SDK lives under build/web (matches build-web.bat) and configures
        # via explicit -S/-B Ninja, NOT a preset (there is no wasm preset).
        bdir = ROOT / "build/web/wasm/obj/sdk" / kind
    if config == "Debug":
        # Debug SDK is release-flavoured (no -dist-debug preset) and installs to
        # its own tree so it never clobbers the Release SDK at the same path.
        install = install.parent / f"{install.name}-debug"
    # MSVC env would inject the wrong toolchain into a wasm build; use a clean env.
    env = dict(os.environ) if is_wasm else msvc_env(t)

    if do_clean:
        _standalone_module().guard_cache_clean((bdir, conan_dir(t)), ROOT / "build")
        for d in (bdir, conan_dir(t)):
            if d.exists():
                log(f"clean: {d}")
                if not DRY_RUN:
                    shutil.rmtree(d, ignore_errors=True)

    conan_install(t, config, env)
    # Empty unless --patented-codecs was passed for a non-dist SDK; see
    # codec_overrides().  release-sdk and dist-sdk have separate binary dirs,
    # so nothing has to be re-pinned to undo a previous variant here.
    overrides = list(codec_flags or []) if variant != "dist" else []
    # BOTH WAYS, ALWAYS.  A CMake option is cached, so passing -D...=ON
    # only when asked would leave a tree that once built a Java SDK
    # building one forever, and the SDK a command produces would be a
    # function of this machine's history rather than of the command.
    # This repository has been bitten by exactly that already.
    overrides += _delivery_module().cmake_flags(delivery_profile, java=script_java != "off",
        restore_authoring=_delivery_module().restore_authoring(bdir / "CMakeCache.txt"))
    overrides += ["-DJCE_BUILD_EDITOR=OFF"]
    overrides += ["-DJCE_BUILD_SCRIPT_JAVA=" +
                  ("OFF" if script_java == "off" or delivery_profile == "embedded" else "ON")]
    if config == "Debug":
        # No debug-sdk preset by design; enable SDK install via overrides.
        overrides += ["-DJCE_ENABLE_SDK_INSTALL=ON",
                      f"-DCMAKE_INSTALL_PREFIX={install}"]
    if t["cross"]:
        log("cross SDK: resolving runnable build-host asset tools")
        overrides += cross_asset_tool_overrides(env)
        log("cross SDK: resolving runnable host shaderc")
        sc = find_host_shaderc()
        if not sc:
            die("cross build requires a runnable host shaderc; build the native "
                "SDK/host tools first or set JCE_SHADERC_EXECUTABLE")
        overrides += [f"-DJCE_SHADERC_EXECUTABLE={sc}"]
    # Conan generation is disabled at the recipe; preserve local IDE user presets.
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
    else:
        run(["cmake", "--preset", preset, *overrides], env=env, cwd=ROOT)
    # The STL bridge exists only in native MSVC builds. Linux/macOS/web SDKs
    # share the fat library and the install-owned scripting aggregate, without
    # requesting a target their toolchain never defines.
    sdk_targets = ["jce_sdk_fat_lib", "jce_scripting_sdk_artifacts"]
    if t["host"] == "windows" and not is_wasm:
        sdk_targets.insert(1, "jce_msvc_stl_shims")
    run(["cmake", "--build", str(bdir), "--target", *sdk_targets,
         "-j", build_jobs(env)], env=env, cwd=ROOT)
    # Host tools to ship with the SDK. A turnkey SDK (jce_add_pak cooking real
    # assets without the editor) REQUIRES jce_cook/jce_pak/jce_bin2obj, so a
    # missing tool is a hard error by default; --tolerate-missing-tools
    # restores the old WARN behavior and stamps the SDK as degraded.
    tools_status = "ok"
    if not (t["cross"] or is_wasm):
        # Native tree: the three targets exist here — build them or die.
        run(["cmake", "--build", str(bdir), "--target",
             "jce_cook", "jce_pak", "jce_bin2obj", "jce_bundle_pack", "-j", build_jobs()], env=env, cwd=ROOT)
    # Wipe the installed header tree first.  `install(DIRECTORY)` is ADDITIVE:
    # it copies what the current configuration selects and never removes what
    # an earlier one left behind.  JCESDKInstall.cmake excludes a private
    # module's headers when its option is OFF, but that exclusion is powerless
    # against a copy installed while it was ON -- the file simply stays.
    #
    # Measured 2026-08-27: dist/sdk/win32-x86_64 shipped
    # include/jce/middleware/ai_dispatch/jce_ai_dispatch.h (dated 08-01, 36
    # JCE_API declarations) beside a library built 08-27 containing ZERO
    # jce_aid_ symbols.  A consumer that includes it compiles and then fails at
    # link with unresolved externals -- which reads as "my project is broken",
    # not as "the SDK is advertising an API it does not carry".
    #
    # Only the headers are wiped: lib/ and bin/ are overwritten wholesale by
    # the same install, and share/ holds generated scripting artefacts that
    # other steps stage separately.
    inc = Path(install) / "include" / "jce"
    if inc.is_dir() and not DRY_RUN:
        log(f"clean stale headers: {inc}")
        shutil.rmtree(inc, ignore_errors=True)
    run(["cmake", "--install", str(bdir)], env=env, cwd=ROOT)
    # Straight after the install, while install_manifest.txt describes THIS
    # one.  Before the shipped-backends report below, so that report describes
    # the pruned prefix and not the accumulated one.
    if not DRY_RUN:
        _prune_install_prefix(bdir, install)
    if t["cross"] or is_wasm:
        # Cross-arch SDK: tools built in-tree would be TARGET-arch and can't
        # run on the build host. Ship the BUILD-HOST tools under bin/host/ so
        # consumers still get a real (non-stub) PAK; JCEConfig probes bin/
        # then bin/host/. (See jce-sdk-host-tools-tension.)
        log("cross SDK: copying build-host cook/pack tools into <sdk>/bin/host")
        ht = ensure_host_tools(env, ("jce_pak", "jce_cook", "jce_bin2obj", "jce_bundle_pack"))
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
            for tool in ("jce_cook", "jce_pak", "jce_bin2obj", "jce_bundle_pack"):
                cand = tdir / (tool + (".exe" if HOST == "windows" else ""))
                if cand.exists() and not _tool_runs(cand):
                    die(f"shipped host tool does not run on this host: {cand}")
    # VERSION.txt — matches package-sdk.bat so consumers can identify the tree.
    if not DRY_RUN:
        _host_tag = "wasm" if is_wasm else f"{SDK_TAG[t['host']]}-{t['arch']}"
        # Reported as well as stamped: an SDK producer who never opens
        # VERSION.txt is the person most likely to ship a Lua-only SDK
        # and find out from a consumer.
        _langs = sdk_shipped_script_backends(install, t)
        _langs_str = ", ".join(_langs) if _langs else "(none)"
        log(f"SDK scripting backends shipped: {_langs_str}")
        # STALE ARTIFACTS ARE A HAZARD, NOT A FEATURE.  cmake --install is
        # ADDITIVE: it never removes what an earlier run put there, and this
        # probe globs "lib/*/" so it sees Release AND Debug.  So an SDK built
        # WITHOUT java, in a prefix that once had it, reports java and hands a
        # consumer's find_package a JCE::ScriptVmJava whose engine half came
        # from a different build.
        #
        # MEASURED: after one `sdk --script-java` and one plain `sdk`, the
        # plain run printed "shipped: cpp, python, java  [java NOT included --
        # pass --script-java]" -- one line disagreeing with itself -- and it
        # still did so after the Release copies were deleted, because the
        # Debug ones remained.
        #
        # The report still says what ARRIVED, which is the question a consumer
        # has; it now also says when what arrived is not what this build made.
        if "java" in _langs and script_java == "off":
            log("SDK scripting: WARNING — java is on disk but THIS build did "
                "not produce it. Those files are left over from an earlier "
                f"`sdk --script-java` into {install}; cmake --install never "
                "removes anything. Pairing them with this engine is an "
                "untested combination. Delete the prefix and rebuild, or pass "
                "--script-java so they are rebuilt to match.")
        elif "java" not in _langs and script_java == "require":
            # --script-java is a REQUIREMENT, not a preference.  Before the
            # backend detected its own toolchain, passing the flag on a machine
            # without a JDK produced a Lua-and-python SDK and one STATUS line in
            # a long log; the person who typed it found out from a consumer.
            die("--script-java was given but the SDK did not end up with the "
                f"java backend (it has: {_langs_str or '(none)'}). The build "
                "machine needs jni.h and javac on JAVA_HOME, and a Python 3 "
                "interpreter to compile com.jce.script. Look above for the "
                "'Java JNI shim SKIPPED' or 'CLASSES will not be installed' "
                "line, which names which one was missing.")
        elif "java" not in _langs and script_java == "auto":
            if t.get("emscripten"):
                log("SDK scripting: java not included — native JVM unavailable on Web.")
            else:
                log("SDK scripting: java not included — no target JDK was found. "
                    "Pass --script-java to require this backend.")
        (install / "VERSION.txt").write_text(
            "JCE SDK build\n"
            f"commit:  {git_short_sha()}\n"
            f"host:    {_host_tag}\n"
            f"variant: {variant}\n"
            f"graphics:{graphics_api_tier(env)}\n"
            f"tools:   {tools_status}\n"
            f"scripts: lua (built in) + backends shipped: {_langs_str}\n"
            "note:    bin/ tools need the MSVC C++ Redistributable on Windows\n",
            encoding="utf-8")
    if not DRY_RUN:
        (install / "delivery-profile.json").write_text(json.dumps({"schema": 1,
            "profile": delivery_profile, "languages": ["lua", *sdk_shipped_script_backends(install, t)],
            "external_vm_allowed": delivery_profile == "authoring"}, indent=2)+"\n", encoding="utf-8")
    if not DRY_RUN:
        _publish_sdk_product(install, t, config, variant, delivery_profile, bdir,
                             require_tools=tools_status == "ok")
    log(f"SDK ({variant}/{config}) -> {install}")


def _standalone_sdk_cache_paths(values):
    args = values["args"]
    return _standalone_module().sdk_outputs().cache_paths(
        globals(), args, resolve_target(args.arch))


@_cache_owned(_standalone_sdk_cache_paths)
def _sdk_standalone(args):
    target = resolve_target(args.arch)
    return _standalone_module().build_sdk(sys.modules[__name__], args, target, msvc_env(target))


def cmd_sdk(args) -> None:
    t = resolve_target(args.arch)
    delivery_profile = _delivery_module().profile(args)
    _standalone_module().sdk_outputs().admit(die, args, t, delivery_profile)
    if delivery_profile == "embedded" and not t.get("emscripten"):
        _sdk_standalone(args)
        return
    if t["host"] != HOST:
        die(f"target {t['key']} must be built on host '{t['host']}'.")
    variants = {"release": ["release"], "dist": ["dist"],
                "both": ["release", "dist"]}[args.variant]
    tolerate = getattr(args, "tolerate_missing_tools", False)
    # `--variant both` runs dist and release in one go, so resolve the codec
    # flags per SDK variant rather than from args.variant.
    codec_flags = codec_overrides(argparse.Namespace(
        patented_codecs=getattr(args, "patented_codecs", None), variant="release"))
    # Same per-variant resolution as the codec flags: `--variant both` builds a
    # dist SDK in the same run, and dist has no Tracy to switch off.
    codec_flags = codec_flags + profiling_overrides(argparse.Namespace(
        profiling=getattr(args, "profiling", None), variant="release"))
    # "auto" | "require" | "off" — see the flag definitions.
    script_java = "auto"
    if getattr(args, "no_script_java", False):
        script_java = "off"
    elif getattr(args, "script_java", False):
        script_java = "require"
    if t.get("emscripten") and script_java == "require":
        die("--script-java requires a native JVM; the Web target cannot provide JNI.")
    for v in variants:
        _sdk_one(t, v, "Release", args.clean, tolerate,
                 codec_flags, script_java, delivery_profile)
    # Debug SDK is release-flavoured only: there is no -dist-debug preset, and
    # `dist` is a royalty-free *ship* build. Build Debug once, for release, and
    # only when a release SDK was requested.  Web ships Release-only (the wasm
    # fat lib is Release; a Debug consumer maps to it via MAP_IMPORTED_CONFIG_DEBUG).
    if not args.no_debug and "release" in variants and not t.get("emscripten"):
        _sdk_one(t, "release", "Debug", args.clean, tolerate,
                 codec_flags, script_java, delivery_profile)
    # Gate: build + run the plain-C99 smoke consumer against each fresh SDK.
    if getattr(args, "smoke", False):
        native_run = not (DRY_RUN or t["cross"] or t.get("emscripten"))
        for v in variants:
            _smoke_one(t, sdk_install_dir(t, v), v,
                       require_run=native_run, delivery_profile=delivery_profile)
            _smoke_scripting_one(t, sdk_install_dir(t, v), v,
                                require_run=native_run, delivery_profile=delivery_profile)
    if getattr(args, "smoke", False) and (t["cross"] or t.get("emscripten")):
        log("sdk: build/install complete; requested smoke EXECUTION PENDING")
    else:
        log("sdk: done")


# ── Subcommand: smoke (C ABI gate against a built SDK) ────────────────────
def _smoke_target_artifacts(t: dict, bdir: Path,
                            stem: str) -> tuple[Path, Path]:
    """Return (launcher, linked payload) for a foreign SDK consumer."""
    if t.get("emscripten"):
        html = bdir / f"{stem}.html"
        if html.is_file():
            # SINGLE_FILE embeds the runtime in HTML: no external launcher or
            # wasm file should be invented. This is an offline artifact gate,
            # never a substitute for actual browser execution.
            try:
                _delivery_module().audit_html(html)
            except ValueError as error:
                die(f"smoke: single HTML artifact audit failed: {error}")
            return html, html
        return bdir / f"{stem}.js", bdir / f"{stem}.wasm"
    exe = bdir / (f"{stem}.exe" if HOST == "windows" else stem)
    return exe, exe


def _extract_introspection(stdout: str, subject: str):
    """The JSON between the consumer's markers, or None.

    Bracketed rather than "the first line starting with {": stdout also carries
    the engine's own startup log, and a brace-sniffing extractor would happily
    return whatever else began with one -- silently, as a parse error about a
    document that was never the answer.
    """
    begin = "JCE_INTROSPECT_BEGIN " + subject
    out, taking = [], False
    for line in stdout.splitlines():
        if line.strip() == begin:
            taking = True
            continue
        if taking and line.strip() == "JCE_INTROSPECT_END":
            return "\n".join(out)
        if taking:
            out.append(line)
    return None


def _smoke_runtime_ok(stdout, marker):
    """One complete success token line, with no failure line, is the run proof."""
    lines = [line.strip() for line in stdout.splitlines()]
    success = sum(line == marker or line.startswith(marker + " ") for line in lines)
    failure = marker.removesuffix("OK") + "FAIL"
    return success == 1 and not any(
        line == failure or line.startswith(failure + " ") for line in lines)


def _smoke_run_admission(t, sdk, variant, require_run, delivery_profile):
    # Runs inside the existing scratch/SDK lease, before any consumer mutation.
    if not require_run:
        return True
    if DRY_RUN:
        die("smoke: --require-run cannot certify --dry-run")
    resolve_sdk(argparse.Namespace(sdk=str(sdk), variant=variant,
                                  delivery_profile=delivery_profile), t)
    if t["cross"] or t.get("emscripten"):
        log("smoke: SKIPPED — EXECUTION_PENDING, selected target cannot run "
            "natively on this host")
        return False
    return True


@_cache_owned(lambda values: _smoke_cache_paths(values))
def _smoke_one(t: dict, sdk: Path, variant: str,
               introspect: str = "", introspect_out: str = "",
               build_dir: Path | None = None, require_run: bool = False,
               delivery_profile: str = "authoring") -> str:
    """Build tests/sdk_smoke OUT-OF-TREE against the SDK at `sdk`, verify the
    cook→pack→embed artifacts offline, then (native arch only) run the
    consumer headless and require its `JCE_SMOKE: OK` marker.

    Returns passed, skipped, execution_pending or dry_run; only passed proves
    the actual native run. The historical CLI may still request cross audits.
    """
    if not _smoke_run_admission(t, sdk, variant, require_run, delivery_profile):
        return "execution_pending"
    jce_cmake = sdk / "lib" / "cmake" / "JCE"
    if not DRY_RUN and not (jce_cmake / "JCEConfig.cmake").exists():
        die(f"smoke: no JCEConfig.cmake under {jce_cmake} — build the SDK first.")

    if not DRY_RUN:
        # Tripwire: target binaries must never leak into the SDK bin/ again
        # (a stale win32-aarch64 SDK shipped a sample game's exe there once).
        #
        # Checked by EXCLUSION, not by one game's name: bin/ carries the host
        # tools and nothing else, so anything else in it IS the leak.  A name
        # glob only ever catches the one sample somebody thought of -- the next
        # project's exe would have walked straight through.
        HOST_TOOL_STEMS = {"jce_cook", "jce_pak", "jce_bin2obj", "jce_bundle_pack", "shaderc"}
        exe_suffix = ".exe" if HOST == "windows" else ""
        bin_dir = sdk / "bin"
        leaked = []
        if bin_dir.is_dir():
            for cand in sorted(bin_dir.iterdir()):
                if cand.is_dir() or cand.suffix.lower() != exe_suffix:
                    continue
                if cand.stem.lower() in HOST_TOOL_STEMS:
                    continue
                leaked.append(cand)
        if leaked:
            die(f"smoke: a non-tool executable leaked into the SDK bin/: "
                f"{leaked[0].name} (expected only {sorted(HOST_TOOL_STEMS)})")
        # Producer-side check: every shipped host tool must execute here.
        ext = ".exe" if HOST == "windows" else ""
        tool_found = False
        for sub_dir in ("bin", os.path.join("bin", "host")):
            for tool in ("jce_cook", "jce_pak", "jce_bin2obj", "jce_bundle_pack"):
                cand = sdk / sub_dir / f"{tool}{ext}"
                if cand.exists():
                    tool_found = True
                    if not _tool_runs(cand):
                        die(f"smoke: shipped tool does not run on this host: {cand}")
        if not tool_found:
            die(f"smoke: SDK at {sdk} ships no host tools (degraded SDK) — "
                "nothing to gate.")

    env = dict(os.environ) if t.get("emscripten") else msvc_env(t)
    bdir = _smoke_build_dir(t, sdk, variant, build_dir)
    cfg = ["cmake", "-S", str(ROOT / "tests" / "sdk_smoke"), "-B", str(bdir),
           "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
           "-U", "JCE_PAK_EXECUTABLE",
           "-U", "JCE_COOK_EXECUTABLE",
           "-U", "JCE_BIN2OBJ_EXECUTABLE",
           f"-DJCE_DIR={jce_cmake}"]
    if HOST == "windows" and not t.get("emscripten"):
        cfg += ["-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"]
    if t.get("emscripten"):
        cfg += [f"-DCMAKE_TOOLCHAIN_FILE={emscripten_paths()['toolchain']}"]
    cfg += ["-U", "JCE_PROJECT_PREBUILT_ASSETS_*",
            "-U", "JCE_PROJECT_PREBUILT_PAK_KEY_C",
            "-U", "JCE_PROJECT_STAGE_LOOSE_ASSETS"]
    dist_assets = require_run and variant == "dist"
    if require_run:
        cfg += [f"-DJCE_BUILD_VARIANT={variant}",
                f"-DJCE_DELIVERY_PROFILE={delivery_profile}"]
        if dist_assets:
            try:
                cfg += _sdk_smoke_assets_module().prepare(
                    sys.modules[__name__], t, sdk, bdir, env)
            except (OSError, ValueError) as error:
                die("smoke: authenticated DIST asset admission failed: " + str(error))
    else:
        cfg += ["-U", "JCE_BUILD_VARIANT", "-U", "JCE_DELIVERY_PROFILE"]
    run(cfg, env=env, cwd=ROOT)
    run(["cmake", "--build", str(bdir), "--parallel", build_jobs(env)], env=env, cwd=ROOT)
    if DRY_RUN:
        return "dry_run"

    # Offline artifact gates — these run even for SDKs whose target arch
    # cannot execute on this host (cross/wasm).
    if dist_assets:
        try:
            _sdk_smoke_assets_module().verify(bdir)
        except (OSError, ValueError) as error:
            die("smoke: authenticated DIST asset proof failed: " + str(error))
        pak = bdir / "jce_generated" / "project_assets.pak"
    else:
        cooked = bdir / "JceSdkSmoke_cooked" / "smoke.png"
        if not cooked.exists():
            die(f"smoke: cooked asset missing: {cooked}")
        with open(cooked, "rb") as f:
            magic = f.read(4)
        if magic != b"JCEA":
            die(f"smoke: {cooked} is not a cooked .jceasset (magic={magic!r}) — "
                "the cook path produced raw bytes.")
        pak = bdir / "JceSdkSmoke_assets.pak"
    exe, payload = _smoke_target_artifacts(t, bdir, "jce_sdk_smoke")
    if not pak.exists():
        die(f"smoke: pak missing: {pak}")
    if not exe.exists():
        die(f"smoke: target launcher missing: {exe}")
    if not payload.exists():
        die(f"smoke: linked target payload missing: {payload}")
    if payload.stat().st_size <= pak.stat().st_size:
        die("smoke: linked payload is not larger than its pak — embed step "
            f"suspect (payload={payload.stat().st_size} "
            f"pak={pak.stat().st_size})")

    if t["cross"] or t.get("emscripten"):
        log("smoke: BUILD/AUDIT PASS; EXECUTION PENDING (target requires "
            "actual browser/device validation)")
        return "execution_pending"

    # Run gate: headless boot (noop renderer), one frame, clean exit. The
    # stdout marker is the primary signal — the engine's own error path
    # decides the exit code when init fails.
    renv = dict(env)
    renv["JCE_BACKEND"] = "noop"
    if HOST == "linux":
        renv.setdefault("SDL_VIDEODRIVER", "dummy")
    if introspect:
        # Opt-in: the default gate path is byte-for-byte what it always was.
        renv["JCE_SMOKE_INTROSPECT"] = introspect
    log(f"smoke: running {exe.name} (JCE_BACKEND=noop)")
    try:
        proc = subprocess.run([str(exe)], env=renv, cwd=str(bdir),
                              capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        die("smoke: consumer hung (>120 s) — should_quit never fired?")
    marker_ok = _smoke_runtime_ok(proc.stdout or "", "JCE_SMOKE: OK")
    if not marker_ok or proc.returncode != 0:
        sys.stderr.write(proc.stdout or "")
        sys.stderr.write(proc.stderr or "")
        die(f"smoke: FAILED (exit={proc.returncode}, "
            f"marker={'yes' if marker_ok else 'MISSING'})")
    for line in (proc.stdout or "").splitlines():
        if "JCE_SMOKE:" in line:
            log(line.strip())
    if introspect:
        body = _extract_introspection(proc.stdout or "", introspect)
        if body is None:
            die(f"smoke: --introspect {introspect} produced no "
                f"JCE_INTROSPECT_BEGIN/END block -- the SDK this ran against "
                f"predates <jce/api_introspect.h>, or the subject name is "
                f"not one the consumer knows")
        if introspect_out:
            # A FILE, not stdout, when the caller asks for one.  log() writes
            # to stdout, so the JSON arrives after a dozen "[jce] ..." lines,
            # and any consumer would have to find the document inside a log
            # stream.  Measured: the first version printed the body without
            # its markers and introspect_bridge.ask_engine() could not find
            # it at all -- which would have shown up only as
            # check_introspection_parity.py being permanently SKIPPED, with
            # nothing saying why.
            outp = Path(introspect_out)
            outp.parent.mkdir(parents=True, exist_ok=True)
            with open(outp, "wb") as fh:
                fh.write(body.encode("utf-8"))
                fh.write(b"\n")
            log(f"smoke: introspection written to {outp}")
        else:
            # To stdout for a person reading it; the [jce] log lines above are
            # part of what they asked for.
            print(body)
    log(f"smoke: PASS ({variant} SDK at {sdk})")
    return "passed"


# ── The MULTI-LANGUAGE half of the smoke gate ─────────────────────────────
@_cache_owned(lambda values: _smoke_cache_paths(values, True))
def _smoke_scripting_one(t: dict, sdk: Path, variant: str,
                         build_dir: Path | None = None, require_run: bool = False,
                         delivery_profile: str = "authoring") -> str:
    """Build tests/sdk_smoke_scripting OUT-OF-TREE against the SDK at `sdk` and
    require its `JCE_SMOKE_SCRIPTING: OK` marker.

    A SEPARATE consumer from _smoke_one and not a bigger one, because they
    gate opposite things: sdk_smoke must keep passing on an engine-only SDK,
    and this one must FAIL on an SDK that claims a scripting backend and does
    not ship it.

    SKIPPING IS NOT SILENT.  An SDK with no lib/cmake/JCE/JCEScripting.cmake
    -- every SDK produced before 2026-08-16, and any built from a tree without
    scripting/ -- legitimately cannot run this; that is reported WITH THE
    REASON, because "the scripting gate did not run" and "the scripting gate
    passed" produced the same empty output for as long as the SDK shipped
    nothing from scripting/ at all."""
    if not _smoke_run_admission(t, sdk, variant, require_run, delivery_profile):
        return "execution_pending"
    jce_cmake = sdk / "lib" / "cmake" / "JCE"
    fragment = jce_cmake / "JCEScripting.cmake"
    if not DRY_RUN and not fragment.exists():
        log(f"smoke-scripting: SKIPPED — {sdk} ships no scripting layer "
            f"(no lib/cmake/JCE/JCEScripting.cmake): it predates 2026-08-16, "
            f"or came from a tree without scripting/. Rebuild the SDK to gate "
            f"it; add an embeddable CPython and -DJCE_BUILD_SCRIPT_JAVA=ON "
            f"for the Python and Java halves.")
        return "skipped"

    shipped = sdk_shipped_script_backends(sdk, t, config="Release") if not DRY_RUN else ["c"]
    if not DRY_RUN:
        # The existing generated config declares producer intent; catalog
        # rosters alone also describe intentionally uncompiled languages.
        text = fragment.read_text(encoding="utf-8")
        names = ("JCE_SCRIPT_AVAILABILITY_SCHEMA", "JCE_SCRIPT_CATALOG_LANGUAGES",
                 "JCE_SCRIPT_COMPILED_LANGUAGES")
        fields = {}
        for name in names:
            entries = re.findall(r'^\s*set\(\s*' + name + r'\s+("[^"\r\n]*"|[^()\s]+)\s*\)\s*$', text, re.M)
            if len(entries) != 1:
                die(f"smoke-scripting: missing/malformed/duplicate {name}; refresh the SDK")
            fields[name] = entries[0].strip('"')
        if fields[names[0]] != "1":
            die("smoke-scripting: unsupported availability schema; refresh the SDK")
        sets = []
        for name in names[1:]:
            languages = fields[name].split(";") if fields[name] else []
            if (len(set(languages)) != len(languages) or
                    any(not re.fullmatch(r"[a-z][a-z0-9_]*", lang) for lang in languages)):
                die(f"smoke-scripting: malformed/duplicate {name}; refresh the SDK")
            sets.append(set(languages))
        catalog, compiled = sets
        if not compiled <= catalog:
            die("smoke-scripting: compiled languages outside the catalog; refresh the SDK")
        declared = set()
        for roster in sorted((jce_cmake / "backends").glob("*/jce_backend.cmake")):
            entry = _read_roster(roster)
            language = entry.get("JCE_BACKEND_LANGUAGE", "")
            stem = entry.get("JCE_BACKEND_VM_TARGET", "")
            raw = roster.read_text(encoding="utf-8")
            keys = [match.group(1) for match in _ROSTER_SET_RE.finditer(raw)]
            if (not re.fullmatch(r"[a-z][a-z0-9_]*", language) or
                    not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", stem) or
                    keys.count("JCE_BACKEND_LANGUAGE") != 1 or
                    keys.count("JCE_BACKEND_VM_TARGET") != 1):
                die(f"smoke-scripting: malformed installed roster {roster}")
            if language in declared:
                die(f"smoke-scripting: duplicate installed language {language}: {roster}")
            declared.add(language)
        if declared != catalog:
            die("smoke-scripting: installed roster catalog mismatch; refresh the SDK")
        missing = sorted(compiled - set(shipped))
        unexpected = sorted(set(shipped) - compiled)
        if missing or unexpected:
            die("smoke-scripting: compiled-language payload mismatch: "
                f"missing={','.join(missing) or 'none'} unexpected={','.join(unexpected) or 'none'}; refresh the SDK")
        unavailable = sorted(catalog - compiled)
        if unavailable:
            log("smoke-scripting: UNAVAILABLE (not compiled in this SDK): " + ", ".join(unavailable))
    if not shipped:
        die(f"smoke-scripting: {sdk} contains JCEScripting.cmake but no "
            "complete scripting backend. A scripting layer that can run no "
            "language is an inconsistent SDK, not a skipped test.")
    api_required = False
    if not DRY_RUN:
        selected = set(shipped)
        backend_root = jce_cmake / "backends"
        for roster in sorted(backend_root.glob("*/jce_backend.cmake")):
            r = _read_roster(roster)
            if r.get("JCE_BACKEND_LANGUAGE", "") not in selected:
                continue
            if r.get("JCE_BACKEND_NEEDS_SCRIPT_API", "").upper() in \
                    ("1", "ON", "TRUE", "YES"):
                api_required = True
                break
    api_required = _delivery_module().smoke_requires_shared_api(sdk, shipped, api_required)
    log("smoke-scripting: installed roster = lua (built-in positive control), " + ", ".join(shipped))

    env = dict(os.environ) if t.get("emscripten") else msvc_env(t)
    bdir = _smoke_build_dir(t, sdk, variant, build_dir, scripting=True)
    cfg = ["cmake", "-S", str(ROOT / "tests" / "sdk_smoke_scripting"),
           "-B", str(bdir), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
           "-U", "JCE_PAK_EXECUTABLE",
           "-U", "JCE_COOK_EXECUTABLE",
           "-U", "JCE_BIN2OBJ_EXECUTABLE",
           f"-DJCE_DIR={jce_cmake}",
           f"-DJCE_SMOKE_SCRIPT_BACKENDS={';'.join(shipped)}",
           f"-DJCE_SMOKE_SCRIPT_API_REQUIRED={'ON' if api_required else 'OFF'}",
           f"-DJCE_SMOKE_CAN_RUN_TARGET={'OFF' if t['cross'] or t.get('emscripten') else 'ON'}"]
    if HOST == "windows" and not t.get("emscripten"):
        cfg += ["-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"]
    if t.get("emscripten"):
        cfg += [f"-DCMAKE_TOOLCHAIN_FILE={emscripten_paths()['toolchain']}"]
    cfg += ["-U", "JCE_PROJECT_PREBUILT_ASSETS_*",
            "-U", "JCE_PROJECT_PREBUILT_PAK_KEY_C",
            "-U", "JCE_PROJECT_STAGE_LOOSE_ASSETS"]
    dist_assets = require_run and variant == "dist"
    if require_run:
        cfg += [f"-DJCE_BUILD_VARIANT={variant}",
                f"-DJCE_DELIVERY_PROFILE={delivery_profile}"]
        if dist_assets:
            try:
                cfg += _sdk_smoke_assets_module().prepare(
                    sys.modules[__name__], t, sdk, bdir, env, fixture="scripting")
            except (OSError, ValueError) as error:
                die("smoke-scripting: authenticated DIST asset admission failed: " + str(error))
    else:
        cfg += ["-U", "JCE_BUILD_VARIANT", "-U", "JCE_DELIVERY_PROFILE"]
    run(cfg, env=env, cwd=ROOT)
    run(["cmake", "--build", str(bdir), "--parallel", build_jobs(env)], env=env, cwd=ROOT)
    if DRY_RUN:
        return "dry_run"

    if dist_assets:
        try:
            _sdk_smoke_assets_module().verify(bdir, fixture="scripting")
        except (OSError, ValueError) as error:
            die("smoke-scripting: authenticated DIST asset proof failed: " + str(error))
    exe, payload = _smoke_target_artifacts(
        t, bdir, "jce_sdk_smoke_scripting")
    if not exe.exists():
        die(f"smoke-scripting: target launcher missing: {exe}")
    if not payload.exists():
        die(f"smoke-scripting: linked target payload missing: {payload}")
    # The C ABI shared library, staged beside the exe by
    # jce_script_stage_runtime().  Checked as an ARTIFACT and not only by
    # running: on a cross SDK there is no run gate, and a staging rule that
    # silently copied nothing would leave this gate green.
    if api_required and not t.get("emscripten"):
        lib_names = ("jce_script_api.dll" if HOST == "windows"
                     else "libjce_script_api.so")
        if not (bdir / lib_names).exists():
            die(f"smoke-scripting: {lib_names} was not staged beside "
                f"{exe.name} — jce_script_stage_runtime() copied nothing.")

    if t["cross"] or t.get("emscripten"):
        log("smoke-scripting: BUILD/AUDIT PASS; EXECUTION PENDING (target "
            "requires actual browser/device validation)")
        return "execution_pending"

    renv = dict(env)
    renv["JCE_BACKEND"] = "noop"
    if HOST == "linux":
        renv.setdefault("SDL_VIDEODRIVER", "dummy")
    log(f"smoke-scripting: running {exe.name} (JCE_BACKEND=noop)")
    try:
        proc = subprocess.run([str(exe)], env=renv, cwd=str(bdir),
                              capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        die("smoke-scripting: consumer hung (>180 s)")
    marker_ok = _smoke_runtime_ok(proc.stdout or "", "JCE_SMOKE_SCRIPTING: OK")
    if not marker_ok or proc.returncode != 0:
        sys.stderr.write(proc.stdout or "")
        sys.stderr.write(proc.stderr or "")
        die(f"smoke-scripting: FAILED (exit={proc.returncode}, "
            f"marker={'yes' if marker_ok else 'MISSING'})")
    for line in (proc.stdout or "").splitlines():
        if "JCE_SMOKE_SCRIPTING:" in line:
            log(line.strip())
    log(f"smoke-scripting: PASS ({variant} SDK at {sdk})")
    return "passed"


def cmd_smoke(args) -> None:
    t = resolve_target(args.arch)
    consumer = getattr(args, "consumer", "both")
    require_run = getattr(args, "require_run", False)
    if require_run and not args.sdk:
        die("smoke: --require-run needs an explicit --sdk install directory")
    sdk = Path(args.sdk).resolve() if args.sdk else sdk_install_dir(t, args.variant)
    introspect = getattr(args, "introspect", "") or ""
    if introspect and consumer == "scripting":
        die("smoke: introspection belongs to the C99 consumer")
    directory = getattr(args, "build_dir", None)
    delivery = getattr(args, "delivery_profile", "authoring")
    statuses = []
    if consumer in ("both", "c99"):
        c99_dir = Path(directory) / "c99" if directory and consumer == "both" else directory
        statuses.append(_smoke_one(t, sdk, args.variant, introspect,
            getattr(args, "introspect_out", "") or "", c99_dir, require_run, delivery))
    if not introspect and consumer in ("both", "scripting"):
        script_dir = Path(directory) / "scripting" if directory and consumer == "both" else directory
        statuses.append(_smoke_scripting_one(t, sdk, args.variant, script_dir, require_run, delivery))
    if require_run:
        if not statuses or any(status not in ("passed", "skipped", "execution_pending")
                               for status in statuses):
            die("smoke: native run did not produce a definitive result")
        if any(status != "passed" for status in statuses):
            raise SystemExit(77)


# ── Subcommand: editor (Track A first-party desktop app) ──────────────────
@_cache_owned(lambda values: [*_authoring_editor_paths(values["t"], "asan"), conan_dir(values["t"])])
def _editor_asan(t: dict, env: dict) -> None:
    """x64-only AddressSanitizer editor (Release + /Zi + /fsanitize=address),
    configured into a dedicated authoring object directory. Invoked via
    scripts/<os>/build-editor.{bat,sh} --variant asan."""
    if t["arch"] != "x86_64":
        die("asan editor build is x64-only")
    bdir, output = _authoring_editor_paths(t, "asan")
    conan_install(t, "Release", env)
    flags = ("/MD /O1 /Ob1 /Zi /DNDEBUG /fsanitize=address /Oy- /FS "
             "/D_DISABLE_STRING_ANNOTATION=1 /D_DISABLE_VECTOR_ANNOTATION=1")
    # CMakeUserPresets.json belongs to local IDE configuration.
    run(["cmake", "-S", str(ROOT), "-B", str(bdir), "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={toolchain_path(t, 'Release')}",
         "-DCMAKE_BUILD_TYPE=Release", "-DJCE_BUILD_VARIANT=asan",
         f"-DJCE_EDITOR_ARTIFACT_DIR={output.as_posix()}",
         "-DJCE_DELIVERY_PROFILE=authoring",
         f"-DCMAKE_C_FLAGS_RELEASE={flags}",
         f"-DCMAKE_CXX_FLAGS_RELEASE={flags}"], env=env, cwd=ROOT)
    run(["cmake", "--build", str(bdir), "--target", "JCE_Editor"], env=env, cwd=ROOT)
    log(f"editor (asan) -> {output}")


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


def codec_overrides(args) -> list:
    """Configure flags for the patented-codec switch — and, by design, usually
    none at all.

    The contract is: dist is always royalty-free, release carries AAC/H.264/
    H.265 by default, and a release build can opt out.  Only the first two were
    actually true.  This function used to pin
    -DJCE_ENABLE_PATENTED_CODECS={OFF if dist else ON} on EVERY configure, and
    CMakePresets.json's `_desktop-base` stamped the same value a second time
    (added in 26379e69).  A -D always overwrites an existing cache entry, so a
    user's `-DJCE_ENABLE_PATENTED_CODECS=OFF` survived exactly one configure
    and was then silently reset to ON by the next build — the opt-out did not
    exist in practice, on any driver.

    The fix is to stop writing the preference unless the user asked for a
    change.  The value the option() carries is a *preference* and lives in the
    cache; the *effective* switch is derived from it per variant by the root
    CMakeLists (which forces OFF for dist and Web and asserts that it held).
    So dist needs no flag from us at all, and release needs one only when
    --patented-codecs was actually passed."""
    want = getattr(args, "patented_codecs", None)
    if want is None:
        return []
    if getattr(args, "variant", None) == "dist":
        # Not an error: dist is royalty-free by construction, and the root
        # CMakeLists asserts it.  Say so rather than implying we honoured it.
        log("note: --patented-codecs is ignored for --variant dist "
            "(dist is always royalty-free)")
        return []
    return [f"-DJCE_ENABLE_PATENTED_CODECS={'ON' if want == 'on' else 'OFF'}"]


def profiling_overrides(args) -> list:
    """Configure flags for the Tracy switch -- tri-state, like the codec one.

    JCE_ENABLE_PROFILING has been a CMake option since Tracy landed, defaulting
    to ON, and no driver ever exposed it.  So every release SDK carries Tracy,
    every game linked against one carries Tracy, and Tracy opens its listening
    socket during static initialisation -- before main(), whether or not anyone
    will ever profile.  Measured on this host: a freshly built game and the
    editor each bind TCP 127.0.0.1:8086 for their whole lifetime.

    Loopback does not raise the Windows Firewall dialog; a Tracy built without
    TRACY_ONLY_LOCALHOST does, and that is what a bundle staged in June was
    doing.  But an unused listener in a shipped game is still an unused
    listener, so this switch makes it removable.

    None means "do not touch the cache", which is what keeps the preference
    persistent: writing -D on every configure is exactly how the codec opt-out
    was silently reverted for months.
    """
    want = getattr(args, "profiling", None)
    if want is None:
        return []
    if getattr(args, "variant", None) == "dist":
        log("note: --profiling is ignored for --variant dist "
            "(dist never ships with Tracy)")
        return []
    return [f"-DJCE_ENABLE_PROFILING={'ON' if want == 'on' else 'OFF'}"]


def _delivery_module():
    spec = importlib.util.spec_from_file_location("jce_delivery", Path(__file__).with_name("delivery.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _sdk_smoke_assets_module():
    spec = importlib.util.spec_from_file_location("jce_sdk_smoke_assets", Path(__file__).with_name("sdk_smoke_assets.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _standalone_module():
    import importlib.util
    spec = importlib.util.spec_from_file_location("jce_standalone", Path(__file__).with_name("standalone.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@_cache_owned(_editor_cache_paths)
def cmd_editor(args) -> None:
    t = resolve_target(args.arch)
    if getattr(args, "artifact_dir", None) and args.variant == "asan":
        die("--artifact-dir currently supports normal release/debug/dist editor builds")
    if _delivery_module().profile(args) == "embedded":
        _standalone_module().build_editor(sys.modules[__name__], args, t, msvc_env(t))
        return
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
    bdir, output = _authoring_editor_paths(t, args.variant)
    if args.clean:
        _standalone_module().guard_cache_clean((bdir, conan_dir(t)), ROOT / "build/desktop")
        for directory in (bdir, conan_dir(t)):
            if directory.exists():
                log(f"clean: {directory}")
                if not DRY_RUN:
                    shutil.rmtree(directory, ignore_errors=True)
    conan_install(t, config, env)
    # Release and Dist share authoring objects. Pin both the variant and
    # output every time so a previous configure cannot redirect the EXE.
    artifacts = getattr(args, "artifact_dir", None)
    if artifacts:
        output = Path(artifacts).resolve()
    overrides = ["-DJCE_BUILD_EDITOR=ON", f"-DJCE_BUILD_VARIANT={args.variant}",
                 f"-DJCE_EDITOR_ARTIFACT_DIR={output.as_posix()}"]
    overrides += codec_overrides(args)
    overrides += _delivery_module().cmake_flags("authoring",
        restore_authoring=_delivery_module().restore_authoring(bdir / "CMakeCache.txt"))
    # Emit a linker map so the build can attach a binary-composition report.
    overrides.append("-DJCE_EMIT_LINK_MAP=ON")
    if t["cross"]:
        overrides += cross_asset_tool_overrides(env)
        sc = find_host_shaderc()
        if sc:
            overrides.append(f"-DJCE_SHADERC_EXECUTABLE={sc}")
    # CMakeUserPresets.json belongs to local IDE configuration.
    run(["cmake", "--preset", preset, "-B", str(bdir), *overrides], env=env, cwd=ROOT)
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
    exe = output / _host_exe("jce_editor")
    log(f"editor ({args.variant}) -> {exe}")
    return exe


# ── Subcommand: app (Track B — build via SDK as a real consumer) ──────────
def _publish_sdk_product(*args, **kwargs):
    spec = importlib.util.spec_from_file_location("jce_sdk_product", Path(__file__).with_name("sdk_product.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.publish_install(sys.modules[__name__], *args, **kwargs)


def _sdk_target_module():
    spec = importlib.util.spec_from_file_location("jce_sdk_target", Path(__file__).with_name("sdk_target.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def resolve_sdk(args, t: dict) -> Path:
    variant = getattr(args, "variant", "release")
    delivery = _delivery_module().profile(args)
    if args.sdk:
        sdk = Path(args.sdk).resolve()
    elif os.environ.get("JCE_SDK_DIR"):
        sdk = Path(os.environ["JCE_SDK_DIR"]).resolve()
    else:
        sdk = sdk_install_dir(t, "dist" if variant == "dist" else "release")
        if delivery == "embedded" and not t.get("emscripten"):
            sdk = sdk.with_name(sdk.name + "-single")
        if variant == "debug":
            sdk = sdk.with_name(sdk.name + "-debug")
    if not (sdk / "lib" / "cmake" / "JCE" / "JCEConfig.cmake").exists():
        die(f"no valid SDK at {sdk} (missing lib/cmake/JCE/JCEConfig.cmake). "
            f"Run `jce.py sdk` first, pass --sdk DIR, or set JCE_SDK_DIR.")
    system = "Emscripten" if t.get("emscripten") else {
        "windows": "Windows", "linux": "Linux", "darwin": "Darwin"}.get(t["host"], "")
    config = "Debug" if variant == "debug" else "Release"
    try:
        _sdk_target_module().require_sdk_target(sdk, system, t["arch"], config, variant, delivery)
    except ValueError as error:
        die(str(error) + "; refresh/select the matching SDK")
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


def _project_pak_key(project: Path) -> bytes:
    """Load (or first-time generate) `<project>/.jce/pak_key.hex`.

    Same file the editor uses (`editor/src/core/jce_pak_key.cpp`): 64 hex
    chars = 32 bytes.  Deterministic rebuilds need the SAME key across runs,
    so this only generates when the file is absent -- it never rotates one.
    """
    key_path = project / ".jce" / "pak_key.hex"
    if key_path.exists():
        text = key_path.read_text(encoding="ascii").strip()
        if len(text) != 64:
            die(f"{key_path}: expected 64 hex chars, got {len(text)}")
        try:
            return bytes.fromhex(text)
        except ValueError:
            die(f"{key_path}: not valid hex")
    key = secrets.token_bytes(32)
    if DRY_RUN:
        return key
    key_path.parent.mkdir(parents=True, exist_ok=True)
    key_path.write_text(key.hex(), encoding="ascii", newline="\n")
    # The key must never reach the repo; the editor seeds the same guard.
    ignore = project / ".jce" / ".gitignore"
    if not ignore.exists():
        ignore.write_text("pak_key.hex\n", encoding="ascii",
                          newline="\n")
    log(f"app: generated project asset key -> {key_path}")
    return key


def _write_pak_key_shares_c(out_c: Path, key: bytes) -> None:
    """Emit the two-XOR-share key TU the runtime links for encrypted PAKs.

    Byte-for-byte the same contract as the editor emitter
    (`jce_pak_key_write_shares_c`) and the engine default
    (`engine/src/application/jce_pak_key_default.c`):

        const unsigned char jce_embedded_pak_key_shares[64];  // mask||key^mask
        const int           jce_embedded_pak_key_present;     // 1 here, 0 there

    The mask is fresh per build, which is exactly what makes an encrypted
    executable non-bit-reproducible -- intended, not a defect.
    """
    mask = secrets.token_bytes(32)
    shares = mask + bytes(k ^ m for k, m in zip(key, mask))
    rows = []
    for i in range(0, 64, 8):
        rows.append("    " + ", ".join(f"0x{b:02X}" for b in shares[i:i + 8]))
    body = ("," + "\n").join(rows)
    src = (
        "/* Auto-generated by scripts/jce.py - DO NOT EDIT, DO NOT COMMIT." "\n"
        " * Two XOR shares of the project asset key (share_a ^ share_b = key)" "\n"
        " * so the raw key never appears as a contiguous 32-byte constant." "\n"
        " * Mirrors editor/src/core/jce_pak_key.cpp; the shape is pinned by" "\n"
        " * tools/lint/check_pak_key_shares_contract.py. */" "\n"
        "const unsigned char jce_embedded_pak_key_shares[64] = {" "\n"
        + body + "\n" + "};" "\n"
        "const int jce_embedded_pak_key_present = 1;" "\n")
    if DRY_RUN:
        return
    out_c.parent.mkdir(parents=True, exist_ok=True)
    out_c.write_text(src, encoding="ascii", newline="\n")

def _stage_runtime_boot_settings(project: Path, cooked: Path, boot: Path) -> None:
    """Match CMake's canonical cooked RP staging before prebuilt packing."""
    pipeline = project / "Settings" / "RenderPipeline.rp.json"
    canonical = cooked / "settings" / "render_pipeline.rp.json"
    if pipeline.is_file():
        data = pipeline.read_bytes()
        if not canonical.is_file() or canonical.read_bytes() != data:
            canonical.parent.mkdir(parents=True, exist_ok=True)
            canonical.write_bytes(data)
    # Without an authored RP, independent cooked policy remains untouched.
    # Do not add a second canonical key to the generated boot resource root.
    for name in ("input_actions.json", "project-settings.json"):
        source = project / ".jce" / name
        output = boot / "settings" / name
        if source.is_file() and not (cooked / "settings" / name).is_file():
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, output)
        else:
            output.unlink(missing_ok=True)


def _dist_prebuilt_assets(project: Path, t: dict, bdir: Path,
                          manifest: dict, env: dict, sdk: Path,
                          sdk_tools: dict | None = None,
                          bundle_admission: dict | None = None) -> list:
    """Encrypt the public cooked graph and declared Bundle union in one PAK.

    Public Bundle/Archive owners inspect and import actual payloads. A sidecar
    cannot supply typed truth. Stock remains an explicitly unaudited superset.
    """
    if sdk_tools is None:
        sdk_tools = _sdk_host_tools(sdk, t)
    gen = bdir / "jce_generated"
    cooked = project / (manifest.get("cooked_assets") or "resources/_cooked")
    packer = sdk_tools.get("jce_pak", host_tool("jce_pak"))
    if not packer.exists():
        if "jce_pak" in sdk_tools:
            die("Confirmed SDK packer disappeared; retry app build")
        packer = ensure_host_tools(env, ("jce_pak",))["jce_pak"]
    delivery = _delivery_module()
    owner = argparse.Namespace(HOST=HOST, ROOT=ROOT, DRY_RUN=DRY_RUN, run=run, _sha256=_sha256)
    try:
        if bundle_admission is None:
            bundle_admission = delivery.inspect_project_bundles(owner, project, manifest, sdk, t, sdk_tools)
        delivery.verify_bundle_inputs(owner, bundle_admission)
        startup = bundle_admission['startup_scene'] if bundle_admission else manifest.get("startup_scene")
        delivery._admission_address(startup)
    except (ValueError, OSError) as error:
        die("Dist requires a validated startup scene: " + str(error))
    use_loose = bundle_admission is None or bundle_admission['use_loose_scene']
    resources = (project / (manifest.get("source_assets") or "resources")).resolve()
    scene = (resources / startup).resolve()
    bundle_tool = None
    if use_loose:
        if not scene.is_relative_to(resources) or not scene.is_file():
            die("Dist startup scene missing or outside source assets: " + str(scene))
        bundle_tool = sdk_tools.get("jce_bundle_pack")
        if bundle_tool is None:
            name = "jce_bundle_pack" + (".exe" if HOST == "windows" else "")
            bundle_tool = next((sdk / folder / name for folder in ("bin", "bin/host")
                                if (sdk / folder / name).is_file()), sdk / "bin" / name)
        if not bundle_tool.is_file():
            die("Selected SDK lacks public cooked graph resolver; refresh SDK: " + str(bundle_tool))
    graph_dir = gen / "content_graph"
    selected_archive = graph_dir / "admission.jbundle"
    generated_graph_files = (selected_archive, graph_dir / "admission.jbundle.json",
                             graph_dir / "bundle_graph.json")
    graph_files = generated_graph_files if use_loose else ()
    pak = gen / "project_assets.pak"
    bom = gen / "project_assets.bom.json"
    receipt = gen / "project_assets.receipt.json"
    boot = gen / "runtime_boot"
    if not DRY_RUN:
        graph_dir.mkdir(parents=True, exist_ok=True)
        for path in (*generated_graph_files, bom, receipt, pak, gen / "project_assets.obj",
                     gen / "project_assets.h", gen / "project_assets.cmake"):
            path.unlink(missing_ok=True)
        (boot / "jce").mkdir(parents=True, exist_ok=True)
        (boot / "jce" / "runtime_boot.json").write_text(
            json.dumps({"contract": "jce.runtime_boot", "schema": 1,
                        "startup_scene": startup}, indent=2) + "\n",
            encoding="utf-8", newline="\n")
        _stage_runtime_boot_settings(project, cooked, boot)
    if use_loose:
        stock = delivery.shader_stock_root(sdk)
        stock_flags = ["--shader-root", str(stock)] if stock else []
        run([str(bundle_tool), "--resource-root", str(resources), "--scene", str(scene),
             "--out", str(graph_dir), "--single-file", "--id", "admission",
             "--cook-assets", "--target", delivery.bundle_platform(t), *stock_flags], env=env, cwd=ROOT)
    selected_sources, selected_assets, provenance, identities = [], [], {}, {}
    if not DRY_RUN:
        try:
            if use_loose:
                _, assets = delivery._single_admission_assets(graph_dir, startup)
                metadata = json.loads(graph_files[1].read_text(encoding="utf-8"))
                selected_sources.append({'path': str(selected_archive), 'size': selected_archive.stat().st_size,
                                         'content_id': metadata.get('content_hash'), 'assets': assets})
            if bundle_admission:
                selected_sources.extend(bundle_admission['bundles'])
            selected_assets, provenance = delivery.merge_selected_assets(
                (source['path'], source['assets']) for source in selected_sources)
            if bundle_admission:
                native = delivery.native_registry_module().plan(manifest, selected_assets,
                    manifest.get('target') or manifest.get('name') or project.name)
                delivery.admit(manifest, [asset['path'] for asset in selected_assets], 'embedded',
                    ['lua', *sdk_shipped_script_backends(sdk)], selected_assets, native_plan=native)
            proof_files = list(graph_files)
            if bundle_admission:
                for source in bundle_admission['bundles']:
                    proof_files.extend((Path(source['path']), Path(source['inspection_path'])))
                    if source['sidecar_sha256'] is not None:
                        proof_files.append(Path(source['path'] + '.json'))
            identities = {str(path): _sha256(path) for path in proof_files}
        except (ValueError, OSError) as error:
            die("Dist typed content admission failed: " + str(error))
        addresses = set(provenance)
        for name in ("render_pipeline.rp.json", "input_actions.json", "project-settings.json"):
            address = "settings/" + name
            source = cooked / address if name == "render_pipeline.rp.json" else project / ".jce" / name
            output = boot / address
            if source.is_file() and (name == "render_pipeline.rp.json" or address not in addresses):
                output.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, output)
            else:
                output.unlink(missing_ok=True)
        boot_addresses = {"jce/runtime_boot.json", "settings/render_pipeline.rp.json",
                          "settings/input_actions.json", "settings/project-settings.json"}
        if any(path.is_file() and path.relative_to(boot).as_posix() not in boot_addresses
               for path in boot.rglob("*")):
            die("Unexpected file in generated runtime boot inputs; refresh this build cache")
    key = _project_pak_key(project)
    key_file = project / ".jce" / "pak_key.hex"
    web = bool(t.get("emscripten"))
    embed = pak if web else gen / "project_assets.obj"
    roots = [sdk / "share" / "jce" / name for name in ("engine_resources", "engine_ui")
             if (sdk / "share" / "jce" / name).is_dir()]
    res_flags = [part for root in (*roots, boot) for part in ("--resource-dir", str(root))]
    # No raw Bundle is separately embedded: validated entries enter this same
    # authenticated PAK, and runtime_boot chooses their actual selected scene.
    archives = [source['path'] for source in selected_sources] if not DRY_RUN else [str(selected_archive)]
    selected_flags = [part for path in archives for part in ("--selected-archive", path)]
    cmd = [str(packer), *res_flags, *selected_flags,
           "--pak-file", str(pak), "--json", str(bom),
           "--header-file", str(gen / "project_assets.h"),
           "--manifest-file", str(gen / "project_assets.cmake"),
           "--obj-format", "none" if web else "coff",
           "--symbol-prefix", "assets_pak_data", "--level", "19",
           "--platform", "web" if web else ("mobile" if t.get("host") in ("android", "ios") else "desktop"),
           "--bundle-target", delivery.bundle_platform(t), "--encrypt-key-file", str(key_file)]
    if not web:
        arch = t.get("eng_arch", t["arch"])
        if arch not in ("x64", "arm64", "x86", "arm"):
            die("Dist host wrapper lacks admitted object architecture: " + str(arch))
        cmd += ["--obj-file", str(embed), "--obj-arch", arch]
    run(cmd, env=env, cwd=ROOT)
    if not DRY_RUN:
        try:
            if any(_sha256(Path(path)) != digest for path, digest in identities.items()):
                raise ValueError("selected content proof changed during packing")
            delivery.verify_bundle_inputs(owner, bundle_admission)
            report = json.loads(bom.read_text(encoding="utf-8"))
            count = delivery.verify_selected_pack(report, pak, embed, selected_sources, selected_assets, provenance)
            result = {"schema": "jce.dist.asset-receipt.v1",
                      "project_assets": "declared-bundle-union" if bundle_admission else "selected-cooked-graph",
                      "startup_scene": startup, "stock_assets": "safe-superset; closure not verified",
                      "selected_count": len(selected_assets), "packed_count": count,
                      "inputs_sha256": identities, "pak_sha256": _sha256(pak),
                      "bom_sha256": _sha256(bom), "embed_sha256": _sha256(embed),
                      "deep_verification": "PASS", "runtime_parity": "NOT VERIFIED"}
            if bundle_admission:
                result['bundle_inputs'] = [{field: source[field] for field in
                    ('path', 'sha256', 'sidecar_sha256', 'sidecar_verified', 'sidecar_compatible',
                     'content_id', 'size')} for source in bundle_admission['bundles']]
                result['raw_bundle_embedding'] = 'OFF; entries share authenticated PAK'
            receipt.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
        except (ValueError, OSError, TypeError, AttributeError) as error:
            die("Dist asset proof failed: " + str(error))
    _write_pak_key_shares_c(gen / "jce_pak_key.c", key)
    var = "JCE_PROJECT_PREBUILT_ASSETS_PAK" if web else "JCE_PROJECT_PREBUILT_ASSETS_OBJ"
    result = [f"-D{var}={embed}",
              f"-DJCE_PROJECT_PREBUILT_PAK_KEY_C={gen / 'jce_pak_key.c'}",
              "-DJCE_PROJECT_STAGE_LOOSE_ASSETS=OFF"]
    if bundle_admission:
        result.append("-DJCE_PROJECT_BUNDLES=")
    return result


@_cache_owned(_app_cache_paths)
def cmd_app(args) -> None:
    project = Path(args.project).resolve()
    if not project.is_dir():
        die(f"project dir not found: {project}")
    manifest = read_manifest(project)
    t = resolve_target(args.arch)
    if t["host"] != HOST:
        die(f"app target {t['key']} must be built on host '{t['host']}'.")
    proof = getattr(args, "registry_proof", None)
    if proof and (not t.get("emscripten") or
                  _delivery_module().profile(args) != "embedded" or
                  args.clean or args.configure_only):
        die("--registry-proof audits an existing embedded Web artifact; "
            "it cannot combine with clean/configure-only or native targets")
    sdk = resolve_sdk(args, t)
    sdk_tools = _confirmed_app_sdk_tools(args, sdk, t)
    try:
        required_languages = _delivery_module().admit_project(sys.modules[__name__], args, project, manifest, sdk, target=t, sdk_tools=sdk_tools)
    except ValueError as error:
        die(str(error))
    try:
        _delivery_module().require_embedded_sdk(sys.modules[__name__], args, sdk)
    except ValueError as error:
        die(str(error))

    jce_cmake = sdk / "lib" / "cmake" / "JCE"
    if not (jce_cmake / "JCEConfig.cmake").exists():
        die(f"JCEConfig.cmake not under {jce_cmake}")
    config = "Debug" if args.variant == "debug" else "Release"
    bdir = _app_build_dir(project, t, args.variant)
    target = args.target or manifest.get("target") or manifest.get("name") or project.name
    if t.get("emscripten"):
        exe = args.exe or f"{target}.html"
    else:
        exe = _delivery_module().native_artifact_name(
            args.exe or manifest.get("exe") or manifest.get("name", project.name), HOST)
    if proof:
        # DIST emits fresh XOR key shares on every build. Rebuilding here would
        # invalidate the nonce/hash-bound receipt from the previous browser run.
        found = next((p for p in (bdir / exe, bdir / config / exe) if p.is_file()), None)
        if not found:
            die(f"registry proof artifact missing: {exe} (under {bdir})")
        _delivery_module().audit_application(sys.modules[__name__], args, found)
        log(f"app: verified existing {found} (without rebuilding)")
        return
    # MSVC env would inject the wrong toolchain into a wasm consumer build;
    # the SDK fat lib carries all deps, so a clean env + Emscripten.cmake is
    # the whole toolchain story (mirrors _sdk_one).
    env = dict(os.environ) if t.get("emscripten") else msvc_env(t)
    if args.clean and bdir.exists():
        _standalone_module().guard_cache_clean((bdir,), project / "build")
        log(f"clean: {bdir}")
        if not DRY_RUN:
            shutil.rmtree(bdir, ignore_errors=True)
    cfg = ["cmake", "-S", str(project), "-B", str(bdir), "-G", "Ninja",
           f"-DCMAKE_BUILD_TYPE={config}",
           f"-DJCE_BUILD_VARIANT={args.variant}",
           # Tool paths are find_program cache entries. A build directory may
           # be reused with --sdk pointing at a different installation, so
           # make the selected SDK authoritative while preserving all other
           # project cache state.
           "-U", "JCE_PAK_EXECUTABLE",
           "-U", "JCE_COOK_EXECUTABLE",
           "-U", "JCE_BIN2OBJ_EXECUTABLE",
           f"-DJCE_DIR={jce_cmake}", *_delivery_module().bundle_cmake_args(project, manifest)]
    cfg.extend(_delivery_module().native_registry_cmake_args(args))
    for name, variable in (("jce_pak", "JCE_PAK_EXECUTABLE"),
                           ("jce_cook", "JCE_COOK_EXECUTABLE"),
                           ("jce_bin2obj", "JCE_BIN2OBJ_EXECUTABLE")):
        if name in sdk_tools:
            cfg.append(f"-D{variable}={sdk_tools[name]}")
    # dist: the SDK helper will not pack raw resources -- supply the prebuilt
    # authenticated PAK and key shares ourselves (see _dist_prebuilt_assets).
    cfg.extend(["-U", "JCE_PROJECT_PREBUILT_ASSETS_PAK",
                "-U", "JCE_PROJECT_PREBUILT_ASSETS_OBJ",
                "-U", "JCE_PROJECT_PREBUILT_ASSETS_ASM",
                "-U", "JCE_PROJECT_PREBUILT_ASSETS_C",
                "-U", "JCE_PROJECT_PREBUILT_PAK_KEY_C"])
    if args.variant == "dist":
        cfg.extend(_dist_prebuilt_assets(project, t, bdir,
                                         manifest, env, sdk, sdk_tools=sdk_tools,
                                         bundle_admission=getattr(args, "_bundle_admission", None)))
    if _delivery_module().profile(args) == "embedded":
        if args.variant == "debug":
            die("embedded SDK is release/dist; use authoring for debug applications")
        if t["host"] == "windows":
            cfg.append("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded")
    if t.get("emscripten"):
        cfg.extend([
            f"-DCMAKE_TOOLCHAIN_FILE={emscripten_paths()['toolchain']}",
            # CMake 3.27 probes this feature with `emcc -Wl,--help` while
            # enabling C/C++.  That starts Emscripten's full JS-symbol cache
            # path merely to inspect a native-GNU-linker capability and can
            # stall in restricted or concurrent build environments.  Web
            # links do not consume GNU linker depfiles, so state this once.
            "-DCMAKE_C_LINKER_DEPFILE_SUPPORTED=FALSE",
            "-DCMAKE_CXX_LINKER_DEPFILE_SUPPORTED=FALSE",
        ])
    elif HOST == "windows":
        cfg.append("-DCMAKE_C_COMPILER=cl")
    run(cfg, env=env, cwd=ROOT)
    if args.configure_only:
        log(f"app: configured {project.name} (via SDK at {sdk}) [--configure-only]")
        return
    run(["cmake", "--build", str(bdir), "--target", target], env=env, cwd=ROOT)
    found = next((p for p in (bdir / exe, bdir / config / exe) if p.exists()), None)
    if not found and not DRY_RUN:
        die(f"build succeeded but artifact missing: {exe} (under {bdir})")
    if found:
        try:
            _delivery_module().verify_bundle_inputs(sys.modules[__name__], getattr(args, "_bundle_admission", None))
        except (ValueError, OSError) as error:
            die("Declared Bundle changed before artifact acceptance: " + str(error))
        _delivery_module().audit_application(sys.modules[__name__], args, found)
    log(f"app: {found if found else exe} (via SDK at {sdk})")


# ── Subcommand: package (stage redistributable bundles) ───────────────────
_EDITOR_BUNDLE_README = """\
JCE Editor bundle ({platform}-{arch}, {variant})
=================================================

Contents:
  jce_editor.exe   The editor. Double-click to launch.
  sdk/             Engine SDK (headers, libs, CMake config, resources).
  LICENSE, THIRD_PARTY_LICENSES.md   Source and dependency license notices.

Optional scripting runtimes (NOT bundled):
  * .NET 8 runtime for C#; Java 21 with JAVA_HOME for Java.
  * A matching Python installation is needed unless its embeddable runtime
    was supplied through package editor --python-runtime. Binding modules
    are staged alongside the editor in either case.
  * Microsoft Visual C++ Redistributable (x64 for an x64 bundle).

Project build tools (NOT bundled; not needed merely to launch the editor):
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


def _announce_codec_policy(variant: str, what: str) -> None:
    """Say plainly what a bundle carries, at the moment it is staged.

    `package editor` and `package game` both default to variant=release, and
    release deliberately carries the vendored AAC / H.264 / H.265 adapters —
    the editor needs them to import legacy assets.  That is the decided
    policy, not an accident.  What was missing is that the person running the
    command was never told: a plain `jce.py package game` produced a
    royalty-bearing bundle with nothing in the output saying so.

    Not a prompt and not a default change: the release bundle is supposed to
    have them.  Just a line that makes the choice visible where it is made,
    so shipping royalty-free is a decision rather than a discovery."""
    if variant == "dist":
        log(f"{what}: dist variant — royalty-free "
            "(AAC / H.264 / H.265 adapters compiled OUT)")
    else:
        log(f"{what}: {variant} variant — INCLUDES patent-encumbered codec "
            "adapters (AAC / H.264 / H.265). You are responsible for "
            "licensing. Use --variant dist for a royalty-free bundle.")

def _package_editor_cache_paths(values):
    paths = _editor_cache_paths(values)
    args = values["args"]
    return paths + _standalone_module().editor_package_cache_paths(
        argparse.Namespace(**globals()), args, resolve_target(args.arch))


@_cache_owned(_package_editor_cache_paths)
def _package_embedded_editor(args) -> None:
    _standalone_module().package_editor(sys.modules[__name__], args)


def cmd_package_editor(args) -> None:
    """Stage the editor delivery selected by the caller."""
    if _delivery_module().profile(args) == "embedded":
        _package_embedded_editor(args)
        return
    t = resolve_target(args.arch)
    if t["host"] != HOST:
        die(f"editor package target {t['key']} must be built on host '{t['host']}'.")
    variant = args.variant                       # release | dist
    suffix  = "-dist" if variant == "dist" else ""
    _announce_codec_policy(variant, "editor bundle")
    _, src  = _authoring_editor_paths(t, variant)
    editor_exe = src / _host_exe("jce_editor")
    sdk_dir = sdk_install_dir(t, variant)
    out = (Path(args.out).resolve() if args.out
           else ROOT / "dist" / "editor" / f"{SDK_TAG[t['host']]}-{t['arch']}{suffix}")

    if not args.skip_build:
        cmd_editor(argparse.Namespace(arch=args.arch, variant=variant, clean=False,
                                      delivery_profile="authoring"))
        cmd_sdk(argparse.Namespace(arch=args.arch, variant=variant,
                                   no_debug=True, clean=False, delivery_profile="authoring"))

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
        _assert_no_profiler_listener(out / editor_exe.name, variant)
        for pattern in ("*.dll", "*.so", "*.so.*", "*.dylib", "*.runtimeconfig.json"):
            for runtime in src.glob(pattern):
                shutil.copy2(runtime, out / runtime.name)
        for runtime in src.glob("jce_script_*"):
            if runtime.is_dir():
                shutil.copytree(runtime, out / runtime.name)
        for assembly in out.glob("*.dll"):
            config = src / (assembly.stem + ".runtimeconfig.json")
            if assembly.name == "JceScript.dll" and not config.is_file():
                die(f"managed scripting runtime configuration missing: {config}")
        python_runtime = getattr(args, "python_runtime", None)
        if python_runtime:
            runtime_dir = Path(python_runtime).resolve()
            dlls = list(runtime_dir.glob("python3*.dll"))
            versions = [dll.stem for dll in dlls if dll.stem != "python3"]
            if not versions or not all((runtime_dir / (v + ext)).is_file()
                                       for v in versions for ext in (".zip", "._pth")):
                die(f"incomplete Python embeddable runtime: {runtime_dir}")
            if not (runtime_dir / "LICENSE.txt").is_file():
                die(f"Python runtime license missing: {runtime_dir}")
            for runtime in runtime_dir.iterdir():
                if not runtime.is_file() or runtime.is_symlink():
                    die(f"expected original flat Python embeddable runtime: {runtime}")
                shutil.copy2(runtime, out / runtime.name)
        for notice in ("LICENSE", "THIRD_PARTY_LICENSES.md"):
            shutil.copy2(ROOT / notice, out / notice)
        shutil.copytree(sdk_dir, out / "sdk", dirs_exist_ok=True)
        (out / "VERSION.txt").write_text(
            "product:  JCE Editor bundle\n"
            f"platform: {SDK_TAG[t['host']]}\narch:     {t['arch']}\n"
            f"variant:  {variant}\ncommit:   {git_short_sha()}\n", encoding="utf-8")
        (out / "README.txt").write_text(
            _EDITOR_BUNDLE_README.format(platform=SDK_TAG[t["host"]],
                                         arch=t["arch"], variant=variant), encoding="utf-8")
    log(f"package editor: {out}")


def _assert_no_profiler_listener(exe: Path, variant: str) -> None:
    """A shipped game must not carry a profiler that opens a listening socket.

    Tracy starts its server during static initialisation, before main() -- so
    the socket exists whether or not anyone ever profiles, and Windows raises
    its firewall dialog on an executable the player only wanted to run.  The
    CMake side already refuses a Tracy built without TRACY_ONLY_LOCALHOST, but
    that governs WHERE it listens, not WHETHER it listens, and it cannot see a
    bundle staged from an SDK that was built earlier under different options.

    So look at the artefact itself.  A bundle staged in June carried 1153 Tracy
    strings and nobody noticed for two months.
    """
    markers = (b"TracyVsync", b"TracyPrf", b"TRACY_TIMER_QPC", b"TracyClient")
    try:
        blob = exe.read_bytes()
    except OSError as exc:
        log(f"WARN: cannot scan {exe.name} for a profiler listener: {exc}")
        return
    hits = [m.decode() for m in markers if m in blob]
    if not hits:
        log(f"no profiler listener in {exe.name} "
            f"(checked {len(markers)} Tracy markers)")
        return
    detail = ", ".join(hits)
    if variant == "dist":
        die(f"{exe.name} was staged as a dist bundle but contains Tracy "
            f"({detail}). dist means 'never ships with Tracy', so the SDK this "
            f"linked against was a release SDK -- rebuild it with "
            f"`python scripts/jce.py sdk --variant dist`. Shipping this exe "
            f"would open a listening socket at startup and prompt the Windows "
            f"Firewall on every player's machine.")
    log(f"WARN: {exe.name} contains Tracy ({detail}). It opens a listening "
        f"socket during static initialisation, so running it prompts the "
        f"Windows Firewall. That is expected for --variant release; use the "
        f"default (dist) for anything you hand to someone else.")


def _package_component(value: str) -> bool:
    """Portable native package leaf contract; no filesystem mutation."""
    if not isinstance(value, str) or not value or ".." in value or value[-1] in ". ":
        return False
    if any(ord(c) < 32 or ord(c) == 127 or c in '/\\:*?"<>|' for c in value):
        return False
    stem = value.split(".", 1)[0].upper()
    return stem not in {"CON", "PRN", "AUX", "NUL"} and not (
        len(stem) == 4 and stem[:3] in {"COM", "LPT"} and stem[3] in "123456789")


def _project_package_output(args, project: Path, sdk: Path, manifest: dict,
                            target: dict, target_name: str, exe: str) -> Path:
    """Read-only CLI preflight. Native admission/publication rechecks ownership."""
    app = manifest.get("name") or target_name
    for leaf in (app, target_name, exe, SDK_TAG[target["host"]], target["arch"], args.variant):
        if not _package_component(leaf):
            die(f"package: unsafe filename component: {leaf!r}")
    try:
        output = (Path(args.out) if getattr(args, "out", None) else
                  project / "dist/apps" / app /
                  f"{SDK_TAG[target['host']]}-{target['arch']}-{args.variant}").resolve()
        workspace = ROOT.resolve()
        sdk = sdk.resolve()
        if (len(str(output).encode("utf-8")) + len(exe.encode("utf-8")) + 2 >= 1024 or
                any(ord(c) < 32 or ord(c) == 127 for c in str(output))):
            die("package: unsafe or overlong output path")
        if output == Path(output.anchor) or project.is_relative_to(output):
            die(f"package: output overlaps project/root: {output}")
        # A source checkout's final outputs belong to dist; explicit external
        # directories remain allowed after the same content/overlap checks.
        if output.is_relative_to(project) and not output.is_relative_to(project / "dist"):
            die(f"package: output overlaps project source/build directory: {output}")
        if (output.is_relative_to(workspace) and
                not output.is_relative_to(workspace / "dist") and
                not output.is_relative_to(project / "dist/apps")):
            die(f"package: output overlaps workspace source/build directory: {output}")
        protected = [sdk]
        for field, default in (("source_assets", "assets"), ("cooked_assets", "resources/_cooked")):
            value = manifest.get(field) or default
            if not isinstance(value, str):
                die(f"package: {field} must be a path string")
            protected.append((project / value).resolve())
        for directory in protected:
            if output.is_relative_to(directory) or directory.is_relative_to(output):
                die(f"package: output overlaps protected directory: {directory}")
        if output.exists() and not output.is_dir():
            die(f"package: output directory is a file: {output}")
        if output.is_dir():
            for entry in output.iterdir():
                if (args.variant != "dist" or entry.name != exe or
                        entry.is_symlink() or getattr(entry, "is_junction", lambda: False)() or
                        not entry.is_file()):
                    die(f"package: output contains an unowned entry (kept): {entry}")
    except (OSError, ValueError) as error:
        die(f"package: cannot safely resolve output: {error}")
    return output


def cmd_package_game(args) -> None:
    """Compatibility entry to the single native headless build/package owner."""
    project = Path(args.project).resolve()
    if not (project / "jce_project.json").is_file():
        die(f"no jce_project.json under {project}")
    manifest = read_manifest(project)
    if getattr(args, "with_loose", False) or manifest.get("stage_loose", False):
        die("package game: --with-loose/stage_loose is unsupported by the headless "
            "request; use the editor's explicit authoring workflow")
    for field, default in (("name", project.name), ("version", "0.0.0")):
        requested = getattr(args, field, None)
        if requested is not None and requested != (manifest.get(field) or default):
            die(f"package game: --{field} override is unsupported; update "
                f"jce_project.json {field!r} before packaging")
    forwarded = argparse.Namespace(**vars(args))
    for field in ("editor", "result", "registry_proof"):
        if not hasattr(forwarded, field):
            setattr(forwarded, field, None)
    if not hasattr(forwarded, "delivery_profile"):
        forwarded.delivery_profile = "embedded"
    log("package game: using the native headless build/package pipeline")
    cmd_build_project(forwarded)


# ── Subcommand: cook / serve / lint (folded wrappers) ─────────────────────
def cmd_cook(args) -> None:
    project = Path(args.project).resolve()
    sdk = getattr(args, "sdk", None)
    target = getattr(args, "sdk_target", None) or resolve_target(None)
    sdk_tools = getattr(args, "sdk_tools", None)
    if sdk_tools is None:
        sdk_tools = _sdk_host_tools(Path(sdk), target) if sdk else {}
    cooker = sdk_tools.get("jce_cook", host_tool("jce_cook"))
    if not cooker.exists():
        if "jce_cook" in sdk_tools:
            die("Confirmed SDK cooker disappeared; retry app build")
        env = msvc_env(resolve_target(None))
        cooker = ensure_host_tools(env, ("jce_cook",))["jce_cook"]
    manifest = read_manifest(project)
    src = project / (manifest.get("source_assets") or "assets")
    out = project / (manifest.get("cooked_assets") or "resources/_cooked")
    # Target platform drives GPU texture compression (desktop -> BC, mobile ->
    # ASTC). Default to the host so a plain `cook` produces compressed textures.
    plat = getattr(args, "platform", None) or resolve_target(None)["host"]
    command = [str(cooker), "--batch", str(src), str(out),
               "--preserve-names", "--level", "0", "--max-texture-size", "2048",
               "--platform", plat]
    if getattr(args, "rgba8", False):
        command.append("--rgba8")
    if getattr(args, "mipmaps", False):
        command.append("--mipmaps")
    run(command, cwd=ROOT)


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


def cmd_build_project(args) -> None:
    """Drive the editor's headless project build from the command line.

    WHY THIS EXISTS.  The editor already owns the whole shipped-game pipeline
    -- asset cook, authenticated PAK, CMake orchestration, package staging, and
    the Dist audit -- and exposes it through a validated env contract
    (JCE_HEADLESS_BUILD_*, editor/src/core/jce_editor_headless_build.cpp:30-38),
    driven from editor/src/jce_editor_main.cpp:519.  Nothing could reach it:
    `grep -rn JCE_HEADLESS_BUILD scripts/ tools/ cmake/ CMakeLists.txt` found
    zero hits, so the only way to produce a game package was for a human to
    click Build in the editor.  A project that needs a weekly playable build,
    an automated release acceptance, and an independent verification run in a
    fresh worktree cannot do any of the three that way.

    THE PROCESS EXIT CODE IS NOT THE RESULT.  The editor quits cleanly whether
    the build succeeded or failed; the driver "owns terminal result
    publication" and writes a jce.editor.headless-build-result.v1 JSON to
    JCE_HEADLESS_BUILD_RESULT.  So this reads that file and fails on anything
    but state == "succeeded".  It also DELETES the result first: a run that
    crashes before publishing would otherwise leave the previous run's success
    on disk and be read as a pass -- the same shape as ctest's stale
    LastTestsFailed.log, which has produced false reports in this repo before.
    """
    project = Path(args.project).resolve()
    if not (project / "jce_project.json").is_file():
        die(f"no jce_project.json under {project}")
    manifest = read_manifest(project)

    t = resolve_target(args.arch)
    if t["host"] != HOST:
        die(f"build-project target {t['key']} must be built on host '{t['host']}'.")
    sdk = resolve_sdk(args, t)
    target = args.target or manifest.get("target") or project.name
    try:
        exe = _delivery_module().native_artifact_name(
            args.exe or manifest.get("exe") or target, HOST)
    except ValueError as error:
        die(str(error))
    out = _project_package_output(args, project, sdk, manifest, t, target, exe)
    try:
        required_languages = _delivery_module().admit_project(sys.modules[__name__], args, project, manifest, sdk, target=t)
    except ValueError as error:
        die(str(error))
    try:
        _delivery_module().require_embedded_sdk(sys.modules[__name__], args, sdk)
    except ValueError as error:
        die(str(error))


    # The editor exe: built by `jce.py editor`, landing in <build>/<variant>.
    #
    # NEWEST WINS, not a fixed dist-then-release order.  The first version of
    # this preferred dist -- "a release acceptance should be driven by the dist
    # editor" -- and on the machine it was written on that picked a build from
    # five weeks earlier that predated the headless contract entirely.  It ran,
    # exited 0, and published nothing; only the missing-result check below said
    # anything.  Freshness is the property that matters for a driver.
    exe_name = "jce_editor.exe" if HOST == "windows" else "jce_editor"
    if args.editor:
        editor = Path(args.editor).resolve()
        if not editor.is_file():
            die(f"no editor executable at {editor}")
    else:
        if _delivery_module().profile(args) == "embedded":
            native = _standalone_module()
            cands = []
            for variant in ("dist", "release"):
                request = argparse.Namespace(variant=variant)
                try:
                    plan = native.native_cache_plan(sys.modules[__name__], request, t,
                                                     exact_variant=True)
                except (ValueError, OSError):
                    continue
                cands.append(native.cached_editor_artifact(plan["build"], variant, HOST))
        else:
            cands = [_authoring_editor_paths(t, variant)[1] / exe_name for variant in ("dist", "release")]
        cands = [c for c in cands if c.is_file()]
        if not cands:
            die("no compatible configured editor executable for the selected delivery profile -- "
                f"run `python scripts/jce.py editor` first, or pass --editor PATH")
        editor = max(cands, key=lambda p: p.stat().st_mtime)

    # PRE-FLIGHT, not post-mortem: an editor built before the
    # JCE_HEADLESS_BUILD_* contract existed does not merely publish nothing --
    # it does not recognise the environment at all, so it opens the GUI and
    # waits for a human.  In a CI that is a hang, not a failure.  The contract's
    # variable names are string literals in any binary that supports it, so
    # refuse up front and say why.
    try:
        if b"JCE_HEADLESS_BUILD_PROJECT" not in editor.read_bytes():
            built = _dt.datetime.fromtimestamp(
                editor.stat().st_mtime).strftime("%Y-%m-%d")
            die(f"{editor} (built {built}) does not contain the "
                f"JCE_HEADLESS_BUILD_* contract -- it predates the headless "
                f"build feature and would open the GUI and wait.  Rebuild with "
                f"`python scripts/jce.py editor`, or pass a newer --editor.")
    except OSError as e:  # noqa: BLE001
        die(f"cannot read editor executable {editor}: {e}")

    # RESULT must live OUTSIDE OUT: the request validator rejects a private
    # result path inside the public package, so it cannot leak into a bundle.
    result = (Path(args.result).resolve() if args.result
              else ROOT / "build" / "headless" /
              f"{target}-{args.variant}-{t['arch']}.result.json")
    if result.is_relative_to(out):
        die("build-project: private result path must be outside the public package")
    if not DRY_RUN:
        result.parent.mkdir(parents=True, exist_ok=True)
        if result.exists():
            result.unlink()

    env = dict(os.environ)
    env.update({
        "JCE_DELIVERY_PROFILE": _delivery_module().profile(args),
        "JCE_REQUIRED_SCRIPT_LANGUAGES": ";".join(required_languages),
        "JCE_REQUIRED_SCRIPT_TARGET": target,
        "JCE_HEADLESS_BUILD_PROJECT": str(project),
        "JCE_HEADLESS_BUILD_SDK":     str(sdk),
        "JCE_HEADLESS_BUILD_TARGET":  target,
        "JCE_HEADLESS_BUILD_EXE":     exe,
        "JCE_HEADLESS_BUILD_VARIANT": args.variant,
        "JCE_HEADLESS_BUILD_ARCH":    t["arch"],
        "JCE_HEADLESS_BUILD_OUT":     str(out),
        "JCE_HEADLESS_BUILD_RESULT":  str(result),
        "JCE_HEADLESS_BUILD_CLEAN":   "1" if args.clean else "0",
    })

    for field in ('JCE_REQUIRED_NATIVE_FACTORIES', 'JCE_REQUIRED_NATIVE_SCRIPT_PATHS',
                  'JCE_NATIVE_SCRIPT_CLOSURE_SHA256'):
        env.pop(field, None)
    plan = getattr(args, '_native_registry_plan', None)
    if plan:
        for flag in _delivery_module().native_registry_module().cmake_args(plan):
            key, value = flag.removeprefix('-D').split('=', 1)
            env[key] = value

    log(f"build-project: {target} ({args.variant}/{t['arch']}) "
        f"via {editor.name}")
    log(f"  project {project}")
    log(f"  sdk     {sdk}")
    log(f"  out     {out}")
    log(f"  result  {result}")
    log(f"$ {editor}")
    if DRY_RUN:
        return
    # Deliberately NOT run(): that helper dies on a non-zero exit, and the
    # result file has to be read even when the editor exits badly -- the
    # process code is not the verdict here, the published result is.
    proc = subprocess.run([str(editor)], cwd=str(ROOT), env=env)

    if not result.is_file():
        die(f"editor exited ({proc.returncode}) without publishing {result} -- "
            f"the build did not reach the driver's result step.  The stale "
            f"result was deleted before this run, so nothing was published; "
            f"this is not a leftover being misread.")
    try:
        res = json.loads(result.read_text(encoding="utf-8"))
    except Exception as e:  # noqa: BLE001
        die(f"unreadable build result {result}: {e}")

    schema = res.get("schema", "")
    if schema != "jce.editor.headless-build-result.v1":
        die(f"unexpected result schema '{schema}' in {result}")

    state = res.get("state", "failed")
    for key in ("artifact_path", "package_path", "asset_bom_path",
                "dist_audit_path", "log_path"):
        if res.get(key):
            log(f"  {key.replace('_', ' '):<16} {res[key]}")
    if res.get("log_complete") is False:
        log(f"  build log {res.get('log_status', 'unavailable')}: "
            f"complete=false truncated={res.get('log_truncated', False)} "
            f"incomplete={res.get('log_incomplete', False)} "
            f"reason={res.get('log_error') or 'no diagnostic text'}")
    if state != "succeeded":
        die(f"build-project FAILED (state={state} stage={res.get('stage')} "
            f"exit_code={res.get('exit_code')}): {res.get('error') or 'no error text'}")
    if res.get("artifact_path"):
        _delivery_module().audit_application(sys.modules[__name__], args, Path(res["artifact_path"]))
    log(f"build-project: OK ({target} -> {res.get('package_path') or out})")


def cmd_bundle(args):
    import importlib.util
    spec = importlib.util.spec_from_file_location("jce_bundle_command",
        Path(__file__).with_name("bundle.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.execute(args, argparse.Namespace(HOST=HOST, ROOT=ROOT,
        ensure_host_tools=ensure_host_tools, run=run))


def cmd_stock_app(args):
    from tools.build import stock_app
    try:
        result = stock_app.execute(args, driver=sys.modules[__name__])
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        die(f"stock-app: {error}")
    print(json.dumps(result, sort_keys=True))


def cmd_lint(args) -> None:
    """Run the lint suite, and by default the architecture audit beside it.

    `run_all.py` is not all the gates: the ABI snapshot against the TRACKED
    contracts/abi-snapshot.txt, dependency boundaries and licences, view
    reservations, binding parity, the script-VM and language-catalog contracts,
    SDK scripting export, the patented-codec gate, and the two script
    generators in check mode all live in tools/audit/run_architecture_audit.py.
    Until 2026-08-31 this subcommand ran only the first of the two, so the
    documented closing procedure and the build driver disagreed about what
    "lint" means -- and the half that guards a tracked contract was the half
    the driver left out.  `--lints-only` restores the old behaviour.
    """
    run([sys.executable, str(ROOT / "tools" / "lint" / "run_all.py")], cwd=ROOT)
    if not getattr(args, "lints_only", False):
        run([sys.executable,
             str(ROOT / "tools" / "audit" / "run_architecture_audit.py")], cwd=ROOT)


def cmd_accept(args):
    """Run the shipped-artifact acceptance protocol.

    scripts/jce_accept.py has existed since 2026-08-27 with ZERO callers: no
    subcommand, no CI step, no documentation pointing at it -- only a mention in
    a comment in tools/jce_determinism.py.  An eight-stage protocol that proves
    the SHIPPED artifact boots, finds its scene, renders a non-blank frame and
    stays inside its budgets is worth exactly nothing while nothing invokes it,
    and "the tool exists" reads as "the thing is covered".

    This is the invocation.  It forwards rather than reimplements: the protocol
    stays general and project-agnostic (it knows no project by name, which is
    the standing rule), and this only makes it reachable the way every other
    workflow in this repo is reachable.
    """
    script = ROOT / "scripts" / "jce_accept.py"
    if not script.is_file():
        die("accept: %s is missing" % script)
    cmd = [sys.executable, str(script), "--project", args.project]
    if args.bundle:      cmd += ["--bundle", args.bundle]
    if args.skip_package: cmd += ["--skip-package"]
    if args.arch:        cmd += ["--arch", args.arch]
    if args.variant:     cmd += ["--variant", args.variant]
    if args.backend:     cmd += ["--backend", args.backend]
    if args.frames:      cmd += ["--frames", str(args.frames)]
    if args.json:        cmd += ["--json", args.json]
    if args.keep:        cmd += ["--keep"]
    for name, val in (("--budget-frame-ms", args.budget_frame_ms),
                      ("--budget-draws", args.budget_draws),
                      ("--budget-rss-mb", args.budget_rss_mb)):
        if val is not None:
            cmd += [name, str(val)]
    # run() dies on a non-zero exit, which is what acceptance means:
    # a failed protocol must fail the command that invoked it.
    run(cmd)


@_cache_owned(_test_cache_paths)
def cmd_test(args) -> None:
    """Build the unit-test binary, then run it.

    Build first, always.  ctest reports an exe that was never built as `Not
    Run`, which reads exactly like a failure and has cost this repository real
    time more than once.  The driver had no test subcommand at all before
    2026-08-31 -- `grep -n 'JCE_BUILD_TESTS\\|ctest\\|jce_tests' scripts/jce.py`
    returned nothing -- so the one command people run did not know the suite
    existed.
    """
    suite = getattr(args, "suite", "all")
    if suite not in ("all", "engine", "editor", "consumer"):
        die("test: invalid --suite (expected all, engine, editor or consumer)")
    ai_flags = _test_ai_dispatch_flags(args)
    sdk = getattr(args, "sdk", None)
    sdk_variant = getattr(args, "sdk_variant", None)
    sdk_delivery = getattr(args, "sdk_delivery_profile", None)
    sdk_selected = sdk is not None
    if (sdk_selected or sdk_variant or sdk_delivery) and not getattr(args, "configure", False):
        die("test: installed-SDK selection requires --configure")
    if (sdk_variant or sdk_delivery) and not sdk_selected:
        die("test: --sdk-variant/--sdk-delivery-profile require --sdk")
    targets = getattr(args, "target", None) or ["jce_tests" if suite == "all" else "jce_tests_" + suite]
    if getattr(args, "target", None) and not getattr(args, "filter", None):
        die("test: explicit --target requires --filter for the CTest run range")
    if any(not isinstance(target, str) or
           re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.+-]*", target) is None for target in targets):
        die("test: invalid build target name")
    repeat = getattr(args, "repeat_until_fail", None)
    if repeat is not None and repeat < 1:
        die("test: --repeat-until-fail must be positive")
    tgt = resolve_target(getattr(args, "arch", None))
    env = msvc_env(tgt)
    variant = getattr(args, "variant", "release")
    build_dir = preset_binary_dir(tgt, variant)
    if getattr(args, "configure", False):
        conan_install(tgt, "Debug" if variant == "debug" else "Release", env)
        # CMakeUserPresets.json belongs to local IDE configuration.
        sdk_flags = []
        if sdk_selected:
            sdk_flags = [f"-DJCE_TEST_SDK_DIR={Path(sdk).resolve().as_posix()}",
                         f"-DJCE_TEST_SDK_ARCH={tgt['arch']}",
                         f"-DJCE_TEST_SDK_VARIANT={sdk_variant or 'release'}",
                         f"-DJCE_TEST_SDK_DELIVERY={sdk_delivery or 'authoring'}"]
        run(["cmake", "--preset", preset_name(tgt, variant),
             "-DJCE_BUILD_TESTS=ON", "-DJCE_BUILD_EDITOR=OFF", f"-DJCE_BUILD_VARIANT={variant}",
             *sdk_flags, *ai_flags,
             *_delivery_module().cmake_flags(getattr(args, "delivery_profile", "authoring"),
                 restore_authoring=_delivery_module().restore_authoring(build_dir / "CMakeCache.txt")),
             "-DJCE_ENABLE_CPPCHECK=OFF"], env=env, cwd=ROOT)
    if not DRY_RUN and not (build_dir / "CMakeCache.txt").is_file():
        raise SystemExit(
            "no configured build at %s -- run `python scripts/jce.py editor` "
            "first, or pass --arch for a different target" % build_dir)
    run(["cmake", "--build", str(build_dir), "--target", *targets], env=env, cwd=ROOT)
    ctest = ["ctest", "--test-dir", str(build_dir),
             "-j", ctest_jobs(getattr(args, "jobs", None))]
    if suite != "all":
        ctest += ["-L", "^domain:" + suite + "$"]
    ctest.append("--no-tests=error")
    if getattr(args, "filter", None):
        ctest += ["-R", args.filter]
    if repeat is not None:
        ctest += ["--repeat", "until-fail:%u" % repeat]
    if getattr(args, "rerun_failed", False):
        ctest.append("--rerun-failed")
    configured_python = None
    cache = build_dir / "CMakeCache.txt"
    cache_lines = cache.read_text(encoding="utf-8", errors="replace").splitlines() if cache.is_file() else []
    for line in cache_lines:
        if line.startswith("_Python3_EXECUTABLE:INTERNAL="):
            candidate = Path(line.partition("=")[2])
            if candidate.is_file():
                configured_python = candidate
            break
    # vcvarsall is created from a clean build-tool PATH. Tests also load the
    # configured Python DLL and invoke javac; those runtime directories must
    # be present for ctest without contaminating the compiler environment.
    test_env = dict(env)
    test_env.update({key: value for key, value in os.environ.items()
                     if key.startswith("JCE_TEST_")})
    if HOST == "windows":
        runtime_dirs = []
        if configured_python is not None:
            runtime_dirs.append(str(configured_python.parent))
        java_home = os.environ.get("JAVA_HOME")
        javac = (Path(java_home) / "bin" / "javac.exe"
                 if java_home else None)
        if javac is None or not javac.is_file():
            ambient_javac = shutil.which("javac")
            javac = Path(ambient_javac) if ambient_javac else None
        if javac is not None and javac.is_file():
            runtime_dirs.append(str(javac.parent))
            test_env["JAVA_HOME"] = str(javac.parent.parent)
        path_key = next((key for key in test_env if key.upper() == "PATH"),
                        "PATH")
        test_env[path_key] = os.pathsep.join(
            runtime_dirs + [test_env.get(path_key, "")])
    run(ctest, env=test_env, cwd=ROOT)

    # Independent acceptance is part of a complete all-suite run. A caller
    # selecting a target, regex, domain or failed-test subset must not acquire
    # an unrelated cooker build after its selected CTest checks complete.
    if (suite != "all" or getattr(args, "target", None)
            or getattr(args, "filter", None) or getattr(args, "rerun_failed", False)):
        return

    # The colour-space chain, which the unit suite structurally cannot reach.
    #
    # tools/verify_colour_space_chain.py crosses a process boundary twice --
    # model importer -> <texture>.import.json -> cooker -> cooked mip bytes --
    # and asserts the BYTES at the far end.  Its own docstring says why it
    # exists: "Every link there was verified once, by hand, by me, on this
    # machine.  That is not verification -- it is an anecdote."
    #
    # It then had NO CALLER, which made it an anecdote with a shebang.  It
    # needs jce_cook.exe, so it cannot live in run_all.py (build-independent
    # by design) and belongs here, where a build is guaranteed.
    run(["cmake", "--build", str(build_dir), "--target", "jce_cook"], env=env, cwd=ROOT)
    verification_python = None
    for candidate in (Path(sys.executable), configured_python):
        if candidate is None:
            continue
        probe = subprocess.run(
            [str(candidate), "-c", "import PIL, numpy"], env=test_env,
            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if probe.returncode == 0:
            verification_python = candidate
            break
    if verification_python is None:
        raise SystemExit("colour-space verification requires Pillow and numpy "
                         "in the driver or configured Python interpreter")
    run([str(verification_python),
         str(ROOT / "tools" / "verify_colour_space_chain.py"),
         "--cook", str(build_dir / "tools" / _host_exe("jce_cook"))], env=test_env, cwd=ROOT)


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
    desktop = ROOT / "build/desktop"
    # XP candidates have a separate audit and publication owner.
    candidates = [desktop / key / "dist" / _delivery_module().native_artifact_name("jce_editor", target["host"])
                  for key, target in TARGETS.items() if not target.get("emscripten")]
    candidates.append(desktop / "macos-universal" / "jce_editor")
    found = [path for path in candidates if path.is_file()]
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


def _xp_tool(name):
    # Parser schemas belong to this implementation, not a selected project root.
    path = Path(__file__).with_name(name + ".py")
    spec = importlib.util.spec_from_file_location("tools.build._jce_schema_" + name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _run_xp_tool(name, command):
    if DRY_RUN:
        log(name + " DRY RUN: " + " ".join(command))
        return
    result = subprocess.run(command, cwd=ROOT)
    if result.returncode != 0:
        die(name + " failed or remains unverified (exit " + str(result.returncode) + ")")


def cmd_platform_build(args) -> None:
    # JCE_BUILD_CONSISTENCY_XP_V3: Windows Android/Web public .bat entry
    # participates in the same jce.py progress/timing/error contract.
    if HOST != "windows" or args.cmd not in ("android", "web"):
        die("platform batch entry needs Windows and android/web")
    script = ROOT / "tools" / "build" / ("build-" + args.cmd + ".bat")
    if not script.is_file():
        die("platform build script missing: " + str(script))
    forwarded = getattr(args, "platform_args", [])
    log("platform recipe: " + str(script))
    command = [os.environ.get("ComSpec", "cmd.exe"), "/d", "/c", str(script), *forwarded]
    run(command, cwd=ROOT)


def dispatch_editor(args) -> None:
    # JCE_XP_PROGRESS_EMBEDDED_V2: XP stages routed by jce.py, visible in CMD.
    if not args.xp:
        if (args.toolchain_root is not None or args.jobs is not None or args.phase != "build"):
            die("editor: --toolchain-root, --jobs and --phase require --xp")
        cmd_editor(args)
        return
    if args.jobs is not None and not 1 <= args.jobs <= 256:
        die("editor --xp: --jobs must be in 1..256")
    selected_jobs = args.jobs if args.jobs is not None else int(build_jobs())
    if not 1 <= selected_jobs <= 256:
        die("editor --xp: CMAKE_BUILD_PARALLEL_LEVEL must be in 1..256")
    log(f"XP Editor dispatched by jce.py: arch={args.arch or 'x64'} "
        f"variant={args.variant} phase={args.phase} jobs={selected_jobs}; "
        "live Conan/Ninja output and periodic heartbeat enabled")
    if HOST != "windows" or args.arch not in (None, "x64", "x86"):
        die("editor --xp requires a Windows build host and x64 or x86")
    if (args.variant not in ("release", "dist") or args.delivery_profile != "embedded"
            or args.clean or args.artifact_dir is not None):
        die("editor --xp uses its own embedded GUI build; unsupported desktop option")
    toolchain = _xp_tool("xp_compat").resolve_toolchain_root(ROOT, args.toolchain_root)
    if not (toolchain / "bin").is_dir():
        die(f"XP toolchain not found: {toolchain}; set --toolchain-root or JCE_XP_TOOLCHAIN_ROOT")
    shaderc_override = os.environ.get("JCE_SHADERC_EXECUTABLE") or os.environ.get("BGFX_SHADERC")
    shaderc = Path(shaderc_override).resolve() if shaderc_override else None
    names = ("jce_pak", "jce_bin2obj")
    with (nullcontext() if DRY_RUN else _cache_scope(_host_cache_paths())):
        if DRY_RUN:
            if shaderc is None:
                log("XP Editor will resolve native shaderc at build time")
            tools = {name: host_tool(name) for name in names}
        else:
            if shaderc is not None:
                if not shaderc.is_file() or not _shaderc_runs(shaderc):
                    die(f"explicit host shaderc cannot execute: {shaderc}")
            else:
                shaderc = find_host_shaderc()
                if shaderc is None:
                    die("XP Editor needs a native shaderc; set JCE_SHADERC_EXECUTABLE")
            tools = ensure_host_tools(os.environ, names)
        cmd_xp_editor(argparse.Namespace(arch=args.arch or "x64", toolchain_root=toolchain,
                     jobs=selected_jobs, phase=args.phase, variant=args.variant,
                     patented_codecs=args.patented_codecs,
                     shaderc=shaderc, host_pak=tools["jce_pak"],
                     host_bin2obj=tools["jce_bin2obj"]))


def cmd_xp_editor(args) -> None:
    command = [sys.executable, str(ROOT / "tools/build/xp_editor.py"),
               "--arch", args.arch, "--toolchain-root", str(args.toolchain_root),
               "--jobs", str(args.jobs if args.jobs is not None else build_jobs()),
               "--phase", args.phase, "--variant", getattr(args, "variant", "release")]
    selected_codecs = getattr(args, "patented_codecs", None)
    if selected_codecs is not None:
        command.extend(("--patented-codecs", selected_codecs))
    for key in ("conan_home", "base_conan_home", "build_profile", "shaderc", "host_pak", "host_bin2obj", "export_inventory"):
        value = getattr(args, key, None)
        if value is not None:
            command.extend(("--" + key.replace("_", "-"), str(value)))
    if getattr(args, "bgfx_bx_debug", "auto") != "auto":
        command.extend(("--bgfx-bx-debug", args.bgfx_bx_debug))
    _run_xp_tool("XP Editor", command)


def cmd_xp_vm(args) -> None:
    command = [sys.executable, str(ROOT / "tools/build/xp_vm.py")]
    for key in ("host", "port", "user", "host_key_sha256", "artifact", "expected_arch",
                "kind", "out", "output_root", "remote_dir", "password_env", "expect_marker",
                "guest_encoding", "timeout", "connect_timeout", "probe_timeout",
                "transfer_timeout", "max_output_bytes", "owned_launcher", "editor_window", "editor_locale"):
        value = getattr(args, key, None)
        if value is not None:
            command.extend(("--" + key.replace("_", "-"), str(value)))
    for value in args.env:
        command.extend(("--env", value))
    if getattr(args, "export_receipt", None) is not None:
        command.extend(("--export-receipt", str(args.export_receipt)))
    for value in getattr(args, "input_relative", []):
        command.extend(("--input-relative", value))
    for value in getattr(args, "collect_relative", []):
        command.extend(("--collect-relative", value))
    if getattr(args, "editor_run_in_background", False):
        command.append("--editor-run-in-background")
    if getattr(args, "require_terminal_ack", False):
        command.append("--require-terminal-ack")
    _run_xp_tool("XP VM smoke", command)


def cmd_xp_os_probe(args) -> None:
    command = [sys.executable, str(ROOT / "tools/build/xp_os.py"),
               "--repo", str(ROOT), "--arch", args.arch,
               "--toolchain-root", str(args.toolchain_root),
               "--jobs", str(args.jobs if args.jobs is not None else build_jobs())]
    if args.run_local:
        command.append("--run-local")
    _run_xp_tool("XP OS controls", command)


def cmd_xp_guest_exports(args) -> None:
    command = [sys.executable, str(ROOT / "tools/build/xp_guest_exports.py")]
    for key in ("host", "port", "user", "host_key_sha256", "expected_arch", "remote_system32",
                "password_env", "artifact", "out", "output_root", "guest_encoding", "timeout",
                "connect_timeout", "probe_timeout", "transfer_timeout", "max_output_bytes",
                "max_dlls", "max_dll_bytes", "max_total_bytes"):
        value = getattr(args, key)
        if value is not None:
            command.extend(("--" + key.replace("_", "-"), str(value)))
    for value in args.dll:
        command.extend(("--dll", value))
    if args.retain_dlls:
        command.append("--retain-dlls")
    _run_xp_tool("XP guest export evidence", command)


def cmd_xp_probe(args) -> None:
    """A1 probe-only LLVM-MinGW-XP route; never uses the MSVC/Conan SDK cache."""
    command = [sys.executable, str(ROOT / "tools/build/xp_compat.py"),
               "--repo", str(ROOT), "--arch", args.arch,
               "--jobs", str(args.jobs if args.jobs is not None else build_jobs())]
    if args.toolchain_root:
        command.extend(("--toolchain-root", args.toolchain_root))
    if DRY_RUN:
        log("XP compatibility probe DRY RUN: " + " ".join(command))
        return
    result = subprocess.run(command, cwd=ROOT)
    if result.returncode != 0:
        die(f"XP probe failed with code {result.returncode}")


def cmd_ide(args) -> None:
    """Prepare IDE presets through the shared compiler and dependency owners."""
    from tools.build.ide import prepare
    try:
        prepare(argparse.Namespace(**globals()), args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        die("ide: " + str(error))


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

    def add_codec_switch(sp):
        # The release opt-out.  Default None == "don't touch the cache", which
        # is what makes the preference persistent: passing -D on every
        # configure is exactly how this switch got silently reverted before
        # (see codec_overrides()).  dist ignores this — it is forced
        # royalty-free by the root CMakeLists and asserted there.
        sp.add_argument("--patented-codecs", choices=["on", "off"], default=None,
                        dest="patented_codecs",
                        help="compile the AAC / H.264 / H.265 adapters "
                             "(default: keep whatever this build dir was last "
                             "configured with; ON for a fresh dir). Ignored for "
                             "--variant dist, which is always royalty-free.")

    def add_graphics_api_tier(sp):
        sp.add_argument(
            "--graphics-api-tier",
            choices=GRAPHICS_API_TIERS,
            default=os.environ.get("JCE_GRAPHICS_API_TIER", "stable"),
            help="bgfx API floor: stable=GL3.1/GLES3.0/Vulkan1.0, "
                 "modern=GL4.3/GLES3.1/Vulkan1.2, "
                 "current=GL4.6/GLES3.2/Vulkan1.4, "
                 "compatibility=GL2.1/GLES2.0/Vulkan1.0. "
                 "WebGL2 supports stable only.")

    sp = sub.add_parser("sdk", help="build + install the redistributable SDK")
    sp.add_argument("--delivery-profile", choices=("embedded", "authoring"), default="embedded",
                    help="embedded shipment (default) or optional external-VM authoring SDK")
    sp.add_argument("--standalone", action="store_true",
                    help="compatibility alias for embedded static-dependency SDK")
    add_arch(sp)
    add_variant(sp, ["release", "dist", "both"], "dist")
    add_codec_switch(sp)
    add_graphics_api_tier(sp)
    _standalone_module().sdk_outputs().add_arguments(sp)
    sp.add_argument("--no-debug", action="store_true", help="skip the Debug SDK")
    sp.add_argument("--clean", action="store_true")
    sp.add_argument("--smoke", action="store_true",
                    help="after install, build + run the C99 smoke consumer "
                         "against the fresh SDK (tests/sdk_smoke)")
    sp.add_argument("--tolerate-missing-tools", action="store_true",
                    help="ship a DEGRADED SDK without cook/pack host tools "
                         "instead of failing (stamped in VERSION.txt)")
    # The Java backend needs a JDK on the PRODUCER's machine (jni.h to compile
    # the shim, javac for com.jce.script).  It used to be OFF by default "so
    # producing an SDK never requires one" -- a correct goal that an OPTIONAL
    # PROBE reaches without a flag, which is why there has never been a
    # --script-python.  Both backends detect their own toolchain now, so the
    # default is: ship it when this machine can.
    #
    # The flag became the REQUIREMENT, because that is the question it could not
    # previously express: "I am cutting a release and this SDK must have java."
    # Without it, a missing JDK produced a quietly java-less SDK and one STATUS
    # line, and the person who typed the flag found out from a consumer.
    # What actually shipped is stamped in the SDK's VERSION.txt either way.
    sp.add_argument("--script-java", action="store_true",
                    help="REQUIRE the Java scripting backend: fail the build "
                         "if this machine cannot produce it. Without this the "
                         "backend still ships whenever a JDK is present.")
    sp.add_argument("--no-script-java", action="store_true",
                    help="do not probe for or ship the Java scripting backend, "
                         "even if this machine has a JDK")
    sp.add_argument("--profiling", choices=["on", "off"], default=None,
                    help="link Tracy into the SDK's engine (default: keep "
                         "whatever this build dir was last configured with; ON "
                         "for a fresh dir). Tracy opens a listening socket at "
                         "static-init time even when nobody profiles, so pass "
                         "off for an SDK you will ship games from. Ignored for "
                         "--variant dist, which never carries Tracy.")
    sp.set_defaults(func=cmd_sdk)

    sp = sub.add_parser("smoke",
                        help="build + run the C99 SDK smoke consumer against "
                             "an existing SDK (C ABI regression gate)")
    add_arch(sp)
    sp.add_argument("--sdk", help="SDK install dir "
                                  "(default: dist/sdk/<host-arch tag>)")
    sp.add_argument("--variant", choices=["release", "dist"], default="release")
    sp.add_argument("--consumer", choices=("both", "c99", "scripting"), default="both",
                    help="select one installed-SDK consumer gate (default: both)")
    sp.add_argument("--build-dir", type=Path,
                    help="isolated consumer scratch (both creates c99/scripting children)")
    sp.add_argument("--require-run", action="store_true",
                    help="require actual native run, explicit --sdk and target metadata; skip=77")
    sp.add_argument("--delivery-profile", choices=("embedded", "authoring"), default="authoring")
    sp.add_argument("--introspect-out", default="", metavar="PATH",
                    help="write the introspection JSON to PATH instead of "
                         "stdout (stdout also carries the [jce] build log)")
    sp.add_argument("--introspect", choices=["components", "stats"],
                    default="",
                    help="ALSO print the engine's machine-readable "
                         "self-description, produced through the INSTALLED "
                         "SDK (see <jce/api_introspect.h>)")
    sp.set_defaults(func=cmd_smoke)

    sp = sub.add_parser("app", help="build a project via the SDK (consumer)")
    sp.add_argument("--delivery-profile", choices=("embedded", "authoring"), default="embedded",
                    help="embedded shipment (default) or optional external-VM authoring SDK")
    sp.add_argument("project")
    add_arch(sp)
    add_variant(sp, ["release", "debug", "dist"], "release")
    sp.add_argument("--sdk", help="SDK root (else JCE_SDK_DIR / dist/sdk/<host>-<arch>)")
    sp.add_argument("--target", help="CMake target (default: manifest 'target' or dir name)")
    sp.add_argument("--exe", help="expected artifact filename")
    sp.add_argument("--registry-proof", help="actual Web registry receipt; audit the previous artifact without rebuilding")
    sp.add_argument("--configure-only", action="store_true", help="configure, skip build")
    sp.add_argument("--clean", action="store_true")
    sp.set_defaults(func=cmd_app)

    sp = sub.add_parser(
        "build-project",
        help="cook + build + package a project through the editor's headless "
             "pipeline (the same one the editor's Build button drives)")
    sp.add_argument("--delivery-profile", choices=("embedded", "authoring"), default="embedded")
    sp.add_argument("project")
    add_arch(sp)
    add_variant(sp, ["release", "debug", "dist"], "release")
    sp.add_argument("--sdk", help="SDK root (else JCE_SDK_DIR / dist/sdk/<host>-<arch>)")
    sp.add_argument("--target", help="CMake target (default: manifest 'target')")
    sp.add_argument("--exe", help="artifact filename (default: manifest 'exe')")
    sp.add_argument("--out", help="package staging dir "
                                  "(default: <project>/dist/apps/<app>/<platform>-<arch>-<variant>)")
    sp.add_argument("--registry-proof", help="actual Web registry receipt; audit the previous artifact without rebuilding")
    sp.add_argument("--result", help="where the editor publishes its result JSON; "
                                     "must be OUTSIDE --out (the request "
                                     "validator rejects it otherwise)")
    sp.add_argument("--editor", help="editor executable (default: the dist one "
                                     "under build/desktop/<target>/, else release)")
    sp.add_argument("--clean", action="store_true")
    sp.set_defaults(func=cmd_build_project)

    # Public platform shims are delegated to the established BAT recipes.
    for platform_name in ("android", "web"):
        sp = sub.add_parser(platform_name, help="Windows platform build via the unified driver")
        sp.set_defaults(func=cmd_platform_build)

    sp = sub.add_parser("editor", help="build the first-party editor")
    sp.add_argument("--delivery-profile", choices=("embedded", "authoring"), default="embedded",
                    help="embedded shipment (default) or optional external-VM authoring SDK")
    add_arch(sp)
    add_variant(sp, ["release", "debug", "dist", "asan"], "release")
    add_codec_switch(sp)
    add_graphics_api_tier(sp)
    sp.add_argument("--clean", action="store_true")
    sp.add_argument("--standalone", action="store_true", help="compatibility alias for embedded native delivery")
    sp.add_argument("--artifact-dir", type=Path, help="editor runtime output directory; reuse the existing native build cache")
    sp.add_argument("--xp", action="store_true", help="build the Windows XP+ single-EXE GUI editor")
    sp.add_argument("--toolchain-root", type=Path,
                    help="with --xp, override the LLVM-MinGW-XP toolchain directory")
    sp.add_argument("--jobs", type=int, default=None,
                    help="XP editor only: Conan/Ninja parallel jobs, default CMAKE_BUILD_PARALLEL_LEVEL or 8")
    sp.add_argument("--phase", choices=("prepare", "resolve", "dependencies", "configure", "build", "audit"),
                    default="build", help="XP editor only: stage to execute")
    sp.set_defaults(func=dispatch_editor)

    sp = sub.add_parser("ide", help="prepare the canonical native Windows IDE preset (no JCE target build)")
    add_arch(sp)
    add_variant(sp, ["release", "debug", "dist"], "release")
    add_graphics_api_tier(sp)
    sp.add_argument("--prepare-only", action="store_true", help="prepare dependencies/local IDE settings without CMake configure")
    sp.add_argument("--rebuild-dependency", action="append", default=[], metavar="NAME",
                    help="rebuild one named Conan dependency from source with the selected toolchain (repeatable)")
    sp.add_argument("--vscode", action="store_true", help="back up and update selected local VS Code JSONC settings")
    sp.add_argument("--shader-build-dir", type=Path, help="explicit existing shader build for optional Modkit configuration")
    sp.add_argument("--rollback-local", type=Path, help="restore one IDE local backup after exact current/backup hash checks")
    sp.set_defaults(func=cmd_ide)

    sp = sub.add_parser("host-tools", help="build host jce_pak/jce_cook/jce_bin2obj")
    add_arch(sp)
    sp.set_defaults(func=cmd_host_tools)

    sp = sub.add_parser(
        "shader-inspect",
        help="compile a .sc and report its uniforms, attributes and cost")
    sp.add_argument("source", help="path to a .sc shader source")
    sp.add_argument("--type", default="fragment",
                    choices=("vertex", "fragment", "compute"))
    sp.add_argument("--backend", action="append",
                    help="d3d11 | vulkan | opengl | gles | metal "
                         "(repeatable; default d3d11)")
    sp.add_argument("--varying", default=None,
                    help="varying.def.sc (default: the PBR one)")
    sp.add_argument("--optimise", type=int, default=3, choices=(0, 1, 2, 3),
                    help="shaderc -O level (default 3, what ships)")
    sp.add_argument("--json", action="store_true")
    sp.add_argument("--code", action="store_true",
                    help="include the code section (source profiles only)")
    sp.set_defaults(func=cmd_shader_inspect)

    sp = sub.add_parser("cook", help="cook a project's raw assets")
    sp.add_argument("project")
    sp.add_argument("--rgba8", action="store_true",
                    help="keep uncompressed RGBA8 textures for UI/data or exact art reproduction")
    sp.add_argument("--mipmaps", action="store_true",
                    help="generate complete mip chains using import color-space settings")
    sp.set_defaults(func=cmd_cook)

    sp = sub.add_parser("serve", help="serve the web build")
    sp.add_argument("--port", type=int, default=None)
    sp.add_argument("--host", default=None)
    sp.add_argument("--dir", "--directory", dest="directory", default=None)
    sp.add_argument("--entry", default=None)
    sp.add_argument("--no-open", action="store_true")
    sp.set_defaults(func=cmd_serve)

    sp = sub.add_parser("bundle", help="pack scenes through the public bundle API")
    for option in ("project", "scenes-dir", "resource-root", "out", "sdk"):
        sp.add_argument("--" + option)
    sp.add_argument("--scene", action="append")
    sp.add_argument("--target", choices=("windows", "linux", "macos", "android", "ios", "web", "auto"), default="auto")
    sp.add_argument("--cook-assets", action="store_true")
    sp.add_argument("--single-file", action="store_true")
    sp.add_argument("--quiet", action="store_true")
    sp.add_argument("--id")
    sp.set_defaults(func=cmd_bundle)

    sp = sub.add_parser("stock-app", help="seal Lua/JS applications with a prebuilt stock runtime")
    from tools.build.stock_app import add_arguments as stock_arguments
    stock_arguments(sp)
    sp.set_defaults(func=cmd_stock_app)

    sp = sub.add_parser("lint",
                        help="run the lint suite AND the architecture audit")
    sp.add_argument("--lints-only", action="store_true",
                    help="skip tools/audit/run_architecture_audit.py "
                         "(it guards the tracked contracts/abi-snapshot.txt)")
    sp.set_defaults(func=cmd_lint)

    sp = sub.add_parser("test",
                        help="build jce_tests, then run registered tests")
    sp.add_argument("--delivery-profile", choices=("embedded", "authoring"), default="authoring",
                    help="native regression profile (default: full authoring coverage)")
    sp.add_argument("--arch", help="desktop target (default: host)")
    sp.add_argument("--variant", choices=["release", "debug"], default="release")
    sp.add_argument("--configure", action="store_true",
                    help="resolve dependencies and configure tests before building")
    sp.add_argument("--ai-dispatch", choices=("on", "off"),
                    help="explicit optional AI dispatch build selection (requires --configure; otherwise preserve cache)")
    sp.add_argument("--sdk", type=Path,
                    help="explicit installed SDK for formal consumers (requires --configure)")
    sp.add_argument("--sdk-variant", choices=("release", "dist"),
                    help="consumer SDK variant, independent of native tests (default release)")
    sp.add_argument("--sdk-delivery-profile", choices=("authoring", "embedded"),
                    help="consumer SDK delivery profile (default authoring)")
    # default=None, NOT 8: ctest_jobs() cannot tell "the user asked for 8" from
    # "nobody said" if argparse fills in the default, and CTEST_PARALLEL_LEVEL
    # must not override a value somebody typed.
    sp.add_argument("-j", "--jobs", type=int, default=None,
                    help="ctest parallelism (default 8, or "
                         "$CTEST_PARALLEL_LEVEL when set)")
    sp.add_argument("--suite", choices=("all", "engine", "editor", "consumer"), default="all",
                    help="build and run one ownership domain (default: every registered test)")
    sp.add_argument("--filter", help="CTest test-name regular expression")
    sp.add_argument("--target", action="append",
                    help="explicit build target (repeatable; requires --filter; default suite aggregate)")
    sp.add_argument("--repeat-until-fail", type=int,
                    help="repeat each selected test up to N times, stopping on failure")
    sp.add_argument("--rerun-failed", action="store_true",
                    help="only re-run tests that failed last time")
    sp.set_defaults(func=cmd_test)

    sp = sub.add_parser("accept",
                        help="acceptance protocol: does the SHIPPED artifact "
                             "boot, find its scene, render, and stay in budget")
    sp.add_argument("project", help="project directory")
    sp.add_argument("--bundle", help="an already-staged bundle")
    sp.add_argument("--skip-package", action="store_true")
    sp.add_argument("--arch")
    sp.add_argument("--variant", default="release")
    sp.add_argument("--backend")
    sp.add_argument("--frames", type=int)
    sp.add_argument("--keep", action="store_true")
    sp.add_argument("--json")
    sp.add_argument("--budget-frame-ms", type=float)
    sp.add_argument("--budget-draws", type=int)
    sp.add_argument("--budget-rss-mb", type=int)
    sp.set_defaults(func=cmd_accept)

    sp = sub.add_parser(
        "design",
        help="author or validate a scene with a language model "
             "(OpenAI / Claude / Gemini / Ollama / any local model)",
        description="Everything after `design` is passed through to "
                    "private/tools/ai/jce_design.py.  Without --send nothing leaves "
                    "this machine; --check validates an existing scene with "
                    "no key and no network.")
    sp.add_argument("rest", nargs="*",
                    help="--brief ... | --check <scene> [--provider ...] "
                         "[--send]")
    sp.set_defaults(func=cmd_design)

    sp = sub.add_parser(
        "automation",
        help="the machine-readable operation surface (MCP + CLI) an agent "
             "drives JCE through",
        description="Everything after `automation` is passed through to "
                    "private/tools/automation/automation_cli.py.  `automation list` "
                    "prints every tool with its permission tier; "
                    "`automation <tool> --json '{...}'` calls one; "
                    "`automation serve-mcp` serves them over MCP on stdio.  "
                    "Project writes must happen inside a changeset, and "
                    "package/publish/push/network are refused without --allow.")
    sp.add_argument("rest", nargs="*",
                    help="list | contract [--write] | serve-mcp | <tool> "
                         "--json '{...}' [--changeset ID] [--allow CAP]")
    sp.set_defaults(func=cmd_automation)

    sp = sub.add_parser(
        "agent-bridge",
        help="build the engine-side bridge the agent layer calls "
             "(recipe compile / physics probe / introspect), as an SDK consumer")
    add_arch(sp)
    sp.add_argument("--sdk", help="SDK install dir "
                                  "(default: dist/sdk/<host-arch tag>)")
    sp.add_argument("--variant", choices=["release", "dist"], default="release")
    sp.set_defaults(func=cmd_agent_bridge)

    sp = sub.add_parser("xp-probe", help="A1 isolated XP OS/Core probe (NOT full JCE SDK)")
    sp.add_argument("--arch", choices=("x64", "x86"), default="x64")
    sp.add_argument("--toolchain-root", help="verified llvm-mingw-xp MSVCRT distribution root")
    sp.add_argument("--jobs", type=int, help="compiler budget (default: shared build budget)")
    sp.set_defaults(func=cmd_xp_probe)

    sp = sub.add_parser("xp-editor", help="Windows XP GNU/MSVCRT single-EXE Editor")
    _xp_tool("xp_editor").add_arguments(sp)
    sp.set_defaults(func=cmd_xp_editor, jobs=None)

    sp = sub.add_parser("xp-vm", help="Pinned SSH Desktop delivery and process smoke",
                        parents=[_xp_tool("xp_vm").build_parser()], add_help=False)
    sp.set_defaults(func=cmd_xp_vm)

    sp = sub.add_parser("xp-os-probe", help="Isolated XP C99 platform controls",
                        parents=[_xp_tool("xp_os").build_parser()], add_help=False)
    sp.set_defaults(func=cmd_xp_os_probe, jobs=None)

    sp = sub.add_parser("xp-guest-exports", help="Read-only pinned XP DLL export inventory",
                        parents=[_xp_tool("xp_guest_exports").build_parser()], add_help=False)
    sp.set_defaults(func=cmd_xp_guest_exports)

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

    pe = pkg_sub.add_parser("editor", help="stage the native single-executable editor")
    add_arch(pe)
    add_variant(pe, ["release", "dist"], "release")
    pe.add_argument("--standalone", action="store_true", help="compatibility alias for default embedded native delivery")
    pe.add_argument("--delivery-profile", choices=("embedded", "authoring"), default="embedded",
                    help="single executable (default); authoring explicitly stages the development SDK/runtime bundle")
    pe.add_argument("--python-runtime", help="original matching Windows Python embeddable runtime directory (including license)")
    pe.add_argument("--skip-build", action="store_true",
                    help="reuse an exactly matching configured editor artifact")
    pe.add_argument("--out", help="output executable path (authoring: bundle directory)")
    pe.set_defaults(func=cmd_package_editor)

    pg = pkg_sub.add_parser("game", help="compatibility entry to native build-project packaging")
    pg.add_argument("project")
    add_arch(pg)
    # The native headless owner handles authenticated Dist packaging.
    add_variant(pg, ["release", "dist"], "release")
    pg.add_argument("--sdk", help="SDK root (else JCE_SDK_DIR / dist/sdk/...)")
    pg.add_argument("--target", help="CMake target (default: manifest 'target')")
    pg.add_argument("--exe", help="built artifact filename")
    pg.add_argument("--name", help="compatibility check: must match manifest name (edit the manifest to rename)")
    pg.add_argument("--version", help="compatibility check: must match manifest version (edit the manifest to change)")
    pg.add_argument("--out", help="safe final directory (default: <project>/dist/apps/<app>/<platform>-<arch>-<variant>)")
    pg.add_argument("--editor", help="editor executable for the native headless pipeline")
    pg.add_argument("--result", help="private build result JSON, outside the package")
    pg.add_argument("--delivery-profile", choices=("embedded", "authoring"), default="embedded")
    pg.add_argument("--clean", action="store_true")
    pg.add_argument("--with-loose", action="store_true",
                    help="unsupported compatibility option; use the explicit editor authoring workflow")
    pg.set_defaults(func=cmd_package_game)

    ps = pkg_sub.add_parser("sdk", help="build + install the SDK (alias of `sdk`)")
    ps.add_argument("--delivery-profile", choices=("embedded", "authoring"), default="embedded")
    _standalone_module().sdk_outputs().add_arguments(ps)
    add_arch(ps)
    add_variant(ps, ["release", "dist", "both"], "dist")
    ps.add_argument("--no-debug", action="store_true")
    ps.add_argument("--clean", action="store_true")
    ps.set_defaults(func=cmd_sdk)

    return p


def main(argv=None) -> int:
    global DRY_RUN, VERBOSE
    # parse_known_args, because `design` and `automation` forward arbitrary
    # flags to their own tools and this parser has never heard of them.  Every
    # OTHER subcommand still rejects an unknown flag -- the guard below is what
    # keeps this from turning a typo anywhere else into silence.
    parser = build_parser()
    args, unknown = parser.parse_known_args(argv)
    FORWARDING = (cmd_design, cmd_automation, cmd_platform_build)
    if getattr(args, "cmd", None) in ("web", "android"):
        args.platform_args = unknown
    if unknown and getattr(args, "func", None) not in FORWARDING:
        parser.error("unrecognized arguments: %s" % " ".join(unknown))
    DRY_RUN = getattr(args, "dry_run", False)
    VERBOSE = getattr(args, "verbose", False)
    if not hasattr(args, "clean"):
        args.clean = False
    # --dist is a shorthand: force the distribution variant when requested.
    if getattr(args, "dist", False):
        args.variant = "dist"
    if hasattr(args, "graphics_api_tier"):
        os.environ["JCE_GRAPHICS_API_TIER"] = args.graphics_api_tier
    # JCE_BUILD_CONSISTENCY_XP_V3: every jce.py entry has a clear terminal result.
    launch_start = time.monotonic()
    label = getattr(args, "cmd", "command")
    detail = " variant=" + str(args.variant) if hasattr(args, "variant") else ""
    log(f"BUILD BEGIN {label}{detail}")
    result = "FAILED"
    try:
        args.func(args)
        result = "DRY_RUN" if DRY_RUN else "OK"
        return 0
    except SystemExit as error:
        if error.code is None or error.code == 0:
            result = "OK"
        raise
    finally:
        log(f"BUILD END {label}: status={result} elapsed={time.monotonic()-launch_start:.1f}s")


if __name__ == "__main__":
    sys.exit(main())
