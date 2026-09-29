#!/usr/bin/env python3
"""Water must be simulated in exactly one place.

The engine used to simulate every water body twice: the scene renderer kept a
private `water_time` accumulator and a private JceWaterFft, while the runtime
kept `buoyancy_time` and evaluated the analytic Gerstner sum.  Nothing was
wrong with either half -- they were each correct for their own consumer -- and
that is exactly why the disagreement survived so long.  A floating body bobbed
to a wave that was not the wave beneath it, and in FFT mode the two were not
even the same ocean.

jce_water_field.{h,c} is now the single authority.  This gate keeps it that
way, because the failure mode is invisible in every screenshot and in every
unit test that looks at only one side.

Two rules:

  1. Only the field module may CONSTRUCT a wave simulation
     (jce_water_fft_create / jce_water_fft_evolve).  A second construction site
     is a second ocean, however carefully its parameters are copied.

  2. Nobody may keep a private water clock accumulator.  The shared clock lives
     in the field set and is advanced by exactly one claimed driver.

Both rules allow an explicit opt-out comment on the preceding line, so a
deliberate exception is a decision someone wrote down rather than a diff nobody
noticed.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The authority itself, plus the tests that necessarily drive it directly.
CONSTRUCT_ALLOWED = {
    "engine/src/middleware/scene/jce_water_field.c",
    "engine/src/middleware/scene/jce_water_fft.c",
    # Declares the entry points; declaring is not constructing.  Allowlisted by
    # name rather than by skipping all headers, because a header CAN construct
    # (an inline or a macro) and that would be exactly the leak this catches.
    "engine/include/jce/middleware/scene/jce_water_fft.h",
}

CLOCK_ALLOWED = {
    "engine/src/middleware/scene/jce_water_field.c",
}

OPT_OUT = "jce-water-authority-exempt"

CONSTRUCT_RE = re.compile(r"\bjce_water_fft_(create|evolve)\s*\(")
# `x.water_time += dt`, `water_phase +=`, `wave_time +=` -- a private
# accumulator by any name.
CLOCK_RE = re.compile(r"\b\w*(water|wave)_(time|phase|clock)\s*\+=")

# User projects are OUT of this gate's scan surface (owner decision, 2026-08-27).
# The general engine and editor are the product; a user project is a downstream
# dogfooding consumer.  Folding the consumer in means a defect inside a game can
# turn the ENGINE's gate red -- and the question this gate answers is whether the
# engine and the editor held their own bar.
SCAN_DIRS = ["engine/src", "engine/include", "editor/src"]
SUFFIXES = {".c", ".h", ".cpp", ".hpp", ".inc.h"}


def scan():
    failures = []
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            try:
                lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                continue
            for i, line in enumerate(lines):
                prev = lines[i - 1] if i else ""
                if OPT_OUT in line or OPT_OUT in prev:
                    continue
                if CONSTRUCT_RE.search(line) and rel not in CONSTRUCT_ALLOWED:
                    failures.append(
                        f"{rel}:{i + 1}: constructs a wave simulation outside the "
                        f"water-field authority.\n"
                        f"    {line.strip()}\n"
                        f"    Acquire a JceWaterField from jce_scene_water_fields() "
                        f"instead; a second JceWaterFft is a second ocean."
                    )
                if CLOCK_RE.search(line) and rel not in CLOCK_ALLOWED:
                    failures.append(
                        f"{rel}:{i + 1}: keeps a private water clock.\n"
                        f"    {line.strip()}\n"
                        f"    Use jce_water_field_set_advance() (one claimed driver) "
                        f"and read jce_water_field_set_get_time()."
                    )
    return failures


def main():
    failures = scan()
    if failures:
        print("check_water_field_authority: FAILED\n")
        for f in failures:
            print("  " + f + "\n")
        print(
            f"  If an exception is genuinely correct, write "
            f"'{OPT_OUT}' on the line or the line above it,\n"
            f"  with a comment saying why."
        )
        return 1
    print("check_water_field_authority: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
