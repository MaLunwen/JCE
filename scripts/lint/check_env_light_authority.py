#!/usr/bin/env python3
"""
check_env_light_authority.py — environment lighting must come from ONE place.

Background (2026-08-04).  Four subsystems each computed their own version of
the same lighting state, so they could disagree and nothing noticed:

  * The renderer's ambient fallback used three hardcoded RGB literals
    (0.25,0.45,0.80 / 0.65,0.78,0.92 / 0.22,0.22,0.28) whenever no HDR skybox
    supplied IBL.  Those literals were somebody approximating the Preetham sky
    by eye, and the two models could drift arbitrarily.
  * The time-of-day sun colour was a four-stop palette lerped on sun altitude,
    unrelated to any atmosphere.
  * The time-of-day sky gradient was nine more palette stops, unrelated to the
    Preetham sky the renderer drew for the same instant.
  * fog_color, fog_density and exposure were computed every frame and read by
    nothing, so a day/night cycle kept its daytime fog and exposure at
    midnight.

All four are now derived: sun colour from atmospheric transmittance, ambient
from an SH9 projection of the sky, the gradient from the same sky, and fog and
exposure through explicit resolvers.

This lint does NOT try to prove the derivation is correct -- unit tests do
that.  It guards the failure MODE, which is proliferation: the disagreement
spread because any translation unit could reach for the sky/time-of-day
headers and roll its own interpretation.  So the rule is structural and
deliberately narrow:

  1. Only an allowlisted set of files may include jce_sky.h or
     jce_time_of_day.h directly.  A new consumer is not forbidden, but it has
     to be a conscious edit to this list rather than an incidental include.

  2. The renderer must not reintroduce a hardcoded sky-ambient literal triple.
     Checked by looking for the specific literals that were removed, so the
     check cannot drift into flagging unrelated colour constants.

Usage:
  python scripts/lint/check_env_light_authority.py
  # exits 0 if clean, 1 with file:line on violation.
"""

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]

# Files permitted to include the sky / time-of-day headers directly.
#
# jce_time_of_day.c and jce_sky.c are the authority itself.  The renderer
# files below are the legitimate consumers: the sky pass evaluates the model,
# the draw path projects it to SH9 ambient, and the scene renderer resolves
# fog and exposure through the published resolvers.
ALLOWED = {
    "engine/src/middleware/world/jce_time_of_day.c",
    "engine/src/middleware/world/jce_sky.c",
    "engine/src/middleware/world/jce_atmosphere.c",
    "engine/src/middleware/scene/jce_sr_environment.c",
    "engine/src/middleware/scene/jce_sr_draw.c",
    "engine/src/middleware/scene/jce_scene_renderer.c",
    # Type-only includes: the renderer struct HOLDS a JceTimeOfDayState member
    # and the public renderer header exposes a time-of-day setter, so both need
    # the declaration.  Neither computes lighting; they only carry the type.
    "engine/src/middleware/scene/jce_sr_internal.h",
    "engine/include/jce/renderer/jce_scene_renderer.h",
}

GUARDED_HEADERS = ("jce_sky.h", "jce_time_of_day.h")

INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"]jce/middleware/world/(jce_sky\.h|jce_time_of_day\.h)[>"]'
)

# The exact literals that used to stand in for the sky.  Matching the specific
# values keeps this from firing on unrelated colour constants.
BANNED_LITERALS = (
    re.compile(r"0\.25f\s*,\s*0\.45f\s*,\s*0\.80f"),
    re.compile(r"0\.65f\s*,\s*0\.78f\s*,\s*0\.92f"),
    re.compile(r"0\.22f\s*,\s*0\.22f\s*,\s*0\.28f"),
)

AMBIENT_SCANNED = (
    "engine/src/middleware/scene/jce_sr_draw.c",
    "engine/src/middleware/scene/jce_scene_renderer.c",
)


def rel(path: pathlib.Path) -> str:
    return path.relative_to(ROOT).as_posix()


def main() -> int:
    violations = []

    # Rule 1: guarded includes only from the allowlist.
    for ext in ("*.c", "*.cpp", "*.h", "*.hpp"):
        for path in (ROOT / "engine").rglob(ext):
            r = rel(path)
            if r in ALLOWED:
                continue
            # The public umbrella is expected to include everything.
            if r.startswith("engine/include/jce/api_"):
                continue
            try:
                lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
            except OSError:
                continue
            for n, line in enumerate(lines, 1):
                if INCLUDE_RE.match(line):
                    hdr = next(h for h in GUARDED_HEADERS if h in line)
                    violations.append(
                        f"{r}:{n}: includes {hdr} directly.\n"
                        f"    Environment lighting has one authority; a new consumer "
                        f"must be added to ALLOWED in this lint deliberately."
                    )

    # Rule 2: no reintroduced sky-ambient literals.
    for r in AMBIENT_SCANNED:
        path = ROOT / r
        if not path.exists():
            continue
        lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
        for n, line in enumerate(lines, 1):
            for pat in BANNED_LITERALS:
                if pat.search(line):
                    violations.append(
                        f"{r}:{n}: hardcoded sky-ambient literal reintroduced.\n"
                        f"    Ambient is projected from the sky (jce_sky_project_sh9); "
                        f"a literal here cannot agree with the sky being drawn."
                    )

    if violations:
        print("check_env_light_authority: FAIL")
        for v in violations:
            print("  " + v)
        return 1

    print("check_env_light_authority: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
