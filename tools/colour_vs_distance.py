#!/usr/bin/env python3
"""colour_vs_distance.py -- is the distant colour change explained by shadow?

WHY THIS EXISTS.  "Objects past a certain distance look darker and colder" was
measured for a long time along IMAGE ROWS, which on any tilted view is not
distance: the far hillside is farther AND faces the sun differently, and the two
effects are inseparable in a row profile.  Every conclusion drawn that way was
about an unknown mixture of the two.

This reads distance PER PIXEL out of the Scene Depth view and shadow PER PIXEL
out of the Shadow Mask view, then bins the shaded frame by actual distance.  It
answers the only question that matters here:

    does surface colour still change with distance at CONSTANT shadow?

If it does not, the distant colour shift is the shadow range running out, and
the fix is the cascade partition.  If it does, something else is distance-
dependent and the shadow work has not touched it.

The depth encoding's transfer curve is deliberately NOT assumed.  Any monotonic
encoding preserves ordering, so binning by the raw encoded value preserves both
the existence and the location of a step; the metre figures printed alongside
are reported under BOTH candidate transfers so a wrong guess cannot masquerade
as a measurement.
"""

import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

OUT = Path(__file__).resolve().parents[1] / "build/envshots"


def load(name):
    return np.asarray(Image.open(OUT / f"{name}.png").convert("RGB")).astype(np.float32)


def crop(img, rect):
    x, y, w, h = rect
    return img[y:y + h, x:x + w]


def read_rect(path):
    """The viewport rectangle, as the panel itself reported it."""
    txt = Path(path).read_text(encoding="utf-8").strip()
    parts = dict(p.split("=") for p in txt.split()[1:])
    return (int(parts["x"]), int(parts["y"]), int(parts["w"]), int(parts["h"]))


def controlled(rect, tag, L):
    """Colour vs distance with material, N.L and shadow all held constant.

    The uncontrolled version below answers a question nobody asked: on a tilted
    view, "farther" also means "different material, different slope, different
    trees". Every control here removes one of those, and what survives is the
    only thing that could be a rendering term.
    """
    sh = crop(load(f"{tag}_shaded"), rect)
    dp = crop(load(f"{tag}_depth"), rect)
    mk = crop(load(f"{tag}_shadowmask"), rect)
    nm = crop(load(f"{tag}_normals"), rect)
    R, G, B = dp[:, :, 0], dp[:, :, 1], dp[:, :, 2]
    geo = (np.abs(R - G) < 6) & (np.abs(G - B) < 6) & (R / 255.0 > 0.002) & (R / 255.0 < 0.998)
    d = R / 255.0
    shd = mk[:, :, 0] / 255.0
    n = nm / 255.0 * 2.0 - 1.0
    ln = np.linalg.norm(n, axis=2)
    ndl = (n @ np.asarray(L)) / np.maximum(ln, 1e-6)
    sR, sG, sB = sh[:, :, 0], sh[:, :, 1], sh[:, :, 2]
    m = (geo & (sG > sR + 4) & (sG > sB + 4)
         & (ln > 0.85) & (ln < 1.15)
         & (shd > 0.80) & (shd < 0.95)
         & (ndl >= 0.15) & (ndl < 0.45))
    out = []
    if m.sum() < 900:
        return out
    dd, ww, ll, nn = d[m], (sR - sB)[m], sh[:, :, :3].mean(2)[m], ndl[m]
    for lo, hi in ((0.25, 0.35), (0.35, 0.45), (0.45, 0.55)):
        q = (dd >= lo) & (dd < hi)
        if q.sum() < 150:
            continue
        out.append((0.5 * (lo + hi), int(q.sum()), float(nn[q].mean()),
                    float(ll[q].mean()), float(ww[q].mean())))
    return out


