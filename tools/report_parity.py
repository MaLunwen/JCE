#!/usr/bin/env python3
"""Where JCE stands against Unity / Unreal / Godot, computed from evidence.

WHY THIS EXISTS.  The 2026-08-31 audit compared 342 features and published an
aggregate -- 51 ahead / 112 parity / 145 behind / 30 absent / 4 n/a -- and then
THE ROWS WERE GONE.  Only the totals survived, in a report.  A measurement
whose evidence is not stored cannot be re-run, so the split could not move as
work landed; and a split that cannot move is a number that quietly stops being
true while still being printed.  That is the exact shape this whole audit is
about, committed by the audit itself.

contracts/engine-parity.json is that measurement made re-runnable: one row per
capability, each carrying the file, command or landed item that settles it.
This prints the split, and says plainly how much of the 342 has been
re-measured -- because "47.7% at parity" over 55 rows and over 342 rows are
different claims and must not be printed the same way.

    python tools/report_parity.py           # human-readable
    python tools/report_parity.py --json
    python tools/report_parity.py --html    # a bar + legend for a report
"""
import argparse
import html
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
LEDGER    = REPO_ROOT / "contracts/engine-parity.json"

ORDER  = ["ahead", "parity", "behind", "absent", "na"]
LABEL  = {"ahead": "领先", "parity": "持平", "behind": "落后",
          "absent": "缺失", "na": "不适用"}
CSSVAR = {"ahead": "--c-ahead", "parity": "--c-parity", "behind": "--c-behind",
          "absent": "--c-absent", "na": "--c-na"}


def load():
    if not LEDGER.is_file():
        raise SystemExit("missing %s" % LEDGER)
    return json.loads(LEDGER.read_text(encoding="utf-8"))


def tally(d):
    counts = {k: 0 for k in ORDER}
    for f in d["features"]:
        counts[f["status"]] = counts.get(f["status"], 0) + 1
    measured = sum(counts.values())
    scored   = measured - counts["na"]
    good     = counts["ahead"] + counts["parity"]
    return counts, measured, scored, good


def as_text(d):
    counts, measured, scored, good = tally(d)
    a = d["audit_day"]
    out = ["JCE vs Unity / Unreal / Godot", ""]
    out.append("RE-MEASURED WITH EVIDENCE: %d row(s)" % measured)
    for k in ORDER:
        out.append("  %-10s %-8s %3d" % (LABEL[k], k, counts[k]))
    if scored:
        out.append("  -> %.1f%% at parity or ahead (%d of %d scored rows)"
                   % (100.0 * good / scored, good, scored))
    out.append("")
    out.append("AUDIT-DAY BASELINE (%s, %s): %d rows -- ahead %d, parity %d, "
               "behind %d, absent %d, n/a %d"
               % (a["date"], a["commit"], a["rows"], a["ahead"], a["parity"],
                  a["behind"], a["absent"], a["na"]))
    out.append("")
    out.append("%d of the audit's %d rows have been re-measured (%.1f%%).  The "
               "rest were never stored, so this cannot say where they stand "
               "today -- only that nobody has looked."
               % (measured, a["rows"], 100.0 * measured / a["rows"]))
    by_area = {}
    for f in d["features"]:
        by_area.setdefault(f["area"], []).append(f)
    out.append("")
    for area in sorted(by_area):
        rows = by_area[area]
        out.append("== %s (%d) ==" % (area, len(rows)))
        for f in sorted(rows, key=lambda r: (ORDER.index(r["status"]), r["id"])):
            out.append("  [%-6s] %-44s %s"
                       % (f["status"], f["id"], f["feature"]))
    return "\n".join(out)


def as_html(d):
    counts, measured, scored, good = tally(d)
    a = d["audit_day"]
    bar, legend = [], []
    for k in ORDER:
        pct = (100.0 * counts[k] / measured) if measured else 0.0
        if pct > 0:
            bar.append('    <span style="width:%.1f%%;background:var(%s)">%s</span>'
                       % (pct, CSSVAR[k], ("%.1f%%" % pct) if pct >= 6 else ""))
        legend.append('    <span><i style="background:var(%s)"></i>%s %d</span>'
                      % (CSSVAR[k], LABEL[k], counts[k]))
    return "\n".join([
        '  <div class="legend" style="margin-bottom:6px"><span><strong>'
        '重新测量过的 %d 行 · 由 contracts/engine-parity.json 生成'
        '</strong></span></div>' % measured,
        '  <div class="bar" role="img" aria-label="%s">' % html.escape(
            "、".join("%s %d" % (LABEL[k], counts[k]) for k in ORDER)),
        "\n".join(bar),
        "  </div>",
        '  <div class="legend">',
        "\n".join(legend),
        "  </div>",
        '  <div class="legend" style="margin-top:6px;color:var(--muted)">'
        '<span>审计日 %s 定格：共 %d 行，领先 %d · 持平 %d · 落后 %d · 缺失 %d · '
        '不适用 %d —— 其中 %d 行已重测（%.1f%%），其余当时没有被存下来</span></div>'
        % (a["date"], a["rows"], a["ahead"], a["parity"], a["behind"],
           a["absent"], a["na"], measured, 100.0 * measured / a["rows"]),
    ])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--html", action="store_true")
    args = ap.parse_args()
    d = load()
    if args.json:
        counts, measured, scored, good = tally(d)
        print(json.dumps({"counts": counts, "measured": measured,
                          "scored": scored, "parity_or_ahead": good,
                          "audit_day": d["audit_day"]},
                         ensure_ascii=False, indent=2))
    elif args.html:
        print(as_html(d))
    else:
        print(as_text(d))
    return 0


if __name__ == "__main__":
    sys.exit(main())
