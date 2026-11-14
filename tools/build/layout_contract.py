#!/usr/bin/env python3
"""JCE desktop preset build-path authority and read-only consistency gate.

Only the CMakePresets/driver lane is normalized here. Native embedded editor
and XP variant caches remain owned by standalone.py and xp_editor.py. This
module never renames, removes, or copies an existing CMake/Conan cache.
"""
from __future__ import annotations

import argparse
import copy
import importlib.util
import json
from pathlib import Path
import re
import sys
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
NATIVE_STEMS = (
    "windows-x64", "windows-arm64", "windows-x86", "linux-x64",
    "linux-arm64", "macos-x64", "macos-arm64",
)
NATIVE_CONAN_TARGETS = NATIVE_STEMS + ("linux-x86",)  # experimental i686 lane has no CMake desktop preset yet
PRESET_KINDS = ("release", "dist", "debug", "release-sdk", "dist-sdk")
OBJECT_KINDS = PRESET_KINDS + ("asan",)
VS_VENDOR = "microsoft.com/VisualStudioSettings/CMake/1.0"
WINDOWS_PRESET_TOOLSET = "v143,version=14.4"


def _target(target: str) -> str:
    if target not in NATIVE_STEMS:
        raise ValueError("unsupported CMake preset target: " + str(target))
    return target


def preset_cmake_dir(root: Path, target: str, kind: str) -> Path:
    """Independent CMake tree for each *preset* target/mode, never an Editor tree."""
    _target(target)
    if kind not in OBJECT_KINDS or (kind == "asan" and target != "windows-x64"):
        raise ValueError("unsupported preset kind: " + str(kind))
    return Path(root) / "build" / "desktop" / target / "obj" / "preset" / kind


def preset_conan_dir(root: Path, target: str) -> Path:
    """Conan target/profile owner. Debug/Release generators are separate inside."""
    if target not in NATIVE_CONAN_TARGETS:
        raise ValueError("unsupported native Conan target: " + str(target))
    return Path(root) / "build" / "desktop" / target / "conan" / "preset"


def preset_cache_variables(root: Path, target: str, kind: str) -> dict[str, str]:
    config = "Debug" if kind == "debug" else "Release"
    return {
        "binaryDir": str(preset_cmake_dir(root, target, kind)),
        "CMAKE_TOOLCHAIN_FILE": str(
            preset_conan_dir(root, target) / "build" / config
            / "generators" / "conan_toolchain.cmake"
        ),
    }


def _resolve(presets: dict[str, dict], name: str, stack=()) -> dict:
    """Resolve the configure fields used by this gate, without macro expansion.

    CMake prefers the first parent and does not inherit presentation/hidden
    fields. Cache/environment entries retain explicit null overrides. Microsoft
    vendor maps inherit per vendor URI, as in CMake Tools' vendor-map resolver;
    CMake itself does not interpret vendor payloads.
    """
    if name not in presets:
        raise ValueError("unknown inherited preset " + name)
    if name in stack:
        raise ValueError("preset inheritance loop: " + " -> ".join((*stack, name)))
    p = presets[name]
    result = {"cacheVariables": {}}
    noninherited = {"name", "hidden", "inherits", "description", "displayName"}
    maps = {"cacheVariables", "environment", "vendor", "warnings", "errors"}

    def overlay(values, inherited=False):
        for key, value in values.items():
            if key == "inherits" or (inherited and key in noninherited):
                continue
            if key in maps:
                result.setdefault(key, {}).update(copy.deepcopy(value))
            elif key in ("architecture", "toolset"):
                # CMake inherits value and strategy independently.
                item = {"value": value} if isinstance(value, str) else value
                result.setdefault(key, {}).update({k: copy.deepcopy(v) for k, v in item.items()
                                                    if k != "value" or v != ""})
            elif value == "" and key in ("generator", "binaryDir", "installDir", "toolchainFile"):
                continue
            else:
                result[key] = copy.deepcopy(value)

    parents = p.get("inherits", ())
    if isinstance(parents, str):
        parents = (parents,)
    for parent in reversed(parents):
        base = _resolve(presets, parent, (*stack, name))
        overlay(base, inherited=True)
    overlay(p)
    # An explicit null condition clears inheritance on this preset, but is not
    # then inherited as a blocking null condition by its descendants.
    if result.get("condition", False) is None:
        result.pop("condition")
    return result


