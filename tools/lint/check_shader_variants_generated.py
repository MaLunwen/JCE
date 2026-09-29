#!/usr/bin/env python3
"""The PBR fragment entry files and the key->program table must match
contracts/shader-keywords.json, and nothing outside the picker may choose a
PBR program.

TWO RULES, BOTH FROM THE SAME FAILURE.

RULE 1 -- NO DRIFT.  Every fs_pbr*.sc and jce_shader_variants_table.inc is
generated.  A hand edit to one of them is a program the manifest does not
know about: it compiles, it loads, and the table keeps handing out the other
one.  This runs the generator's own check mode, so the gate and the generator
cannot disagree about what "matching" means.

RULE 2 -- ONE DECISION POINT.  jce_renderer_get_program_variant is the only
place a PBR program is chosen.  The named getters are one-line calls into it,
and no other translation unit reads the table.  The rule exists because the
previous attempt at a keyword axis wired six of seven selection sites: forcing
its variant on every draw moved ZERO pixels, while the engine's own
enable_csm=false moved 7979 on the same frame.  Nothing was red.  Nothing
could be.

NEGATIVE CONTROLS, both verified to make this exit 1:
  * append a line to engine/shaders/pbr/fs_pbr.sc  -> STALE
  * add `r->program_variant[0][0]` to any other .c -> second reader
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GEN = ROOT / "tools" / "shadergen" / "gen_shader_variants.py"
PICKER = ROOT / "engine" / "src" / "renderer" / "jce_renderer.c"
# The FIELD, not the function: jce_renderer_get_program_variant
# contains the field's name as a substring, and the first version of
# this gate flagged every caller of the one function it exists to
# protect.  A gate that cannot tell its subject from its own name is
# the shape this file is about.
TABLE_READER = "->program_variant"


def main() -> int:
    failures: list[str] = []

    # ── Rule 1 ───────────────────────────────────────────────────────────
    if not GEN.exists():
        failures.append(f"the generator is missing: {GEN.relative_to(ROOT)}")
    else:
        r = subprocess.run([sys.executable, str(GEN)], cwd=str(ROOT),
                           capture_output=True, text=True)
        if r.returncode != 0:
            failures.append("generated shader variants do not match the "
                            "manifest:\n" + (r.stdout or r.stderr).rstrip())

    # ── Rule 2 ───────────────────────────────────────────────────────────
    readers: list[str] = []
    for path in sorted(ROOT.glob("engine/src/**/*.c")):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        # strip comments so a file that only TALKS about the table is not a
        # reader -- the same mistake that made an earlier gate in this tree
        # count a docstring as a caller.
        stripped = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        stripped = re.sub(r"//[^\n]*", "", stripped)
        if TABLE_READER not in stripped:
            continue
        if path.resolve() == PICKER.resolve():
            continue
        readers.append(path.relative_to(ROOT).as_posix())
    if readers:
        failures.append(
            "the PBR program table is read outside "
            "jce_renderer_get_program_variant:\n  "
            + "\n  ".join(readers)
            + "\n  Every draw path must ask that one function instead. "
              "Six of seven agreeing and the seventh not is a bug nothing "
              "reports.")

    if failures:
        print("check_shader_variants_generated: FAIL")
        for f in failures:
            print("  " + f)
        return 1

    print("check_shader_variants_generated: OK - generated files match "
          "contracts/shader-keywords.json and the program table has one reader")
    return 0


if __name__ == "__main__":
    sys.exit(main())
