#!/usr/bin/env python3
"""
check_dependency_boundaries.py — enforce the machine-readable dependency
ownership matrix (contracts/dependency-ownership.yml).

Two checks, both driven by the same data file:

  1. INCLUDE boundaries — every first-party TU that `#include`s a
     third-party header must live under one of that capability's
     `include_whitelist` directory prefixes.  A dependency include outside
     its owner module means a caller is bypassing the JCE facade.

  2. LINK boundaries — every CMake target that links a third-party
     package must appear in that capability's `link_whitelist`.  Links are
     extracted from CMakeLists.txt files (LINK_PUBLIC/LINK_PRIVATE lists of
     the `_jce_add_layer()` helper plus plain `target_link_libraries`).

  3. FIRST-PARTY DIRECTION — `scripting/` (the language-binding layer) may
     depend on the engine's public headers; nothing under `engine/` may
     depend on anything under `scripting/`.  Unlike 1 and 2 this needs no
     ownership matrix, so it runs even where the matrix is absent.

     It exists because inside `engine/` that rule could only ever be a
     comment.  Today an engine TU including a scripting header fails to
     compile — but only because no engine target carries scripting's include
     directory, which is one `target_include_directories` away from being
     false, and at that moment the layering inverts silently.

Scope: engine/src, engine/include, engine/tools_include, editor/src,
tools/, scripting/ (C/C++ only).  User projects are OUT -- see the note
inside SCAN_ROOTS below.  tests/** is exempt
(white-box tests are sanctioned; the C99 consumer contract is enforced by
the tests/sdk_smoke build itself).  Vendored `third_party/` trees and the
vendored stb_image implementation are skipped.

The audit plan behind this gate:
  .docs/way/JCE_DEPENDENCY_MULTILANGUAGE_CODE_AUDIT_PLAN_STRICT.md  §9/§B1/§B3

Usage:
  python tools/audit/check_dependency_boundaries.py            # human report
  python tools/audit/check_dependency_boundaries.py --json     # machine report
  # exit 0 = clean, 1 = violations, 2 = data-file/parse error.

PyYAML is used when available; otherwise a built-in parser for the strict
YAML subset used by dependency-ownership.yml keeps CI dependency-free.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
OWNERSHIP_FILE = REPO_ROOT / "contracts" / "dependency-ownership.yml"

# Directories scanned for first-party sources (repo-relative, posix).
SCAN_ROOTS = [
    "engine/src",
    "engine/include",
    "engine/tools_include",
    "editor/src",
    # User projects are OUT of this gate's scan surface (owner decision, 2026-08-27).
    # The general engine and editor are the product; a user project is a downstream
    # dogfooding consumer.  Folding the consumer in means a defect inside
    # examples/caged_kingdom/ can turn the ENGINE's architecture gate red -- and the question
    # this gate answers is whether the engine and the editor held their boundaries.
    # Whether consumers deserve a gate of their own is a separate question.
    "tools",
    # The language-binding layer.  Scanned like any other first-party tree, so
    # a backend that reaches a third-party dependency directly instead of
    # through its owning module is a violation there too.
    "scripting",
]

SOURCE_EXTS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".mm", ".m")

# Path fragments that are never first-party responsibility.
GLOBAL_EXCLUDES = (
    "/third_party/",
    "/internal/stb_image",   # vendored single-header implementation
    "/__pycache__/",
)

# ── Dependency include signatures ──────────────────────────────────────
# Maps the `capability` name from dependency-ownership.yml to the include
# patterns that identify a direct use of that dependency.  Patterns match
# against the include path inside `#include <...>` / `#include "..."`.
# SDL is split by subsystem: utility headers (portable-libc doctrine) are
# engine-wide; window/event/input subsystem headers stay in os/platform.
# Window / event / input / render / GPU subsystem headers — these carry the
# platform surface JCE keeps confined to os/platform.  Everything else in SDL
# (the umbrella <SDL3/SDL.h> and the utility headers below) is the sanctioned
# engine-wide portable substrate (SDL-as-portable-libc; see the repo's own
# tools/lint/check_engine_native_io.py doctrine and jce_core PUBLIC sdl).
_SDL_SUBSYSTEM = (
    "SDL_video|SDL_events|SDL_keyboard|SDL_mouse|SDL_gamepad|SDL_joystick|"
    "SDL_touch|SDL_pen|SDL_render|SDL_gpu|SDL_haptic|SDL_sensor|SDL_camera")

INCLUDE_SIGNATURES: dict[str, list[re.Pattern[str]]] = {
    # Order matters: subsystem headers are checked before the umbrella so a
    # window/input/render include is attributed to window-events-input.
    "window-events-input": [re.compile(r"^SDL3/(" + _SDL_SUBSYSTEM + r")\.h$")],
    # Umbrella + every non-subsystem SDL header = engine-wide portable substrate.
    "sdl-portability-substrate": [re.compile(r"^SDL3?/SDL\.h$"),
                                  re.compile(r"^SDL3/SDL_(?!(" + _SDL_SUBSYSTEM.replace("SDL_", "") + r"))\w+\.h$")],
    "image-decode-ldr": [re.compile(r"^SDL3_image/"), re.compile(r"^SDL_image\.h$")],
    "image-decode-hdr": [re.compile(r"stb_image")],
    "gpu-backend": [re.compile(r"^bgfx/"), re.compile(r"^bx/"), re.compile(r"^bimg/")],
    "font-rasterization": [re.compile(r"^ft2build\.h$"), re.compile(r"^freetype/")],
    "text-shaping": [re.compile(r"^hb\.h$"), re.compile(r"^hb-")],
    "editor-ui": [re.compile(r"^imgui"), re.compile(r"^imconfig")],
    "runtime-ui": [re.compile(r"^RmlUi/")],
    "ecs": [re.compile(r"^flecs")],
    "json": [re.compile(r"cJSON\.h$")],
    "gltf-parse": [re.compile(r"^cgltf")],
    "authoring-import": [re.compile(r"^assimp/")],
    "mesh-optimize": [re.compile(r"^meshoptimizer\.h$")],
    "convex-decomposition": [re.compile(r"VHACD", re.IGNORECASE)],
    "skeletal-animation": [re.compile(r"^ozz/")],
    "behavior-tree": [re.compile(r"^behaviortree_cpp/")],
    "navigation": [re.compile(r"^Recast"), re.compile(r"^Detour"), re.compile(r"^recastnavigation/")],
    "physics-3d": [re.compile(r"^btBullet"), re.compile(r"^LinearMath/"),
                re.compile(r"^BulletCollision/"), re.compile(r"^BulletDynamics/"),
                re.compile(r"^BulletSoftBody/")],
    "physics-2d": [re.compile(r"^box2d/")],
    "audio-device-mixer": [re.compile(r"^miniaudio\.h$")],
    "codec-opus": [re.compile(r"^opus/"), re.compile(r"^opus\.h$"), re.compile(r"^opus_multistream\.h$")],
    "container-ogg": [re.compile(r"^ogg/")],
    "codec-av1": [re.compile(r"^dav1d/")],
    "codec-vpx": [re.compile(r"^vpx/")],
    "container-webm": [re.compile(r"^mkvparser"), re.compile(r"^mkvmuxer"), re.compile(r"^webm/")],
    "scripting-lua": [re.compile(r"^lua\.h$"), re.compile(r"^lauxlib\.h$"), re.compile(r"^lualib\.h$"), re.compile(r"^lua\.hpp$")],
    "virtual-filesystem": [re.compile(r"^physfs\.h$")],
    "compression": [re.compile(r"^zstd\.h$"), re.compile(r"^zdict\.h$")],
    "fast-hash": [re.compile(r"^xxhash\.h$"), re.compile(r"^xxh3\.h$")],
    "allocator": [re.compile(r"^mimalloc")],
    "job-system": [re.compile(r"TaskScheduler"), re.compile(r"^enkiTS/")],
    "http": [re.compile(r"^curl/")],
    "udp-transport": [re.compile(r"^enet/")],
    "schema-messages": [re.compile(r"^google/protobuf/"), re.compile(r"\.pb\.h$")],
    "profiler": [re.compile(r"^tracy/"), re.compile(r"^Tracy", )],
}

# CMake package -> owner token (for link-boundary checking).
CMAKE_PACKAGE_OWNERS: dict[str, str] = {
    "sdl::sdl": "sdl",
    "SDL3_image::SDL3_image": "sdl_image",
    "bgfx::bgfx": "bgfx",
    "bgfx::bimg": "bgfx",
    "bgfx::bimg_encode": "bgfx",
    "bgfx::bx": "bgfx",
    "Freetype::Freetype": "freetype",
    "harfbuzz::harfbuzz": "harfbuzz",
    "imgui::imgui": "imgui",
    "RmlUi::RmlUi": "rmlui",
    "flecs::flecs_static": "flecs",
    "cjson::cjson": "cjson",
    "cgltf::cgltf": "cgltf",
    "assimp::assimp": "assimp",
    "meshoptimizer::meshoptimizer": "meshoptimizer",
    "v-hacd::v-hacd": "v-hacd",
    "ozz-animation::ozz-animation": "ozz-animation",
    "BT::behaviortree_cpp": "behaviortree.cpp",
    "recastnavigation::recastnavigation": "recastnavigation",
    "Bullet::Bullet": "bullet3",
    "box2d::box2d": "box2d",
    "miniaudio::miniaudio": "miniaudio",
    "Opus::opus": "opus",
    "Ogg::ogg": "ogg",
    "dav1d::dav1d": "dav1d",
    "libvpx::libvpx": "libvpx",
    "webm::webm": "libwebm",
    "lua::lua": "lua",
    "physfs-static": "physfs",
    "zstd::libzstd_static": "zstd",
    "xxHash::xxhash": "xxhash",
    "mimalloc-static": "mimalloc",
    "enkits::enkits": "enkits",
    "CURL::libcurl": "libcurl",
    "enet::enet": "enet",
    "protobuf::protobuf": "protobuf",
    "Tracy::TracyClient": "tracy",
}

CMAKE_FILES = [
    "CMakeLists.txt",
    "engine/CMakeLists.txt",
    "editor/CMakeLists.txt",
    "engine/cmake/JCESDKInstall.cmake",
]

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')


# ── Ownership file loading ─────────────────────────────────────────────

def _mini_yaml_load(text: str) -> dict:
    """Parse the strict YAML subset used by dependency-ownership.yml.

    Supports: top-level `key: value`, a top-level `capabilities:` list of
    block mappings, scalar values, inline `[a, b]` lists, and comments.
    """
    root: dict = {}
    caps: list[dict] = []
    cur: dict | None = None
    in_caps = False
    pending_list_key: str | None = None

    def parse_scalar(v: str):
        v = v.strip()
        if v.startswith("[") and v.endswith("]"):
            inner = v[1:-1].strip()
            if not inner:
                return []
            return [parse_scalar(x) for x in inner.split(",")]
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
            return v[1:-1]
        return v

    for raw in text.splitlines():
        line = raw.split(" #", 1)[0].rstrip() if not raw.lstrip().startswith("#") else ""
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip())
        stripped = line.strip()
        if indent == 0:
            in_caps = False
            pending_list_key = None
            if stripped == "capabilities:":
                in_caps = True
                continue
            if ":" in stripped:
                k, _, v = stripped.partition(":")
                root[k.strip()] = parse_scalar(v)
            continue
        if not in_caps:
            continue
        if stripped.startswith("- "):
            cur = {}
            caps.append(cur)
            pending_list_key = None
            stripped = stripped[2:]
        if cur is None:
            continue
        if stripped.startswith("- ") or (pending_list_key and not (":" in stripped)):
            cur.setdefault(pending_list_key or "", []).append(parse_scalar(stripped.lstrip("- ")))
            continue
        if ":" in stripped:
            k, _, v = stripped.partition(":")
            k = k.strip()
            v = v.strip()
            if v == "":
                pending_list_key = k
                cur[k] = []
            else:
                pending_list_key = None
                cur[k] = parse_scalar(v)
    root["capabilities"] = caps
    return root


def load_ownership() -> dict:
    text = OWNERSHIP_FILE.read_text(encoding="utf-8")
    try:
        import yaml  # type: ignore
        return yaml.safe_load(text)
    except ImportError:
        return _mini_yaml_load(text)


# ── Checks ─────────────────────────────────────────────────────────────

def owner_token(owner_field: str) -> str:
    """`owner: bullet3` / `owner: stb_image (vendored 2.30)` -> first token."""
    return str(owner_field).split()[0].strip()


def build_rules(data: dict):
    """capability -> {owner, include_whitelist, link_whitelist}."""
    rules: dict[str, dict] = {}
    for cap in data.get("capabilities", []):
        name = str(cap.get("capability", "")).strip()
        if not name:
            continue
        rules[name] = {
            "owner": owner_token(cap.get("owner", "")),
            "include_whitelist": [str(p) for p in cap.get("include_whitelist", [])],
            "link_whitelist": [str(x) for x in cap.get("link_whitelist", [])],
        }
    return rules


def check_includes(rules: dict) -> list[dict]:
    violations: list[dict] = []
    sigs = [(name, pats) for name, pats in INCLUDE_SIGNATURES.items()]
    for root in SCAN_ROOTS:
        base = REPO_ROOT / root
        if not base.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            posix_dir = Path(dirpath).relative_to(REPO_ROOT).as_posix() + "/"
            if any(x in posix_dir for x in GLOBAL_EXCLUDES):
                dirnames[:] = []
                continue
            for fn in filenames:
                if not fn.endswith(SOURCE_EXTS):
                    continue
                rel = (Path(dirpath) / fn).relative_to(REPO_ROOT).as_posix()
                if any(x in "/" + rel for x in GLOBAL_EXCLUDES):
                    continue
                try:
                    lines = (Path(dirpath) / fn).read_text(
                        encoding="utf-8", errors="replace").splitlines()
                except OSError:
                    continue
                for lineno, line in enumerate(lines, 1):
                    m = INCLUDE_RE.match(line)
                    if not m:
                        continue
                    inc = m.group(1)
                    for cap_name, pats in sigs:
                        if not any(p.search(inc) for p in pats):
                            continue
                        rule = rules.get(cap_name)
                        wl = rule["include_whitelist"] if rule else []
                        if not any(rel.startswith(w.rstrip("/") + "/") or rel == w.rstrip("/")
                                   for w in wl):
                            violations.append({
                                "kind": "include",
                                "dep": rule["owner"] if rule else cap_name,
                                "capability": cap_name,
                                "file": rel,
                                "line": lineno,
                                "detail": line.strip(),
                            })
                        break
    return violations


_LAYER_RE = re.compile(
    r"_jce_add_layer\(\s*(\w+)(.*?)\)\s*$", re.DOTALL | re.MULTILINE)
_TLL_RE = re.compile(
    r"target_link_libraries\(\s*(\w+)\s+(PUBLIC|PRIVATE|INTERFACE)?\s*([^)]*)\)",
    re.DOTALL)


def check_links(rules: dict) -> list[dict]:
    violations: list[dict] = []
    target_links: dict[str, set[str]] = {}

    for cmk in CMAKE_FILES:
        p = REPO_ROOT / cmk
        if not p.is_file():
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        # _jce_add_layer(<name> ... LINK_PUBLIC a b LINK_PRIVATE c d ...)
        for m in re.finditer(r"_jce_add_layer\(\s*(\w+)", text):
            name = m.group(1)
            # capture until the matching close paren (balance 1 level deep)
            depth, i = 1, m.end()
            while i < len(text) and depth:
                if text[i] == "(":
                    depth += 1
                elif text[i] == ")":
                    depth -= 1
                i += 1
            body = text[m.end():i - 1]
            for pkg in CMAKE_PACKAGE_OWNERS:
                if re.search(re.escape(pkg) + r"(\s|$)", body):
                    target_links.setdefault(name, set()).add(pkg)
        for m in _TLL_RE.finditer(text):
            tgt, _vis, libs = m.group(1), m.group(2), m.group(3)
            for pkg in CMAKE_PACKAGE_OWNERS:
                if re.search(re.escape(pkg) + r"(\s|$)", libs):
                    target_links.setdefault(tgt, set()).add(pkg)

    # owner token -> union of link whitelists over all capabilities it owns
    owner_wl: dict[str, set[str]] = {}
    owner_caps: dict[str, list[str]] = {}
    for cap_name, rule in rules.items():
        owner_wl.setdefault(rule["owner"], set()).update(rule["link_whitelist"])
        owner_caps.setdefault(rule["owner"], []).append(cap_name)

    for tgt, pkgs in sorted(target_links.items()):
        for pkg in sorted(pkgs):
            tok = CMAKE_PACKAGE_OWNERS[pkg]
            wl = owner_wl.get(tok, set())
            if tgt not in wl:
                violations.append({
                    "kind": "link",
                    "dep": tok,
                    "capability": ",".join(owner_caps.get(tok, ["?"])),
                    "file": "CMake target " + tgt,
                    "line": 0,
                    "detail": f"{tgt} links {pkg} but is not in link_whitelist({sorted(wl)})",
                })
    return violations


# ── Check 3: first-party direction ─────────────────────────────────────
#
# scripting/ -> engine/ is allowed.  engine/ -> scripting/ is not, in either
# spelling: an #include of a scripting-owned header, or an engine CMake file
# naming a scripting path or a target scripting declares.
#
# Both halves are READ FROM THE TREE rather than listed here, so
# scripting/python, scripting/java and scripting/cpp inherit the rule the
# moment they exist — nobody has to remember to extend a list, which is the
# failure mode this repository keeps rediscovering.

DIRECTION_PROVIDER = "scripting"
DIRECTION_CONSUMER = "engine"
_HEADER_EXTS = (".h", ".hh", ".hpp", ".hxx", ".inl")
_CMAKE_TARGET_RE = re.compile(r"add_(?:library|executable)\(\s*([A-Za-z0-9_]+)")


def _provider_include_spellings() -> set[str]:
    """Every way a scripting-owned header can appear in an #include."""
    out: set[str] = set()
    base = REPO_ROOT / DIRECTION_PROVIDER
    if not base.is_dir():
        return out
    for p in base.rglob("*"):
        if p.suffix not in _HEADER_EXTS:
            continue
        rel = p.relative_to(REPO_ROOT).as_posix()
        out.add(rel)                                  # "scripting/.../x.h"
        parts = rel.split("/")
        if "include" in parts:                        # <jce/script_api/x.h>
            i = len(parts) - 1 - parts[::-1].index("include")
            out.add("/".join(parts[i + 1:]))
    return out


