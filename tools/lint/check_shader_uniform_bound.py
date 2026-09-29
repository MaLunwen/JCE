#!/usr/bin/env python3
"""
check_shader_uniform_bound.py — a shader uniform nothing creates.

WHY THIS EXISTS.  A `uniform vec4 u_thing;` that no C code ever creates by
name is read by the shader as whatever bgfx leaves there.  The feature does
not error, does not warn, and does not work -- it just reads zero, which for
a direction vector, a colour or a strength is often indistinguishable from
"disabled".  An unbound SAMPLER is worse in a quieter way: it reads whichever
texture was last bound to that stage, so the feature does something, just not
its own thing.

This tree has already shipped one.  `wind_direction_ws` was declared and had
NO WRITER anywhere, so every consumer of wind direction read a zero vector
while the wind system looked wired from both ends.

WHAT THIS CHECKS.  Every `uniform <type> u_name;` AND every
`SAMPLER2D(s_name, stage)` under engine/shaders must have the literal string
"u_name" / "s_name" somewhere under engine/src or editor/src -- which is how
jce_uniform_create and bgfx_create_uniform take it.  Samplers are declared
through a macro rather than the keyword and fail the same way: an unbound
sampler reads whatever texture was last left on that stage.

IT SCANS THE FILESYSTEM, NOT `git ls-files`, and that is deliberate.  The
first run of this scan reported four dead cloud uniforms (u_cloud_atlas,
u_cloud_params, u_cloud_period, u_cloud_quality) because it read the git
index, and the file that creates them -- jce_sr_cloud_atlas.c -- was a
concurrent session's UNTRACKED work in progress.  The question is whether the
code on disk binds the uniform; a gate that answers it from the index reddens
on a colleague mid-change.

TWO MORE FALSE POSITIVES the first version produced, both from scanning too
little: u_grid_camera and u_grid_fade are created in
editor/src/scene/jce_editor_scene_render.cpp, and the scan was looking only
at engine/src.  A shader is not engine-only just because it lives under
engine/shaders.

ALSO CHECKED ON 2026-09-01, ALSO CLEAN: sampler STAGE collisions.  Two
samplers sharing a stage in one program means one silently wins, which is the
same shape as the bgfx view-id collisions check_view_reservations.py gates.
61 shader files declare samplers; exactly one pair shares a stage --
s_cluster and s_iesLut, both on 14 in fs_pbr_body.sh -- and they are
mutually exclusive by construction, under `#ifdef JCE_FORWARDPLUS` / `#else`,
documented at that file's lines 195-197.  A scan without preprocessor
awareness reports that pair; there is no second one to find.

WHAT THIS DOES NOT CHECK.  That the uniform is SET with a sensible value, or
set at all on the frame the shader runs.  Creation is the floor, not the
contract.

Usage:
    python tools/lint/check_shader_uniform_bound.py
    python tools/lint/check_shader_uniform_bound.py --list
Exit 0 clean, 1 on any uniform or sampler nothing creates.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]
SHADER_DIR = REPO_ROOT / "engine/shaders"
SOURCE_DIRS = ("engine/src", "editor/src")
SOURCE_SUFFIXES = (".c", ".cpp", ".h", ".hpp", ".inc")

SKIP_DIRS = ("third_party", "external", "vendor", "3rdparty")

UNIFORM = re.compile(r"^\s*uniform\s+\w+\s+(u_\w+)", re.M)
# Samplers are declared through a macro rather than the `uniform` keyword and
# fail exactly the same way -- an unbound sampler reads the last texture left
# on that stage, or nothing.  68 of them, all bound.
SAMPLER = re.compile(r"^\s*SAMPLER\w*\(\s*(\w+)\s*,", re.M)


def declared() -> dict:
    """uniform name -> shader files declaring it."""
    out: dict = {}
    for path in SHADER_DIR.rglob("*"):
        if path.suffix not in (".sc", ".sh"):
            continue
        if any(d in path.parts for d in SKIP_DIRS):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for name in UNIFORM.findall(text) + SAMPLER.findall(text):
            out.setdefault(name, set()).add(path.name)
    return out


def source_blob() -> str:
    parts = []
    for rel in SOURCE_DIRS:
        root = REPO_ROOT / rel
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            if any(d in path.parts for d in SKIP_DIRS):
                continue
            parts.append(path.read_text(encoding="utf-8", errors="replace"))
    return " ".join(parts)


def main() -> int:
    if not SHADER_DIR.is_dir():
        print("check_shader_uniform_bound: FAIL - engine/shaders not found",
              file=sys.stderr)
        return 1

    decls = declared()
    if len(decls) < 150:
        print("check_shader_uniform_bound: FAIL - found only %d uniform and "
              "sampler declaration(s); the shader shape changed and this "
              "extractor no longer sees them" % len(decls), file=sys.stderr)
        return 1

    blob = source_blob()
    dead = sorted(u for u in decls if ('"%s"' % u) not in blob)

    if "--list" in sys.argv:
        for name in sorted(decls):
            mark = "DEAD" if name in dead else "ok"
            print("%-28s %-6s %s" % (name, mark, ", ".join(sorted(decls[name]))))

    if dead:
        for name in dead:
            kind = "sampler" if name.startswith("s_") else "uniform"
            reads = ("whatever texture was last left on that stage"
                     if kind == "sampler" else
                     "whatever is there, usually zero -- which for a "
                     "direction, a colour or a strength is indistinguishable "
                     "from the feature being off")
            print("  %s: %s declared in %s, and NO source under %s ever names "
                  "it.  Nothing creates it, so the shader reads %s.  "
                  "`wind_direction_ws` shipped that way."
                  % (name, kind, ", ".join(sorted(decls[name])),
                     " or ".join(SOURCE_DIRS), reads), file=sys.stderr)
        print("check_shader_uniform_bound: FAIL - %d unbound declaration(s)."
              % len(dead), file=sys.stderr)
        return 1

    print("check_shader_uniform_bound: OK (%d shader uniform(s) and "
          "sampler(s); every one named by C or C++ source)" % len(decls))
    return 0


if __name__ == "__main__":
    sys.exit(main())
