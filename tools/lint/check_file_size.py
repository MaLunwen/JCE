#!/usr/bin/env python3
"""
check_file_size.py — the gate for AGENTS.md §11's "不新增 god file（>3000 行）",
and the repository's line-count statistics.

WHY THIS EXISTS.  §11 lists nine invariants whose violation is "直接 reject".
Eight of them have a checker.  This one did not: a repo-wide grep for the
number 3000 across scripts/, tools/, cmake/ and CMakeLists.txt found no length
check, and `ls $(git rev-parse --git-common-dir)/hooks` shows no non-sample
hook.  The rule was restated five times (AGENTS.md:17, :138, :143, :226;
editor/AGENTS.md:72; engine/src/os/platform/AGENTS.md:36) and enforced zero
times.

WHY A RATCHET AND NOT A CAP.  A plain "fail over 3000" gate cannot be adopted:
thirteen first-party files are already over, totalling ~53k lines, so the gate
would be red on the day it landed and would be disabled by the first person it
inconvenienced.  Worse, a plain cap would not have caught the failure this
repository actually had.  Git history:

    2026-06-23  an eight-commit campaign (ab705369 … 9cac6d8b) split the
                renderer and landed it FULLY COMPLIANT --
                jce_scene_renderer.c 11559 -> 2470, jce_sr_draw.c born at
                1579, jce_sr_environment.c at 1397, jce_sr_internal.h at 1334.
    68 days later, at HEAD: 6560, 5836, 5390, 3052 -- +14,058 lines, all four
                back over the cap, with run_all.py green on every commit in
                between.

A human enforced the rule once, by hand, and the tree silently undid it.  So
the gate here is a RATCHET: every file has a budget, and a file may never
exceed it.  A file under the cap gets the cap as its budget; a file already
over gets its recorded baseline, which can only ever go DOWN.  That fails the
regrowth above on the first commit that adds a line, while staying green on
the day it is adopted.

WHAT IT DELIBERATELY DOES NOT COUNT, and it says so out loud rather than
shrinking its scope in silence:
  * vendored third-party source (`/third_party/` and the explicit VENDORED
    list below -- a single-header library dropped into an engine directory
    does not become first-party by its location);
  * generated artefacts (`*.gen.*`, or a DO-NOT-EDIT marker in the head of the
    file) -- their size is the generator's business, and editing them is
    already refused by the scriptgen gates;
  * anything git does not track, which on `main` includes all of tests/.
Every exclusion is COUNTED and printed.  A checker that quietly narrows what
it looks at is the failure mode this repository has recorded more than once.

USAGE
    python tools/lint/check_file_size.py              # gate  (exit 0/1)
    python tools/lint/check_file_size.py --stats      # statistics report
    python tools/lint/check_file_size.py --stats --top 30
    python tools/lint/check_file_size.py --update-baseline
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BASELINE = Path(__file__).resolve().parent / "file_size_baseline.json"

# AGENTS.md §11.  Spelled once.
CAP = 3000

# run_all.py's convention: 2 means "the precondition is absent", and the
# runner scores it as neither pass nor fail.
EXIT_SKIPPED = 2

# Extensions this gate is about: first-party translation units and headers.
# Python/CMake/shader sources are deliberately out of scope -- §11's clause is
# about C/C++ god files, and a 1500-line CMakeLists is a different problem with
# a different remedy.
SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".inc.h")

# Trees whose contents are first-party by path but third-party by origin.
# Each entry needs a REASON, so a later reader can tell an exemption from an
# oversight -- the same rule tools/lint/editor_consumption_exempt.txt states.
VENDORED = {
    "engine/src/renderer/internal/stb_image.h":
        "stb single-header library, vendored verbatim; upstream owns its size",
    "engine/src/renderer/internal/stb_image_write.h":
        "same library, writer half",
    "engine/src/renderer/internal/stb_image_resize2.h":
        "same library, resampler half",
}

# A file whose head carries one of these is generated.  Checked in the first
# 12 lines only: a mention of the phrase deep inside a hand-written file is
# prose, not a marker.
GENERATED_MARKERS = ("DO NOT EDIT", "GENERATED. DO NOT", "@generated",
                     "AUTO-GENERATED", "自动生成，请勿手工修改")


def tracked_sources() -> list[str]:
    """Every git-tracked path this gate is about.

    git ls-files and not a filesystem walk, deliberately: an untracked file is
    not part of the repository's design, and on `main` that includes the whole
    of tests/.  Using the index also means a clean clone and this working tree
    measure the same set.
    """
    out = subprocess.run(["git", "ls-files", "-z"], cwd=REPO_ROOT,
                         capture_output=True)
    if out.returncode != 0:
        # SKIPPED, not FAILED, and exit 2 -- the convention run_all.py already
        # tolerates and check_agents_md.py already uses.  The gate's
        # PRECONDITION is absent (no git index to measure), which is a
        # different thing from the rule being violated.  Reporting it as a
        # failure would make every `git archive` sandbox and every source
        # tarball look like a god-file violation; reporting it as a silent
        # pass would be worse.  Say which, and say why.
        print("check_file_size: SKIPPED — `git ls-files` did not run, so "
              "there is no index to measure. This gate reads the INDEX on "
              "purpose: an untracked file is not part of the repository's "
              "design, and on main that includes all of tests/. Run it inside "
              "a git working tree.")
        sys.exit(EXIT_SKIPPED)
    paths = [p for p in out.stdout.decode("utf-8", "replace").split("\0") if p]
    return [p for p in paths if p.endswith(SOURCE_SUFFIXES)]


def classify(rel: str) -> str:
    """'first-party' | 'vendored' | 'generated'."""
    if rel in VENDORED or "/third_party/" in rel:
        return "vendored"
    name = Path(rel).name
    if ".gen." in name:
        return "generated"
    try:
        with (REPO_ROOT / rel).open("r", encoding="utf-8", errors="replace") as f:
            head = "".join(next(f, "") for _ in range(12))
    except OSError:
        return "first-party"
    if any(m in head for m in GENERATED_MARKERS):
        return "generated"
    return "first-party"


def line_count(rel: str) -> int:
    """Physical lines, the same unit §11 and `wc -l` use.

    NOT logical or non-blank lines: the rule a reader is being held to is the
    one they can check with wc, and a gate that counted differently from the
    tool everyone reaches for would be argued with instead of obeyed.
    """
    try:
        with (REPO_ROOT / rel).open("rb") as f:
            return f.read().count(b"\n")
    except OSError:
        return 0


def line_count_at_head(rel: str) -> int:
    """The same count, but of what is COMMITTED.

    The baseline is written from HEAD and never from the working tree.  This
    repository is a shared worktree -- eleven checkouts on one .git, with
    concurrent sessions holding uncommitted edits -- so a baseline taken from
    disk would freeze whatever someone else happened to have in flight as a
    permanent budget.  Measured, not theorised: while this gate was being
    written, engine/src/middleware/scene/jce_sr_environment.c stood at 5390
    lines at HEAD and 5601 on disk.  Recording 5601 would have handed a
    god file 211 lines of headroom that nobody ever reviewed.

    The GATE still measures the working tree -- that is where a growth is
    caught, and every other checker in run_all.py measures the same place.
    """
    out = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=REPO_ROOT,
                         capture_output=True)
    if out.returncode != 0:
        return 0
    return out.stdout.count(b"\n")


def layer_of(rel: str) -> str:
    """AGENTS.md §3's layer for a path, or a coarse bucket for non-engine code."""
    if rel.startswith("engine/src/os/"):          return "L2 os"
    if rel.startswith("engine/src/renderer/"):    return "L3 renderer"
    if rel.startswith("engine/src/resource/"):    return "L3 resource"
    if rel.startswith("engine/src/middleware/"):  return "L4 middleware"
    if rel.startswith("engine/src/runtime/"):     return "L5 runtime"
    if rel.startswith("engine/src/application/"): return "L6 application"
    if rel.startswith("engine/include/"):         return "public headers"
    if rel.startswith("engine/"):                 return "engine (other)"
    if rel.startswith("editor/"):                 return "L7 editor"
    if rel.startswith("scripting/"):              return "scripting"
    if rel.startswith("tools/"):                  return "tools"
    if rel.startswith("examples/caged_kingdom/"):          return "L7 caged_kingdom"
    return "other"


