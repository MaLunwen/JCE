#!/usr/bin/env python3
"""Read-only, cross-platform JCE build directory authority (v11).

Static source/route and path-owner checks only: not a native-build pass.  No
cache movement, creation, cleanup, package downloads or native compilation.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import re
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
TC = "/build/Release/generators/conan_toolchain.cmake"
PLATFORM_PRESETS = {
    "android-arm64-release": ("mobile/android-arm64/obj/preset/release", "mobile/android-arm64/conan/preset"),
    "ios-arm64-release": ("mobile/ios-arm64/obj/preset/release", "mobile/ios-arm64/conan/preset"),
    "wasm-release": ("web/wasm/obj/preset/release", "web/wasm/conan/preset"),
    "wasm-dist": ("web/wasm/obj/preset/dist", "web/wasm/conan/preset"),
    "jni-release": ("jni/desktop/obj/preset/release", "jni/desktop/conan/preset"),
}
REQUIRED_ROOTS = {
    ".locks", "host", "host-conan", "desktop", "mobile", "web", "jni", "consumers",
    "android", "sdk", "compat", "conan-config", "shader-cache", "tests",
    "validation", "reports", "automation",
}
DESKTOP_TARGETS = {
    "windows-x64", "windows-x86", "windows-arm64", "linux-x64",
    "linux-x86", "linux-arm64", "macos-x64", "macos-arm64",
    "windows-x64-xp", "windows-x86-xp", "macos-universal",
}
MOBILE_TARGETS = {"android-arm64", "android-arm", "android-x86", "android-x86_64", "ios-arm64"}
LEGACY_PATCH_BACKUPS = {
    ".jce_ci_patch_v0148_run59", ".jce_ci_layout_v0148_run59_v10",
}


def _load(rel: str, root: Path):
    path = root / rel
    spec = importlib.util.spec_from_file_location("jce_layout_full_v11_" + path.stem, path)
    if spec is None or spec.loader is None:
        raise ValueError(f"module unavailable: {rel}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _cmake(root: Path, rel: str) -> str:
    return "${sourceDir}/build/" + rel


def _res(presets: dict, name: str, trail=()) -> dict:
    if name in trail or name not in presets:
        raise ValueError(f"missing or cyclic preset: {name}")
    item = presets[name]
    values = {"cacheVariables": {}}
    parents = item.get("inherits", ())
    if isinstance(parents, str):
        parents = (parents,)
    for parent in parents:
        r = _res(presets, parent, (*trail, name))
        values.update({k: v for k, v in r.items() if k != "cacheVariables"})
        values["cacheVariables"].update(r.get("cacheVariables", {}))
    values.update({k: v for k, v in item.items() if k not in ("inherits", "cacheVariables")})
    values["cacheVariables"].update(item.get("cacheVariables", {}))
    return values


def check_presets(root: Path, data: dict) -> list[str]:
    errors = []
    entries = data.get("configurePresets", [])
    names = [e.get("name") for e in entries]
    if len(names) != len(set(names)):
        errors.append("duplicate configurePreset names")
    presets = {e["name"]: e for e in entries if isinstance(e, dict) and "name" in e}
    paths: dict[str, str] = {}
    # CI must not confuse host-tools' permanent build/host objects with
    # platform objects; host tool commands still consume build/host/tools.
    expected = {"host-release": ("host", "host-conan")}
    expected.update(PLATFORM_PRESETS)
    for name, (binary, conan) in expected.items():
        try:
            preset = _res(presets, name)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        got_binary = preset.get("binaryDir")
        want_binary = _cmake(root, binary) if name != "host-release" else _cmake(root, "host")
        if got_binary != want_binary:
            errors.append(f"{name}: binaryDir mismatch {got_binary!r} != {want_binary!r}")
        got_toolchain = preset.get("cacheVariables", {}).get("CMAKE_TOOLCHAIN_FILE")
        want_toolchain = _cmake(root, conan) + TC
        if got_toolchain != want_toolchain:
            errors.append(f"{name}: Conan generator mismatch {got_toolchain!r} != {want_toolchain!r}")
        if got_binary in paths:
            errors.append(f"{name}: shares CMake binaryDir with {paths[got_binary]}")
        paths[got_binary] = name
        want_variant = "dist" if name == "wasm-dist" else None
        if want_variant and preset.get("cacheVariables", {}).get("JCE_BUILD_VARIANT") != want_variant:
            errors.append(f"{name}: missing Dist variant")
    build = {x.get("name"): x for x in data.get("buildPresets", [])}
    for name in ("build-wasm-release", "build-wasm-dist", "build-ios-arm64-release", "build-android-arm64-release", "build-jni-release"):
        if name not in build or build[name].get("configurePreset") != name[len("build-"):]:
            errors.append(f"missing or invalid build preset {name}")
    # Declared native desktop configs must not overlap with host/mobile/web/JNI.
    for entry in entries:
        name = entry.get("name", "")
        if not name or name.startswith("_"):
            continue
        try:
            p = _res(presets, name)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        binary = p.get("binaryDir")
        if isinstance(binary, str) and name not in expected:
            if binary in paths:
                errors.append(f"{name}: shared native build tree with {paths[binary]}")
            else:
                paths[binary] = name
    return errors


def _has(source: str, fragment: str, path: str, errors: list[str]):
    if fragment not in source:
        errors.append(f"{path}: missing canonical build owner: {fragment}")


def check_sources(root: Path) -> list[str]:
    errors = []
    scripts = {
        "tools/build/build-android.bat": (
            r"set \"CONAN_DIR=build\mobile\android-%ARCH%\conan\platform\"",
            r"set \"BUILD_DIR=build\mobile\android-%ARCH%\obj\platform\release\"",
            r"set \"GRADLE_BUILD_DIR=build\mobile\android-%ARCH%\gradle\"",
            r"set \"APK_COPY_DIR=build\mobile\android-%ARCH%\release\"",
            "JCE_ANDROID_GRADLE_BUILD_DIR", "JCE_ANDROID_STAGING_ROOT",
            r"set \"APK=%GRADLE_BUILD_DIR%\app\outputs\apk\debug\app-debug.apk\"",
        ),
        "tools/build/build-web.bat": (
            r"set \"CONAN_DIR=build\web\wasm\conan\platform\"",
            r"set \"BUILD_DIR=build\web\wasm\obj\platform\%VARIANT%\"",
            "JCE_BUILD_VARIANT=%VARIANT%",
        ),
        "tools/build/build-ios-arm64.sh": (
            'CONAN_DIR="build/mobile/ios-arm64/conan/xcode"',
            'BUILD_DIR="build/mobile/ios-arm64/obj/xcode/release"',
            '-G Xcode',
        ),
        "tools/build/android/build.gradle": (
            "JCE_ANDROID_GRADLE_BUILD_DIR", "layout.buildDirectory.set",
        ),
        "tools/build/android/app/build.gradle": (
            "JCE_ANDROID_STAGING_ROOT", "jniLibs.srcDirs", "assets.srcDirs",
        ),
        "tools/build/jce.py": (
            'ROOT / "build" / "web" / "wasm" / "conan" / "sdk"',
            'ROOT / "build/web/wasm/obj/sdk" / kind',
            'ROOT / "build" / family / t["key"] / "smoke"',
        ),
    }
    for name, needles in scripts.items():
        try:
            source = (root / name).read_text(encoding="utf-8")
        except OSError as exc:
            errors.append(f"{name}: unreadable: {exc}")
            continue
        for needle in needles:
            _has(source, needle.replace('\\"', '"'), name, errors)
    return errors


def check_roots(root: Path, data: dict) -> list[str]:
    errors = []
    if data.get("schema") != 1 or data.get("root") != "build":
        errors.append("incorrect build-root registry schema")
    roots = data.get("roots", {})
    for key in REQUIRED_ROOTS - set(roots):
        errors.append("missing registered build root: " + key)
    for key, info in roots.items():
        if not isinstance(info, dict) or not info.get("purpose") or not info.get("retention"):
            errors.append("unowned root: " + key)
    return errors


def audit(root: Path, *, with_v10: bool = True) -> list[str]:
    root = Path(root).resolve()
    errors = []
    try:
        data = json.loads((root / "CMakePresets.json").read_text(encoding="utf-8"))
        registry = json.loads((root / "contracts/build-output-roots.json").read_text(encoding="utf-8"))
        errors += check_presets(root, data)
        errors += check_roots(root, registry)
        errors += check_sources(root)
        if with_v10:
            v10 = _load("tools/build/layout_contract.py", root)
            errors += ["desktop: " + e for e in v10.audit(root)]
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
        errors.append(f"invalid/unreadable layout source: {exc}")
    return sorted(set(errors))


def inventory(root: Path) -> list[dict]:
    """Top level and direct native/platform children only. No recursive traversal."""
    root = Path(root).resolve()
    out = root / "build"
    if not out.exists():
        return []
    if out.is_symlink() or not out.is_dir():
        return [{"path": "build", "status": "INVALID_OR_ALIASED"}]
    data = json.loads((root / "contracts/build-output-roots.json").read_text(encoding="utf-8"))
    registered = set(data["roots"])
    rows = []
    for entry in sorted(out.iterdir(), key=lambda x: x.name.casefold()):
        if entry.is_symlink():
            status = "ALIAS_DO_NOT_FOLLOW"
        elif entry.name in registered:
            status = "REGISTERED_ROOT"
        elif entry.name in LEGACY_PATCH_BACKUPS:
            status = "LEGACY_PATCH_BACKUP_PRESERVE"
        else:
            status = "UNREGISTERED_REVIEW"
        rows.append({"path": entry.relative_to(root).as_posix(), "status": status})
        if entry.name not in ("desktop", "mobile", "web", "jni") or not entry.is_dir() or entry.is_symlink():
            continue
        for child in sorted(entry.iterdir(), key=lambda x: x.name.casefold()):
            if child.is_symlink():
                substatus = "ALIAS_DO_NOT_FOLLOW"
            elif entry.name == "desktop" and child.name in DESKTOP_TARGETS:
                substatus = "TARGET_OWNER_LEGACY_CACHE" if (child / "CMakeCache.txt").exists() else "TARGET_OWNER"
            elif entry.name == "mobile" and child.name in MOBILE_TARGETS:
                substatus = "TARGET_OWNER_LEGACY_CACHE" if (child / "CMakeCache.txt").exists() else "TARGET_OWNER"
            elif entry.name == "web" and child.name == "wasm":
                substatus = "TARGET_OWNER_LEGACY_CACHE" if (child / "CMakeCache.txt").exists() else "TARGET_OWNER"
            elif entry.name == "jni" and child.name == "desktop":
                substatus = "TARGET_OWNER_LEGACY_CACHE" if (child / "CMakeCache.txt").exists() else "TARGET_OWNER"
            elif (entry.name == "desktop" and re.fullmatch(r"(?:windows|linux|macos)-(?:x64|x86|arm64)-(?:conan|debug|sdk|dist-sdk|asan)", child.name)) or (entry.name in ("web", "mobile") and child.name.endswith("-conan")):
                substatus = "LEGACY_FLAT_CACHE_KEEP_IN_PLACE"
            else:
                substatus = "OTHER_CHILD_REVIEW"
            rows.append({"path": child.relative_to(root).as_posix(), "status": substatus})
    return rows


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--check", action="store_true")
    p.add_argument("--inventory", action="store_true")
    p.add_argument("--strict-existing", action="store_true", help="fail on unregistered/aliased existing entries")
    p.add_argument("--json", action="store_true", help="emit machine-readable read-only result")
    p.add_argument("--root", type=Path, default=ROOT)
    args = p.parse_args(argv)
    errors = audit(args.root)
    paths = inventory(args.root) if (args.inventory or args.strict_existing) else []
    if args.strict_existing:
        errors += [row["path"] + ": " + row["status"] for row in paths
                   if row["status"] in ("ALIAS_DO_NOT_FOLLOW", "INVALID_OR_ALIASED", "UNREGISTERED_REVIEW")]
    if args.json:
        print(json.dumps({"schema": 1, "contract": "jce.build-layout.v11",
                          "passed": not errors, "issues": errors, "entries": paths},
                         ensure_ascii=False, indent=2))
    else:
        for error in errors:
            print("LAYOUT FAIL:", error)
        for row in paths:
            print("LAYOUT", row["status"], row["path"])
        print("LAYOUT", "PASS" if not errors else "FAIL", "(read-only; no native build executed)")
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
