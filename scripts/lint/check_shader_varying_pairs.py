#!/usr/bin/env python3
"""A fragment shader's $input set must match its vertex shader's $output set.

This is not pedantry.  Adding `v_localpos` to vs_foliage.sc for hashed alpha
left fs_foliage.sc declaring three varyings against a vertex shader emitting
four, and left fs_foliage_shadow.sc declaring four against vs_foliage_shadow.sc
emitting three.  Both programs became invalid, and the symptom named none of
that: bushes and tree leaves stopped drawing entirely, while their shadows
stayed on the ground as SPHERES -- because the shadow pass silently falls back
to a sphere-proxy blob when its program is missing.

Nothing errored.  The shaders compiled, the build was green, 311 tests passed,
and the only evidence was that the foliage was gone.

One vertex shader is shared by several fragment shaders, so adding an output is
a change to every program it is paired with, not just the one being edited.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / "engine" / "shaders"

# vs_<stem> pairs with fs_<stem>; these are the pairings the engine builds.
IO = re.compile(r"^\$(input|output)\s+(.+)$", re.MULTILINE)


def varyings(path):
    """Varying names in a shader's $input/$output line (v_* only)."""
    text = path.read_text(encoding="utf-8", errors="replace")
    names = set()
    for kind, rest in IO.findall(text):
        # A vertex shader's $input is attributes, not varyings; skip those.
        if path.name.startswith("vs_") and kind == "input":
            continue
        for tok in rest.split(","):
            tok = tok.strip()
            if tok.startswith("v_"):
                names.add(tok)
    return names


def main() -> int:
    problems = []
    pairs = 0
    for vs in sorted(SHADERS.rglob("vs_*.sc")):
        fs = vs.with_name("fs_" + vs.name[3:])
        if not fs.exists():
            continue
        pairs += 1
        v_out, f_in = varyings(vs), varyings(fs)
        missing = f_in - v_out
        extra = v_out - f_in
        if missing:
            problems.append(
                f"{fs.relative_to(ROOT)}: reads {sorted(missing)} which "
                f"{vs.name} does not output"
            )
        if extra:
            problems.append(
                f"{vs.relative_to(ROOT)}: outputs {sorted(extra)} which "
                f"{fs.name} does not declare"
            )

    if not pairs:
        print("check_shader_varying_pairs: FAIL - no vs/fs pairs found",
              file=sys.stderr)
        return 1
    if problems:
        print("check_shader_varying_pairs: FAIL", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        print("\n  A mismatched varying set is an INVALID PROGRAM.  It compiles,\n"
              "  the build stays green, and the geometry simply stops drawing.",
              file=sys.stderr)
        return 1
    print(f"check_shader_varying_pairs: OK ({pairs} vs/fs pairs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