def collect() -> tuple[dict, dict]:
    """(first-party {rel: lines}, excluded {kind: [(rel, lines)]})."""
    first: dict[str, int] = {}
    excluded: dict[str, list] = defaultdict(list)
    for rel in tracked_sources():
        kind = classify(rel)
        n = line_count(rel)
        if kind == "first-party":
            first[rel] = n
        else:
            excluded[kind].append((rel, n))
    return first, excluded


def load_baseline() -> dict:
    if not BASELINE.is_file():
        return {}
    try:
        return json.loads(BASELINE.read_text(encoding="utf-8")).get("files", {})
    except (OSError, ValueError):
        return {}


def budget_for(rel: str, base: dict) -> int:
    """The line count this file may not exceed.

    Under the cap -> the cap: it may grow to 3000 and no further.
    Already over  -> its baseline: it may not grow at all, and every commit
                     that shrinks it lowers the budget on the next
                     --update-baseline.  The ratchet only turns one way.
    """
    recorded = base.get(rel)
    if recorded is None:
        return CAP
    return max(CAP, int(recorded))


# ── stats ────────────────────────────────────────────────────────────────

def report_stats(top: int) -> int:
    first, excluded = collect()
    total = sum(first.values())
    print(f"JCE line-count statistics — {len(first)} tracked first-party "
          f"source files, {total:,} lines\n")

    by_layer: dict[str, list[int]] = defaultdict(list)
    for rel, n in first.items():
        by_layer[layer_of(rel)].append(n)
    print(f"{'layer':<22}{'files':>7}{'lines':>12}{'share':>8}{'largest':>10}")
    print("-" * 59)
    for layer in sorted(by_layer, key=lambda k: -sum(by_layer[k])):
        ns = by_layer[layer]
        print(f"{layer:<22}{len(ns):>7}{sum(ns):>12,}"
              f"{100.0 * sum(ns) / total:>7.1f}%{max(ns):>10,}")
    print("-" * 59)
    print(f"{'TOTAL':<22}{len(first):>7}{total:>12,}{100.0:>7.1f}%\n")

    by_ext: dict[str, list[int]] = defaultdict(list)
    for rel, n in first.items():
        by_ext[Path(rel).suffix].append(n)
    print("by extension: " + "   ".join(
        f"{e} {len(v)}f/{sum(v):,}L"
        for e, v in sorted(by_ext.items(), key=lambda kv: -sum(kv[1]))))
    print()

    over = sorted(((n, r) for r, n in first.items() if n > CAP), reverse=True)
    print(f"over the §11 cap of {CAP} lines: {len(over)} files, "
          f"{sum(n for n, _ in over):,} lines "
          f"({100.0 * sum(n for n, _ in over) / total:.1f}% of first-party)")
    for n, rel in over:
        print(f"  {n:>7,}  {rel}")
    print()

    print(f"largest {top} first-party files:")
    for n, rel in sorted(((n, r) for r, n in first.items()), reverse=True)[:top]:
        mark = "  <-- over cap" if n > CAP else ""
        print(f"  {n:>7,}  {rel}{mark}")
    print()

    # Say what was left out, and how big it was.  A statistic that hides its
    # own exclusions is how "the engine is 200k lines" becomes folklore.
    for kind in sorted(excluded):
        items = excluded[kind]
        print(f"excluded ({kind}): {len(items)} files, "
              f"{sum(n for _, n in items):,} lines")
        for rel, n in sorted(items, key=lambda t: -t[1])[:5]:
            print(f"  {n:>7,}  {rel}")
        if len(items) > 5:
            print(f"  … {len(items) - 5} more")
    return 0