def _provider_cmake_targets() -> set[str]:
    out: set[str] = set()
    base = REPO_ROOT / DIRECTION_PROVIDER
    if not base.is_dir():
        return out
    for p in base.rglob("CMakeLists.txt"):
        text = p.read_text(encoding="utf-8", errors="replace")
        out.update(_CMAKE_TARGET_RE.findall(text))
    return out


def check_direction() -> list[dict]:
    violations: list[dict] = []
    spellings = _provider_include_spellings()
    targets = _provider_cmake_targets()
    base = REPO_ROOT / DIRECTION_CONSUMER
    if not base.is_dir() or (not spellings and not targets):
        return violations

    for dirpath, dirnames, filenames in os.walk(base):
        posix_dir = Path(dirpath).relative_to(REPO_ROOT).as_posix() + "/"
        if any(x in posix_dir for x in GLOBAL_EXCLUDES):
            dirnames[:] = []
            continue
        for fn in filenames:
            path = Path(dirpath) / fn
            rel = path.relative_to(REPO_ROOT).as_posix()
            is_source = fn.endswith(SOURCE_EXTS)
            is_cmake = fn == "CMakeLists.txt" or fn.endswith(".cmake")
            if not (is_source or is_cmake):
                continue
            try:
                lines = path.read_text(encoding="utf-8",
                                       errors="replace").splitlines()
            except OSError:
                continue
            for lineno, line in enumerate(lines, 1):
                hit = None
                if is_source:
                    m = INCLUDE_RE.match(line)
                    if m and m.group(1) in spellings:
                        hit = f"includes a {DIRECTION_PROVIDER}/ header"
                elif is_cmake:
                    if DIRECTION_PROVIDER + "/" in line:
                        hit = f"names a {DIRECTION_PROVIDER}/ path"
                    else:
                        for t in targets:
                            if re.search(r"\b" + re.escape(t) + r"\b", line):
                                hit = f"names the {DIRECTION_PROVIDER}/ target {t}"
                                break
                if hit:
                    violations.append({
                        "kind": "direction",
                        "dep": DIRECTION_PROVIDER,
                        "capability": f"{DIRECTION_CONSUMER} must not depend on "
                                      f"{DIRECTION_PROVIDER}",
                        "file": rel,
                        "line": lineno,
                        "detail": f"{hit}: {line.strip()}",
                    })
    return violations


