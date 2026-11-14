"""Owned Game View PNG receipts for the visible Editor diagnostic.

Only the native scheduled Game target is requested. This helper takes no OS
screenshots, changes no rendering policy, and never invents pixel evidence.
"""
from __future__ import annotations

import hashlib
import math
import os
from pathlib import Path
import re
import struct
import zlib


SHOT_SECONDS = (6, 12, 24, 36, 42, 48, 54, 60)
MAX_PNG_BYTES = 64 * 1024 * 1024
MAX_PIXELS = 32 * 1024 * 1024
_UINT64_MAX = (1 << 64) - 1
_DECIMAL = r"(?:0|[1-9][0-9]*)"
_SECONDS = r"(?:[0-9]+(?:\.[0-9]+)?)"
_ADMIT = re.compile(
    r"Game View capture admitted source_frame=(" + _DECIMAL + r") "
    r"submit_frame=(" + _DECIMAL + r") due_seconds=(" + _SECONDS + r") "
    r"admitted_seconds=(" + _SECONDS + r") counter=(" + _DECIMAL + r") "
    r"frequency=(" + _DECIMAL + r") path=(.+)$")
_TERMINAL = re.compile(
    r"Game View capture terminal success=([01]) source_frame=(" + _DECIMAL + r") "
    r"submit_frame=(" + _DECIMAL + r") counter=(" + _DECIMAL + r") "
    r"frequency=(" + _DECIMAL + r") path=(.+)$")
_ENDED = re.compile(r"Game View capture schedule ended reason=(stopped|shutdown) "
                    r"elapsed_seconds=(" + _SECONDS + r")$")


def plan(out):
    """Pure, bounded request construction; no files or directories are created."""
    out = Path(out).resolve()
    shots = []
    for seconds in SHOT_SECONDS:
        relative = f"screenshots/play_{seconds:02d}s.png"
        path = str(out / relative)
        if any(char in path for char in (";", "|", "\r", "\n")) or len(path.encode("utf-8")) >= 1024:
            raise ValueError("screenshot output path cannot be represented by the native schedule")
        shots.append({"id": f"play_{seconds:02d}s", "due_play_seconds": seconds,
                      "relative_path": relative, "native_path": path})
    return shots


def environment(shots):
    if not shots:
        return {}
    return {"JCE_KPI_GAME_SHOTS": ";".join(
                f"{shot['due_play_seconds']}|{shot['native_path']}" for shot in shots),
            "JCE_KPI_GAME_CAPTURE_SIZE": "natural"}