def _host_metadata(name: str):
    for prefix, host, vendor_host in (
        ("windows-", "Windows", "Windows"), ("linux-", "Linux", "Linux"),
        ("macos-", "Darwin", "macOS"), ("ios-", "Darwin", "macOS"),
    ):
        if name.startswith(prefix):
            return ({"type": "equals", "lhs": "${hostSystemName}", "rhs": host}, [vendor_host])
    if name in ("host-release", "jni-release", "android-arm64-release", "wasm-release", "wasm-dist"):
        return ({"type": "inList", "string": "${hostSystemName}",
                 "list": ["Windows", "Linux", "Darwin"]}, ["Windows", "Linux", "macOS"])
    return None


def _as_cmake_path(root: Path, path: Path) -> str:
    return "${sourceDir}/" + path.relative_to(root).as_posix()


def check_presets(repo: Path, data: dict) -> list[str]:
    """Verify every cross-host preset against the runtime path authority."""
    errors = []
    minimum = data.get("cmakeMinimumRequired", {})
    if data.get("version") == 6 and tuple(minimum.get(k, 0) for k in ("major", "minor", "patch")) < (3, 25, 0):
        errors.append("preset schema 6 requires CMake 3.25 or later")
    names = [x.get("name") for x in data.get("configurePresets", [])]
    if len(names) != len(set(names)):
        errors.append("CMakePresets.json contains duplicate configurePreset names")
    presets = {x["name"]: x for x in data.get("configurePresets", [])}
    paths = {}
    labels = {}
    for name, entry in presets.items():
        try:
            resolved = _resolve(presets, name)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        if resolved.get("hidden"):
            continue
        label = resolved.get("displayName")
        if not isinstance(label, str) or not label:
            errors.append(f"{name}: public configurePreset requires displayName")
        elif label in labels:
            errors.append(f"{name}: duplicate configurePreset displayName with {labels[label]}")
        else:
            labels[label] = name
        binary_dir = resolved.get("binaryDir")
        if isinstance(binary_dir, str) and binary_dir:
            if binary_dir in paths:
                errors.append(f"{name}: shared binaryDir with {paths[binary_dir]}")
            else:
                paths[binary_dir] = name
        host = _host_metadata(name)
        if host is not None:
            condition, vendor_hosts = host
            if resolved.get("condition") != condition:
                errors.append(f"{name}: host condition differs from the target family")
            if resolved.get("vendor", {}).get(VS_VENDOR, {}).get("hostOS") != vendor_hosts:
                errors.append(f"{name}: IDE hostOS differs from the target family")
    for target in NATIVE_STEMS:
        kinds = (*PRESET_KINDS, "asan") if target == "windows-x64" else PRESET_KINDS
        for kind in kinds:
            name = target + "-" + kind
            try:
                resolved = _resolve(presets, name)
            except ValueError as e:
                errors.append(str(e))
                continue
            expected_dir = _as_cmake_path(repo, preset_cmake_dir(repo, target, kind))
            got_dir = resolved.get("binaryDir")
            if got_dir != expected_dir:
                errors.append(f"{name}: binaryDir {got_dir!r} != {expected_dir!r}")
            if resolved.get("hidden"):
                errors.append(f"{name}: canonical preset cannot be hidden")
            if resolved.get("generator") != "Ninja":
                errors.append(f"{name}: canonical preset generator must remain Ninja")
            variables = resolved.get("cacheVariables", {})
            sdk = kind.endswith("-sdk")
            if variables.get("JCE_DELIVERY_PROFILE") != ("embedded" if sdk else "authoring"):
                errors.append(f"{name}: delivery profile differs from the preset purpose")
            if variables.get("JCE_BUILD_EDITOR") != ("OFF" if sdk else "ON"):
                errors.append(f"{name}: Editor selection differs from the preset purpose")
            if target.startswith("windows-"):
                if resolved.get("architecture") != {"value": target.removeprefix("windows-"), "strategy": "external"}:
                    errors.append(f"{name}: Windows Ninja architecture must be explicit and external")
                if resolved.get("toolset") != {"value": WINDOWS_PRESET_TOOLSET, "strategy": "external"}:
                    errors.append(f"{name}: Windows Ninja toolset must select the shared MSVC family")
            toolchain = _as_cmake_path(repo,
                Path(preset_cache_variables(repo, target, kind)["CMAKE_TOOLCHAIN_FILE"]))
            got_tc = resolved.get("cacheVariables", {}).get("CMAKE_TOOLCHAIN_FILE")
            if got_tc != toolchain:
                errors.append(f"{name}: CMAKE_TOOLCHAIN_FILE {got_tc!r} != {toolchain!r}")
            expected_config = "Debug" if kind == "debug" else "Release"
            got_config = resolved.get("cacheVariables", {}).get("CMAKE_BUILD_TYPE")
            if got_config != expected_config:
                errors.append(f"{name}: CMAKE_BUILD_TYPE {got_config!r} != {expected_config!r}")
            expected_variant = "dist" if kind in ("dist", "dist-sdk") else (
                "debug" if kind == "debug" else "asan" if kind == "asan" else "release")
            got_variant = resolved.get("cacheVariables", {}).get("JCE_BUILD_VARIANT")
            if got_variant != expected_variant:
                errors.append(f"{name}: JCE_BUILD_VARIANT {got_variant!r} != {expected_variant!r}")
    # Dist presets inherited Release's binaryDir before v10; no sharing allowed.
    build_names = [x.get("name") for x in data.get("buildPresets", [])]
    if len(build_names) != len(set(build_names)):
        errors.append("CMakePresets.json contains duplicate buildPreset names")
    build_labels = {}
    for x in data.get("buildPresets", []):
        if x.get("configurePreset") not in presets:
            errors.append(f"buildPreset {x.get('name')}: nonexistent configurePreset")
            continue
        if not x.get("hidden"):
            host = _host_metadata(x["configurePreset"])
            if host is not None and x.get("condition") != host[0]:
                errors.append(f"buildPreset {x.get('name')}: host condition differs from its configure preset")
            label = x.get("displayName")
            expected_label = "Build " + presets[x["configurePreset"]].get("displayName", "")
            if label != expected_label:
                errors.append(f"buildPreset {x.get('name')}: displayName must identify the associated configure preset")
            if label in build_labels:
                errors.append(f"buildPreset {x.get('name')}: duplicate displayName with {build_labels[label]}")
            else:
                build_labels[label] = x.get("name")
    return errors