def _report(violations: list[dict]) -> None:
    for v in violations:
        loc = f"{v['file']}:{v['line']}" if v["line"] else v["file"]
        print(f"  [{v['kind']}] {loc}: {v['dep']} (capability: {v['capability']})")
        print(f"      {v['detail']}")
    print()
    print(f"{len(violations)} violation(s).")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    # Runs before the ownership-matrix gate below, because it needs no matrix:
    # a fresh clone with no docs/ still gets the layering direction enforced.
    direction = check_direction()

    if not OWNERSHIP_FILE.is_file():
        # SKIP, not fail.  The matrix lives under docs/, which .gitignore
        # excludes, so a fresh clone has none and this gate is simply not
        # armed there.  Exiting 2 made the whole suite red on every clean
        # checkout, and a permanently red gate stops being read.  Same rule as
        # check_dependency_licenses.py skipping when Conan is unavailable.
        #
        # Naming the cost: with no matrix, nothing checks that a capability is
        # reached only through its owning library.  This is correct only while
        # docs/ stays untracked — track the file and the gate arms itself
        # again with no change here.
        #
        # Check 3 is NOT skipped here: it reads the tree, not the matrix, and
        # a gate that reports "skipped" while holding violations it already
        # found would be the worst of both.
        msg = (f"dependency-boundaries: ownership matrix SKIPPED — none at "
               f"{OWNERSHIP_FILE.relative_to(REPO_ROOT).as_posix()} "
               f"(working-tree-only file; docs/ is gitignored); "
               f"first-party direction still checked")
        if args.json:
            print(json.dumps({"skipped": "ownership-matrix", "reason": msg,
                              "violations": direction}, indent=1))
            return 1 if direction else 0
        print(msg)
        if direction:
            _report(direction)
            return 1
        return 0
    try:
        data = load_ownership()
        rules = build_rules(data)
    except Exception as e:  # noqa: BLE001 — surface any parse failure as exit 2
        print(f"error: cannot parse {OWNERSHIP_FILE.name}: {e}", file=sys.stderr)
        return 2

    violations = direction + check_includes(rules) + check_links(rules)

    if args.json:
        print(json.dumps(violations, indent=1))
        return 1 if violations else 0

    if not violations:
        print("dependency-boundaries check: OK "
              f"({len(rules)} capabilities, whitelists respected; "
              f"{DIRECTION_CONSUMER}/ -> {DIRECTION_PROVIDER}/ direction clean)")
        return 0
    print("dependency-boundaries check: FAILED")
    print(f"(ownership matrix: {OWNERSHIP_FILE.relative_to(REPO_ROOT).as_posix()})")
    print()
    _report(violations)
    return 1


if __name__ == "__main__":
    sys.exit(main())
