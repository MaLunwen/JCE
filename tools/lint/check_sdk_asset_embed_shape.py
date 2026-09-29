#!/usr/bin/env python3
"""
Check that editor-driven project asset embedding preserves the same build
artifact shape as the legacy host tools: a cooked JPAK payload wrapped as a
linker object/assembly source, with C-array output only as a fallback.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "cmake" / "JCESDKHelpers.cmake"
BUILD_MANAGER = ROOT / "editor" / "src" / "core" / "jce_build_manager.cpp"


def require(text: str, needle: str, label: str, failures: list[str]) -> None:
    if needle not in text:
        failures.append(f"{label}: missing {needle!r}")


def main() -> int:
    failures: list[str] = []
    helper = HELPER.read_text(encoding="utf-8", errors="replace")
    manager = BUILD_MANAGER.read_text(encoding="utf-8", errors="replace")

    require(helper, "JCE_PROJECT_PREBUILT_ASSETS_OBJ", str(HELPER), failures)
    require(helper, "JCE_PROJECT_PREBUILT_ASSETS_ASM", str(HELPER), failures)
    require(helper, "JCE_PROJECT_PREBUILT_ASSETS_C", str(HELPER), failures)
    require(helper, "EXTERNAL_OBJECT TRUE", str(HELPER), failures)

    obj_pos = helper.find("JCE_PROJECT_PREBUILT_ASSETS_OBJ")
    c_pos = helper.find("JCE_PROJECT_PREBUILT_ASSETS_C")
    if obj_pos < 0 or c_pos < 0 or obj_pos > c_pos:
        failures.append(f"{HELPER}: asset object path must be checked before C fallback")

    require(manager, "project_assets.obj", str(BUILD_MANAGER), failures)
    require(manager, "project_assets.S", str(BUILD_MANAGER), failures)
    require(manager, "JCE_PROJECT_PREBUILT_ASSETS_OBJ", str(BUILD_MANAGER), failures)
    require(manager, "JCE_PROJECT_PREBUILT_ASSETS_ASM", str(BUILD_MANAGER), failures)
    require(manager, "-U JCE_PROJECT_PREBUILT_ASSETS_OBJ", str(BUILD_MANAGER), failures)
    require(manager, "asset_pack_zstd_level_for_variant", str(BUILD_MANAGER), failures)

    if failures:
        for item in failures:
            print(item)
        return 1
    print("sdk asset embed shape OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
