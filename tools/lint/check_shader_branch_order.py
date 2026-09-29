#!/usr/bin/env python3
"""An if/else-if ladder of `x > K` tests must have strictly descending K.

This is not a style rule.  fs_sky.sc dispatched its sky modes as
`> 2.5`, `> 1.5`, `> 3.5`, `> 0.5`, and the `> 3.5` arm was therefore
unreachable: every value above 2.5 had already been claimed two arms earlier.
That arm held the physical-atmosphere transmittance lookup and the entire
volumetric cloud march.  Selecting the mode did not error, did not warn, and did
not render black -- it quietly drew the mode above it.  The feature was dead for
its whole existence and every visual check of it was really a check of something
else.

The failure is invisible by construction, so it needs a mechanical check.  A
greater-than ladder that is not sorted is not a dispatch; it is shadowing that
compiles.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADER_ROOT = ROOT / "engine" / "shaders"

# `if (expr > 1.5) {`  /  `} else if (expr > 2.5) {`
IF_RE = re.compile(r"^\s*(\}\s*else\s+)?if\s*\(\s*([A-Za-z_][\w.\[\]]*)\s*>\s*"
                   r"(-?\d+(?:\.\d*)?)\s*\)\s*\{")


def scan(text):
    """Yield (line_no, subject, threshold, is_else_if, depth) for ladder arms.

    DEPTH IS NOT DECORATION.  Without it the grouper below broke a ladder on
    any intervening `if` -- including the ladder's own nested ones -- and
    fs_sky.sc, the file whose mode dispatch this gate was written after, has
    five of those between its four mode arms.  Its ladder was split into a
    2-arm fragment plus two singletons, so re-injecting the historical
    ordering (2.5, 1.5, 3.5, 0.5) left `> 3.5` unreachable and the gate still
    exited 0.  Measured: one 2-arm ladder grouped across the whole 139-file
    tree.

    The depth reported is the one BEFORE the line, so `} else if (...) {`
    (net zero braces) reports the same depth as the `if` it continues, and a
    nested `if` reports a deeper one.
    """
    depth = 0
    for n, line in enumerate(text.split("\n"), 1):
        m = IF_RE.match(line)
        if m:
            # The closing brace of `} else if (...) {` sits BEFORE the arm on
            # its own line and is not counted until the end of the loop, so
            # subtract the closes that precede the keyword.  Without this an
            # `else if` reports one level deeper than the `if` it continues
            # and NOTHING groups -- measured: 33 ladders, 0 of them multi-arm.
            head = line[:m.start(0) + line[m.start(0):].index("if")]
            yield (n, m.group(2), float(m.group(3)), bool(m.group(1)),
                   depth - head.count("}"))
        depth += line.count("{") - line.count("}")


def main() -> int:
    problems = []
    checked = 0
    for path in sorted(SHADER_ROOT.rglob("*")):
        if path.suffix not in (".sc", ".sh"):
            continue
        checked += 1
        arms = list(scan(path.read_text(encoding="utf-8", errors="replace")))

        # One open ladder per brace depth: an `else if` continues the ladder
        # at ITS depth, and a nested `if` starts its own without disturbing
        # the outer one.  Grouping by adjacency instead made every nested
        # branch a ladder terminator -- see scan().
        open_ladders = {}
        for n, subj, thr, is_else, depth in arms:
            # Anything deeper than this arm is now closed.
            for d in [k for k in open_ladders if k > depth]:
                lad = open_ladders.pop(d)
                if len(lad) > 1:
                    problems += check_ladder(path, lad)
            cur = open_ladders.get(depth)
            if is_else and cur and cur[-1][1] == subj:
                cur.append((n, subj, thr))
            else:
                if cur and len(cur) > 1:
                    problems += check_ladder(path, cur)
                open_ladders[depth] = [(n, subj, thr)]
        for lad in open_ladders.values():
            if len(lad) > 1:
                problems += check_ladder(path, lad)

    if not checked:
        print("check_shader_branch_order: FAIL - no shader files found",
              file=sys.stderr)
        return 1

    if problems:
        print("check_shader_branch_order: FAIL", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        print("\n  A later arm with a HIGHER threshold can never be reached:\n"
              "  the earlier arm already matched everything above it.  Sort the\n"
              "  ladder in descending order.", file=sys.stderr)
        return 1

    print(f"check_shader_branch_order: OK ({checked} shader files)")
    return 0


def check_ladder(path, ladder):
    out = []
    for i in range(1, len(ladder)):
        n, subj, thr = ladder[i]
        prev_n, _, prev_thr = ladder[i - 1]
        if thr >= prev_thr:
            out.append(
                f"{path.relative_to(ROOT)}:{n}: unreachable arm - "
                f"'{subj} > {thr}' comes after '{subj} > {prev_thr}' "
                f"(line {prev_n}), which already matched every such value"
            )
    return out


if __name__ == "__main__":
    sys.exit(main())
