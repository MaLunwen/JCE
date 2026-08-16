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
    """Yield (line_no, subject, threshold, is_else_if) for ladder arms."""
    for n, line in enumerate(text.split("\n"), 1):
        m = IF_RE.match(line)
        if m:
            yield n, m.group(2), float(m.group(3)), bool(m.group(1))


def main() -> int:
    problems = []
    checked = 0
    for path in sorted(SHADER_ROOT.rglob("*")):
        if path.suffix not in (".sc", ".sh"):
            continue
        checked += 1
        arms = list(scan(path.read_text(encoding="utf-8", errors="replace")))

        # Group consecutive arms that share a subject into one ladder: an
        # `else if` continues the ladder started by the preceding `if`.
        ladder = []
        for n, subj, thr, is_else in arms:
            if is_else and ladder and ladder[-1][1] == subj:
                ladder.append((n, subj, thr))
            else:
                if len(ladder) > 1:
                    problems += check_ladder(path, ladder)
                ladder = [(n, subj, thr)]
        if len(ladder) > 1:
            problems += check_ladder(path, ladder)

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
