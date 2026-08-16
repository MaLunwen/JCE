#!/usr/bin/env python3
"""Guarded performance runs for the editor.

WHY A HARNESS AND NOT A COMMAND LINE
------------------------------------
Two failure modes produced wrong numbers repeatedly, and both are silent:

  1. The editor holds a single-instance lock. A second process finds the lock,
     activates the existing window and exits 0 WITHOUT RENDERING A FRAME. The
     caller then parses the log file from a previous run -- possibly of a
     different build -- and reports it as a fresh measurement. That is how a
     draw count appeared to "vary" between 145, 147 and 150 across runs of one
     binary, which sent an entire optimisation down the wrong path.

  2. A run that dies early leaves the previous log in place, with the same
     effect.

So this deletes the log first, refuses to start while another editor is alive,
and raises when the log is missing, or contains the single-instance message, or
has no perf line. A measurement that did not happen must look like an error,
never like a number.

WHAT IT DOES NOT DO
-------------------
It does not make runs comparable across invocations. The editor persists its
viewport pose in per-user session state, so two runs minutes apart can be
looking at different parts of the scene: the 200k dense bench has been measured
at 3.10 and at 5.28 ms with identical code for exactly that reason, while
200k-spread, street, graveyard and es stay within a few percent. For a
before/after, INTERLEAVE the two builds (A, B, A, B, ...) and compare medians;
never compare a run today against a number from an hour ago.

For image comparisons see tools/visual_diff.py, which carries the determinism
recipe and refuses to report when its own noise floor is too high.

USAGE
-----
    from tools.perf_bench import run
    r = run({"JCE_SCENE": "...", "JCE_BENCH_AUTOSPAWN": "3,200000"},
            Path("out.log"), frames=500)
    r["frame"], r["draws"], r["phases"]["sr_loop"]

    python tools/perf_bench.py --scene street_demo/.../street.scene.json \\
        --spawn 3,200000 --runs 3
"""
import argparse
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build/desktop/windows-x64/release/jce_editor.exe"

PERF_RE = re.compile(
    r"engine: perf: ([\d.]+) ms avg \((\d+) FPS\) \| cpu ([\d.]+) / gpu ([\d.]+)"
    r" ms \| (\d+) draws \| gpu-mem (\d+) MB \(tex (\d+) \+ rt (\d+)\)"
    r" \| rss (\d+) MB \| commit (\d+)")
PHASE_RE = re.compile(r"perf-phases: avg-ms/frame over \d+: (.*?) at jce_engine")


def _editor_alive():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq jce_editor.exe", "/NH"],
                         capture_output=True, text=True).stdout
    return "jce_editor" in out


def wait_idle(timeout=180):
    """Refuse to start while another editor holds the single-instance lock."""
    deadline = time.time() + timeout
    while _editor_alive():
        if time.time() > deadline:
            raise RuntimeError("an editor is still running after %ds; refusing "
                               "to start (its lock would make this run a no-op "
                               "and we would read the OLD log)" % timeout)
        time.sleep(0.3)


def run(env_over, log_path, frames=400, timeout=1800):
    """One guarded run. Returns parsed metrics; raises rather than returning
    anything derived from a run that did not happen."""
    log_path = Path(log_path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    if log_path.exists():
        log_path.unlink()          # never parse a stale log
    wait_idle()

    env = dict(os.environ)
    # JCE_PERF_LOG is a BOOLEAN enable; the destination is JCE_LOG_FILE. Setting
    # the path in JCE_PERF_LOG enables logging and writes it somewhere else,
    # which is how the first version of this file raised "the run produced
    # nothing" on a run that had in fact produced a perfectly good log.
    env.update({
        "JCE_PERF_LOG": "1",
        "JCE_LOG_FILE": str(log_path),
        "JCE_MAX_FRAMES": str(frames),
        "JCE_FRAME_DT_FIXED": "0.0166667",   # frame-driven animation
    })
    env.update(env_over)                     # caller wins over the defaults
    subprocess.run([str(EXE)], env=env, cwd=str(ROOT),
                   capture_output=True, text=True, timeout=timeout)

    if not log_path.exists():
        raise RuntimeError("no log at %s -- the run produced nothing" % log_path)
    text = log_path.read_text(encoding="utf-8", errors="replace")
    if "single-instance lock already held" in text:
        raise RuntimeError("the run hit the single-instance lock and exited "
                           "without rendering; this log is NOT a measurement")

    rows = PERF_RE.findall(text)
    if not rows:
        raise RuntimeError("no perf line in %s -- the run did not reach steady "
                           "state" % log_path)
    last = rows[-1]
    out = {
        "frame": float(last[0]), "fps": int(last[1]),
        "cpu": float(last[2]), "gpu": float(last[3]),
        "draws": int(last[4]), "gpu_mem": int(last[5]),
        "tex": int(last[6]), "rt": int(last[7]),
        "rss": int(last[8]), "commit": int(last[9]),
        "phases": {},
    }
    ph = PHASE_RE.findall(text)
    if ph:
        out["phases"] = {k: float(v)
                         for k, v in re.findall(r"([a-z_0-9]+)=([\d.]+)", ph[-1])}
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scene", required=True)
    ap.add_argument("--spawn", help='benchmark autospawn, e.g. "3,200000"')
    ap.add_argument("--spread", help="JCE_BENCH_SPREAD world units")
    ap.add_argument("--env", action="append", default=[], metavar="KEY=VAL")
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--phase", action="append", default=[],
                    help="also report this perf-phase (repeatable)")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    out = Path(args.out) if args.out else ROOT / ".jce/perf_bench"
    env = {"JCE_SCENE": args.scene}
    if args.spawn:
        env["JCE_BENCH_AUTOSPAWN"] = args.spawn
        env["JCE_BENCH_ISOLATE"] = "1"
    if args.spread:
        env["JCE_BENCH_SPREAD"] = args.spread
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v

    frames, draws, phases = [], [], {p: [] for p in args.phase}
    for i in range(args.runs):
        r = run(env, out / ("run_%d.log" % i), frames=args.frames)
        frames.append(r["frame"])
        draws.append(r["draws"])
        for p in args.phase:
            phases[p].append(r["phases"].get(p, 0.0))
        print("  run %d: frame=%.2f ms  draws=%d" % (i, r["frame"], r["draws"]))

    print("frame median %.2f ms  (%s)" %
          (statistics.median(frames), " ".join("%.2f" % f for f in frames)))
    print("draws %s%s" % (draws, "" if len(set(draws)) == 1 else
                          "   <-- draw count VARIED: the runs are not "
                          "comparable, the camera moved"))
    for p in args.phase:
        print("%-12s median %.3f ms  (%s)" %
              (p, statistics.median(phases[p]),
               " ".join("%.3f" % x for x in phases[p])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