def _load(path: Path, alias: str):
    spec = importlib.util.spec_from_file_location(alias, path)
    if spec is None or spec.loader is None:
        raise ValueError("cannot import build owner: " + str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_driver(repo: Path) -> list[str]:
    """Check true Python return values, not merely source comments/strings."""
    errors = []
    repo_path = str(Path(repo).resolve())
    if repo_path not in sys.path:
        sys.path.insert(0, repo_path)
    driver = _load(repo / "tools/build/jce.py", "jce_build_layout_driver")
    standalone = _load(repo / "tools/build/standalone.py", "jce_build_layout_standalone")
    if WINDOWS_PRESET_TOOLSET != "v143,version=" + driver._VC_TOOLSET_PREFIX:
        errors.append("Windows preset toolset differs from the shared MSVC environment owner")
    for target in NATIVE_STEMS:
        t = dict(driver.TARGETS[target], key=target)
        for kind in PRESET_KINDS:
            observed = driver.preset_binary_dir(t, kind)
            expected = preset_cmake_dir(repo, target, kind)
            if observed != expected:
                errors.append(f"driver {target}-{kind}: {observed} != {expected}")
        if driver.conan_dir(t) != preset_conan_dir(repo, target):
            errors.append(f"driver {target}: Conan preset cache differs")
        # The embedded Editor is a separate cache family with isolated variants.
        for variant in ("release", "dist"):
            plan = standalone.native_cache_layout(SimpleNamespace(ROOT=repo),
                SimpleNamespace(variant=variant), t)
            root = repo / "build" / "desktop" / target
            if plan["build"] != root / "obj" / variant:
                errors.append(f"Editor {target}-{variant}: incorrect object owner")
            if plan["conan"] != root / "conan" / variant:
                errors.append(f"Editor {target}-{variant}: incorrect Conan owner")
            if plan["build"] == driver.preset_binary_dir(t, "release"):
                errors.append(f"Editor {target}-{variant}: shares a preset object tree")
    return errors


def check_sources(repo: Path) -> list[str]:
    """Protect special-case producers and the editor's old/new profile parsing."""
    errors = []
    sources = {
        "xp": repo / "tools/build/xp_editor.py",
        "panel": repo / "editor/src/panels/jce_panel_build_profiles.cpp",
        "wrapper": repo / "scripts/jce.py",
    }
    raw = {key: path.read_text(encoding="utf-8") for key, path in sources.items()}
    for part in ('desktop_target / "obj" / variant',
                 'desktop_target / "conan" / variant'):
        if part not in raw["xp"]:
            errors.append("XP layout must retain its own variant-isolated owner: " + part)
    for fragment in ('folder == "preset"', '"/conan/preset"', 'const std::string suf = "-conan";'):
        if fragment not in raw["panel"]:
            errors.append("Build Profiles panel cannot parse both modern/legacy Conan roots")
    if 'tools/build/jce.py' not in raw["wrapper"]:
        errors.append("scripts/jce.py is not the canonical thin entry")
    return errors


def audit(repo: Path, *, static_only: bool = False) -> list[str]:
    repo = Path(repo).resolve()
    data = json.loads((repo / "CMakePresets.json").read_text(encoding="utf-8"))
    errors = check_presets(repo, data)
    if not static_only:
        errors += check_driver(repo)
        errors += check_sources(repo)
    return errors


def inventory(repo: Path) -> list[dict]:
    """Read-only, nonrecursive desktop root inventory. Legacy is not an error."""
    root = Path(repo).resolve() / "build" / "desktop"
    results = []
    if not root.is_dir():
        return results
    legacy = re.compile(r"^(?:windows-(?:x64|arm64|x86)|linux-(?:x64|arm64)|macos-(?:x64|arm64))-(?:debug|sdk|dist-sdk|asan|conan)$")
    for path in sorted(root.iterdir(), key=lambda p: p.name.lower()):
        if path.is_symlink():
            status = "ALIAS_NOT_FOLLOWED"
        elif path.name in NATIVE_STEMS or path.name in ("windows-x64-xp", "windows-x86-xp"):
            status = "TARGET_ROOT_WITH_LEGACY_CACHE" if (path / "CMakeCache.txt").exists() else "TARGET_ROOT"
        elif legacy.fullmatch(path.name):
            status = "LEGACY_FLAT_CACHE_KEEP_IN_PLACE"
        else:
            status = "OTHER_OWNER_REVIEW"
        results.append({"entry": str(path), "status": status})
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description="JCE desktop build layout contract (read-only)")
    parser.add_argument("--check", action="store_true", help="validate CMakePresets + driver + editor/XP owners")
    parser.add_argument("--inventory", action="store_true", help="also classify existing build/desktop entries, read-only")
    parser.add_argument("--root", type=Path, default=ROOT, help="source root (default: this checkout)")
    args = parser.parse_args()
    try:
        issues = audit(args.root)
        for issue in issues:
            print("LAYOUT FAIL:", issue)
        if args.inventory:
            for item in inventory(args.root):
                print(item["status"] + ": " + item["entry"])
        if issues:
            print("LAYOUT FAIL: preset/driver contract diverged. No directories changed.")
            return 1
        print("LAYOUT PASS: CMakePresets, jce driver, embedded Editor and XP owners agree.")
        print("Historical CMake/Conan build trees are not modified or moved.")
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print("LAYOUT FAIL:", exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
