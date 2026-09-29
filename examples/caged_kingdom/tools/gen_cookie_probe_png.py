#!/usr/bin/env python3
"""gen_cookie_probe_png.py -- author the light-cookie measurement fixtures.

A light cookie is a mask a light projects, so the only way to see whether one
reached the pixels is to project something unmistakable.  This writes two
256x256 black-and-white masks: a checker and vertical stripes.

TWO PATTERNS, AND THAT IS THE POINT.  "Every light has its OWN cookie" cannot
be tested with one image: two lights projecting the same mask look identical
whether each sampled its own atlas layer or both borrowed the single one a 2D
bind can carry.  With two, the single-bind failure is visible at a glance --
both patches show the same pattern.

Hand-rolled PNG (zlib + struct, no Pillow) so the fixtures regenerate on any
machine that can run the rest of the toolchain, and byte-deterministically --
no timestamps, no encoder version in the output.

Regenerate with:
    python examples/caged_kingdom/tools/gen_cookie_probe_png.py
"""
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "resources/assets/textures/cookie_probe.png"
OUT2 = ROOT / "resources/assets/textures/cookie_probe_b.png"

DIM = 256
CELL = 32   # 8x8 cells: big enough that a capture cannot mistake it for noise


def chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def render(kind: str) -> bytes:
    rows = []
    for y in range(DIM):
        row = bytearray([0])            # filter type 0 (None) per scanline
        for x in range(DIM):
            if kind == "checker":
                on = ((x // CELL) + (y // CELL)) % 2 == 0
            else:                       # vertical stripes
                on = (x // CELL) % 2 == 0
            v = 255 if on else 0
            row += bytes((v, v, v))
        rows.append(bytes(row))
    raw = b"".join(rows)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", DIM, DIM, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    return png


def main() -> int:
    for path, kind in ((OUT, "checker"), (OUT2, "stripes")):
        png = render(kind)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(png)
        print("wrote %s (%d bytes, %dx%d, %dpx %s)"
              % (path.relative_to(ROOT), len(png), DIM, DIM, CELL, kind))
    return 0


if __name__ == "__main__":
    sys.exit(main())
