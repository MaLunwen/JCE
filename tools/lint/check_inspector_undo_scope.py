#!/usr/bin/env python3
"""
check_inspector_undo_scope.py — insp_track_edit() must not sit inside a
one-frame ImGui predicate.

WHY THIS EXISTS.  The editor's Inspector does not edit through mutators: the
widgets write straight into engine component memory (`ImGui::DragFloat(lbl,
&rb->mass, ...)`), and undo is bolted on beside them by insp_track_edit()
(editor/src/panels/jce_panel_inspector_common.cpp:22-32):

    if (ImGui::IsItemActivated()   && !s_insp_batch_open) { begin_batch_edit(); ... }
    if (ImGui::IsItemDeactivated() &&  s_insp_batch_open) { end_batch_edit();   ... }

Both halves key on the *item*, not on the return value, so the call has to run
on EVERY frame the widget is drawn.  Put it inside the widget's own `if` body
and it runs only on the frame the predicate is true — which for a value-changed
predicate is neither the activation frame nor the deactivation frame:

    if (ImGui::Combo(lbl, &layer, names, N)) {   // true only when the value CHANGES
        rb->physics_layer = (uint32_t)layer;     // ...but the popup opened, and will
        insp_track_edit();                       //    close, on OTHER frames
    }

Measured consequence (2026-08-31, jce_panel_inspector_physics.cpp:72-75): a
combo change is neither pushed to the undo stack nor marks s.scene_modified, so
the edit is invisible to Ctrl+Z AND to the dirty flag.  The fields behind these
predicates are exactly the discrete ones a designer sets once and trusts:
physics layer, CCD mode, collider mode, clear mode, probe count, volume shape.

THE FIX IS NOT TO DELETE THE CALL.  Hoisting it out of the `if` — keeping the
assignment inside — makes both halves run every frame, and the ImGui item
semantics then line up: for a Combo, IsItemActivated fires when the popup opens
(before any change, so the snapshot is the pre-edit state) and IsItemDeactivated
when it closes.  That shape is ALREADY the idiom at four sites in this tree,
e.g. editor/src/panels/jce_panel_inspector_network.cpp:150-156.  This checker
makes the tree converge on it.

WHY A SCOPE ALLOW-LIST AND NOT A "ONE-FRAME" DENY-LIST.  A first pass that
flagged every tracker inside any `if (ImGui::...)` block reported 69 sites; an
external audit of the same code reported 99.  Both were wrong, and wrong the
same way: they counted section predicates.  ImGui::TreeNodeEx / CollapsingHeader
return true for as long as the section is OPEN, so a tracker inside one runs
every frame and is correct — and that is the ordinary way this file is written,
so a rule built from "is it inside an if?" mostly reports correct code.  The
allow-list below therefore names the multi-frame predicates — a small closed
set — and everything else defaults to a finding.  Default-deny costs nothing
today (no predicate in the tree falls outside both lists) and it is the
direction that fails safe when ImGui grows a new one-frame widget.

USAGE
    python tools/lint/check_inspector_undo_scope.py            # gate (0/1)
    python tools/lint/check_inspector_undo_scope.py --list     # findings only
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]
PANEL_DIR = REPO_ROOT / "editor" / "src" / "panels"
BASELINE = LINT_DIR / "inspector_undo_baseline.json"

TRACKER = re.compile(r"\binsp_track_edit\s*\(")

# `if (` / `} else if (` whose condition mentions an ImGui call.
IF_IMGUI = re.compile(r"\b(?:else\s+)?if\s*\(")
IMGUI_CALL = re.compile(r"\bImGui::(\w+)\s*\(")

# Predicates that stay true for as long as a section is open, so a tracker
# inside their body still runs on the activation and deactivation frames.
SCOPE_PREDICATES = {
    "TreeNode", "TreeNodeEx", "TreeNodeV", "CollapsingHeader",
    "BeginTable", "BeginChild", "BeginCombo", "BeginListBox",
    "BeginMenu", "BeginMenuBar", "BeginMainMenuBar",
    "BeginPopup", "BeginPopupModal", "BeginPopupContextItem",
    "BeginPopupContextWindow", "BeginPopupContextVoid",
    "BeginTabBar", "BeginTabItem", "BeginTooltip", "BeginItemTooltip",
    "BeginDragDropSource", "BeginDragDropTarget",
    "Begin",
}

# The two findings need DIFFERENT fixes, so the gate names which one applies.
#
# CONTINUOUS — the widget is held across many frames (a drag, a colour pick, a
#   text field).  IsItemActivated fires before the first change and
#   IsItemDeactivated after the last, so hoisting insp_track_edit() out of the
#   `if` body makes one batch span the whole interaction.  Leave the assignment
#   inside.  Shape: editor/src/panels/jce_panel_inspector_network.cpp:150-156.
#
# DISCRETE — the value changes once, in a frame that is neither the activation
#   nor the deactivation frame of the item under the cursor (a combo changes
#   inside its popup; a button's press and release are different frames).
#   Hoisting does NOT help.  Use the temporarily-revert / snapshot / re-apply
#   idiom that insp_undo_bool() and insp_undo_int() already implement
#   (editor/src/panels/jce_panel_inspector_common.cpp:65-81, 122 call sites).
CONTINUOUS_PREDICATES = {
    "DragFloat", "DragFloat2", "DragFloat3", "DragFloat4",
    "DragInt", "DragInt2", "DragInt3", "DragInt4",
    "DragScalar", "DragScalarN", "DragFloatRange2", "DragIntRange2",
    "SliderFloat", "SliderFloat2", "SliderFloat3", "SliderFloat4",
    "SliderInt", "SliderInt2", "SliderInt3", "SliderInt4",
    "SliderAngle", "SliderScalar", "SliderScalarN",
    "VSliderFloat", "VSliderInt", "VSliderScalar",
    "InputFloat", "InputFloat2", "InputFloat3", "InputFloat4",
    "InputInt", "InputInt2", "InputInt3", "InputInt4",
    "InputDouble", "InputScalar", "InputScalarN",
    "InputText", "InputTextMultiline", "InputTextWithHint",
    "ColorEdit3", "ColorEdit4", "ColorPicker3", "ColorPicker4",
}


def strip_comments(text: str) -> list[str]:
    """Source lines with every comment body blanked, line structure kept.

    A checker that greps raw lines cannot tell a CALL from a MENTION of one,
    and it gets both directions wrong.  A comment naming insp_track_edit()
    inside a one-frame predicate was reported as a misplaced call -- which is
    how this was noticed -- and, far worse, a `/*` opened on an earlier line
    hid every real call below it, so the exact defect this gate exists to
    catch could be commented past.

    Comment characters become spaces rather than disappearing, so line numbers
    and column-ish context still point at the source.  String and character
    literals are preserved verbatim: "http://" and "/*" occur inside them and
    are not comments.
    """
    out: list[str] = []
    i, n = 0, len(text)
    quote = ""          # '"' or "'" while inside a literal
    block = False       # inside /* ... */
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if block:
            if ch == "*" and nxt == "/":
                out.append("  ")
                i += 2
                block = False
                continue
            out.append(ch if ch == "\n" else " ")
            i += 1
            continue
        if quote:
            out.append(ch)
            if ch == "\\" and nxt:
                out.append(nxt)
                i += 2
                continue
            if ch == quote:
                quote = ""
            i += 1
            continue
        if ch == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if ch == "/" and nxt == "*":
            block = True
            out.append("  ")
            i += 2
            continue
        if ch in ('"', "'"):
            quote = ch
        out.append(ch)
        i += 1
    return "".join(out).splitlines()


def classify(cond: str) -> str | None:
    """Return None when the condition opens a multi-frame scope, else the name
    of the one-frame ImGui predicate that makes a nested tracker unreachable."""
    names = IMGUI_CALL.findall(cond)
    if not names:
        return None
    one_frame = [n for n in names if n not in SCOPE_PREDICATES]
    return one_frame[0] if one_frame else None


def scan(path: Path) -> list[tuple[int, str, str]]:
    """[(line_no, predicate, source_line)] for trackers under a one-frame if.

    An `if` condition may wrap across lines and its body may be braced on a
    later line, unbraced, or opened and closed on the same line.  So the
    condition is collected as a statement rather than matched per line: start
    at `if (`, keep appending until the brace depth rises (a block opened) or
    a `;` closes an unbraced body.
    """
    lines = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    findings: list[tuple[int, str, str]] = []

    depth = 0
    collecting: str | None = None      # condition text of an `if` not yet resolved
    stack: list[tuple[int, str]] = []   # (brace depth, one-frame predicate) still open

    for n, line in enumerate(lines, 1):
        if collecting is None and IF_IMGUI.search(line):
            collecting = line
        elif collecting is not None:
            collecting += " " + line

        before = depth
        opened_here = False
        for ch in line:
            if ch == "{":
                depth += 1
                opened_here = True
            elif ch == "}":
                stack = [f for f in stack if f[0] < depth]
                depth -= 1

        # A tracker on this line is judged against the blocks open around it,
        # plus an unbraced single-statement `if` on the very same line.
        if TRACKER.search(line):
            pred = stack[-1][1] if stack else None
            # `collecting` is still open, so the `if` this line belongs to has
            # not been resolved into `stack` yet.  That covers the unbraced
            # single-statement body AND the whole-statement-on-one-line form
            #     if (ImGui::Combo(...)) { c->x = m; insp_track_edit(); }
            # whose braces open and close before the push happens -- it read as
            # "no enclosing block" and slipped the gate until 2026-08-31.
            if pred is None and collecting is not None:
                pred = classify(collecting)
            if pred:
                findings.append((n, pred, line.strip()))

        if collecting is not None:
            if opened_here and depth > before:
                pred = classify(collecting)
                if pred:
                    stack.append((depth, pred))
                collecting = None
            elif ";" in line or opened_here:
                # Unbraced body, or a block opened and closed on one line:
                # either way the `if` statement is finished with this line.
                collecting = None

    return findings


LOOP_HEAD = re.compile(r"^\s*(?:for|while)\s*\(")
BARE_TRACKER = re.compile(r"^\s*insp_track_edit\s*\(\s*\)\s*;\s*$")


def scan_orphans(path: Path) -> list[tuple[int, str, str]]:
    """[(line_no, reason, source_line)] for trackers that watch nothing.

    RULE 1 of this checker asks whether a tracker sits under the WRONG
    predicate.  This one asks whether it is next to a widget at all.

    insp_track_edit() polls ImGui's *last item*.  Dropped somewhere that draws
    nothing, it silently polls whichever widget happened to be drawn last --
    once per iteration if it landed in a loop -- and can open a batch that the
    real owner then never closes.  Nothing catches that: it compiles, and
    RULE 1 sees no predicate to complain about.

    Measured (2026-08-31): the batch converter that fixed 95 call sites also
    dropped three consecutive trackers into a read-only snprintf loop in
    jce_panel_inspector_physics.cpp -- a pure insertion, no line replaced, so
    the diff looked like every other hunk in that commit.  The compiler was
    happy and RULE 1 was happy; it was found by reading.  Hence these two
    shapes, both of which have no legitimate use:

      (a) two or more consecutive identical trackers -- one widget, one call;
      (b) a tracker in a `for`/`while` body that contains no ImGui call at all.
    """
    lines = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    findings: list[tuple[int, str, str]] = []

    prev_tracker_line = -2
    # (brace depth at which the loop body opened, saw an ImGui call yet)
    loops: list[list] = []
    depth = 0

    for n, line in enumerate(lines, 1):
        is_loop_head = LOOP_HEAD.match(line) is not None
        if "ImGui::" in line:
            for fr in loops:
                fr[1] = True

        if TRACKER.search(line):
            # A line carrying BOTH a widget and a tracker is the normal idiom
            # and may repeat freely; only a run of BARE trackers is meaningless.
            bare = BARE_TRACKER.match(line) is not None
            if bare and n == prev_tracker_line + 1 and BARE_TRACKER.match(lines[n - 2]):
                findings.append((n, "consecutive duplicate", line.strip()))
            elif loops and not loops[-1][1]:
                findings.append((n, "loop body draws nothing", line.strip()))
            prev_tracker_line = n

        for ch in line:
            if ch == "{":
                depth += 1
                if is_loop_head:
                    loops.append([depth, False])
                    is_loop_head = False
            elif ch == "}":
                depth -= 1
                loops = [fr for fr in loops if fr[0] <= depth]

    return findings


def self_test() -> None:
    """Run every time.  A gate that cannot fail is indistinguishable from no
    gate, and this one was BOTH blind and over-eager before comment stripping:
    it flagged a comment, and it would have missed a real call hidden under an
    unterminated block comment.  Both directions are asserted here, with the
    positive control first -- if stripping ever eats code, case 2 goes green
    and says so."""
    import tempfile

    def findings_for(src: str) -> list[tuple[int, str, str]]:
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "t.cpp"
            f.write_text(src, encoding="utf-8", newline=chr(10))
            return scan(f)

    # 1. a real call inside a one-frame predicate is still caught
    real = ("void f() {" + chr(10) +
            "    if (ImGui::SmallButton(\"x\")) {" + chr(10) +
            "        insp_track_edit();" + chr(10) +
            "    }" + chr(10) + "}" + chr(10))
    assert findings_for(real), "self-test: a real misplaced call went unreported"

    # 2. the same text in a comment is not a call site
    commented = ("void f() {" + chr(10) +
                 "    if (ImGui::SmallButton(\"x\")) {" + chr(10) +
                 "        /* use insp_track_edit() here */" + chr(10) +
                 "        x = 0;" + chr(10) +
                 "    }" + chr(10) + "}" + chr(10))
    assert not findings_for(commented), "self-test: a comment counted as a call"

    # 3. a // inside a string literal does not blank the rest of the line
    in_string = ("void f() {" + chr(10) +
                 "    const char *u = \"http://x\"; if (ImGui::SmallButton(u)) {"
                 + chr(10) +
                 "        insp_track_edit();" + chr(10) +
                 "    }" + chr(10) + "}" + chr(10))
    assert findings_for(in_string), "self-test: a string literal was read as a comment"


def main() -> int:
    self_test()
    if not PANEL_DIR.is_dir():
        print("check_inspector_undo_scope: FAIL - %s not found" % PANEL_DIR,
              file=sys.stderr)
        return 1

    sources = sorted(PANEL_DIR.glob("*.cpp"))
    if not sources:
        print("check_inspector_undo_scope: FAIL - no panel sources found",
              file=sys.stderr)
        return 1

    total_trackers = 0
    all_findings: list[tuple[Path, int, str, str]] = []
    orphans: list[tuple[Path, int, str, str]] = []
    for src in sources:
        text = src.read_text(encoding="utf-8", errors="replace")
        # Mentions in comments are not call sites.  This number is quoted in
        # the gate's own output and in commit messages, so it counts code.
        total_trackers += len(TRACKER.findall(chr(10).join(strip_comments(text))))
        for n, pred, line in scan(src):
            all_findings.append((src, n, pred, line))
        for n, why, line in scan_orphans(src):
            orphans.append((src, n, why, line))

    # RATCHET, not a cap -- same reason check_file_size.py is one.  Seventy-seven
    # call sites were already misplaced when this gate was written, so a plain
    # "fail on any finding" gate would be red on the day it landed and would be
    # switched off by the first person it inconvenienced.  Instead every file
    # carries a budget that may only ever go DOWN, and a file with no budget may
    # have no findings at all.  That fails the next misplaced call immediately
    # while staying green today.
    per_file: dict[str, int] = {}
    for src, _, _, _ in all_findings:
        rel = src.relative_to(REPO_ROOT).as_posix()
        per_file[rel] = per_file.get(rel, 0) + 1

    if "--update-baseline" in sys.argv:
        BASELINE.write_text(
            json.dumps({
                "_note": "Per-file budgets for insp_track_edit() calls gated "
                         "by a one-frame ImGui predicate, where the call runs "
                         "on neither the activation nor the deactivation frame "
                         "and is therefore a no-op.  This map is EMPTY as of "
                         "2026-08-31: every such call was fixed, so the gate is "
                         "now absolute and any new one fails it.  Do not write a "
                         "count into prose here -- the machine number is the one "
                         "the checker prints.  Regenerate with --update-baseline "
                         "and say in the commit message why a budget came back.",
                "files": dict(sorted(per_file.items())),
            }, indent=2, ensure_ascii=False) + chr(10),
            encoding="utf-8", newline=chr(10))
        print("check_inspector_undo_scope: baseline written (%d file(s), %d "
              "finding(s))" % (len(per_file), len(all_findings)))
        return 0

    baseline: dict[str, int] = {}
    if BASELINE.is_file():
        baseline = json.loads(BASELINE.read_text(encoding="utf-8")).get("files", {})

    regressions = []
    for rel, n in sorted(per_file.items()):
        budget = baseline.get(rel, 0)
        if n > budget:
            regressions.append((rel, n, budget))

    want_list = "--list" in sys.argv
    # Quiet unless something regressed: a gate that prints its whole backlog on
    # every green run trains people to stop reading it.
    if want_list or regressions:
        out = sys.stdout if want_list else sys.stderr
        for src, n, pred, line in all_findings:
            rel = src.relative_to(REPO_ROOT).as_posix()
            if regressions and not want_list and rel not in {r for r, _, _ in regressions}:
                continue
            kind = "CONTINUOUS" if pred in CONTINUOUS_PREDICATES else "DISCRETE"
            print("  [%s] %s:%d  insp_track_edit() inside `if (ImGui::%s(...))`"
                  % (kind, rel, n, pred), file=out)
            print("      %s" % line[:110], file=out)

    if orphans:
        for src, n, why, line in orphans:
            print("  %s:%d  insp_track_edit() watches no widget (%s)"
                  % (src.relative_to(REPO_ROOT).as_posix(), n, why),
                  file=sys.stderr)
            print("      %s" % line[:110], file=sys.stderr)
        print("check_inspector_undo_scope: FAIL - %d insp_track_edit() call(s) "
              "sit where no ImGui widget was drawn, so they poll whichever item "
              "happened to be last and can strand an open batch.  Delete them, "
              "or move them next to the widget they were meant to track."
              % len(orphans), file=sys.stderr)
        return 1

    if regressions and not want_list:
        for rel, n, budget in regressions:
            print("  %s: %d insp_track_edit() call(s) gated by a one-frame "
                  "predicate, budget %d" % (rel, n, budget), file=sys.stderr)
    if regressions:
        cont = sum(1 for _, _, p_, _ in all_findings if p_ in CONTINUOUS_PREDICATES)
        disc = len(all_findings) - cont
        print("check_inspector_undo_scope: FAIL - %d of %d insp_track_edit() "
              "call(s) run on neither the activation nor the deactivation "
              "frame.  %d are behind a CONTINUOUS widget (fix: hoist the call "
              "out of the `if` body, leave the assignment inside -- see "
              "editor/src/panels/jce_panel_inspector_network.cpp:150-156); %d "
              "are behind a DISCRETE widget (fix: the revert/snapshot/re-apply "
              "idiom of insp_undo_bool/insp_undo_int, "
              "editor/src/panels/jce_panel_inspector_common.cpp:65-81)."
              % (len(all_findings), total_trackers, cont, disc),
              file=sys.stderr)
        return 1

    print("check_inspector_undo_scope: OK (%d insp_track_edit() call(s) across "
          "%d panel source(s); %d still gated by a one-frame predicate, all "
          "within their recorded budget -- burn them down, do not raise it)"
          % (total_trackers, len(sources), len(all_findings)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