def png_receipt(path):
    """Check bounded native RGB/RGBA PNG structure, CRCs and deflated scanlines.

    Successful decoding does not establish useful, nonblank or correct pixels.
    No image library or platform capture API is needed.
    """
    path = Path(path)
    if path.is_symlink() or path.parent.is_symlink() or not path.is_file():
        raise ValueError("PNG is missing or is not an owned regular file")
    if not 8 <= path.stat().st_size <= MAX_PNG_BYTES:
        raise ValueError("PNG byte count is outside the bounded limit")
    data = path.read_bytes()
    if len(data) > MAX_PNG_BYTES or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("PNG signature or byte count is invalid")
    position, count, ended, idat_closed = 8, 0, False, False
    dimensions = None
    idat = []
    channels = None
    while position < len(data):
        if len(data) - position < 12 or count >= 4096:
            raise ValueError("PNG chunk framing is invalid")
        length = struct.unpack_from(">I", data, position)[0]
        kind = data[position + 4:position + 8]
        stop = position + 12 + length
        if stop > len(data) or not re.fullmatch(b"[A-Za-z]{4}", kind):
            raise ValueError("PNG chunk length or type is invalid")
        payload = data[position + 8:position + 8 + length]
        crc = struct.unpack_from(">I", data, position + 8 + length)[0]
        if zlib.crc32(kind + payload) & 0xffffffff != crc:
            raise ValueError("PNG chunk CRC mismatch")
        if count == 0 and kind != b"IHDR":
            raise ValueError("PNG does not begin with IHDR")
        if kind == b"IHDR":
            if dimensions is not None or length != 13:
                raise ValueError("PNG IHDR is invalid")
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            if not width or not height or width * height > MAX_PIXELS or depth != 8 or color not in (2, 6) or any((compression, filtering, interlace)):
                raise ValueError("PNG is outside the native RGB/RGBA noninterlaced contract")
            dimensions, channels = (width, height), 3 if color == 2 else 4
        elif kind == b"IDAT":
            if idat_closed:
                raise ValueError("PNG IDAT sequence is not contiguous")
            idat.append(payload)
        elif kind == b"IEND":
            if length or not idat or stop != len(data):
                raise ValueError("PNG IEND or trailing bytes are invalid")
            ended = True
        elif not kind[0] & 32 and kind != b"PLTE":
            raise ValueError("PNG has an unsupported critical chunk")
        if idat and kind != b"IDAT":
            idat_closed = True
        count += 1
        position = stop
    if not ended or dimensions is None:
        raise ValueError("PNG is incomplete")
    width, height = dimensions
    stride = width * channels + 1
    expected = height * stride
    decoder = zlib.decompressobj()
    pixels = decoder.decompress(b"".join(idat), expected + 1)
    if len(pixels) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise ValueError("PNG pixel stream is incomplete or exceeds its dimensions")
    if any(pixels[row * stride] > 4 for row in range(height)):
        raise ValueError("PNG scanline filter is invalid")
    return {"status": "valid", "width": width, "height": height,
            "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "validation": "PNG CRC and bounded pixel-stream validation; visual content unreviewed"}


def _path_key(path):
    # The native schedule repeats the supplied path; tolerate Windows separators
    # and case while matching only the eight paths created by this invocation.
    return str(path).replace("\\", "/").casefold() if os.name == "nt" else str(path)


def _payload(line):
    parts = line.rsplit(" at ", 1)
    return parts[0] if len(parts) == 2 and re.fullmatch(r"[^\r\n]+:[0-9]+", parts[1]) else line


def _counter(value, *, positive=False, maximum=_UINT64_MAX):
    parsed = int(value)
    if parsed < (1 if positive else 0) or parsed > maximum:
        raise ValueError("native screenshot integer is out of range")
    return parsed


def collect(out, shots, log_text, contexts=None):
    """Join owned native receipts and actual files; retain unavailable outcomes."""
    out = Path(out).resolve()
    by_path = {_path_key(shot["native_path"]): shot for shot in shots}
    admissions = {shot["id"]: [] for shot in shots}
    terminals = {shot["id"]: [] for shot in shots}
    ends, issues = [], []
    for line_number, line in enumerate(log_text.splitlines(), 1):
        if "Game View capture " not in line:
            continue
        payload = _payload(line)
        admit, terminal, ended = _ADMIT.search(payload), _TERMINAL.search(payload), _ENDED.search(payload)
        try:
            if (admit or terminal or ended) and "[MAIN]" not in line:
                raise ValueError("native capture receipt is not on the main thread")
            if ended:
                elapsed = float(ended[2])
                if not math.isfinite(elapsed):
                    raise ValueError("nonfinite schedule elapsed time")
                ends.append({"line": line_number, "reason": ended[1], "elapsed_play_seconds": elapsed})
                continue
            if not admit and not terminal:
                if "Game View capture admitted " in line or "Game View capture terminal " in line:
                    raise ValueError("unrecognized native capture receipt")
                continue
            fields = admit.groups() if admit else terminal.groups()
            shot = by_path.get(_path_key(fields[-1]))
            if shot is None:
                raise ValueError("capture receipt does not name an owned output")
            if admit:
                source, submit, due, elapsed, counter, frequency, _ = fields
                due, elapsed = float(due), float(elapsed)
                if not all(math.isfinite(v) for v in (due, elapsed)) or abs(due-shot["due_play_seconds"]) > 1e-6 or elapsed < due:
                    raise ValueError("native admission does not match the requested Play clock")
                receipt = {"line": line_number, "source_frame": _counter(source, maximum=0xffffffff),
                           "submit_frame": _counter(submit, maximum=0xffffffff),
                           "due_play_seconds": due, "admitted_play_seconds": elapsed,
                           "counter": str(_counter(counter)), "frequency_hz": str(_counter(frequency, positive=True))}
                admissions[shot["id"]].append(receipt)
            else:
                success, source, submit, counter, frequency, _ = fields
                receipt = {"line": line_number, "success": success == "1",
                           "source_frame": _counter(source, maximum=0xffffffff),
                           "submit_frame": _counter(submit, maximum=0xffffffff),
                           "counter": str(_counter(counter)), "frequency_hz": str(_counter(frequency, positive=True))}
                terminals[shot["id"]].append(receipt)
        except ValueError as exc:
            issues.append({"line": line_number, "reason": str(exc)})

    if len(ends) > 1:
        issues.append({"reason": "multiple terminal schedule receipts"})
    schedule_end = ends[0] if len(ends) == 1 else None
    records, windows, archive_members = [], [], []
    for shot in shots:
        row = {k: v for k, v in shot.items() if k != "native_path"}
        a, t = admissions[shot["id"]], terminals[shot["id"]]
        row.update(admissions=a, terminals=t)
        valid = len(a) == 1 and len(t) <= 1
        if len(a) > 1 or len(t) > 1 or (t and not a):
            row["status"] = "ambiguous_receipts"
            issues.append({"shot": shot["id"], "reason": "duplicate or orphan capture receipt"})
        elif not a:
            row["status"] = ("not_reached" if schedule_end and schedule_end["elapsed_play_seconds"] < shot["due_play_seconds"]
                             else "not_admitted" if schedule_end else "unobserved")
        else:
            admission = a[0]
            terminal = t[0] if t else None
            if terminal and (terminal["frequency_hz"] != admission["frequency_hz"] or
                    any(terminal[key] != admission[key] for key in ("source_frame", "submit_frame")) or
                    terminal["line"] <= admission["line"] or int(terminal["counter"]) < int(admission["counter"])):
                valid = False
                issues.append({"shot": shot["id"], "reason": "native capture terminal identity or clock mismatch"})
                row["status"] = "ambiguous_receipts"
            else:
                row["status"] = "completed_file_unverified" if terminal and terminal["success"] else "failed" if terminal else "unresolved"
                if terminal:
                    row["observed_capture_latency_ms"] = ((int(terminal["counter"])-int(admission["counter"]))*1000/int(admission["frequency_hz"]))
            if valid:
                windows.append({"shot": shot["id"], "begin": admission["counter"],
                                "end": terminal["counter"] if terminal else None,
                                "frequency_hz": admission["frequency_hz"],
                                "closure": "terminal_observed" if terminal else "unresolved_through_capture_end"})
        path = out / shot["relative_path"]
        try:
            if path.parent.resolve() != out / "screenshots":
                raise ValueError("PNG parent escaped the owned screenshot directory")
            row["file"] = png_receipt(path)
        except (OSError, ValueError, zlib.error) as exc:
            row["file"] = {"status": "missing" if not path.exists() else "invalid", "reason": str(exc)}
        if row["status"] == "completed_file_unverified":
            row["status"] = "complete" if row["file"]["status"] == "valid" else "file_missing_or_invalid"
        if row["file"]["status"] == "valid":
            row["file"]["receipt_verified"] = row["status"] == "complete"
            archive_members.append(shot["relative_path"])
            if not valid:
                issues.append({"shot": shot["id"],
                               "reason": "Valid owned PNG lacks a unique valid native admission; its activity cannot be bounded on the frame clock"})
        records.append(row)

    excluded = set()
    aligned = not issues and (not shots or bool(schedule_end) or bool(windows))
    if contexts is not None and windows:
        first, last = int(contexts[0]["clock"]["begin"]), int(contexts[-1]["clock"]["end"])
        frequency = contexts[0]["clock"]["frequency_hz"]
        for window in windows:
            begin = int(window["begin"])
            end = int(window["end"]) if window["end"] is not None else last
            if window["frequency_hz"] != frequency or not first <= begin <= last:
                aligned = False
                issues.append({"shot": window["shot"], "reason": "capture counter cannot be joined to this native frame clock"})
                continue
            window["last_observed_end"] = str(min(end, last))
            for index, context in enumerate(contexts):
                clock = context["clock"]
                if int(clock["begin"]) <= end and int(clock["end"]) >= begin:
                    excluded.add(index)
    elif contexts is None:
        aligned = False
    complete = sum(row["status"] == "complete" for row in records)
    return {"schema": 1, "status": "DISABLED" if not shots else "COMPLETE" if complete == len(shots) else "PARTIAL" if complete else "UNVERIFIED_IMAGES" if archive_members else "UNAVAILABLE",
            "target": "native Game View final render target; not the desktop or whole Editor UI",
            "clock": "Play schedule elapsed time, paused while paused, each dt clamped to 0.1 seconds; not wall time",
            "complete_images": complete, "requested_images": len(shots),
            "archived_images": len(archive_members), "unverified_images": len(archive_members) - complete,
            "schedule_end": schedule_end, "shots": records,
            "archive_members": archive_members, "issues": issues,
            "performance_exclusion": {"available": aligned, "windows": windows,
                "frame_indices": sorted(excluded) if aligned else [],
                "policy": "Whole native intervals touching admission through terminal; unresolved admissions extend through the last captured interval. Raw timing is retained separately. No new settling interval is imposed.",
                "limits": "These are capture-activity intervals, not proof that all indirect screenshot overhead is confined to them."},
            "limits": ["PNG validation does not certify useful or correct rendered content; visual inspection remains required.",
                       "Native source_frame and submit_frame describe the Game target request, not CPU CSV ordinals or presented frames.",
                       "Every valid bounded owned PNG is retained; only a matching native success receipt verifies completion. Other PNGs remain explicitly unverified.",
                       "Capture/readback/file work can perturb performance; the raw analysis includes that work."]}
