"""Create a sparse progressive MP4 with packet offsets above a chosen gap.

For large-file media regressions only. Copies media unchanged, adjusts stco/co64
inside bounded moov metadata, and inserts an extended-size free box after ftyp.
The destination must be new. Fragmented input is deliberately unsupported.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import struct


def boxes(stream, start, end):
    position = start
    while position < end:
        stream.seek(position)
        header = stream.read(8)
        if len(header) != 8:
            raise ValueError("truncated box header")
        size, kind = struct.unpack(">I4s", header)
        if size == 1:
            raw = stream.read(8)
            if len(raw) != 8:
                raise ValueError("truncated extended box header")
            size = struct.unpack(">Q", raw)[0]
        elif not size:
            size = end - position
        if size < 8 or size > end - position:
            raise ValueError("invalid box extent")
        yield position, size, kind
        position += size


def adjust(data, start, end, gap, boundary):
    position = start
    while position < end:
        size, kind = struct.unpack_from(">I4s", data, position)
        header = 8
        if size == 1:
            size = struct.unpack_from(">Q", data, position + 8)[0]
            header = 16
        if not size:
            size = end - position
        if size < header or size > end - position:
            raise ValueError("invalid metadata box")
        body, following = position + header, position + size
        if kind in {b"moov", b"trak", b"mdia", b"minf", b"stbl"}:
            adjust(data, body, following, gap, boundary)
        elif kind in {b"stco", b"co64"}:
            count = struct.unpack_from(">I", data, body + 4)[0]
            width, fmt = (4, ">I") if kind == b"stco" else (8, ">Q")
            if count > (following - body - 8) // width:
                raise ValueError("invalid chunk offset table")
            for index in range(count):
                at = body + 8 + index * width
                offset = struct.unpack_from(fmt, data, at)[0]
                if offset >= boundary:
                    offset += gap
                struct.pack_into(fmt, data, at, offset)
        position = following


def mark_sparse(stream):
    if os.name != "nt":
        return
    import ctypes
    from ctypes import wintypes
    import msvcrt
    control = ctypes.WinDLL("kernel32", use_last_error=True).DeviceIoControl
    control.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID,
                        wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                        ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
    control.restype = wintypes.BOOL
    returned = wintypes.DWORD()
    if not control(msvcrt.get_osfhandle(stream.fileno()), 0x900C4,
                   None, 0, None, 0, ctypes.byref(returned), None):
        raise ctypes.WinError(ctypes.get_last_error())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", type=Path)
    ap.add_argument("destination", type=Path)
    ap.add_argument("--gap", type=int, default=3 * 1024**3)
    args = ap.parse_args()
    source, destination = args.source.resolve(strict=True), args.destination.resolve()
    if args.gap < 16 or destination.exists():
        ap.error("gap must be >=16 and destination must not exist")
    size = source.stat().st_size
    with source.open("rb") as src:
        layout = list(boxes(src, 0, size))
        if not layout or layout[0][2] != b"ftyp" or any(k == b"moof" for _, _, k in layout):
            ap.error("source must be a progressive MP4 starting with ftyp")
        boundary = layout[0][1]
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("xb") as dst:
            mark_sparse(dst)
            src.seek(0)
            dst.write(src.read(boundary))
            dst.write(struct.pack(">I4sQ", 1, b"free", args.gap))
            dst.seek(boundary + args.gap)
            src.seek(boundary)
            shutil.copyfileobj(src, dst, 64 * 1024)
            for position, count, kind in layout:
                if kind != b"moov":
                    continue
                if count > 32 * 1024**2:
                    raise ValueError("metadata exceeds fixture budget")
                src.seek(position)
                metadata = bytearray(src.read(count))
                adjust(metadata, 0, count, args.gap, boundary)
                dst.seek(position + args.gap)
                dst.write(metadata)
    print("sparse fixture:", destination, "logical bytes:", destination.stat().st_size)


if __name__ == "__main__":
    main()