def main():
    # `--controlled A B sunX sunY sunZ` compares two captures of the same scene
    # from DIFFERENT camera positions, with everything else held constant.
    #
    # This is the test that settles whether a colour trend along distance is a
    # rendering term at all. A rendering term is a function of distance, so it
    # must give the same colour at the same distance no matter where the camera
    # stands. If the two curves do not coincide, the trend is carried by WHICH
    # objects occupy that distance band from that viewpoint -- scene content,
    # not shading. Both captures must report the same shadow_far (check
    # JCE_DBG_CSM_LOG), or "the same depth" is not the same distance.
    if len(sys.argv) > 2 and sys.argv[2] == "--controlled":
        rect = read_rect(sys.argv[1])
        a, b = sys.argv[3], sys.argv[4]
        L = [float(v) for v in sys.argv[5:8]]
        print("green foliage, one material class, N.L 0.15-0.45, shadow 0.80-0.95")
        print(f"  {'capture':>10s} {'depth':>7s} {'n':>6s} {'N.L':>7s} {'luma':>7s} {'warmth':>7s}")
        rows = {}
        for tag in (a, b):
            rows[tag] = controlled(rect, tag, L)
            for r in rows[tag]:
                print(f"  {tag:>10s} {r[0]:7.2f} {r[1]:6d} {r[2]:+7.3f} {r[3]:7.1f} {r[4]:+7.2f}")
        common = {r[0] for r in rows[a]} & {r[0] for r in rows[b]}
        if common:
            print()
            print("  same distance, different camera position:")
            for dep in sorted(common):
                wa = next(r[4] for r in rows[a] if r[0] == dep)
                wb = next(r[4] for r in rows[b] if r[0] == dep)
                print(f"    depth {dep:.2f}   warmth {wa:+6.2f} vs {wb:+6.2f}   "
                      f"difference {wb - wa:+6.2f}")
            print("  a rendering term that depends on distance cannot produce a")
            print("  difference here; scene content can.")
        return

    if len(sys.argv) < 5:
        sys.exit("usage: colour_vs_distance.py <rect.txt> <shaded> <depth> <mask> [far_m]\n"
                 "       colour_vs_distance.py <rect.txt> --controlled <tagA> <tagB> <sunX> <sunY> <sunZ>")
    rect = read_rect(sys.argv[1])
    shaded = crop(load(sys.argv[2]), rect)
    depth = crop(load(sys.argv[3]), rect)
    mask = crop(load(sys.argv[4]), rect)
    far_m = float(sys.argv[5]) if len(sys.argv) > 5 else None
    print(f"viewport rect x={rect[0]} y={rect[1]} w={rect[2]} h={rect[3]}")

    # Geometry = where the depth view is greyscale.  Sky, water and any shader
    # that ignores the view mode are excluded BY the view, not by a guess about
    # where they are on screen -- which is the whole reason it emits grey.
    dR, dG, dB = depth[:, :, 0], depth[:, :, 1], depth[:, :, 2]
    grey = (np.abs(dR - dG) < 6) & (np.abs(dG - dB) < 6)
    d = dR / 255.0
    geo = grey & (d > 0.002) & (d < 0.998)
    n = int(geo.sum())
    print(f"geometry pixels (greyscale in the depth view): {n}  "
          f"({100 * geo.mean():.1f}% of viewport)")
    if n < 5000:
        sys.exit("too few geometry pixels -- the depth view did not take")

    warm = (shaded[:, :, 0] - shaded[:, :, 2])[geo]      # R-B: warm sun vs blue ambient
    luma = shaded[:, :, :3].mean(2)[geo]
    sh = (mask[:, :, 0] / 255.0)[geo]                    # 1 = lit, 0 = occluded
    dd = d[geo]

    print()
    print("Binned by ACTUAL per-pixel distance (not image row).")
    if far_m:
        print(f"  depth 1.0 = shadow far plane = {far_m:.0f} m")
        print("  metres shown for both candidate encodings (linear | sRGB-ish)")
    print()
    hdr = f"{'depth':>6s} {'px':>7s} {'shadow':>7s} {'warmth':>7s} {'luma':>7s}"
    if far_m:
        hdr += f"  {'~m lin':>7s} {'~m srgb':>8s}"
    print(hdr)
    edges = np.linspace(dd.min(), dd.max(), 17)
    rows = []
    for i in range(16):
        m = (dd >= edges[i]) & (dd < edges[i + 1])
        c = int(m.sum())
        if c < 300:
            continue
        mid = 0.5 * (edges[i] + edges[i + 1])
        row = (mid, c, float(sh[m].mean()), float(warm[m].mean()), float(luma[m].mean()))
        rows.append(row)
        line = f"{mid:6.3f} {c:7d} {row[2]:7.3f} {row[3]:+7.2f} {row[4]:7.1f}"
        if far_m:
            line += f"  {mid * far_m:7.1f} {(mid ** 2.2) * far_m:8.1f}"
        print(line)

    if len(rows) < 4:
        sys.exit("not enough populated bins")

    print()
    print("Is the warmth trend explained by the shadow trend?")
    a = np.array(rows)
    dw = a[-1, 3] - a[0, 3]
    ds = a[-1, 2] - a[0, 2]
    print(f"  near->far warmth change : {dw:+.2f} R-B levels")
    print(f"  near->far shadow change : {ds:+.3f} (1 = fully lit)")

    # The decisive cut: hold shadow roughly constant and look again.  If warmth
    # still moves with distance among pixels that are equally lit, the shadow
    # range is not the whole story.
    print()
    print("Same trend among pixels that are ALL fully lit (shadow > 0.95):")
    lit = sh > 0.95
    if lit.sum() < 2000:
        print("  too few fully-lit pixels to say")
    else:
        e2 = np.linspace(dd[lit].min(), dd[lit].max(), 9)
        prev = None
        for i in range(8):
            m = lit & (dd >= e2[i]) & (dd < e2[i + 1])
            if m.sum() < 300:
                continue
            mid = 0.5 * (e2[i] + e2[i + 1])
            w = float(warm[m].mean())
            step = "" if prev is None else f"  step {w - prev:+6.2f}"
            print(f"  depth {mid:5.3f}  n={int(m.sum()):7d}  warmth {w:+6.2f}"
                  f"  luma {float(luma[m].mean()):6.1f}{step}")
            prev = w


if __name__ == "__main__":
    main()
