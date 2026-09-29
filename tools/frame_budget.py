#!/usr/bin/env python3
"""
frame_budget.py — a render-frame budget that can actually go red.

WHY THIS EXISTS.  This repository measures frame cost in several places and
enforces it in none.  jce_accept.py's perf stage parses frame_ms / fps / cpu /
gpu / draws / rss out of the engine's own perf line and then sets PASS
unconditionally, printing the numbers as prose: any number passes, so the only
way that stage can fail is if the LINE is missing.  A measurement nobody
compares against anything is a report, not a gate.

WHAT IT BUDGETS, AND WHY NOT MILLISECONDS BY DEFAULT.  The obvious gate --
"draw_submit must stay under N ms" -- is a flaky gate on a developer machine,
and a flaky gate is worse than none because it teaches people to re-run.  A
captured 60-frame window on this tree makes the problem concrete:

    draw_submit   p50 2.293  p95 2.678  max  4.015 ms
    end_frame     p50 5.152  p95 71.883 max 2043.304 ms

end_frame's maximum is the startup frame and the present wait; budgeting on it
measures the display, not the renderer.  So the primary criteria here are the
ones that do NOT move with the machine:

  * DRAW COUNT.  For a fixed scene and a fixed camera, the number of draw calls
    the renderer submits is a property of the renderer, not of the hardware.  A
    culling regression, a lost instancing batch or a duplicated pass shows up
    here immediately and identically on any box.

  * PHASE SHARE.  draw_submit as a fraction of scene_render is a ratio, so the
    machine's speed cancels.  A submit path that starts doing per-draw work the
    rest of the frame does not is visible in the ratio long before it is
    visible in a wall-clock number somebody has to re-baseline.

Absolute milliseconds are still CAPTURED and REPORTED -- they are what a human
wants to see -- and can be gated, but only when the budget file explicitly
carries a number for this machine.  The file says so on its own first line.

NESTING.  Phases nest (app_update contains ed_layout contains ed_panels), so
summing every phase is double counting and is never a frame total here.  Ratios
are only compared between a phase and an ancestor that actually contains it.

Usage:
  python tools/frame_budget.py                     # measure + enforce
  python tools/frame_budget.py --frames 120
  python tools/frame_budget.py --report            # measure, print, do not gate
  python tools/frame_budget.py --update-baseline   # record what it measured
  python tools/frame_budget.py --csv <file>        # enforce an existing capture

Exit 0 within budget, 1 over it, 2 when the measurement itself did not happen.
"""

import argparse
import collections
import csv
import json
import os
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from jce_determinism import DETERMINISM, actual_backend, parse_perf  # noqa: E402

EXE = ROOT / "build" / "desktop" / "windows-x64" / "release" / "jce_editor.exe"
BUDGETS = ROOT / "tools" / "frame_budget.json"


def p95(values):
    v = sorted(values)
    if not v:
        return 0.0
    return v[min(len(v) - 1, int(round(0.95 * (len(v) - 1))))]


def _ratchet_draws(old, measured):
    """A draw budget that may fall but never rise by itself."""
    proposed = measured * 2 if measured else None
    previous = (old or {}).get("max_draws")
    if proposed is None:
        return previous
    if previous is None or proposed < previous:
        return proposed
    if proposed > previous:
        print("frame_budget: NOT raising max_draws %d -> %d.  The measured draw "
              "count went UP; a budget that re-derives itself from the run it "
              "is meant to police is not a budget.  Edit the number by hand and "
              "say why in the commit message." % (previous, proposed),
              file=sys.stderr)
    return previous


def load_budgets():
    if not BUDGETS.is_file():
        return None
    return json.loads(BUDGETS.read_text(encoding="utf-8"))


