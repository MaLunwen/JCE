#!/usr/bin/env python3
"""
check_mesh_enable_consumers.py — every path that turns a MeshRenderer into
geometry must first ask whether that MeshRenderer is enabled.

Background (2026-07-26).  jce_scene_set_comp_enabled records the flag
correctly and jce_scene_component_enabled reads it back correctly, so a unit
test of the state round-trip passes while the feature is still broken: what
matters is whether the DRAW paths consult it.  Of the four consumers in
jce_sr_draw.c only the generic one did.  The other three -- the parallel
colour gather, the factor-only primitive instance batch and the
texture-array instance batch -- read the component, resolved its mesh and
built a draw command regardless.  Whether disabling a mesh did anything then
came down to which batching path a given host happened to take that frame,
which is how the editor Game View and the standalone runtime ended up
disagreeing about whether a planet was on screen.

The rule this enforces is deliberately structural rather than behavioural:
a renderer test would need a device, and the failure mode here is someone
adding a FIFTH batching path a year from now.  So: any function that both
obtains a JceMeshRenderer and resolves it to a mesh must also contain a
JCE_COMP_FLAG_MESH_RENDERER enable test.

Usage:
  python scripts/lint/check_mesh_enable_consumers.py
  # exits 0 if clean, 1 with file:line:function on violation.
"""

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]

# Files that actually turn components into draw submissions.
SCANNED = (
    "engine/src/middleware/scene/jce_sr_draw.c",
    "engine/src/middleware/scene/jce_sr_environment.c",
    "engine/src/middleware/scene/jce_sr_cull.c",
)

GET_MR = re.compile(r"jce_scene_get_mesh_renderer(_const)?\s*\(")
RESOLVE = re.compile(r"sr_resolve_mesh\s*\(")
ENABLE = re.compile(
    r"jce_scene_component_enabled\s*\([^;]*JCE_COMP_FLAG_MESH_RENDERER", re.S
)
# A function definition at column 0: "static bool name(" / "void name(" ...
FUNC_START = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t\*]*\b(\w+)\s*\([^;]*$")

# Culling collects bounds, it does not submit geometry; a disabled entity is
# filtered at draw time. Listed explicitly so the exemption is a decision
# rather than an accident of the heuristic.
ALLOW_FUNCTIONS = {
    "sr_entity_bounds",
    "sr_entity_world_aabb",
}


def split_functions(text):
    """Yield (name, start_line, body) for each column-0 function definition."""
    lines = text.splitlines()
    spans = []
    for index, line in enumerate(lines):
        match = FUNC_START.match(line)
        if match and not line.lstrip().startswith(("/*", "*", "//")):
            spans.append((match.group(1), index))
    for position, (name, start) in enumerate(spans):
        end = spans[position + 1][1] if position + 1 < len(spans) else len(lines)
        yield name, start + 1, "\n".join(lines[start:end])


def main():
    violations = []
    for relative in SCANNED:
        path = ROOT / relative
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for name, line, body in split_functions(text):
            if name in ALLOW_FUNCTIONS:
                continue
            if not (GET_MR.search(body) and RESOLVE.search(body)):
                continue
            if ENABLE.search(body):
                continue
            violations.append((relative, line, name))

    if violations:
        print("mesh-enable consumer check: FAILED")
        print("(a path builds geometry from a MeshRenderer without testing"
              " JCE_COMP_FLAG_MESH_RENDERER)")
        for relative, line, name in violations:
            print(f"  {relative}:{line}: {name}() resolves a mesh but never"
                  f" checks whether the MeshRenderer is enabled")
        return 1

    print("mesh-enable consumer check: OK"
          " (every mesh-resolving draw path honours the enable flag)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
