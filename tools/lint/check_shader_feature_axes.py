#!/usr/bin/env python3
"""check_shader_feature_axes.py -- a shader feature macro that nothing defines
is a permutation axis that can never be compiled in.

The PBR uber-shader is one shared body (`engine/shaders/pbr/fs_pbr_body.sh`)
plus thin entry `.sc` files that `#define` a macro and include it, so each
macro yields a distinct compiled program with the disabled feature's code AND
uniforms absent.  That is the whole variant system in this tree: five
hand-written axes, no `multi_compile`, no `shader_feature`, no generator.

Hand-written means a macro can be GUARDED and never DEFINED, and nothing says
so -- not the compiler (the `#else` branch compiles fine), not the linker, not
a test.  The guarded code simply never exists in any program.

**MEASURED, and it was not merely dead.** `JCE_RENDER_COOKIE_2D_ARRAY` guarded
a `SAMPLER2DARRAY(s_cookie, 13)` declaration and two `texture2DArray` fetches,
and was defined nowhere.  So sampler 13 was `SAMPLER2D` in every compiled
program -- while `jce_lighting_system.c` bound a 16-layer array texture to it
unconditionally on every backend reporting `BGFX_CAPS_TEXTURE_2D_ARRAY`.  The
real cookie never reached the shader; what did was a texture the tree contains
no code to write into.  Light cookies did nothing, on every desktop backend,
silently: cookie on vs off moved 335 px at 1/255 (the noise floor is 46 px at
1/255).  After the bind was corrected: 487,015 px, 44.4%, peak 216/255.

So the rule is: every `JCE_*` macro used in a preprocessor guard under
`engine/shaders/` must be `#define`d somewhere under `engine/shaders/`, or be
listed in `shader_axis_exempt.txt` WITH a reason on the same line.  A bare path
with no reason is ignored, so the exemption file fails closed.

EXIT CODES
    0  every guarded axis is definable
    1  a guarded axis is defined nowhere and is not exempt
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / "engine" / "shaders"
EXEMPT = Path(__file__).resolve().parent / "shader_axis_exempt.txt"

# `#if defined(X)`, `#elif defined(X)`, `#ifdef X`, `#ifndef X`.  Only JCE_*:
# BGFX_*, and the language/platform macros bgfx itself defines, are not ours.
GUARD = re.compile(
    r'^\s*#\s*(?:if|elif)\s+.*?defined\s*\(\s*(JCE_[A-Z0-9_]+)\s*\)'
    r'|^\s*#\s*ifn?def\s+(JCE_[A-Z0-9_]+)\b')
DEFINE = re.compile(r'^\s*#\s*define\s+(JCE_[A-Z0-9_]+)\b')

# Include guards are not feature axes: a name that only ever appears as its own
# file's `#ifndef X` / `#define X` pair is a header guard, and this checker
# would otherwise demand an axis for every .sh in the tree.  They are excluded
# by construction rather than by pattern -- a guard IS defined, in the same
# file, so it passes the rule anyway.  Named here because a reader will ask.


def read_exempt():
    """path-less: one NAME per line, then whitespace, then the reason.

    A line with a name and NO reason is IGNORED -- not accepted.  That is the
    same fail-closed shape api_closure_exempt.txt uses, and for the same
    reason: an exemption whose justification nobody wrote is an exemption
    nobody can review.
    """
    out = {}
    if not EXEMPT.is_file():
        return out
    for raw in EXEMPT.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[1].strip():
            out[parts[0]] = parts[1].strip()
    return out


def main() -> int:
    if not SHADERS.is_dir():
        print("check_shader_feature_axes: SKIPPED - no engine/shaders")
        return 0

    used, defined = {}, set()
    for p in sorted(SHADERS.rglob("*")):
        if p.suffix not in (".sc", ".sh"):
            continue
        rel = p.relative_to(ROOT).as_posix()
        for i, ln in enumerate(p.read_text(encoding="utf-8",
                                           errors="replace").splitlines(), 1):
            m = GUARD.match(ln)
            if m:
                name = m.group(1) or m.group(2)
                used.setdefault(name, []).append("%s:%d" % (rel, i))
            m = DEFINE.match(ln)
            if m:
                defined.add(m.group(1))

    exempt = read_exempt()
    dead = sorted(n for n in used if n not in defined)
    failures = [n for n in dead if n not in exempt]

    for n in dead:
        if n in exempt:
            print("  [exempt] %-30s %s" % (n, exempt[n]))

    if failures:
        print("check_shader_feature_axes: FAIL - %d axis/axes guarded and "
              "never defined:" % len(failures))
        for n in failures:
            print("  %s -- guarded at:" % n)
            for s in used[n]:
                print("      " + s)
        print("  A guarded macro nothing defines is code that exists in no "
              "compiled program.  Define it in an entry .sc, delete the guard, "
              "or add it to tools/lint/shader_axis_exempt.txt WITH the "
              "reason and what is missing.")
        return 1

    print("check_shader_feature_axes: OK - %d guarded axis/axes, all definable "
          "(%d exempt with a recorded reason)"
          % (len(used), len([n for n in dead if n in exempt])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