# ── gate ─────────────────────────────────────────────────────────────────

def run_gate() -> int:
    first, excluded = collect()
    base = load_baseline()

    violations = []
    for rel, n in sorted(first.items()):
        b = budget_for(rel, base)
        if n > b:
            violations.append((rel, n, b, rel in base))

    n_excl = sum(len(v) for v in excluded.values())
    if not violations:
        over = sum(1 for n in first.values() if n > CAP)
        print(f"check_file_size: OK — {len(first)} first-party source files "
              f"within budget ({over} legacy file(s) over the {CAP}-line cap, "
              f"each frozen at its baseline and none grown); "
              f"{n_excl} vendored/generated file(s) not counted.")
        return 0

    print("check_file_size: FAILED")
    print()
    for rel, n, b, was_recorded in violations:
        if was_recorded:
            print(f"  {rel}")
            print(f"      {n} lines, was {b} at baseline — a file already over "
                  f"the {CAP}-line cap may not grow.")
            print(f"      Split it, or move the addition into a new "
                  f"translation unit.")
        else:
            print(f"  {rel}")
            print(f"      {n} lines, over AGENTS.md §11's cap of {CAP}.")
            print(f"      This file is newly a god file.")
        print()
    print("If the growth is genuinely justified, record it deliberately WITH "
          "its reason:")
    print("    python tools/lint/check_file_size.py --raise <path> "
          "--reason \"<why>\"")
    print("The reason is stored beside the number, because a number that went "
          "up alone is indistinguishable from a silenced gate.")
    print("(--update-baseline re-reads HEAD and is for recording SHRINKS; it "
          "cannot record a growth that is not committed yet, which is why "
          "--raise exists.)")
    return 1