def capture(frames, timeout):
    """Run the engine bounded and return (phase_rows, perf_sample, log_text)."""
    if not EXE.is_file():
        print("frame_budget: SKIPPED - %s not built.  Build it first:\n"
              "    cmake --build build/desktop/windows-x64 --target JCE_Editor"
              % EXE.relative_to(ROOT).as_posix(), file=sys.stderr)
        return None, None, None

    tmp = Path(tempfile.mkdtemp(prefix="jce_frame_budget_"))
    csv_path = tmp / "frames.csv"
    log_path = tmp / "run.log"

    env = os.environ.copy()
    # The determinism recipe is single-sourced; a budget measured with a free
    # timestep and a free TAA phase is a budget for a different frame each run.
    env.update(DETERMINISM)
    env.update({
        "JCE_PERF_LOG": "1",
        "JCE_LOG_FILE": str(log_path),
        "JCE_MAX_FRAMES": str(frames),
        "JCE_KPI_FRAME_LOG": str(csv_path),
        "JCE_MULTI_INSTANCE": "1",
    })

    proc = subprocess.run([str(EXE)], env=env, cwd=str(ROOT),
                          capture_output=True, text=True, timeout=timeout)
    text = ""
    if log_path.is_file():
        text = log_path.read_text(encoding="utf-8", errors="replace")
    text += (proc.stdout or "") + (proc.stderr or "")

    # A run that produced numbers and then died is not a measurement.  This is
    # the mistake perf_bench.py records having made by discarding the exit code.
    if proc.returncode != 0:
        print("frame_budget: SKIPPED - the run exited %d; a measurement from a "
              "process that did not finish is not a measurement.\n  last: %s"
              % (proc.returncode, (proc.stderr or "").strip()[-300:] or "(empty)"),
              file=sys.stderr)
        return None, None, text

    if not csv_path.is_file():
        print("frame_budget: SKIPPED - no %s; the build predates "
              "JCE_KPI_FRAME_LOG." % csv_path.name, file=sys.stderr)
        return None, None, text

    rows = list(csv.DictReader(csv_path.open(encoding="utf-8")))
    return rows, parse_perf(text), text


