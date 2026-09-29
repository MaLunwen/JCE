#!/usr/bin/env python3
"""
check_instance_sort_depth.py -- keep instance batches front-to-back.

Background (2026-07-27).  The instance batchers collapse thousands of copies
into one instanced draw.  The GPU rasterises instances in buffer order, so the
order INSIDE a batch decides whether early-Z can reject the layers behind the
front-most surface -- and unlike solo draws there is no per-object sort to fall
back on.

Measured with RenderDoc on a 150k-cube scene: with the entries in arbitrary
order, one 21221-instance draw of ~250k triangles took 24.795 ms of a 25.894 ms
frame.  That is 31M indices/s, far below any modern GPU's geometry rate, i.e.
pure overdraw.  Ordering the same instances front-to-back took that draw to
1.257 ms -- 19.7x -- for no visual change at all.

"No visual change at all" is exactly the problem.  The rendered image is
identical whether or not the ordering is there, so every correctness gate in
the repo passes while the frame silently costs twenty times more: the unit
suites, the space determinism gates and the editor/runtime parity harness are
all blind to it.  A 19.7x win resting on the last line of a comparator needs
something that fails when that line goes away.

A unit test was the first choice and was rejected: the comparators are static
and their entry structs are engine-internal, so a test would have to mirror
two struct layouts (the existing grass test does exactly that and says why),
which breaks silently whenever a field is added.  This lint reads the source
instead, so it cannot drift out of sync with the layout.

The rule: every instance-batch comparator must compare the depth key, and must
do so LAST -- after the grouping keys.  Comparing it earlier is just as wrong
as omitting it: the grouping keys are what define a batch, so depth outranking
them would split one instanced draw into many and lose the collapse the
batcher exists for.

Usage:
  python tools/lint/check_instance_sort_depth.py
  # exits 0 if clean, 1 with file:line on violation.
"""

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "engine/src/middleware/scene/jce_sr_draw.c"

# The comparators that order an instance batch, and the depth field each must
# end with.  A new batcher belongs here the day it is written.
COMPARATORS = {
    "sr_prim_inst_cmp": "view_dist2",
    "sr_tex_inst_cmp": "view_dist2",
    "sr_tex_inst_wha_cmp": "view_dist2",
}

FUNC_RE = r"^(?:static\s+)?int\s+{name}\s*\([^;]*\)\s*$"


def function_body(text, name):
    """Return (start_line, body) for a column-0 function definition."""
    lines = text.splitlines()
    pattern = re.compile(FUNC_RE.format(name=re.escape(name)))
    for index, line in enumerate(lines):
        if not pattern.match(line):
            continue
        depth, started, body = 0, False, []
        for probe in lines[index:]:
            body.append(probe)
            depth += probe.count("{") - probe.count("}")
            if "{" in probe:
                started = True
            if started and depth <= 0:
                return index + 1, "\n".join(body)
        return index + 1, "\n".join(body)
    return None, None


def main():
    if not SOURCE.is_file():
        print(f"instance-sort depth check: SKIPPED (missing {SOURCE})")
        return 0

    text = SOURCE.read_text(encoding="utf-8", errors="replace")
    violations = []

    for name, field in COMPARATORS.items():
        line, body = function_body(text, name)
        if body is None:
            violations.append(
                (name, 0, f"comparator not found; if it was renamed, update "
                          f"COMPARATORS in this lint so the rule follows it"))
            continue

        # Strip comments so a mention in prose does not satisfy the rule.
        code = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
        code = re.sub(r"//[^\n]*", " ", code)

        if field not in code:
            violations.append(
                (name, line,
                 f"does not compare {field}: instances inside one batch are "
                 f"submitted in arbitrary order, so early-Z cannot reject "
                 f"overdraw (measured 19.7x on a 21k-instance draw)"))
            continue

        # The depth term must come last: everything after its first mention
        # must not introduce another comparison of a different field.
        tail = code[code.index(field):]
        later_fields = set(re.findall(r"e[ab]->(\w+)", tail))
        stray = later_fields - {field}
        if stray:
            violations.append(
                (name, line,
                 f"compares {sorted(stray)} AFTER {field}: depth must be the "
                 f"LAST key, or it outranks a grouping key and splits the "
                 f"batch into separate draws"))

    if violations:
        print("instance-sort depth check: FAILED")
        for name, line, why in violations:
            where = f"{SOURCE.relative_to(ROOT).as_posix()}:{line}" if line else "?"
            print(f"  {where}: {name}() {why}")
        return 1

    print(f"instance-sort depth check: OK "
          f"({len(COMPARATORS)} batch comparators end with the depth key)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
