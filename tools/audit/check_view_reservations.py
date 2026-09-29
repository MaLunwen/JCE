#!/usr/bin/env python3
"""
check_view_reservations.py -- freeze the bgfx view-id reservations.

WHAT PROBLEM THIS SOLVES
------------------------
bgfx view state is last-write-wins and view ORDER is a global remap, so two
subsystems that reach for the same view id do not conflict loudly -- one of
them silently stops producing pixels.  That is not hypothetical: the editor's
ECS-UI Canvas overlay sat on the scene renderer's dynamic-CSM band, drew every
frame into a target the colour pass then cleared, and produced nothing in
either editor viewport while the shipped runtime rendered it fine.  Nothing
errored and nothing logged.

engine/include/jce/renderer/jce_views.h carries a FROZEN, append-only set of
the base-relative offsets the scene renderer has ever named, and the editor's
viewport offsets are static_asserted against it.  That catches the same-base
case at compile time.

This checker exists for what a compiler cannot see:

  1. A NARROWED range.  The frozen set is a union that must only ever grow --
     shrinking it hands ids the renderer once named back to whoever asks first.
     A static_assert written in terms of the constants it checks cannot catch
     that; a recorded baseline can.
  2. The two editor viewport BASES drifting closer than one base's full claim.
     Cross-BASE, so no assert inside one viewport can reach the other.
  3. A base's claim SWALLOWING A FIXED ABSOLUTE ID.  This is the one that was
     live: the GPU-cull counter reset moved to base+57, and 3+57 is 60, which
     is JCE_VIEW_EDITOR_PREVIEW.  A per-base offset colliding with an absolute
     id is invisible to every per-base rule.
  4. A viewport CONSUMER offset landing inside the frozen set.  The editor
     static_asserts this for its own offsets; recording them here means the
     contract states the same thing where a reviewer reads it, and catches a
     consumer that is not the editor.

FIRST VERSION OF THIS FILE COULD NOT DO 3 OR 4.  It parsed [frozen-offsets] and
[bases] and ignored [viewport-consumer-offsets] and [absolute-bands] entirely,
and its fixed-id rule tested three of the seven fixed ids it records.  Half the
contract was decorative -- which is the failure mode this repo has hit before
and the reason the rules above are enumerated rather than implied.

WHY A CONTRACT FILE AND NOT A UNIT TEST
---------------------------------------
`git ls-files tests` returns ZERO on this branch: tests/ is gitignored, so a
gate that lives there does not exist in a clean clone and must never be cited
as the gate.  contracts/ and tools/audit/ are both tracked, and
run_architecture_audit.py is what a reviewer actually runs.

EXIT CODES
    0  the header agrees with contracts/view-reservations.txt
    1  a violation
    2  usage / parse error
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "engine/include/jce/renderer/jce_views.h"
CONTRACT = ROOT / "contracts/view-reservations.txt"

# Which recorded bases are scene-renderer BASES (they claim a whole span) and
# which are single FIXED ids that nothing may swallow.
SPAN_BASES = ("editor-scene-view", "editor-game-view", "shipped-runtime")


def die(msg):
    print("check_view_reservations: ERROR " + msg)
    sys.exit(2)


def read_header():
    if not HEADER.is_file():
        die("missing " + str(HEADER))
    src = HEADER.read_text(encoding="utf-8")

    def macro(name):
        m = re.search(r"^#define\s+" + re.escape(name) + r"\s+(\d+)u?\s*$",
                      src, re.M)
        return int(m.group(1)) if m else None

    count = macro("JCE_VIEW_SR_FROZEN_RANGE_COUNT")
    if count is None:
        die("JCE_VIEW_SR_FROZEN_RANGE_COUNT not found in jce_views.h")

    ranges = []
    for i in range(count):
        lo = macro("JCE_VIEW_SR_FROZEN_%d_LO" % i)
        hi = macro("JCE_VIEW_SR_FROZEN_%d_HI" % i)
        if lo is None or hi is None:
            die("JCE_VIEW_SR_FROZEN_%d_LO/_HI missing while the range count "
                "says %d" % (i, count))
        if hi < lo:
            die("frozen range %d is inverted: %d..%d" % (i, lo, hi))
        ranges.append((lo, hi))

    # The LIVE bands -- what the builder NAMES right now.  Distinct from the
    # frozen union above on purpose: frozen says "a foreign pass may never be
    # here", live says "the renderer is here today".  Rule 3 below is about the
    # live set; applying it to the frozen union would flag every offset a band
    # has vacated, which is the opposite of what append-only means.
    live = []
    core = macro("JCE_VIEW_SR_CORE_SPAN")
    if core: live.append((0, core - 1))
    dlo, dn = macro("JCE_VIEW_SR_DYN_CSM_OFFSET"), macro("JCE_VIEW_SR_DYN_CSM_COUNT")
    if dlo is not None and dn: live.append((dlo, dlo + dn - 1))
    plo, pn = macro("JCE_VIEW_SR_POINT_CUBE_OFFSET"), macro("JCE_VIEW_SR_POINT_CUBE_COUNT")
    if plo is not None and pn: live.append((plo, plo + pn - 1))
    r = macro("JCE_VIEW_SR_GPU_CULL_RESET_OFFSET")
    if r is not None: live.append((r, r))
    # SSGI: the march and its composite.  THIS LIST IS HAND-MAINTAINED, which
    # means a band added to jce_views.h and not added here is a band this
    # checker does not know exists -- the same "coverage hardcoded into the
    # checker" shape the audit that produced this file was about.  Adding a
    # JCE_VIEW_SR_* offset without a line here is the mistake to watch for.
    g = macro("JCE_VIEW_SR_SSGI_OFFSET")
    gc = macro("JCE_VIEW_SR_SSGI_COMPOSITE_OFFSET")
    if g is not None and gc is not None: live.append((min(g, gc), max(g, gc)))
    # Camera-stack overlays: one view per overlay camera.
    o, om = macro("JCE_VIEW_SR_CAMERA_OVERLAY_OFFSET"), macro("JCE_VIEW_SR_CAMERA_OVERLAY_MAX")
    if o is not None and om: live.append((o, o + om - 1))

    bases = {}
    for name, key in (("JCE_VIEW_EDITOR_SCENE", "editor-scene-view"),
                      ("JCE_VIEW_RUNTIME_GAME", "shipped-runtime"),
                      ("JCE_VIEW_EDITOR_GAME", "editor-game-view"),
                      ("JCE_VIEW_EDITOR_OVERLAY", "editor-overlay"),
                      ("JCE_VIEW_EDITOR_PREVIEW", "editor-material-preview"),
                      ("JCE_VIEW_EDITOR_PICK", "editor-pick"),
                      ("JCE_VIEW_EDITOR_PICK_READBACK", "editor-pick-readback"),
                      ("JCE_VIEW_IMGUI", "imgui"),
                      ("JCE_VIEW_OCCLUSION", "occlusion-proxy"),
                      ("JCE_VIEW_UI", "ui-overlay")):
        v = macro(name)
        if v is not None:
            bases[key] = v
    return ranges, bases, live


def read_contract():
    if not CONTRACT.is_file():
        die("missing " + str(CONTRACT) + " (run with --update to create it)")
    ranges, bases, consumers, absolute = [], {}, {}, []
    section = None
    for raw in CONTRACT.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            continue
        if section == "frozen-offsets":
            m = re.match(r"^(\d+)-(\d+)\b", line)
            if m:
                ranges.append((int(m.group(1)), int(m.group(2))))
        elif section == "bases":
            m = re.match(r"^(\d+)\s+(\S+)", line)
            if m:
                bases[m.group(2)] = int(m.group(1))
        elif section == "viewport-consumer-offsets":
            m = re.match(r"^(\d+)\s+(\S+)", line)
            if m:
                consumers[m.group(2)] = int(m.group(1))
        elif section == "absolute-bands":
            m = re.match(r"^(\d+)-(\d+)\s+(\S+)", line)
            if m:
                absolute.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    return ranges, bases, consumers, absolute


def render_contract(ranges, bases):
    """Rewrite only the two generated sections, keeping the prose AND keeping
    the comments ABOVE their data (the first version emitted data straight
    after the section header, which pushed each section's explanation below
    the rows it explains).

    IT ALSO KEEPS THE PER-ROW ANNOTATIONS.  A base row is
    `<id> <name> <free prose>`, and the prose is where this file records why a
    band may be shared, reclaimed or overlapped -- the header demands that
    reclaiming 'has to be argued for', and an --update that deleted the
    argument every time made that impossible to obey.  Measured: one run
    erased the editor-scene-view note about the modal probe capture."""
    lines = (CONTRACT.read_text(encoding="utf-8").splitlines()
             if CONTRACT.is_file() else [])

    # id -> whatever followed the name on the existing row.
    notes = {}
    sect = None
    for raw in lines:
        st = raw.strip()
        if st.startswith("[") and st.endswith("]"):
            sect = st[1:-1]
            continue
        if sect != "bases" or not st or st.startswith("#"):
            continue
        parts = st.split(None, 2)
        if len(parts) == 3 and parts[0].isdigit():
            notes[int(parts[0])] = parts[2]

    out, section, pending = [], None, None
    for raw in lines:
        stripped = raw.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if pending:
                out.extend(pending)
                pending = None
            section = stripped[1:-1]
            out.append(raw)
            if section == "frozen-offsets":
                pending = ["%d-%d" % (lo, hi) for lo, hi in ranges]
            elif section == "bases":
                pending = []
                for k, v in sorted(bases.items(), key=lambda kv: kv[1]):
                    note = notes.get(v)
                    # Pad to the column the file already used, so re-recording
                    # an unchanged contract produces an EMPTY diff -- a
                    # generator whose output churns is one whose real changes
                    # are hard to see.
                    row = ("%-5d%-31s%s" % (v, k, note)).rstrip() if note                           else "%-5d%s" % (v, k)
                    pending.append(row)
            continue
        if pending is not None and section in ("frozen-offsets", "bases"):
            if stripped.startswith("#") or not stripped:
                out.append(raw)          # keep the explanation above the data
                continue
            continue                     # drop the old data rows
        out.append(raw)
    if pending:
        out.extend(pending)
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true",
                    help="record the header's current ranges/bases into the "
                         "contract.  Read the diff before committing.")
    args = ap.parse_args()

    ranges, bases, live = read_header()

    if args.update:
        CONTRACT.write_text(render_contract(ranges, bases), encoding="utf-8",
                            newline="\n")
        print("check_view_reservations: recorded %d frozen range(s), %d base(s)"
              " -> %s" % (len(ranges), len(bases),
                          CONTRACT.relative_to(ROOT)))
        return 0

    want_ranges, want_bases, consumers, absolute = read_contract()
    top = max((hi for _lo, hi in ranges), default=0)
    reserved = lambda o: any(lo <= o <= hi for lo, hi in ranges)
    fail = []

    # RULE 1 -- the frozen set may only grow.
    for lo, hi in want_ranges:
        if not any(a <= lo and hi <= b for a, b in ranges):
            fail.append(
                "frozen range %d-%d recorded in the contract is no longer "
                "covered by jce_views.h.  The frozen set is a UNION that may "
                "only grow: narrowing it hands ids the scene renderer has "
                "named back to whoever takes them first." % (lo, hi))

    # RULE 2 -- the two editor viewport bases must be a full claim apart.
    a, b = bases.get("editor-scene-view"), bases.get("editor-game-view")
    if a is not None and b is not None and abs(b - a) <= top:
        fail.append(
            "the editor viewport bases are %d apart but one base CLAIMS up to "
            "offset %d, so the lower viewport's bands reach into the upper "
            "one's range.  Cross-base; no static_assert can see it."
            % (abs(b - a), top))

    # RULE 3 -- no offset the renderer NAMES TODAY may land on a fixed absolute
    # id.  Tested against the LIVE bands, not the frozen union: a frozen range
    # a band has vacated is exactly what append-only is for, and flagging it
    # would make the rule fire on its own success.  This is the rule that was
    # live: the
    # GPU-cull reset at base+57 was absolute 60 == JCE_VIEW_EDITOR_PREVIEW.
    fixed = {k: v for k, v in bases.items() if k not in SPAN_BASES}
    for bname in SPAN_BASES:
        bv = bases.get(bname)
        if bv is None:
            continue
        for fname, fv in fixed.items():
            if not (bv <= fv <= bv + top):
                continue
            off = fv - bv
            if any(lo <= off <= hi for lo, hi in live):
                fail.append(
                    "%s (base %d) NAMES offset %d, which is absolute id %d "
                    "(%s).  A base-relative offset landing on a fixed id is "
                    "invisible to every per-base rule -- it is exactly how "
                    "base+57 became JCE_VIEW_EDITOR_PREVIEW."
                    % (bname, bv, off, fv, fname))

    # RULE 4 -- a viewport consumer offset must not be inside the frozen set.
    for cname, off in consumers.items():
        if reserved(off):
            fail.append(
                "viewport consumer offset %d (%s) is inside the frozen "
                "reservation set, so that pass would inherit a scene-renderer "
                "sort position and be erased by the colour pass's clear."
                % (off, cname))

    # RULE 5 -- an absolute band must not overlap a base's claim unless the
    # contract row says why (the impostor bake's row records its modality).
    for lo, hi, name in absolute:
        if name.upper() == "RETIRED":
            continue
        for bname in SPAN_BASES:
            bv = bases.get(bname)
            if bv is None:
                continue
            if hi < bv or lo > bv + top:
                continue
            row = next((r for r in CONTRACT.read_text(encoding="utf-8")
                        .splitlines()
                        if r.strip().startswith("%d-%d" % (lo, hi))), "")
            # MODAL: the two never run in the same frame, and the row says how
            # that is enforced.  ALIAS: the band is not a second owner at all,
            # it is another NAME for ids the base already owns.  Either way the
            # row has to say which, so the argument is in the file rather than
            # in somebody's head.
            if "MODAL" not in row and "ALIAS" not in row:
                fail.append(
                    "absolute band %d-%d (%s) overlaps %s's claim (%d..%d) and "
                    "its contract row does not say how the two are kept apart. "
                    "Record the mechanism (MODAL or ALIAS, with the reason) or move "
                    "the band."
                    % (lo, hi, name, bname, bv, bv + top))

    if fail:
        for f in fail:
            print("check_view_reservations: FAIL - " + f)
        print("check_view_reservations: if the change is intentional, re-run "
              "with --update and commit the contract in the SAME commit as the "
              "header change.")
        return 1

    print("check_view_reservations: OK - %d frozen range(s), %d base(s), "
          "%d consumer offset(s), %d absolute band(s), %d live band(s); "
          "widest claim %d offsets"
          % (len(ranges), len(bases), len(consumers), len(absolute),
             len(live), top + 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
