#!/usr/bin/env python3
"""Every parity claim carries its evidence, or it is not a claim.

contracts/engine-parity.json says where JCE stands against Unity / Unreal /
Godot, one row per capability.  The failure it exists to prevent is the one the
audit that produced it already committed: a split published as a number, with
the rows behind it stored nowhere, so it could never be re-run and quietly
stopped being true while still being printed.

So a row without evidence is refused here.  "behind" with nothing behind it is
an opinion; "parity" with nothing behind it is a claim nobody can check, and it
will be read as measured because it sits in a table of measurements.

Also refused:
  * a status outside the five the file itself declares -- a typo'd status
    silently drops a row out of every count.
  * a duplicate id, which double-counts one capability.
  * `since` without a date, because "recently" is not a date and the point of
    the field is to say when a row moved.
  * an id or feature that is empty.

WHAT THIS DOES NOT CHECK, said out loud: whether a row is TRUE.  It checks
that a human wrote down what settles it.  Truth is the evidence's job, and
this file cannot follow a file:line into the tree and read it.
"""
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LEDGER    = REPO_ROOT / "contracts/engine-parity.json"

MIN_EVIDENCE = 40   # chars.  "landed #93." is not evidence; the sentence is.
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def self_test() -> None:
    """The three rejections, on every run, so a rewrite of the checks below
    cannot quietly stop rejecting."""
    bad = [
        ({"id": "a", "area": "x", "feature": "f", "status": "parity",
          "evidence": "short"}, "evidence"),
        ({"id": "b", "area": "x", "feature": "f", "status": "probably-fine",
          "evidence": "x" * 60}, "status"),
        ({"id": "", "area": "x", "feature": "f", "status": "parity",
          "evidence": "x" * 60}, "id"),
    ]
    for row, why in bad:
        problems = check_row(row, {"parity", "ahead", "behind", "absent", "na"},
                             set())
        assert problems, "a row with a bad %s was accepted: %r" % (why, row)


def check_row(f, valid, seen):
    out = []
    fid = (f.get("id") or "").strip()
    if not fid:
        out.append("a row with no id")
        return out
    if fid in seen:
        out.append("%s: duplicate id -- one capability counted twice" % fid)
    if not (f.get("feature") or "").strip():
        out.append("%s: no `feature`; the id alone does not say what was "
                   "compared" % fid)
    st = f.get("status")
    if st not in valid:
        out.append("%s: status %r is not one of %s.  A typo'd status drops the "
                   "row out of every count silently."
                   % (fid, st, ", ".join(sorted(valid))))
    ev = (f.get("evidence") or "").strip()
    if len(ev) < MIN_EVIDENCE:
        out.append("%s: evidence is %d chars.  A parity claim with nothing "
                   "behind it reads as measured because it sits in a table of "
                   "measurements -- which is the exact failure this ledger "
                   "exists to stop." % (fid, len(ev)))
    since = f.get("since")
    if since is not None and not DATE.match(str(since)):
        out.append("%s: `since` is %r; it must be YYYY-MM-DD, because the "
                   "field exists to say WHEN the row moved" % (fid, since))
    if not (f.get("compared") or "").strip():
        out.append("%s: no `compared`; a standing against 'standard engines' "
                   "has to name which behaviour it was measured against"
                   % fid)
    return out


def main() -> int:
    self_test()
    if not LEDGER.is_file():
        print("check_parity_ledger: FAIL - missing %s"
              % LEDGER.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
        return 1
    try:
        d = json.loads(LEDGER.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        print("check_parity_ledger: FAIL - %s is not valid JSON: %s"
              % (LEDGER.name, e), file=sys.stderr)
        return 1

    valid = set(d.get("_status_values", {}))
    if not valid:
        print("check_parity_ledger: FAIL - the file declares no "
              "_status_values, so nothing can be checked against it",
              file=sys.stderr)
        return 1

    problems, seen = [], set()
    for f in d.get("features", []):
        problems += check_row(f, valid, seen)
        seen.add((f.get("id") or "").strip())

    # ── THE CANDIDATES FILE ───────────────────────────────────────────
    # contracts/engine-parity-candidates.json holds measured-but-UNVERIFIED
    # rows: the output of a fan-out whose adversarial refutation pass did not
    # finish.  It exists so ~8M tokens of measurement is not thrown away with
    # a session, and it is dangerous for exactly one reason -- a second file
    # full of rows that look like ledger rows.  Two rules keep it from becoming
    # a second source of truth:
    #
    #   1. no id in both.  A row is either checked (ledger) or not (candidates);
    #      an id in both means somebody promoted one and left the copy, and the
    #      copy is what the next reader finds.
    #   2. every candidate carries the same evidence a ledger row does, so
    #      promoting one is a verification step and not a rewrite.
    #
    # The file is OPTIONAL: it is emptied by closing every row, and a checker
    # that fails when its input is absent would punish finishing the work.
    cand_path = REPO_ROOT / "contracts" / "engine-parity-candidates.json"
    if cand_path.exists():
        try:
            cand = json.loads(cand_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            problems.append("engine-parity-candidates.json is unreadable (%s); "
                            "an unreadable candidate list is indistinguishable "
                            "from an empty one" % e)
            cand = {"rows": []}
        crows = cand.get("rows") or []
        if not isinstance(crows, list):
            problems.append("engine-parity-candidates.json has no `rows` list")
            crows = []
        for c in crows:
            cid = (c.get("id") or "").strip()
            if cid and cid in seen:
                problems.append(
                    "%r is in BOTH the ledger and the candidates file. It was "
                    "promoted and the unverified copy was left behind; delete "
                    "the candidate." % cid)
            # Same bar as a ledger row, minus the promotion itself.
            for k in ("id", "area", "feature", "status", "evidence", "compared"):
                if not str(c.get(k) or "").strip():
                    problems.append("candidate %r is missing %r; a candidate "
                                    "without it cannot be verified later, only "
                                    "re-measured" % (cid or "<no id>", k))
            if c.get("status") not in valid:
                problems.append("candidate %r has status %r, not one of %s"
                                % (cid or "<no id>", c.get("status"),
                                   ", ".join(sorted(valid))))
            if len(str(c.get("evidence") or "")) < 40:
                problems.append("candidate %r has evidence too short to check "
                                "later" % (cid or "<no id>"))
        if crows:
            print("check_parity_ledger: %d unverified candidate(s) in "
                  "contracts/engine-parity-candidates.json -- NOT counted as "
                  "measurements of this engine" % len(crows))

    a = d.get("audit_day") or {}
    for k in ("date", "commit", "rows"):
        if k not in a:
            problems.append("audit_day is missing %r; the historical baseline "
                            "has to say which day and which commit it is "
                            "about, or it is just a number" % k)
    if a.get("rows") and len(seen) > a["rows"]:
        problems.append("more re-measured rows (%d) than the audit compared "
                        "(%d); one of the two numbers is wrong"
                        % (len(seen), a["rows"]))

    if problems:
        print("check_parity_ledger: FAIL - %d problem(s):" % len(problems),
              file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 1

    print("check_parity_ledger: OK (%d row(s), every one with a status, a "
          "comparison and evidence; audit-day baseline %d rows from %s)"
          % (len(seen), a.get("rows", 0), a.get("date", "?")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
