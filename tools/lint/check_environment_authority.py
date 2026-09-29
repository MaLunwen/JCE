#!/usr/bin/env python3
"""
check_environment_authority.py — the scene's environment is advanced by the
SCENE, and by exactly one place.

WHY THIS EXISTS.  JceEnvironmentState is scene-owned on purpose: its header says
"on the SCENE and not on the renderer, so that physics and gameplay can read the
same wind the renderer draws with", and it says that because the renderer and
the runtime once built the same water field from two different winds.

The STATE moved.  The DRIVER did not.  Until 2026-09-01 jce_environment_advance
had exactly one caller in the whole repository and it was inside the scene
RENDERER (sr_advance_environment_state).  A headless build creates no renderer
-- the engine logs "HEADLESS boot: no window, no GPU device, no audio/UI", and
that is the shipped dedicated-server mode -- so on a server:

  * world_time_seconds never advanced, and the gust envelope is a pure function
    of it, so wind_speed_now() was pinned to the sustained speed forever;
  * global_wetness and snow_amount are INTEGRATORS, so ground never got wet and
    snow never settled or melted;
  * nothing placed the sun either -- advance() moves day_fraction and does not
    touch sun_direction_ws, whose only writer was also the renderer -- so
    jce_environment_is_daytime(), the predicate gameplay asks "is it night?"
    with, returned TRUE forever, because the default sun is above the horizon.

Scene-owned state advanced only by the renderer is the same defect the state
move was made to fix, one level down.  It is invisible in an editor and in a
windowed game, which is why it lasted: every configuration a developer looks at
has a renderer in it.

WHAT THIS CHECKS.

  RULE 1  jce_environment_advance() and the jce_environment_apply_*() writers
          are called from ONE file: engine/src/middleware/scene/
          jce_scene_environment.c.  Not from the renderer, not from the editor,
          not from the runtime.  One driver is the property; "the scene layer"
          would still allow two.

  RULE 2  jce_scene_update() calls jce_scene_environment_advance().  This is the
          headless driver: without it a dedicated server is back to a frozen
          world and nothing else in the build would notice.

  RULE 3  The editor calls jce_scene_environment_advance() too.  Edit mode runs
          no simulation, so jce_scene_update() never runs there; drop this call
          and the sky freezes for anyone authoring a day/night cycle -- the
          failure the editor's own private clock existed to paper over.

WHAT THIS DOES NOT CHECK.  That the advance is called with a sane dt, or once
per frame rather than twice.  A text scan sees call sites, not schedules.

Usage:  python tools/lint/check_environment_authority.py
Exit 0 clean, 1 on any finding.
"""

import re
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]

DRIVER = REPO_ROOT / "engine" / "src" / "middleware" / "scene" / "jce_scene_environment.c"
SCENE_UPDATE = REPO_ROOT / "engine" / "src" / "middleware" / "scene" / "jce_scene.c"
EDITOR_DIR = REPO_ROOT / "editor" / "src"

# The mutators.  Readers (jce_environment_wind_speed_now, _is_daytime, ...) are
# deliberately absent: anyone may READ the environment, that is the point of it.
MUTATORS = re.compile(r"\bjce_environment_(?:advance|apply_weather_\w+)\s*\(")
ADVANCE = re.compile(r"\bjce_scene_environment_advance\s*\(")

# Where a call is allowed to appear at all.  The environment's own module is
# where the functions are DEFINED; tests exercise them directly by design.
ALLOWED_PREFIXES = (
    "engine/src/middleware/world/",
    "tests/",
)


