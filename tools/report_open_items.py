#!/usr/bin/env python3
"""What is still open, read out of the gates that hold it -- not a hand list.

WHY A GENERATOR.  This repository's open work is already written down, in the
baselines the lint gates enforce: tools/lint/component_field_baseline.json,
tools/lint/project_settings_baseline.json, and the exempt lists beside them.
Every entry in those files carries a REASON, and the gates fail on an entry
that does not -- so the list is complete, current and explained by
construction.

A hand-maintained TODO list beside them would be a second copy that goes stale
the moment somebody closes an item, and the stale copy is the one people read.
This prints the same data instead.

    python tools/report_open_items.py            # human-readable
    python tools/report_open_items.py --json     # machine-readable
    python tools/report_open_items.py --html     # a table to paste into a report

STATUS is derived, not stored:

    NEEDS A SUBSYSTEM  the reason names a module, path or solver that does not
                       exist yet
    NEEDS AN ENCODING  the value cannot be honoured without changing what the
                       bytes mean (a "0 means both" trap)
    NEEDS A DECISION   the reason says the answer is a design choice
    DECIDED NO         it should be removed rather than wired
    WIRE               none of the above -- the destination exists

The classifier is deliberately crude and says so: it reads the reason text,
which is prose a human wrote.  Treat the status as a sort key, not a verdict;
the reason is the answer.
"""
import argparse
import html
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

SOURCES = [
    ("组件字段", "tools/lint/component_field_baseline.json", "fields", "reasons",
     "check_component_field_consumed.py"),
    ("项目设置", "tools/lint/project_settings_baseline.json", "unread", "reasons",
     "check_project_settings_consumed.py"),
]

# Ordered: the first pattern that matches wins, so the more specific
# classifications sit above the general ones.
RULES = [
    ("DECIDED NO",        r"DECIDED NO|A DECIDED NO|should arguably go|correctly so|A LABEL"),
    ("NEEDS AN ENCODING", r"encoding change|value for \"unset\"|tri-state|means BOTH"),
    ("NEEDS A SUBSYSTEM", r"does not exist|NO .*MODULE|MISSING SUBSYSTEM|needs a subsystem|"
                          r"is a subsystem|declared stub|no .*path for it|"
                          r"NEEDS A PUBLIC API|nowhere to go|no engine-side"),
    ("NEEDS A DECISION",  r"is a decision|a design choice|means choosing|"
                          r"different vehicle model|a new surface"),
]


def classify(reason: str) -> str:
    for status, pat in RULES:
        if re.search(pat, reason, re.I):
            return status
    return "WIRE"


def collect() -> list:
    rows = []
    for area, rel, list_key, reason_key, gate in SOURCES:
        p = REPO_ROOT / rel
        if not p.is_file():
            continue
        d = json.loads(p.read_text(encoding="utf-8"))
        reasons = d.get(reason_key, {})
        for item in d.get(list_key, []):
            reason = (reasons.get(item) or "").strip()
            rows.append({
                "area":   area,
                "item":   item,
                "status": classify(reason) if reason else "NO REASON RECORDED",
                "reason": reason,
                "gate":   gate,
                "source": rel,
            })
    rows.sort(key=lambda r: (r["area"], r["status"], r["item"]))
    return rows


def as_text(rows: list) -> str:
    out = []
    by_area = {}
    for r in rows:
        by_area.setdefault(r["area"], []).append(r)
    for area, rs in by_area.items():
        out.append("== %s (%d) ==" % (area, len(rs)))
        for r in rs:
            out.append("  [%-18s] %s" % (r["status"], r["item"]))
            out.append("      %s" % r["reason"][:200])
        out.append("")
    out.append("%d open item(s) across %d area(s).  Each is enforced by its gate; "
               "closing one means removing it from that gate's baseline."
               % (len(rows), len(by_area)))
    return "\n".join(out)


def as_html(rows: list) -> str:
    """A table body, for pasting into a report.  Escaped, so a reason
    containing < or & cannot break the page it lands in."""
    out = []
    for r in rows:
        out.append(
            '      <tr><td class="d">%s</td><td><span class="pill %s">%s</span></td>'
            '<td><code>%s</code></td><td>%s</td></tr>'
            % (html.escape(r["area"]),
               "TODO" if r["status"] != "DECIDED NO" else "NA",
               html.escape(r["status"]),
               html.escape(r["item"]),
               html.escape(r["reason"])))
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--html", action="store_true")
    args = ap.parse_args()

    rows = collect()
    if not rows:
        print("report_open_items: no baselines found -- run from the repo, or "
              "the gates have been renamed", file=sys.stderr)
        return 1

    missing = [r["item"] for r in rows if r["status"] == "NO REASON RECORDED"]
    if args.json:
        print(json.dumps(rows, ensure_ascii=False, indent=2))
    elif args.html:
        print(as_html(rows))
    else:
        print(as_text(rows))

    if missing:
        # Not an error here -- the gates already fail on it -- but saying so is
        # the difference between a report and a report you can trust.
        print("\nWARNING: %d entr(ies) carry no reason: %s"
              % (len(missing), ", ".join(missing)), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
