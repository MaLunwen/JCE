#!/usr/bin/env python3
"""check_shadow_pancake.py -- every shadow caster gets clamped, or none do.

A cascade's orthographic box is fitted to the camera's frustum slice and pushed
back toward the sun by a heuristic (jce_csm.c, `near_extend`) that is sized
from a world-XZ window of +/- 2*radius.  The horizontal reach of a caster that
shadows a cascade is `height / tan(sun elevation)`, which is unbounded as the
sun drops and has nothing to do with the cascade radius -- so at a low sun, or
at a short shadow distance, casters fall outside the window, get clipped by the
light's near plane, and the shadows they owe are simply absent.  On screen: a
shadow with a straight edge across it that moves when the camera merely turns.

The fix is a clamp in the shadow VERTEX shader, not a bigger box -- pushing the
near plane out was written, proved sufficient, and measured: it deepens
`depth_range`, the depth bias is expressed in NORMALIZED depth, and the same
bias then means a larger world offset.  The rendered shadow area went DOWN
2-3.6% at every yaw.  So `shadow_pancake.sh` clamps z onto the near plane and
leaves the box alone.

WHY THIS FILE EXISTS.  That clamp has to be in EVERY vertex shader that feeds a
shadow map.  Miss one and that path keeps the defect while the others are
fixed, which is the worst of the three outcomes: the bug becomes intermittent
and correlates with the mesh type rather than with anything a reporter can
name.  This repository has shipped that shape more than once -- a feature whose
working half hides its broken half.

WHAT IS CHECKED
  1. Every `vs_*shadow*.sc` under engine/shaders includes shadow_pancake.sh
     AND calls jce_shadow_pancake(, directly or through a local quoted include.
  2. There is at least one such file, and at least as many as the day this was
     written -- a glob that silently matches nothing is a gate that cannot
     fail, and this repository has had those too.
  3. The header still branches on the depth convention.  The near plane is at
     z = -w under OpenGL / OpenGL ES and z = 0 everywhere else; collapsing that
     to one branch does not fail to compile, it fails to clamp (or clamps away
     half the depth range) on one family of backends, silently.

EXIT CODES
    0  every shadow vertex shader clamps
    1  at least one does not, or the header lost its convention branch
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / "engine" / "shaders"
HEADER = SHADERS / "pbr" / "shadow_pancake.sh"

# The count on the day this was written.  It is a FLOOR, not an equality: a new
# shadow vertex shader must be caught by rule 1, not by this number, and a
# number that has to be edited every time the tree grows is a number people
# start editing without reading.  It exists only so that a glob that matches
# nothing -- a rename, a moved directory, a typo in the pattern -- cannot pass.
MIN_SHADOW_VS = 5

CALL = "jce_shadow_pancake("
QUOTED_INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"\n]+)"', re.MULTILINE)


def shadow_vertex_shaders():
    """Vertex shaders that feed a shadow map.

    Matched by name rather than by reading the C that loads them: the program
    table (jce_shaders.c) names PROGRAMS, and a program's vertex shader is
    resolved by bgfx at load time from a pak, so following it from here would
    mean reimplementing that resolution and getting to be wrong about it
    separately.  The naming convention is load-bearing and is worth stating:
    a vertex shader that feeds a shadow map has `shadow` in its name.
    """
    out = []
    for p in sorted(SHADERS.rglob("vs_*.sc")):
        if "shadow" in p.name:
            out.append(p)
    return out


def strip_comments(source):
    # Keep line boundaries and prevent comments from joining adjacent tokens.
    return re.sub(r"/\*.*?\*/|//[^\n]*",
                  lambda match: " " + "\n" * match[0].count("\n"),
                  source, flags=re.S)


def local_shader_sources(path, visited):
    """Yield uncommented sources from the shader's quoted include closure."""
    path = path.resolve()
    if path in visited:
        return
    visited.add(path)
    code = strip_comments(path.read_text(encoding="utf-8"))
    yield path, code
    for name in QUOTED_INCLUDE.findall(code):
        included = (path.parent / name).resolve()
        if included.is_relative_to(SHADERS) and included.is_file():
            yield from local_shader_sources(included, visited)


def main():
    failures = []

    if not HEADER.is_file():
        print("FAIL: %s is missing" % HEADER.relative_to(ROOT))
        return 1
    header = strip_comments(HEADER.read_text(encoding="utf-8"))
    header_path = HEADER.resolve()

    # Rule 3, first: if the header has lost its branch, every call site below is
    # calling something that clamps against the wrong plane on half the
    # backends, and reporting them as PASS would be worse than saying nothing.
    if "BGFX_SHADER_LANGUAGE_GLSL" not in header:
        failures.append(
            "%s no longer branches on BGFX_SHADER_LANGUAGE_GLSL: the near "
            "plane is at z = -w on GL/GLES and z = 0 elsewhere, and one "
            "branch cannot be right for both"
            % HEADER.relative_to(ROOT))
    if "-clip.w" not in header.replace(" ", ""):
        failures.append(
            "%s no longer clamps against -w: that is the GL/GLES near plane"
            % HEADER.relative_to(ROOT))
    if not re.search(r"clip\.z\s*=\s*max\(\s*clip\.z\s*,\s*0\.0\s*\)", header):
        failures.append(
            "%s no longer clamps against 0.0: that is the near plane on "
            "D3D, Vulkan and Metal" % HEADER.relative_to(ROOT))

    files = shadow_vertex_shaders()
    if len(files) < MIN_SHADOW_VS:
        failures.append(
            "found %d shadow vertex shaders under %s, expected at least %d -- "
            "the pattern matches less than it did, so this check is not "
            "checking what it claims to"
            % (len(files), SHADERS.relative_to(ROOT), MIN_SHADOW_VS))

    for p in files:
        sources = dict(local_shader_sources(p, set()))
        rel = p.relative_to(ROOT).as_posix()
        if header_path not in sources:
            failures.append("%s does not include %s" % (rel, HEADER.name))
        # The convention header defines the function. Only a source using it
        # outside that header proves the caster wires the clamp.
        if not any(CALL in code for path, code in sources.items() if path != header_path):
            failures.append(
                "%s writes gl_Position without %s -- a caster further up-sun "
                "than the cascade box reaches is clipped away on this path, "
                "and the shadow it owes is missing" % (rel, CALL))

    if failures:
        for f in failures:
            print("FAIL: %s" % f)
        return 1
    print("OK: %d shadow vertex shaders clamp to the near plane (%s)"
          % (len(files), ", ".join(p.name for p in files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
