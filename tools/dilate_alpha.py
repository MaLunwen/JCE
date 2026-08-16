#!/usr/bin/env python3
"""dilate_alpha.py -- flood opaque colour into transparent texels.

THE BUG THIS FIXES.  An alpha-masked texture whose transparent texels carry
dark RGB darkens as it recedes. Mip generation averages RGB and alpha
INDEPENDENTLY, so a mip texel covering 70% transparent-black and 30% opaque-leaf
gets roughly 30% of the leaf's colour while its alpha stays high enough to draw.
The object keeps its silhouette and loses its colour.

Measured on this project's tree textures, transparent RGB against opaque RGB:

    Leaf_Pine_C.png           (18.5, 31.9, 0)  vs  (51, 88,  0)   0.36x
    Leaves_GiantPine_C.png    (24.3, 43.4, 0)  vs  (61, 109, 0)   0.40x
    Leaves_TwistedTree_C.png  (64.8, 8.9, 8.9) vs  (167, 23, 23)  0.39x
    Leaves_NormalTree_C.png   (45.7, 63.8, 0)  vs  (88, 123, 0)   0.52x

with 62-75% of each texture transparent. That is a real latent hazard and worth
fixing on its own.

WHAT IT DOES NOT FIX, measured.  It was written to explain a distance band past
which foliage turns dark and trunks go from warm brown to cold grey, and it
does NOT explain it:

  * rendered A/B, same camera, dilated vs original: far/near foliage luminance
    ratio 0.786 against 0.788 -- no change;
  * offline, box-filtering each mip and measuring the luminance of texels that
    survive the material's real alphaCutoff of 0.2: identical at every level.

The reason is that these are FLAT-COLOUR masks. Leaves_NormalTree_C.png has
exactly ONE unique opaque RGB value (standard deviation 0), so averaging opaque
against opaque cannot shift the visible colour, and the texels that survive the
cutoff are opaque-dominated. Dilation removes a hazard that this content does
not currently trigger.

Applying it is therefore left to a deliberate choice rather than done here: it
rewrites art to fix something not currently observable.

THE FIX.  Push the nearest opaque colour outward into the transparent region
(dilation, "alpha bleeding", "solidify") so that averaging neighbours can only
ever mix leaf colour with leaf colour. ALPHA IS NOT TOUCHED -- the silhouette,
the cutout and every alpha test are bit-identical. Only texels that were
invisible change, and only in channels nothing samples directly.

Idempotent: running it twice is a no-op, because after the first pass there are
no transparent texels left holding a colour different from their neighbours.

Usage:
    python tools/dilate_alpha.py <dir-or-file> [more...] [--check]

--check reports what would change and exits non-zero if anything would, so it
can gate a build.
"""

import sys
from pathlib import Path

try:
    from PIL import Image
    import numpy as np
except ImportError:
    sys.exit("dilate_alpha needs Pillow and numpy")

ALPHA_OPAQUE = 8          # alpha above this counts as a colour source


def dilate(rgba: "np.ndarray") -> "np.ndarray":
    """Flood opaque RGB outward until every texel has a colour. Alpha unchanged.

    Push-pull (Gortler et al.), not an iterative neighbour spread: these masks
    are 2048x2048 with 70% transparent, and single-texel steps cannot cross
    that -- a 64-step spread moved Leaf_Pine_C's transparent mean from 16.8 to
    17.5 when the opaque mean is 46.3, i.e. it barely touched the region that
    matters. The pyramid converges everywhere in log2(size) levels.

    PUSH:  repeatedly halve the image, averaging colour weighted by coverage,
           so every level knows the mean colour of whatever it covers.
    PULL:  walk back down, filling uncovered texels from the level above.
    """
    h, w = rgba.shape[:2]
    known = (rgba[:, :, 3] > ALPHA_OPAQUE).astype(np.float32)
    if known.max() == 0.0 or known.min() == 1.0:
        return rgba                       # nothing to spread, or nothing to fill

    col = rgba[:, :, :3].astype(np.float32) * known[:, :, None]

    # ── push ────────────────────────────────────────────────────────
    pyr = [(col, known)]
    while pyr[-1][1].shape[0] > 1 and pyr[-1][1].shape[1] > 1:
        c, k = pyr[-1]
        hh, ww = c.shape[0] // 2 * 2, c.shape[1] // 2 * 2
        c, k = c[:hh, :ww], k[:hh, :ww]
        c2 = (c[0::2, 0::2] + c[1::2, 0::2] + c[0::2, 1::2] + c[1::2, 1::2])
        k2 = (k[0::2, 0::2] + k[1::2, 0::2] + k[0::2, 1::2] + k[1::2, 1::2])
        pyr.append((c2, k2))

    # ── pull ────────────────────────────────────────────────────────
    for lvl in range(len(pyr) - 1, 0, -1):
        c_up, k_up = pyr[lvl]
        c_dn, k_dn = pyr[lvl - 1]
        # Mean colour at the coarse level, where it has any coverage at all.
        safe = np.maximum(k_up, 1e-6)[:, :, None]
        mean_up = c_up / safe
        hh, ww = k_dn.shape[0] // 2 * 2, k_dn.shape[1] // 2 * 2
        big = np.repeat(np.repeat(mean_up, 2, 0), 2, 1)[:hh, :ww]
        cov = np.repeat(np.repeat((k_up > 0).astype(np.float32), 2, 0), 2, 1)[:hh, :ww]
        gap = (k_dn[:hh, :ww] <= 0) & (cov > 0)
        c_dn[:hh, :ww][gap] = big[gap]
        k_dn[:hh, :ww][gap] = 1.0

    filled, kf = pyr[0]
    out = rgba.copy()
    has = kf > 0
    rgb = np.zeros_like(filled)
    rgb[has] = filled[has] / kf[has][:, None]
    # Only transparent texels change; alpha and every opaque texel are
    # bit-identical, so the silhouette and every alpha test are unaffected.
    tr = rgba[:, :, 3] <= ALPHA_OPAQUE
    out[:, :, :3][tr] = np.clip(rgb[tr] + 0.5, 0, 255).astype(np.uint8)
    return out


def process(path: Path, check: bool) -> bool:
    """Returns True if the file changed (or would change under --check)."""
    try:
        im = Image.open(path)
    except Exception:
        return False
    if im.mode not in ("RGBA", "LA", "P"):
        return False
    im = im.convert("RGBA")
    a = np.asarray(im)
    if (a[:, :, 3] > ALPHA_OPAQUE).all():
        return False                       # nothing transparent to fill

    fixed = dilate(a)
    if np.array_equal(fixed, a):
        return False

    tr = a[:, :, 3] <= ALPHA_OPAQUE
    before = a[:, :, :3][tr].mean()
    after = fixed[:, :, :3][tr].mean()
    print(f"  {path.name:32s} transparent RGB mean {before:6.1f} -> {after:6.1f}"
          f"  ({100*tr.mean():.0f}% of texels)")
    if not check:
        Image.fromarray(fixed, "RGBA").save(path)
    return True


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check = "--check" in sys.argv
    if not args:
        sys.exit(__doc__)

    targets: list[Path] = []
    for a in args:
        p = Path(a)
        targets.extend(sorted(p.rglob("*.png")) if p.is_dir() else [p])

    print(("would change" if check else "dilating") + f" ({len(targets)} candidates):")
    changed = sum(process(t, check) for t in targets)
    print(f"{changed} file(s) {'need dilation' if check else 'dilated'}")
    return 1 if (check and changed) else 0


if __name__ == "__main__":
    sys.exit(main())
