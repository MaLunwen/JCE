#!/usr/bin/env python3
"""Every on-disk format the engine READS must have something that WRITES it.

The cooked-terrain v3 codec shipped with a directory, a ridge channel, a tiled
writer, and a full unit-test suite -- and nothing in the tree could produce a
file in the format.  `jce_terrain_format.c` was compiled into the engine but
into no tool, so `jce_terrain_write_tiled` had zero call sites outside its own
tests.  Every test passed.  A format with no producer is not a format, and
nothing about that state looks wrong from inside the test suite: the tests
construct their own buffers, so they are green precisely because they never
needed a producer.

This check is deliberately narrow.  It does not try to discover formats; it
asserts a fixed list of (codec source, tool that must link it), because the
failure it guards against is a codec drifting OUT of a tool's source list --
which is a specific, checkable edit, not a property to be inferred.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# (codec translation unit, cmake target that must compile it, why)
REQUIRED = [
    (
        "engine/src/resource/jce_terrain_format.c",
        "jce_cook",
        "the cooked-terrain v3 codec; jce_cook --terrain is its only producer",
    ),
]


def target_sources(cmake_text, target):
    """Source lines inside add_executable(<target> ...)."""
    m = re.search(r"add_executable\s*\(\s*" + re.escape(target) + r"\b", cmake_text)
    if not m:
        return None
    i = m.end()
    depth = 1
    # Walk from the '(' we already consumed to its matching ')'.
    start = cmake_text.rindex("(", 0, m.end())
    i = start + 1
    depth = 1
    while i < len(cmake_text) and depth > 0:
        if cmake_text[i] == "(":
            depth += 1
        elif cmake_text[i] == ")":
            depth -= 1
        i += 1
    return cmake_text[start:i]



def producer_sources(cmake_text, target, helper_text=""):
    """Follow only the explicitly maintained offline-source helper edge."""
    cmake_text = re.sub(r"(?m)#.*$", "", cmake_text)
    direct = target_sources(cmake_text, target)
    if direct is None:
        return None
    include = 'include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/JCEHostAssetSources.cmake")'
    call = re.search(r"\bjce_host_asset_sources\s*\(\s*"+re.escape(target)+r"\s*\)", cmake_text)
    if include not in cmake_text or not call:
        return direct
    helper_text = re.sub(r"(?m)#.*$", "", helper_text)
    function = re.search(r"function\s*\(\s*jce_host_asset_sources\s+target\s*\)(.*?)endfunction\s*\(",
                         helper_text, re.S)
    if not function:
        return direct
    # Adapt the existing balanced command reader; unrelated helper text cannot
    # admit a codec whose target_sources command has lost the actual source.
    body = function.group(1).replace('target_sources(${target}', 'add_executable('+target)
    shared = target_sources(body, target)
    return direct+(shared or "")

def main() -> int:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    problems = []

    for codec, target, why in REQUIRED:
        if not (ROOT / codec).exists():
            problems.append(f"{codec}: file is gone, but the rule still names it")
            continue
        block = producer_sources(cmake, target,
            (ROOT / "cmake/JCEHostAssetSources.cmake").read_text(encoding="utf-8")
            if (ROOT / "cmake/JCEHostAssetSources.cmake").exists() else "")
        if block is None:
            problems.append(f"add_executable({target} ...) not found in CMakeLists.txt")
            continue
        leaf = Path(codec).name
        if leaf not in block:
            problems.append(
                f"{target} does not compile {codec}\n"
                f"      ({why})\n"
                f"      Without it the format has a reader and no writer, and "
                f"every test still passes."
            )

    if problems:
        print("check_format_has_producer: FAIL", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 1

    print(f"check_format_has_producer: OK ({len(REQUIRED)} format(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
