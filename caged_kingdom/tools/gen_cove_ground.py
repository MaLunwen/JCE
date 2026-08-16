#!/usr/bin/env python3
"""gen_cove_ground.py -- the Hidden Cove terrain layer-0 albedo.

WHY THIS FILE EXISTS AT ALL.  The texture it produces was originally made by an
ad-hoc one-liner that was never saved. The asset was in the tree with no source,
which means nobody could answer "why does the ground look like that" or change
it without starting over. An asset whose generator is gone is not authored, it
is found.

WHAT THE SIZE IS FOR.  The terrain multiplies its UV by the layer tile scale, so
the texel density on the ground is

    texels per metre = resolution * tileScale / world_size

At 512 and tileScale 24 over a 400 m world that is 31 texels/m, and the tile
repeats every 16.7 m. Measured on a rotating camera, the ground was the largest
remaining flicker source in the scene -- motion-compensated residual over the
ground region:

    tileScale 24   5.672, 5.754
    tileScale  8   4.369, 4.561
    tileScale  2   2.507, 2.670

That is ordinary minification aliasing, not high-frequency content in the
texture: a radial FFT of the original puts 98.5% of its energy below 32 cycles
and 0.1% above 96. Anisotropic filtering is already forced on and the runtime
mip chain is already built. What was missing was texel density -- the tile was
being asked to cover too much ground.

So this generates at 1024 and the scene drops tileScale to 16. Measured, on the
canopy view, ground region:

    512  @ 24   flicker 5.672   ground detail 1.307
    1024 @ 24   flicker 5.010   ground detail 0.679
    1024 @ 16   flicker 4.270   ground detail 0.580   <- shipped
    1024 @ 12   flicker 3.699   ground detail 0.533

A 25% flicker reduction. STATE THE COST HONESTLY: the detail metric is lower
than the original texture's, and only part of that is the tiling -- at the SAME
tileScale of 24 the new texture still measures 0.679 against 1.307, so roughly
half the difference is this texture being smoother than the one it replaces.
That original was an unsaved one-liner and cannot be recovered to close the gap
exactly. Lower tiling is a real trade, not a free win, and calling it "sharpness
preserved" (as an earlier draft of this comment did) was wrong.

The octave count turned out NOT to matter: 4, 5, 6 and 7 octaves measured
3.764 / 3.729 / 3.699 / 3.685 flicker and 0.525 / 0.532 / 0.533 / 0.533 detail.
The finest octaves are below what the mip chain delivers at this distance, so
they cost generation time and change nothing on screen. 6 is kept because the
7th demonstrably buys nothing.

SEAMLESS.  Every octave wraps: the value noise is sampled on a lattice whose
period divides the image, and the lattice index is taken modulo that period, so
the two sides of the seam hash to the same corner. Without that a 24x tiling
shows a grid of seams, which reads as a broken texture rather than as a tiling
mistake.
"""

import math
import pathlib
import struct
import zlib

RES = 1024
OUT = (pathlib.Path(__file__).resolve().parents[1]
       / "resources" / "assets" / "textures" / "cove_ground.png")


def _hash(ix, iy, period, seed):
    """Lattice hash, wrapped to `period` so the image tiles exactly."""
    ix %= period
    iy %= period
    n = (ix * 374761393 + iy * 668265263 + seed * 144665) & 0xFFFFFFFF
    n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFF) / 65535.0


def _vnoise(u, v, freq, seed):
    """Value noise with a smoothstep fade, periodic with period `freq`."""
    x, y = u * freq, v * freq
    ix, iy = int(math.floor(x)), int(math.floor(y))
    fx, fy = x - ix, y - iy
    sx = fx * fx * (3.0 - 2.0 * fx)
    sy = fy * fy * (3.0 - 2.0 * fy)
    a = _hash(ix,     iy,     freq, seed)
    b = _hash(ix + 1, iy,     freq, seed)
    c = _hash(ix,     iy + 1, freq, seed)
    d = _hash(ix + 1, iy + 1, freq, seed)
    top = a + (b - a) * sx
    bot = c + (d - c) * sx
    return top + (bot - top) * sy


def _fbm(u, v, seed, octaves=6, base=4):
    """Octaves at 4, 8, 16, 32, 64, 128 cycles -- all integer, so all still tile.

    Stops at 6 because the 7th was measured and changed nothing: flicker 3.699
    -> 3.685, detail 0.533 -> 0.533. Those octaves sit below what the mip chain
    delivers at this tile scale, so they are generation cost with no on-screen
    effect either way.
    """
    total, amp, norm, freq = 0.0, 1.0, 0.0, base
    for _ in range(octaves):
        total += _vnoise(u, v, freq, seed) * amp
        norm += amp
        amp *= 0.5
        freq *= 2
    return total / norm


SAND = (0.847, 0.804, 0.690)
MOSS = (0.415, 0.510, 0.325)
DARK = (0.298, 0.353, 0.267)


def _write_png(path, rows, w, h):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b""))


def main():
    rows = []
    for j in range(RES):
        v = j / RES
        row = bytearray()
        for i in range(RES):
            u = i / RES
            # Patchy moss over sand, plus a broad tonal drift so the tile does
            # not read as one flat colour when it repeats.
            patch = _fbm(u, v, seed=11)
            drift = _vnoise(u, v, 4, seed=29)
            t = max(0.0, min(1.0, (patch - 0.42) * 2.6))
            shade = 0.86 + 0.28 * drift
            base = tuple(SAND[k] + (MOSS[k] - SAND[k]) * t for k in range(3))
            # A little of the darker green only in the densest moss, so the
            # patches have interior variation instead of a flat fill.
            deep = max(0.0, min(1.0, (patch - 0.62) * 3.2))
            base = tuple(base[k] + (DARK[k] - base[k]) * deep for k in range(3))
            for k in range(3):
                c = base[k] * shade
                row.append(max(0, min(255, int(c * 255.0 + 0.5))))
        rows.append(row)
    _write_png(OUT, rows, RES, RES)
    print(f"wrote {OUT}  ({RES}x{RES})")


if __name__ == "__main__":
    main()
