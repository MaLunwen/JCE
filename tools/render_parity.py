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


def read_png(path):
    """Return (width, height, channels, bytearray pixels). 8-bit only."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)

    pos = 8
    width = height = None
    bitdepth = ctype = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, bitdepth, ctype, _comp, _filt, interlace = \
                struct.unpack(">IIBBBBB", chunk)
            if bitdepth != 8 or ctype not in (2, 6) or interlace != 0:
                raise ValueError(
                    "%s: unsupported PNG (bitdepth=%d colortype=%d "
                    "interlace=%d); expected 8-bit RGB/RGBA progressive"
                    % (path, bitdepth, ctype, interlace))
        elif tag == b"IDAT":
            idat += chunk
        elif tag == b"IEND":
            break

    channels = 3 if ctype == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * channels)

    # Undo per-scanline filters (PNG spec types 0-4).
    src = 0
    for y in range(height):
        ftype = raw[src]
        src += 1
        line_off = y * stride
        prev_off = line_off - stride
        for x in range(stride):
            v = raw[src + x]
            a = out[line_off + x - channels] if x >= channels else 0
            b = out[prev_off + x] if y > 0 else 0
            c = (out[prev_off + x - channels]
                 if (y > 0 and x >= channels) else 0)
            if ftype == 0:
                r = v
            elif ftype == 1:
                r = (v + a) & 0xFF
            elif ftype == 2:
                r = (v + b) & 0xFF
            elif ftype == 3:
                r = (v + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                r = (v + pred) & 0xFF
            else:
                raise ValueError("%s: bad filter %d" % (path, ftype))
            out[line_off + x] = r
        src += stride
    return width, height, channels, out


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


def capture(exe, cwd, backend, frame, out_png, timeout_s):
    if os.path.exists(out_png):
        os.remove(out_png)
    env = dict(os.environ)
    env["JCE_BACKEND"] = backend
    env["JCE_CAPTURE_FRAME"] = str(frame)
    env["JCE_CAPTURE_PATH"] = os.path.abspath(out_png)

    print("[parity] %-7s launching %s (capture frame %d)"
          % (backend, os.path.basename(exe), frame))
    proc = subprocess.Popen([exe], cwd=cwd, env=env,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
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
                    return True       # written and stable
                last_size = size
            time.sleep(0.5)
        return os.path.exists(out_png) and os.path.getsize(out_png) > 0
    finally:
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
