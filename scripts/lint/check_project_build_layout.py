#!/usr/bin/env python3
"""
Check editor native project builds use a platform build root with variant
output/report directories, matching the engine workspace layout:

    build/<platform>-<arch>/{release,dist,reports}
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD_MANAGER = ROOT / "editor" / "src" / "core" / "jce_build_manager.cpp"
BUILD_PANEL = ROOT / "editor" / "src" / "panels" / "jce_panel_build_profiles.cpp"


def require(text: str, needle: str, label: str, failures: list[str]) -> None:
    if needle not in text:
        failures.append(f"{label}: missing {needle!r}")


def forbid(text: str, needle: str, label: str, failures: list[str]) -> None:
    if needle in text:
        failures.append(f"{label}: forbidden legacy layout fragment {needle!r}")


def main() -> int:
    failures: list[str] = []
    manager = BUILD_MANAGER.read_text(encoding="utf-8", errors="replace")
    panel = BUILD_PANEL.read_text(encoding="utf-8", errors="replace")

    require(manager, "project_build_root", str(BUILD_MANAGER), failures)
    require(manager, "variant_output_dir", str(BUILD_MANAGER), failures)
    require(manager, "project_reports_dir", str(BUILD_MANAGER), failures)
    require(manager, "CMAKE_RUNTIME_OUTPUT_DIRECTORY", str(BUILD_MANAGER), failures)
    require(manager, "CMAKE_LIBRARY_OUTPUT_DIRECTORY", str(BUILD_MANAGER), failures)
    require(manager, "CMAKE_ARCHIVE_OUTPUT_DIRECTORY", str(BUILD_MANAGER), failures)

    forbid(manager, 'std::string(platform_tag) + "-" + arch +\n                                  "-" + variant',
           str(BUILD_MANAGER), failures)

    require(panel, '"/build/" + std::string(plat) + "-" + arch_cli',
            str(BUILD_PANEL), failures)
    require(panel, '"/dist/" + name + "-" + ver',
            str(BUILD_PANEL), failures)
    require(panel, 'dir += "/release/";', str(BUILD_PANEL), failures)
    forbid(panel, 'dir += "-release/";', str(BUILD_PANEL), failures)
    forbid(panel, '"/dist/games/"', str(BUILD_PANEL), failures)

    if failures:
        for item in failures:
            print(item)
        return 1
    print("project build layout OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
