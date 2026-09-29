#!/usr/bin/env python3
"""render_parity.py - cross-backend render consistency check.

Boots the SAME binary once per renderer backend, captures the SAME bgfx
frame index on each (via the engine's JCE_BACKEND / JCE_CAPTURE_FRAME /
JCE_CAPTURE_PATH env hooks, see jce_renderer.c), then pixel-compares the
captures. Catches the class of bug that compiles everywhere but renders
differently per backend (e.g. the fs_pbr mat3 TBN transpose that cut
every light pool in half on OpenGL only, 2026-06-10).

Typical use (from the repo root, after a build):

    python tools/render_parity.py --backends d3d11,opengl
    python tools/render_parity.py --backends d3d11,opengl,vulkan ^
        --exe build/desktop/windows-x64/release/jce_editor.exe --frame 600

Notes
-----
* The scene that renders at startup is whatever the exe loads by default
  (the editor restores its last scene). Use a STATIC scene for tight
  thresholds: animated content (particles, time-of-day, video) diffs
  legitimately. Async asset streaming settles within a few hundred
  frames - capture late (default --frame 600).
* Backends legitimately differ by tiny amounts (rasterization rules,
  precision, dither). Thresholds are therefore statistical, not exact.
* Editor UI overlays (FPS counter etc.) cause small localized diffs;
  --crop x,y,w,h can restrict the comparison to the viewport.

Exit code: 0 = within thresholds, 1 = divergence, 2 = setup error.
Stdlib only (self-contained PNG reader: 8-bit RGB/RGBA, non-interlaced
- exactly what the engine's SDL_image-backed screenshot writer emits).
"""

import argparse
import os
import struct
import subprocess
import sys
import time
import zlib


# ── minimal PNG reader ──────────────────────────────────────────────




# ── comparison ──────────────────────────────────────────────────────


def compare(img_a, img_b, crop, step):
    """Return (mean_abs, pct_over_8, max_delta) over sampled RGB pixels."""
    wa, ha, ca, pa = img_a
    wb, hb, cb, pb = img_b
    if (wa, ha) != (wb, hb):
        raise ValueError("size mismatch: %dx%d vs %dx%d" % (wa, ha, wb, hb))

    x0, y0, x1, y1 = 0, 0, wa, ha
    if crop:
        x0, y0, cw, ch = crop
        x1, y1 = min(wa, x0 + cw), min(ha, y0 + ch)

    total = 0
    sum_abs = 0
    over8 = 0
    max_d = 0
    for y in range(y0, y1, step):
        ra = y * wa * ca
        rb = y * wb * cb
        for x in range(x0, x1, step):
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
                over8 += 1
            total += 1
    mean_abs = (sum_abs / 3.0) / max(total, 1)
    return mean_abs, 100.0 * over8 / max(total, 1), max_d


# ── capture orchestration ───────────────────────────────────────────


import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).resolve().parent))
from jce_determinism import (DETERMINISM, assert_backend, read_png,   # noqa: E402
                             assert_has_content)


def _verify_backend(log_path, backend):
    """Fail loudly when the engine did not use the backend we asked for."""
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError as exc:
        raise RuntimeError(
            "no captured stdout at %s for backend %r (%s) -- cannot prove "
            "which backend produced this capture" % (log_path, backend, exc))
    got = assert_backend(text, backend)
    print("[parity] %-7s engine reported: %s" % (backend, got))


