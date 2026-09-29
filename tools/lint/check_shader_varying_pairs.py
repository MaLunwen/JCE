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


LINKED = re.compile(
    r'shader_load_program(?:_fs)?_named\s*\([^;]*?"([a-z_0-9]+)"\s*,'
    r'\s*"([a-z_0-9]+)"', re.S)

CODE_TREES = ("engine/src", "editor/src")


def linked_pairs() -> set:
    """(vs_base, fs_base) pairs the renderer actually links, from the source.

    THE PROGRAMS ARE NOT ALL SAME-STEM.  shader_load_program_named() takes the
    two base names separately, and the renderer uses it for exactly the cases
    where a shared vertex stage feeds a differently-named fragment stage:
    pbr_skinned_inst -> pbr, shadow_skinned_inst -> shadow,
    gbuffer_vel_inst -> gbuffer_vel, pick_skinned -> pick.  Pairing by stem
    compared none of those, which is backwards -- a shared vertex shader is
    exactly where a varying drifts out from under one of its consumers.
    """
    out = set()
    for tree in CODE_TREES:
        base = ROOT / tree
        if not base.is_dir():
            continue
        for src in list(base.rglob("*.c")) + list(base.rglob("*.cpp")):
            text = src.read_text(encoding="utf-8", errors="replace")
            for m in LINKED.finditer(text):
                out.add((m.group(1), m.group(2)))
    return out


def main() -> int:
    problems = []
    all_vs = sorted(SHADERS.rglob("vs_*.sc"))
    all_fs = sorted(SHADERS.rglob("fs_*.sc"))
    by_name = {p.name: p for p in all_vs + all_fs}

    # 1. Exact pairs: same stem, plus every literal pair the renderer links.
    exact = set()
    for vs in all_vs:
        fs = vs.with_name("fs_" + vs.name[3:])
        if fs.exists():
            exact.add((vs, fs))
    for a, b in linked_pairs():
        v, f = by_name.get("vs_%s.sc" % a), by_name.get("fs_%s.sc" % b)
        if v is not None and f is not None:
            exact.add((v, f))

    pairs = len(exact)
    for vs, fs in sorted(exact):
        v_out, f_in = varyings(vs), varyings(fs)
        missing = f_in - v_out
        if missing:
            problems.append(
                f"{fs.relative_to(ROOT)}: reads {sorted(missing)} which "
                f"{vs.name} does not output"
            )
        # `extra` is only meaningful for a DEDICATED vertex stage.  A shared
        # one legitimately outputs more than any single consumer reads.
        shared = sum(1 for (v, _) in exact if v == vs) > 1
        extra = v_out - f_in
        if extra and not shared:
            problems.append(
                f"{vs.relative_to(ROOT)}: outputs {sorted(extra)} which "
                f"{fs.name} does not declare"
            )

    # 2. Everything else: a fragment stage must be satisfiable by at least ONE
    #    vertex stage in its own directory.  That is how the postfx family is
    #    linked -- shader_load_program_named(pak, "postfx", fs_name) with a
    #    computed fs -- and 36 of 65 fragment shaders were compared to nothing
    #    at all before this.
    paired_fs = {f for (_, f) in exact}
    dir_out = {}
    for vs in all_vs:
        dir_out.setdefault(vs.parent, set()).update(varyings(vs))
    unpaired = orphan = 0
    for fs in all_fs:
        if fs in paired_fs:
            continue
        candidates = dir_out.get(fs.parent)
        if not candidates:
            orphan += 1
            continue
        unpaired += 1
        missing = varyings(fs) - candidates
        if missing:
            problems.append(
                f"{fs.relative_to(ROOT)}: reads {sorted(missing)} which no "
                f"vertex shader in {fs.parent.name}/ outputs"
            )

    if not pairs:
        print("check_shader_varying_pairs: FAIL - no vs/fs pairs found",
              file=sys.stderr)
        return 1
    covered = pairs + unpaired
    if covered < len(all_fs) - orphan:
        print("check_shader_varying_pairs: FAIL - %d fragment shader(s) "
              "resolved to neither a pair nor a directory candidate; the "
              "matching no longer sees the tree." % (len(all_fs) - orphan -
                                                     covered), file=sys.stderr)
        return 1
    if problems:
        print("check_shader_varying_pairs: FAIL", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        print("\n  A mismatched varying set is an INVALID PROGRAM.  It compiles,\n"
              "  the build stays green, and the geometry simply stops drawing.",
              file=sys.stderr)
        return 1
        print("check_shader_varying_pairs: OK (%d exact pair(s) + %d fragment "
          "shader(s) checked against their directory's vertex stages; %d "
          "with no vertex shader in scope)" % (pairs, unpaired, orphan))
    return 0


if __name__ == "__main__":
    sys.exit(main())