def load_baseline_raw() -> dict:
    """The whole document, not just its "files" map -- --raise must preserve
    the note and the reasons it did not touch."""
    if not BASELINE.is_file():
        return {}
    try:
        return json.loads(BASELINE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def raise_baseline(paths, reason) -> int:
    """Record the DISK count for exactly `paths`, with `reason` beside it.

    Deliberately NOT a disk sweep: --update-baseline reads HEAD because this
    repository is eleven worktrees on one .git, and a sweep from disk would
    freeze a concurrent session's in-flight edits as a permanent budget.
    Naming files one at a time keeps that protection while making a REVIEWED
    growth expressible in the commit that makes it -- which the gate's own
    failure message has always told people to do and which, until now, could
    not be done before committing.
    """
    if not reason.strip():
        print("check_file_size: --raise needs --reason \"<why this growth is "
              "justified>\".  A number that went up with no reason beside it "
              "is indistinguishable from a silenced gate.", file=sys.stderr)
        return 1
    data = load_baseline_raw()
    if not data:
        print("check_file_size: no baseline to raise", file=sys.stderr)
        return 1
    files = data.setdefault("files", {})
    reasons = data.setdefault("_reasons", {})
    problems = []
    raised = 0
    for rel in paths:
        rel = rel.replace("\\", "/")
        if not (REPO_ROOT / rel).is_file():
            problems.append("not a file: " + rel)
            continue
        n = line_count(rel)
        old_n = files.get(rel)
        if n <= CAP:
            problems.append("%s is %d lines, at or under the cap of %d -- it "
                            "does not belong in the baseline" % (rel, n, CAP))
            continue
        if old_n is not None and n <= old_n:
            problems.append("%s is %d lines, at or under its recorded %d -- "
                            "nothing to raise" % (rel, n, old_n))
            continue
        files[rel] = n
        # APPEND, never replace.  A file raised twice has two reasons and the
        # budget covers both; keeping only the newest would explain part of
        # the number and quietly drop the rest -- which is exactly the audit
        # trail this field exists to be.  (run_dedup_audit.py --write-baseline
        # had the same defect and the same fix.)
        prev = reasons.get(rel, "").strip()
        note = "%s -> %d: %s" % (old_n if old_n is not None else "unlisted",
                                 n, reason.strip())
        reasons[rel] = (prev + "  ||  " + note) if prev else note
        print("  raised %s: %s -> %d"
              % (rel, old_n if old_n is not None else "(unlisted)", n))
        raised += 1
    if problems:
        for pr in problems:
            print("  " + pr, file=sys.stderr)
        print("check_file_size: --raise REFUSED, nothing written",
              file=sys.stderr)
        return 1
    data["files"] = dict(sorted(files.items()))
    data["_reasons"] = dict(sorted(reasons.items()))
    BASELINE.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8", newline="\n")
    print("check_file_size: raised %d file(s), reason recorded in the baseline"
          % raised)
    return 0


def update_baseline() -> int:
    first, _ = collect()
    # From HEAD, not from disk -- see line_count_at_head() for the shared-
    # worktree reason.  A file whose HEAD size is at or under the cap is not
    # recorded at all: its budget is the cap.
    over = {}
    for rel in first:
        n = line_count_at_head(rel)
        if n > CAP:
            over[rel] = n
    BASELINE.write_text(
        json.dumps(
            {
                "_note": (
                    "Line-count budgets for files already over AGENTS.md "
                    "§11's 3000-line cap when the gate was adopted. A file "
                    "listed here may not GROW; a file not listed may not "
                    "CROSS the cap. Regenerate with "
                    "`python tools/lint/check_file_size.py "
                    "--update-baseline` and say in the commit message why a "
                    "number went up."
                ),
                "cap": CAP,
                "files": dict(sorted(over.items())),
            },
            indent=2,
        ) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print(f"check_file_size: baseline written — {len(over)} file(s) over the "
          f"{CAP}-line cap, {sum(over.values()):,} lines total.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stats", action="store_true",
                    help="print the line-count report instead of gating")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--raise", dest="raise_paths", nargs="+", metavar="PATH",
                    help="record the DISK count for exactly these files "
                         "(needs --reason); --update-baseline reads HEAD "
                         "and so cannot record an uncommitted growth")
    ap.add_argument("--reason", default="",
                    help="why the growth is justified; stored beside the number")
    a = ap.parse_args()
    if a.update_baseline:
        return update_baseline()
    if a.raise_paths:
        return raise_baseline(a.raise_paths, a.reason)
    if a.stats:
        return report_stats(a.top)
    return run_gate()


if __name__ == "__main__":
    sys.exit(main())