def capture(exe, cwd, backend, frame, out_png, timeout_s):
    if os.path.exists(out_png):
        os.remove(out_png)
    env = dict(os.environ)
    # Pin determinism.  Without these two captures of ONE backend differ by
    # more than this tool's default 3.0% threshold -- i.e. the instrument
    # was reading below its own noise floor (3.2587%, measured).
    env.update(DETERMINISM)
    env["JCE_BACKEND"] = backend
    env["JCE_CAPTURE_FRAME"] = str(frame)
    env["JCE_CAPTURE_PATH"] = os.path.abspath(out_png)
    # JCE_BACKEND is a REQUEST; bgfx falls back down a chain when a backend
    # fails to init.  Capture STDOUT so the caller can assert what was ACTUALLY
    # selected -- otherwise a "parity" run can compare a backend against itself
    # and report perfect agreement.
    #
    # STDOUT, not JCE_LOG_FILE: the "renderer: <name>" line is emitted during
    # bgfx init, BEFORE the log file takes over.  Measured 2026-08-27 on one
    # run: present in stdout (138 lines), absent from the log file (70 lines).
    # Redirected to a file rather than a pipe because this function polls the
    # process for up to `timeout_s` -- a pipe that fills would deadlock it.
    log_path = os.path.abspath(out_png) + ".stdout.txt"
    if os.path.exists(log_path):
        os.remove(log_path)

    print("[parity] %-7s launching %s (capture frame %d)"
          % (backend, os.path.basename(exe), frame))
    log_fh = open(log_path, "w", encoding="utf-8", errors="replace")
    proc = subprocess.Popen([exe], cwd=cwd, env=env,
                            stdout=log_fh, stderr=subprocess.STDOUT)
    try:
        deadline = time.time() + timeout_s
        last_size = -1
        while time.time() < deadline:
            if proc.poll() is not None:
                print("[parity] %-7s exe exited early (code %s)"
                      % (backend, proc.returncode))
                break
            if os.path.exists(out_png):
                size = os.path.getsize(out_png)
                if size > 0 and size == last_size:
                    # The process is still alive; flush what it has written so
                    # far before parsing.  The renderer line is emitted long
                    # before the capture frame, so it is already there.
                    log_fh.flush()
                    _verify_backend(log_path, backend)
                    return True       # written and stable
                last_size = size
            time.sleep(0.5)
        log_fh.flush()
        ok = os.path.exists(out_png) and os.path.getsize(out_png) > 0
        if ok:
            _verify_backend(log_path, backend)
        return ok
    finally:
        log_fh.close()
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser(
        description="cross-backend render parity check")
    ap.add_argument("--exe",
                    default="build/desktop/windows-x64/release/jce_editor.exe")
    ap.add_argument("--backends", default="d3d11,opengl",
                    help="comma list; first entry is the reference")
    ap.add_argument("--frame", type=int, default=600,
                    help="bgfx frame index to capture (late = streaming settled)")
    ap.add_argument("--timeout", type=int, default=120,
                    help="seconds to wait per backend")
    ap.add_argument("--out", default="build/parity")
    ap.add_argument("--crop", default=None,
                    help="x,y,w,h region to compare (e.g. the 3D viewport)")
    ap.add_argument("--step", type=int, default=4,
                    help="sample every Nth pixel (1 = exhaustive)")
    ap.add_argument("--threshold-mean", type=float, default=2.0,
                    help="max mean abs per-channel diff (0-255 scale)")
    ap.add_argument("--skip-noise-floor", action="store_true",
                    help="do not re-capture the reference backend to measure "
                         "this run's same-backend floor (saves one capture; "
                         "the verdict is then unanchored)")
    ap.add_argument("--threshold-pct", type=float, default=3.0,
                    help="max %% of sampled pixels with channel delta > 8")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("[parity] exe not found: %s" % exe)
        return 2
    cwd = os.getcwd()
    backends = [b.strip() for b in args.backends.split(",") if b.strip()]
    if len(backends) < 2:
        print("[parity] need at least 2 backends")
        return 2
    crop = None
    if args.crop:
        crop = tuple(int(v) for v in args.crop.split(","))
        if len(crop) != 4:
            print("[parity] --crop expects x,y,w,h")
            return 2

    os.makedirs(args.out, exist_ok=True)
    shots = {}
    for b in backends:
        png = os.path.join(args.out, "parity_%s.png" % b)
        if not capture(exe, cwd, b, args.frame, png, args.timeout):
            print("[parity] %-7s FAILED to produce a capture" % b)
            return 2
        shots[b] = png
        print("[parity] %-7s captured -> %s" % (b, png))

    ref = backends[0]
    print("[parity] decoding %s (reference)" % ref)
    ref_img = read_png(shots[ref])
    # Blank captures compare perfectly equal.  visual_diff.py passed vacuously
    # three times that way before it grew this guard; this tool never had it.
    for b in backends:
        assert_has_content(read_png(shots[b]) if b != ref else ref_img, shots[b])

    # SAME-BACKEND noise floor, measured on THIS run.  A cross-backend verdict
    # is only meaningful above the spread one backend shows against itself --
    # and this tool's default threshold (3.0%) used to sit BELOW the 3.2587%
    # floor that unpinned TAA jitter produced.  Refuse to answer rather than
    # quietly report noise as agreement.
    floor_mean = floor_pct = 0.0
    if not args.skip_noise_floor:
        rep = os.path.join(args.out, "parity_%s_repeat.png" % ref)
        if not capture(exe, cwd, ref, args.frame, rep, args.timeout):
            print("[parity] %-7s FAILED to produce the noise-floor capture" % ref)
            return 2
        rep_img = read_png(rep)
        assert_has_content(rep_img, rep)
        floor_mean, floor_pct, floor_max = compare(ref_img, rep_img, crop, args.step)
        print("[parity] noise floor (%s vs itself): mean=%.3f  pixels>8=%.2f%%  "
              "max=%d" % (ref, floor_mean, floor_pct, floor_max))
        if floor_mean > args.threshold_mean or floor_pct > args.threshold_pct:
            print("[parity] SETUP ERROR: the configured thresholds "
                  "(mean<=%.3f, pct<=%.2f%%) are BELOW this run's own noise "
                  "floor.  Any verdict at these settings would be reading "
                  "noise.  Raise the thresholds above the floor, or find out "
                  "why one backend does not reproduce itself."
                  % (args.threshold_mean, args.threshold_pct))
            return 2

    failed = False
    for b in backends[1:]:
        img = read_png(shots[b])
        mean_abs, pct8, max_d = compare(ref_img, img, crop, args.step)
        ok = (mean_abs <= args.threshold_mean
              and pct8 <= args.threshold_pct)
        print("[parity] %s vs %s: mean=%.3f  pixels>8=%.2f%%  max=%d  -> %s"
              % (ref, b, mean_abs, pct8, max_d, "OK" if ok else "DIVERGED"))
        if not ok:
            failed = True

    if failed:
        print("[parity] DIVERGENCE detected - inspect the PNGs in %s"
              % args.out)
        return 1
    print("[parity] all backends consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