def strip_comments(text: str) -> str:
    """Blank out // and /* */ so a function NAMED in prose is not a finding.

    Every one of these functions is discussed at length in the comments that
    explain the design, including in the driver's own header block.  A gate
    that fires on the sentence explaining it teaches people to delete the
    explanation."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


# Vendored trees are not ours and are enormous: engine/src/middleware/audio and
# /video carry dav1d, fdk-aac and libvpx.  Walking them cost this gate hundreds
# of megabytes of char-by-char comment stripping and made it CRASH
# intermittently (Windows 0xC0000409) -- 39/39 reported green while the process
# exited 1.  A gate that fails at random is worse than no gate: it teaches
# people to re-run until it passes.
SKIP_DIRS = ("third_party", "external", "vendor", "3rdparty")


def sources(root: Path):
    for pat in ("**/*.c", "**/*.cpp", "**/*.h", "**/*.inc.h"):
        for p in root.glob(pat):
            if any(d in p.parts for d in SKIP_DIRS):
                continue
            yield p


def main() -> int:
    for required in (DRIVER, SCENE_UPDATE):
        if not required.is_file():
            print("check_environment_authority: FAIL - %s not found"
                  % required.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
            return 1

    findings = []

    # ── RULE 1: one driver ────────────────────────────────────────────
    driver_rel = DRIVER.relative_to(REPO_ROOT).as_posix()
    mutator_calls = 0
    roots = [REPO_ROOT / "engine", REPO_ROOT / "editor", REPO_ROOT / "tools"]
    for root in roots:
        if not root.is_dir():
            continue
        for src in sources(root):
            rel = src.relative_to(REPO_ROOT).as_posix()
            if rel == driver_rel or rel.startswith(ALLOWED_PREFIXES):
                continue
            # A public header DECLARES these; it does not call them.  The one
            # exception is *.inc.h, which is a translation unit shipped as a
            # header (jce_default_main.inc.h) and does contain calls.
            if rel.startswith("engine/include/") and rel.endswith(".h") \
                    and not rel.endswith(".inc.h"):
                continue
            raw = src.read_text(encoding="utf-8", errors="replace")
            # Cheap reject before the expensive strip: a file that never names
            # the prefix cannot contain a call, and this is the difference
            # between stripping comments out of a handful of files and out of
            # the whole engine.
            if "jce_environment_" not in raw:
                continue
            code = strip_comments(raw)
            for m in MUTATORS.finditer(code):
                line = code.count("\n", 0, m.start()) + 1
                findings.append((rel, line, "rule 1",
                                 "%s outside %s -- the environment must have "
                                 "ONE driver, and a renderer is not it (a "
                                 "headless build has no renderer)"
                                 % (m.group(0).rstrip("("), driver_rel)))
    mutator_calls = len(MUTATORS.findall(
        strip_comments(DRIVER.read_text(encoding="utf-8", errors="replace"))))
    if mutator_calls == 0:
        findings.append((driver_rel, 0, "rule 1",
                         "the designated driver calls no environment mutator "
                         "at all -- the advance has gone somewhere else, or "
                         "nowhere"))

    # ── RULE 2: jce_scene_update drives it (the headless path) ────────
    code = strip_comments(SCENE_UPDATE.read_text(encoding="utf-8", errors="replace"))
    i = code.find("void jce_scene_update(")
    if i < 0:
        findings.append((SCENE_UPDATE.relative_to(REPO_ROOT).as_posix(), 0,
                         "rule 2", "jce_scene_update() not found"))
    else:
        # body = to the next line-start '}' , which is how this file formats
        j = code.find("\n}\n", i)
        body = code[i:j if j > 0 else len(code)]
        if not ADVANCE.search(body):
            findings.append((SCENE_UPDATE.relative_to(REPO_ROOT).as_posix(),
                             code.count("\n", 0, i) + 1, "rule 2",
                             "jce_scene_update() does not call "
                             "jce_scene_environment_advance() -- a dedicated "
                             "server's clock, wetness, snow and sun stop, and "
                             "nothing with a renderer would show it"))

    # ── RULE 3: the editor drives it in edit mode ─────────────────────
    editor_calls = 0
    if EDITOR_DIR.is_dir():
        for src in sources(EDITOR_DIR):
            raw = src.read_text(encoding="utf-8", errors="replace")
            if "jce_scene_environment_advance" not in raw:
                continue
            editor_calls += len(ADVANCE.findall(strip_comments(raw)))
    if editor_calls == 0:
        findings.append(("editor/src", 0, "rule 3",
                         "no editor call to jce_scene_environment_advance() -- "
                         "edit mode runs no simulation, so the authoring "
                         "preview's sky freezes"))

    if findings:
        for rel, line, rule, why in findings:
            where = "%s:%d" % (rel, line) if line else rel
            print("  %s  [%s] %s" % (where, rule, why), file=sys.stderr)
        print("check_environment_authority: FAIL - %d finding(s).  The scene "
              "owns its environment; exactly one place may advance it, and both "
              "the runtime and the editor must." % len(findings),
              file=sys.stderr)
        return 1

    print("check_environment_authority: OK (%d environment mutator call(s), all "
          "in %s; jce_scene_update drives it for the runtime and headless; %d "
          "editor call(s) drive it in edit mode)"
          % (mutator_calls, driver_rel, editor_calls))
    return 0


if __name__ == "__main__":
    sys.exit(main())
