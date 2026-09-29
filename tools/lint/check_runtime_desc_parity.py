#!/usr/bin/env python3
"""
check_runtime_desc_parity.py — the editor's Play path and the shipped game must
hand jce_runtime_create the same JceRuntimeDesc fields.

WHY THIS EXISTS.  There are two places in this repository that build a
JceRuntimeDesc and start a runtime with it:

    engine/include/jce/application/jce_default_main.inc.h   the shipped game
    editor/src/core/jce_editor_play.cpp                     the editor's Play

They are hand-written copies of each other.  Nothing compared them, and on
2026-08-31 eleven of eighteen fields were set on exactly one side:

  * navmesh_path  — Play only.  A shipped game booted with no navmesh, so
    every nav agent had nothing to path on, while the SAME scene pathfound
    correctly the moment a designer pressed Play.
  * saves_dir     — Play only.  SavePoint had nowhere to write in a shipped
    build.
  * fixed_timestep, gravity, gravity2d, solver_iterations, sleep_threshold,
    disable_auto_physics, max_frame_dt — Play only, and all seven come from
    Project Settings.  So a shipped game silently ignored every physics and
    time value the designer tuned: the build that shipped did not simulate
    like the build it was tuned in.  That gap was structural, not a forgotten
    line — JceProjectSettings is an EDITOR type and `grep -rn ProjectSettings
    engine/` finds nothing, so the shipped runtime could not see those values
    at all.
  * locale, locales_dir — shipped only.  A designer cannot preview localised
    game text in Play.

"Works in the editor, broken in the shipped exe" is the failure this project
records more than any other.  This gate turns that class into a build error:
a field set on one side and not the other fails until it is either wired on
both or exempted IN WRITING.

RULE 2 EXISTS BECAUSE OF RULE 1'S FIX.  Closing the physics gap meant the
shipped path reads the FILE the editor writes (.jce/project-settings.json)
rather than the editor type it writes it from.  That is a two-sided contract,
and this repository's own record is that updating one side of a two-sided
contract is worse than updating neither.  So every JSON key the shipped reader
asks for must still be a key the editor's writer emits.

WHAT ABOUT JceRuntimeInput?  Checked on 2026-09-01, and it is SYMMETRIC --
all fourteen fields reach the runtime from both hosts.  Recorded here because
it is the obvious next question and the answer is not visible from one grep:
the two hosts use DIFFERENT mechanisms, so looking for `in.<field> =` in
jce_editor_play.cpp finds only eight of the fourteen and reads like a gap.

  * eight (walk_x, walk_z, jump_pressed, jump_held, sprint, attack_pressed,
    speed_mult, keyboard) are written into a JceRuntimeInput and passed to
    jce_runtime_set_input;
  * the other six (pointer_dx, pointer_dy, pointer_wheel, pointer_buttons,
    touch_count, touches) go through jce_runtime_set_pointer_input and
    jce_runtime_set_touch_input, which jce_editor_play_set_pointer_input and
    _set_touch_input forward to, driven by the game input bridge from
    jce_editor_main.cpp.

So a naive text rule over that struct would report six false positives.  If
one is ever written, it has to know about both paths.

USAGE
    python tools/lint/check_runtime_desc_parity.py           # gate (0/1)
    python tools/lint/check_runtime_desc_parity.py --list    # table, no verdict
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]

HEADER = REPO_ROOT / "engine/include/jce/application/jce_runtime.h"
SHIPPED = REPO_ROOT / "engine/include/jce/application/jce_default_main.inc.h"
PLAY = REPO_ROOT / "editor/src/core/jce_editor_play.cpp"
SETTINGS_WRITER = REPO_ROOT / "editor/src/core/jce_project_settings.cpp"
EXEMPT = LINT_DIR / "runtime_desc_parity_exempt.txt"

# The struct is `typedef struct { ... } JceRuntimeDesc;` — anonymous, so the
# extractor keys off the closing line rather than a tag name.
STRUCT_END = re.compile(r"^\s*\}\s*JceRuntimeDesc\s*;")
STRUCT_BEGIN = re.compile(r"^\s*typedef\s+struct\s*\{")

# `struct` / `union` / `enum` may prefix the type, and a field declared that way
# was INVISIBLE to this gate until 2026-09-01: `struct JceFileSystem *asset_fs;`
# carries two type tokens where the pattern allowed one, so the field sat
# outside the very parity check that exists to catch a one-sided field.  Same
# shape as check_inspector_undo_scope's single-line blind spot: a parser that
# misses a legitimate declaration form reports a clean sheet.
FIELD_PLAIN = re.compile(
    r"^[ \t]*(?:const\s+)?(?:struct\s+|union\s+|enum\s+)?"
    r"[A-Za-z_][A-Za-z0-9_]*\s*\**\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*;", re.M)
FIELD_FPTR = re.compile(r"\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*\(")

# jce_json_get_<kind>(obj, "key", ...) inside the shipped reader.
JSON_KEY = re.compile(r"jce_json_get(?:_[a-z]+)?\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*\"([^\"]+)\"")
JSON_CHILD = re.compile(r"jce_json_get\s*\(\s*root\s*,\s*\"([^\"]+)\"")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def desc_fields() -> list[tuple[str, bool]]:
    lines = HEADER.read_text(encoding="utf-8", errors="replace").splitlines()
    end = next((i for i, l in enumerate(lines) if STRUCT_END.match(l)), -1)
    if end < 0:
        return []
    start = end
    while start > 0 and not STRUCT_BEGIN.match(lines[start]):
        start -= 1
    body = strip_comments("\n".join(lines[start + 1:end]))
    names: list[tuple[str, bool]] = []
    for m in FIELD_PLAIN.finditer(body):
        names.append((m.group(1), bool(m.group(2))))
    for m in FIELD_FPTR.finditer(body):
        names.append((m.group(1), False))
    # Preserve declaration order, drop duplicates.
    seen, out = set(), []
    for n, arr in names:
        if n not in seen:
            seen.add(n)
            out.append((n, arr))
    return out


def assigned(path: Path, field: str, is_array: bool) -> int:
    """Count the times a path WRITES `rd.<field>` / `rd-><field>`.

    Writes only.  A first version counted any mention, and its negative
    control did not go red: deleting `rd.navmesh_path = ...` left the
    neighbouring `LOG_INFO("navmesh: %s", rd.navmesh_path)` behind, the count
    stayed above zero, and the gate reported parity for a field the shipped
    path no longer set.  A gate whose negative control passes is not a gate.

    Three shapes count, because all three are how this codebase fills the
    descriptor:

      1. assignment, through the value or a pointer -- `rd.pak = ...`,
         `rd->fixed_timestep = ...`, `rd.gravity[1] = ...`;
      2. address-of as an out-parameter -- `&rd.something`;
      3. for an ARRAY field, the bare name passed as an argument, which decays
         to a pointer the callee fills -- `jce_json_get_floats(p, "gravity",
         rd->gravity, 3, def)`.  Restricted to array fields so that passing a
         scalar BY VALUE (a read) is not mistaken for a write.

    Everything else -- a read in a log line, a comparison -- does not count.
    """
    text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    f = re.escape(field)
    n = len(re.findall(
        r"\b(?:rd|desc)\s*(?:\.|->)\s*%s\s*(?:\[[^\]]*\])?\s*=(?!=)" % f, text))
    n += len(re.findall(r"&\s*(?:rd|desc)\s*(?:\.|->)\s*%s\b" % f, text))
    if is_array:
        n += len(re.findall(
            r"\b(?:rd|desc)\s*(?:\.|->)\s*%s\s*(?=[,)])" % f, text))
    return n


def read_exemptions() -> dict[str, str]:
    """field -> reason.  A line without a reason is itself a failure."""
    out: dict[str, str] = {}
    if not EXEMPT.is_file():
        return out
    for raw in EXEMPT.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        field, _, reason = line.partition(" ")
        out[field.strip()] = reason.strip()
    return out


def main() -> int:
    for p in (HEADER, SHIPPED, PLAY, SETTINGS_WRITER):
        if not p.is_file():
            print("check_runtime_desc_parity: FAIL - missing %s"
                  % p.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
            return 1

    fields = desc_fields()
    if len(fields) < 5:
        print("check_runtime_desc_parity: FAIL - parsed only %d JceRuntimeDesc "
              "field(s); the struct shape changed and this extractor no longer "
              "sees it" % len(fields), file=sys.stderr)
        return 1

    exempt = read_exemptions()
    want_list = "--list" in sys.argv

    rows, asym, unreasoned = [], [], []
    for f, is_arr in fields:
        s, p = assigned(SHIPPED, f, is_arr), assigned(PLAY, f, is_arr)
        both = (s > 0) == (p > 0)
        rows.append((f, s, p, both, f in exempt))
        if both:
            continue
        if f in exempt:
            if not exempt[f]:
                unreasoned.append(f)
        else:
            asym.append((f, s, p))

    if want_list:
        print("%-24s %6s %6s  %s" % ("field", "shipped", "play", "state"))
        for f, s, p, both, ex in rows:
            state = "ok" if both else ("EXEMPT" if ex else "ASYMMETRIC")
            print("%-24s %6d %6d  %s" % (f, s, p, state))

    # Dead exemptions: a field that no longer exists in the struct.
    field_names = {n for n, _ in fields}
    dead = [f for f in exempt if f not in field_names]

    # Rule 2 — the shipped reader's JSON keys must be keys the editor writes.
    writer = SETTINGS_WRITER.read_text(encoding="utf-8", errors="replace")
    shipped_text = SHIPPED.read_text(encoding="utf-8", errors="replace")
    block = ""
    m = re.search(r"s_default_apply_project_settings\s*\([^)]*\)\s*\{(.*?)\n\}",
                  shipped_text, re.S)
    if m:
        block = m.group(1)
    missing_keys = []
    if block:
        for key in sorted(set(JSON_KEY.findall(block)) | set(JSON_CHILD.findall(block))):
            if ('"%s"' % key) not in writer:
                missing_keys.append(key)

    ok = True
    for f, s, p in asym:
        ok = False
        side = "shipped only" if s else "editor Play only"
        print("  %s: set %s (shipped=%d play=%d).  Wire it on both sides, or "
              "add it to tools/lint/runtime_desc_parity_exempt.txt with a "
              "reason on the same line." % (f, side, s, p), file=sys.stderr)
    for f in unreasoned:
        ok = False
        print("  %s: exempted with no reason.  An exemption without a stated "
              "reason is indistinguishable from an oversight." % f,
              file=sys.stderr)
    for f in dead:
        ok = False
        print("  %s: exempted but no longer a JceRuntimeDesc field.  Drop the "
              "exemption." % f, file=sys.stderr)
    for k in missing_keys:
        ok = False
        print("  project-settings key \"%s\": the shipped reader in %s asks for "
              "it, but %s no longer writes it.  One side of a two-sided "
              "contract moved." % (k, SHIPPED.name, SETTINGS_WRITER.name),
              file=sys.stderr)

    if not ok:
        print("check_runtime_desc_parity: FAIL", file=sys.stderr)
        return 1

    print("check_runtime_desc_parity: OK (%d JceRuntimeDesc field(s); %d "
          "exempted with a reason; project-settings keys agree between the "
          "shipped reader and the editor writer)"
          % (len(fields), len(exempt)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
