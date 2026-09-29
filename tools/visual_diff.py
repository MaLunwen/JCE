#!/usr/bin/env python3
"""Deterministic A/B screenshot comparison for the editor.

WHY THIS EXISTS
---------------
Comparing two editor window captures is the only way to answer "did this
rendering change alter the image", and for a long while every such comparison
here was worthless. Two captures of the SAME BUILD differed across 12-30% of
pixels, so any real change hid under the floor and any noise looked like a
finding. Three separate causes, each of which had to be removed:

  1. The editor's own perf text. Diffing two captures of one build by region put
     ALL of the difference in the in-viewport stats overlay and three thin panel
     bands -- the 3D content was pixel-identical. Frame-time digits change every
     run. MASK_BANDS below excludes them.

  2. The debug camera spin was relative. `orbit_yaw += step` starts from
     whatever yaw is there, and the editor persists camera pose in per-user
     session state, so each run began where the previous one stopped: measured
     92.174 rad vs 94.258 rad, same build, same command. Fixed in
     jce_scene_render_camera.cpp -- the pose is now an absolute function of the
     step count. Nothing here can compensate for an old binary.

  3. The capture raced the pose. JCE_DBG_VISTA_SPIN advances per viewport
     render; JCE_WINCAP_FRAME counts editor updates, and the ratio is not fixed.
     JCE_DBG_VISTA_SPIN_CAPTURE triggers the capture from the spin's own
     counter, so pose and capture cannot drift apart.

Plus JCE_FRAME_DT_FIXED so animation is frame-driven, and JCE_STREAM_SYNC=1 so
world-chunk application stops racing the frame loop.

With all of that, the same build twice lands at 0.0019% of pixels over
threshold, down from 12.16%. That is the difference between an instrument and a
random number generator.

WHAT IT DOES NOT COVER
----------------------
The capture is the whole editor window, and the editor UI is not the subject.
MASK_BANDS removes the text that varies, but panel CONTENT varies between
sessions too -- one run measured a 4.1% floor with all of it in the inspector
column and the status bar, while the 3D area was clean. Chasing that with more
mask bands is chasing symptoms.

So the tool does not trust itself: it measures its own floor first and REFUSES
to report an A/B when that floor is too high (--max-floor). Pass --crop to
restrict the comparison to the 3D viewport, which is the reliable mode when a
layout puts live text outside the masked bands.

--crop cuts both ways: it can hide the signal as easily as the noise. The
far-cascade round-robin difference lives in a thin strip at the top of the
viewport (the horizon, which is what the far cascades cover) -- cropping to
y >= 260 reported it as 0.00000%, a clean pass for a change that does alter the
image. Choose the crop to CONTAIN the region under test, and prefer no crop
when the floor allows it.

graveyard and elemental_serenity do not repeat under this recipe at all; they
have their own nondeterminism (not investigated). Check with --floor-only
before trusting any comparison taken on them.

USAGE
-----
    # what does a switch cost visually?
    python tools/visual_diff.py --scene street_demo/.../street.scene.json \\
        --spawn 3,200000 --spread 4000 \\
        --b JCE_CSM_FAR_INTERVAL=3

    # is this scene even usable as a comparison surface?
    python tools/visual_diff.py --scene ... --floor-only

Exit code is 1 when the A/B difference exceeds --factor times the measured
noise floor, so this can gate a change once a scene is known to repeat.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jce_determinism import DETERMINISM, assert_has_content   # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from render_parity import read_png          # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build/desktop/windows-x64/release/jce_editor.exe"

# Regions holding live numeric text in the editor window capture. Located by
# diffing two captures of one build and profiling the difference by row and
# column -- 100% of it fell inside these. Coordinates are for the 2560x1600
# window the harness runs; a different window size needs them re-derived.
MASK_BANDS = [
    (440,   90,  960,  250),   # in-viewport stats overlay
    (0,   1150, 2560, 1200),   # panel readout bands
    (0,   1300, 2560, 1340),
    (0,   1560, 2560, 1600),
]

# The capture-determinism recipe is single-sourced in jce_determinism.py.
# It used to live here, which is why render_parity.py -- the sanctioned
# cross-backend instrument -- had none of it.

# Pose lock. JCE_BENCH_CAM rewrites the viewport pose every frame, so the
# capture no longer needs the spin trick to be deterministic -- and the spin
# trigger was firing at step 250 while the scene needed ~320 frames to come up,
# which is how three comparisons ended up photographing an empty viewport.
# Callers may override this to look somewhere else.
DEFAULT_CAM = "45,-25,120,0,0,0"


def _masked(x, y):
    for x0, y0, x1, y1 in MASK_BANDS:
        if x0 <= x < x1 and y0 <= y < y1:
            return True
    return False


def compare_masked(img_a, img_b, step=2, crop=None):
    """(mean_abs, pct_over_8, max_delta, sampled) over unmasked RGB pixels."""
    w, h, ca, pa = img_a
    w2, h2, cb, pb = img_b
    if (w, h) != (w2, h2):
        raise ValueError("size mismatch: %dx%d vs %dx%d" % (w, h, w2, h2))
    ry0, ry1 = (crop[1], min(h, crop[1] + crop[3])) if crop else (0, h)
    rx0, rx1 = (crop[0], min(w, crop[0] + crop[2])) if crop else (0, w)
    tot = over = sum_abs = max_d = 0
    for y in range(ry0, ry1, step):
        ra = y * w * ca
        rb = y * w2 * cb
        for x in range(rx0, rx1, step):
            if _masked(x, y):
                continue
            ia = ra + x * ca
            ib = rb + x * cb
            d0 = abs(pa[ia] - pb[ib])
            d1 = abs(pa[ia + 1] - pb[ib + 1])
            d2 = abs(pa[ia + 2] - pb[ib + 2])
            d = d0 + d1 + d2
            sum_abs += d
            if d > max_d:
                max_d = d
            if d0 > 8 or d1 > 8 or d2 > 8:
                over += 1
            tot += 1
    return (sum_abs / 3.0) / max(tot, 1), 100.0 * over / max(tot, 1), max_d, tot


# assert_has_content is single-sourced in jce_determinism.py.
def _editor_alive():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq jce_editor.exe", "/NH"],
                         capture_output=True, text=True).stdout
    return "jce_editor" in out


def capture(env_over, out_png, frames, log_path):
    """Run one capture. A run that did not happen must raise, not return a file.

    The editor holds a single-instance lock: a second process activates the
    existing window and exits 0 without rendering. Without the wait and the
    existence check below, the caller compares a stale PNG from an earlier build
    and reports it as a result.
    """
    for p in (out_png, log_path):
        if p.exists():
            p.unlink()
    for _ in range(600):
        if not _editor_alive():
            break
        time.sleep(0.3)
    else:
        raise RuntimeError("an editor is still running; refusing to start")

    env = dict(os.environ)
    env.update(DETERMINISM)
    env.setdefault("JCE_BENCH_CAM", DEFAULT_CAM)
    env.update(env_over)
    env["JCE_WINCAP_PATH"] = str(out_png)
    # Capture LATE. The scene has to be up before the shot is worth anything.
    env.setdefault("JCE_WINCAP_FRAME", str(max(1, frames - 40)))
    env["JCE_MAX_FRAMES"] = str(frames)
    # JCE_PERF_LOG is a BOOLEAN enable -- the engine only tests
    # `perf_env[0] != '0'` (jce_engine.c:519).  The destination is
    # JCE_LOG_FILE.  Putting the path in JCE_PERF_LOG enables logging and
    # writes it somewhere else; perf_bench.py:92 records the same trap and
    # the run it once made look empty.  Same recipe here.
    env["JCE_PERF_LOG"] = "1"
    env["JCE_LOG_FILE"] = str(log_path)
    subprocess.run([str(EXE)], env=env, cwd=str(ROOT),
                   capture_output=True, text=True, timeout=1800)
    if not out_png.exists():
        raise RuntimeError("no capture written to %s -- the run did not reach "
                           "the capture step" % out_png)
    assert_has_content(read_png(str(out_png)), str(out_png))
    return out_png


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scene", required=True)
    ap.add_argument("--spawn", help='benchmark autospawn, e.g. "3,200000"')
    ap.add_argument("--spread", help="JCE_BENCH_SPREAD world units")
    ap.add_argument("--a", action="append", default=[], metavar="KEY=VAL",
                    help="env for the baseline side (repeatable)")
    ap.add_argument("--b", action="append", default=[], metavar="KEY=VAL",
                    help="env for the candidate side (repeatable)")
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--factor", type=float, default=3.0,
                    help="fail when the A/B over-8 %% exceeds this many times "
                         "the measured noise floor (default 3)")
    ap.add_argument("--floor-reps", type=int, default=3, metavar="N",
                    help="measure the noise floor N times and keep the worst "
                         "(default 3). A bimodal floor certifies a comparison "
                         "on one sample and rejects it on the next, so one "
                         "pair is not enough to trust an A/B")
    ap.add_argument("--max-floor", type=float, default=0.01, metavar="PCT",
                    help="refuse to report an A/B when the measured noise floor "
                         "exceeds this over-8 %% (default 0.01)")
    ap.add_argument("--crop", metavar="X,Y,W,H",
                    help="compare only this rectangle -- use the 3D viewport "
                         "when editor panels sit outside the masked bands")
    ap.add_argument("--floor-only", action="store_true",
                    help="just measure whether this scene repeats at all")
    ap.add_argument("--out", default=None, help="directory for the PNGs")
    args = ap.parse_args()

    out = Path(args.out) if args.out else ROOT / ".jce/visual_diff"
    out.mkdir(parents=True, exist_ok=True)

    base = {"JCE_SCENE": args.scene, "JCE_BENCH_ISOLATE": "1"}
    if args.spawn:
        base["JCE_BENCH_AUTOSPAWN"] = args.spawn
    if args.spread:
        base["JCE_BENCH_SPREAD"] = args.spread

    def side(extra):
        e = dict(base)
        for kv in extra:
            k, _, v = kv.partition("=")
            e[k] = v
        return e

    crop = None
    if args.crop:
        crop = tuple(int(v) for v in args.crop.split(","))
        if len(crop) != 4:
            ap.error("--crop wants X,Y,W,H")

    a_env = side(args.a)
    # Sample the floor more than once. A floor is not always noise around a
    # centre -- this scene's is bimodal, and two runs of one configuration are
    # either byte-identical or they differ by 3.2587% (max delta 500) in one
    # block of the viewport. Sampling that once, the tool reads 0.00000% about
    # two times in three and then certifies an A/B whose own difference is
    # drawn from the same distribution. That is not a theoretical failure: it
    # is how the fused instance upload got charged with a 3.25859% "signal"
    # that was the scene's, not the change's. So take the WORST of several
    # floor pairs, and let one bad pair reject the whole comparison.
    reps = max(1, args.floor_reps)
    print("[visual_diff] noise floor: the same configuration, %d pair(s)" % reps)
    floor = None
    for r in range(reps):
        f0 = capture(a_env, out / ("floor_%d_0.png" % r), args.frames,
                     out / ("floor_%d_0.log" % r))
        f1 = capture(a_env, out / ("floor_%d_1.png" % r), args.frames,
                     out / ("floor_%d_1.log" % r))
        m = compare_masked(read_png(str(f0)), read_png(str(f1)), crop=crop)
        print("             pair %d  mean=%.5f  over-8=%.5f%%  max=%d  (%d px)"
              % ((r,) + m))
        if floor is None or m[1] > floor[1]:
            floor = m

    if args.floor_only:
        ok = floor[1] <= args.max_floor
        print("[visual_diff] %s as a comparison surface (worst pair %.5f%%)" %
              ("USABLE" if ok else "NOT USABLE -- this scene does not repeat",
               floor[1]))
        return 0 if ok else 1

    if floor[1] > args.max_floor:
        print("[visual_diff] ABORT: the worst floor pair is %.5f%%, above the "
              "%.5f%% this tool will divide by. Two runs of the SAME "
              "configuration already disagree that much, so any A/B taken now "
              "is meaningless -- and note that other pairs in this same run may "
              "have read 0.00000%%, which is what makes a single-pair floor "
              "dangerous rather than merely imprecise. Look at %s/floor_*.png "
              "to see what moved -- if it is editor UI outside the masked "
              "bands, re-run with --crop set to the 3D viewport."
              % (floor[1], args.max_floor, out))
        return 2

    print("[visual_diff] A/B")
    b = capture(side(args.b), out / "b.png", args.frames, out / "b.log")
    sig = compare_masked(read_png(str(f0)), read_png(str(b)), crop=crop)
    print("             mean=%.5f  over-8=%.5f%%  max=%d" % sig[:3])

    # Compare directly rather than dividing. A clean crop can put BOTH at
    # exactly zero, and 0/0 is not "infinitely worse" -- it is identical.
    budget = floor[1] * args.factor
    ratio = (sig[1] / floor[1]) if floor[1] > 0 else (0.0 if sig[1] == 0 else float("inf"))
    print("[visual_diff] signal %.5f%% vs budget %.5f%% (%.1fx floor)"
          % (sig[1], budget, ratio))
    if sig[1] > budget:
        print("[visual_diff] FAIL: the image changed beyond noise. Look at the "
              "PNGs in %s before deciding -- a pixel count under-reports a "
              "structured artifact." % out)
        return 1
    print("[visual_diff] OK: not distinguishable from noise")
    return 0


if __name__ == "__main__":
    sys.exit(main())
