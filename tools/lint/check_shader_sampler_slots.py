#!/usr/bin/env python3
"""Sampler stage 5 is the engine-wide shadow-map slot.

The engine binds the shadow map to stage 5 from shared code (`sr_bind_frame_shadow_state`)
for whatever draw is in flight.  A program that declares its own sampler there is
correct right up until that shared bind runs for its draw -- and then either the
shadow or the program's own texture is wrong, depending on which bind happened
last.  Neither outcome errors, and neither looks like a slot conflict: it looks
like the surface is shaded oddly.

fs_water.sc had exactly this, with `s_water_data` on stage 5, and it was harmless
only because water received no shadows at all.  Adding shadow receiving is what
made it a bug, which is the shape worth guarding: the conflict is created by the
feature that was missing, not by the code that declared the slot.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADER_DIRS = [ROOT / "engine" / "shaders"]

DECL = re.compile(
    r"^\s*SAMPLER(?:2D|CUBE|2DARRAY|3D|2DSHADOW|2DARRAYSHADOW)\s*\(\s*"
    r"(\w+)\s*,\s*(\d+)\s*\)",
    re.MULTILINE,
)

RESERVED = {5: "s_shadowMap"}


def main() -> int:
    problems = []
    checked = 0
    for base in SHADER_DIRS:
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".sc", ".sh"):
                continue
            checked += 1
            text = path.read_text(encoding="utf-8", errors="replace")
            for m in DECL.finditer(text):
                name, stage = m.group(1), int(m.group(2))
                want = RESERVED.get(stage)
                if want and name != want:
                    line = text.count("\n", 0, m.start()) + 1
                    problems.append(
                        f"{path.relative_to(ROOT)}:{line}: "
                        f"stage {stage} is reserved for '{want}' but declares "
                        f"'{name}'"
                    )

    if not checked:
        # A guard that silently checks nothing is worse than no guard: it
        # reports success forever while the directory it watches has moved.
        print("check_shader_sampler_slots: FAIL - no shader files found", file=sys.stderr)
        return 1

    if problems:
        print("check_shader_sampler_slots: FAIL", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        print(
            "\n  Stage 5 is bound by sr_bind_frame_shadow_state() for every lit\n"
            "  draw.  Move the conflicting sampler to a free stage.",
            file=sys.stderr,
        )
        return 1

    print(f"check_shader_sampler_slots: OK ({checked} shader files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