def summarise(rows, warmup):
    """Per-phase sample lists, with two exclusions that are NOT tidying.

    WARMUP.  The first frames of a run load and compile.  Measured on this tree
    over 150 frames: scene_render p95 is 8.026 ms including everything and
    7.472 ms from frame 20 on -- 7% of the number is the startup, and a budget
    set against the contaminated figure has 7% less room than it looks like it
    has.  draw_submit was unchanged (4.066 either way), which is the useful
    detail: warmup does not contaminate every phase, so skipping is a per-run
    correction rather than an assumption.

    ZERO SAMPLES.  The engine writes a dense table -- every registered phase,
    every frame -- so a phase that did not run in a frame contributes 0.0000.
    Frames 1 and 2 are all-zero for exactly that reason.  A zero means "did not
    run", not "ran instantly", and averaging the two together answers a
    question nobody asked.  Percentiles are taken over the frames a phase
    ACTUALLY ran in; a phase that stops running entirely is caught by the
    budget's own "never appeared in the capture" finding, not by a diluted
    percentile."""
    per = collections.defaultdict(list)
    seen_frames = set()
    for r in rows:
        try:
            f = int(r["frame_index"])
            ms = float(r["ms"])
        except (KeyError, ValueError, TypeError):
            continue
        seen_frames.add(f)
        if f < warmup:
            continue
        if ms > 0.0:
            per[r["phase"]].append(ms)
    return per, len(seen_frames)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    # >= 121 on purpose.  The engine emits its perf line -- the ONLY source of
    # the draw count, which is this gate's machine-independent criterion -- once
    # every 120 frames (jce_engine.c: `if (++s_n >= 120u)`).  A 60- or 90-frame
    # run produces a complete phase table and NO draw count, and the budget then
    # silently checks one criterion fewer.
    ap.add_argument("--frames", type=int, default=150)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--warmup", type=int, default=20,
                    help="frames to drop from the front (default 20; measured "
                         "to move scene_render p95 by 7%% on this tree and "
                         "draw_submit by 0%%)")
    ap.add_argument("--report", action="store_true",
                    help="measure and print, but never fail")
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--csv", help="enforce an existing capture instead of running")
    args = ap.parse_args(argv)

    budgets = load_budgets()
    if budgets is None and not args.update_baseline:
        print("frame_budget: SKIPPED - %s missing; run --update-baseline once "
              "to record what this tree costs today."
              % BUDGETS.relative_to(ROOT).as_posix(), file=sys.stderr)
        return 2

    if args.csv:
        rows = list(csv.DictReader(Path(args.csv).open(encoding="utf-8")))
        sample, text = None, ""
    else:
        rows, sample, text = capture(args.frames, args.timeout)
        if rows is None:
            return 2

    per, frame_count = summarise(rows, args.warmup)
    if frame_count < 2:
        print("frame_budget: SKIPPED - the capture holds %d frame(s); nothing "
              "to take a percentile of." % frame_count, file=sys.stderr)
        return 2

    if not args.csv and text:
        be = actual_backend(text)
        print("frame_budget: %d frames, backend %s" % (frame_count, be or "?"))
    else:
        print("frame_budget: %d frames" % frame_count)

    findings = []
    report = {"frames": frame_count, "phases": {}, "draws": None}

    # ── 1. draw count ────────────────────────────────────────────────
    if sample:
        report["draws"] = sample["draws"]
        cap = (budgets or {}).get("max_draws")
        print("  draws            %6d   (budget %s)"
              % (sample["draws"], cap if cap is not None else "-"))
        if cap is not None and sample["draws"] > cap:
            findings.append(
                "draw count %d over the budget of %d.  For a fixed scene this "
                "number is a property of the renderer, not of the machine: a "
                "culling regression, a lost instancing batch or a duplicated "
                "pass all land here." % (sample["draws"], cap))

    # ── 2. phase shares (machine-independent) ────────────────────────
    for entry in (budgets or {}).get("shares", []):
        child, parent, cap = entry["phase"], entry["of"], entry["max_share"]
        if child not in per or parent not in per:
            findings.append("share %s/%s: phase %s never appeared in the "
                            "capture -- it was renamed, or the path that emits "
                            "it no longer runs"
                            % (child, parent, child if child not in per else parent))
            continue
        c, p = p95(per[child]), p95(per[parent])
        share = (c / p) if p > 0.0 else 0.0
        report["phases"][child] = {"p95_ms": round(c, 4), "share_of": parent,
                                   "share": round(share, 4)}
        print("  %-16s p95 %6.3f ms   %5.1f%% of %s (budget %.1f%%)"
              % (child, c, share * 100.0, parent, cap * 100.0))
        if share > cap:
            findings.append(
                "%s is %.1f%% of %s, over the %.1f%% budget.  A ratio is used "
                "here because it cancels the machine: this is the submit path "
                "doing more per draw than it did, not a slower box."
                % (child, share * 100.0, parent, cap * 100.0))

    # ── 3. absolute ms, only where the file opts in ──────────────────
    for phase, cap_ms in (budgets or {}).get("max_p95_ms", {}).items():
        if phase not in per:
            findings.append("max_p95_ms names %s, which never appeared in the "
                            "capture" % phase)
            continue
        v = p95(per[phase])
        print("  %-16s p95 %6.3f ms   (machine budget %.3f ms)" % (phase, v, cap_ms))
        if v > cap_ms:
            findings.append("%s p95 %.3f ms over the %.3f ms budget recorded "
                            "for this machine" % (phase, v, cap_ms))

    if args.update_baseline:
        top = sorted(per.items(), key=lambda kv: -p95(kv[1]))[:8]
        doc = {
            "_note": ("Frame budgets.  max_draws and shares are machine-"
                      "INDEPENDENT and are the real gate; max_p95_ms is "
                      "wall-clock and therefore specific to the machine that "
                      "recorded it -- leave it empty unless one machine owns "
                      "the number.  Regenerate with "
                      "`python tools/frame_budget.py --update-baseline` and say "
                      "in the commit message why a number moved."),
            "_measured": {k: round(p95(v), 4) for k, v in top},
            # RATCHET, not a follower.  Recomputing this from the current run
            # every time means a change that doubles the draw count, followed by
            # --update-baseline, silently doubles its own budget -- the budget
            # would track the code instead of constraining it.  So it may go
            # DOWN freely and never up on its own; raising it is a decision
            # somebody states, by editing the number and saying why.
            "max_draws": _ratchet_draws(budgets, report["draws"]),
            "shares": (budgets or {}).get("shares", []),
            "max_p95_ms": (budgets or {}).get("max_p95_ms", {}),
        }
        BUDGETS.write_text(json.dumps(doc, indent=2) + "\n",
                           encoding="utf-8", newline="\n")
        print("frame_budget: baseline written -> %s"
              % BUDGETS.relative_to(ROOT).as_posix())
        return 0

    if findings and not args.report:
        for f in findings:
            print("  OVER BUDGET: %s" % f, file=sys.stderr)
        print("frame_budget: FAIL - %d budget(s) exceeded." % len(findings),
              file=sys.stderr)
        return 1

    if findings:
        for f in findings:
            print("  (report-only) %s" % f)
    print("frame_budget: OK (%d frame(s); %d budget(s) checked)"
          % (frame_count,
             (1 if (budgets or {}).get("max_draws") is not None else 0)
             + len((budgets or {}).get("shares", []))
             + len((budgets or {}).get("max_p95_ms", {}))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
