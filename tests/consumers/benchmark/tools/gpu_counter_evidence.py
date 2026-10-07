"""Validate delayed GPU counter words independently of input/submission logs."""
import json
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))

from tools.benchmark_suite import sha256_file

FRAMES = (20, 50, 79, 80, 81, 82, 90)


def read_survivors(path, records, expected, frames=FRAMES):
    path = Path(path).resolve()
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    if [row["source_frame"] for row in rows] != list(frames):
        raise ValueError("GPU counter requests are missing, duplicated or out of order")
    serials = []
    for row in rows:
        groups, width, height = (row[key] for key in ("groups", "width", "height"))
        if row["input_records"] != records or groups <= 0 or groups > width * height or \
                row["completed_frame"] < row["source_frame"] or \
                row["byte_size"] != width * height * 16 or \
                row["byte_order"] not in ("little", "big"):
            raise ValueError("GPU readback metadata is invalid")
        serials.append(int(row["dispatch_serial"]))
        raw = Path(row["raw_words"]).resolve()
        if raw.parent != path.parent:
            raise ValueError("GPU counter words leave the capture directory")
        blob = raw.read_bytes()
        if len(blob) != row["byte_size"]:
            raise ValueError("GPU readback words are truncated")
        words = struct.unpack(("<" if row["byte_order"] == "little" else ">") +
                              "f" * (len(blob) // 4), blob)
        counts = []
        for index in range(width * height):
            low, high, pad0, pad1 = words[index * 4:index * 4 + 4]
            if not all(math.isfinite(value) for value in (low, high, pad0, pad1)) or \
                    not (0 <= low <= 65535 and low == int(low) and
                         0 <= high <= 65535 and high == int(high)) or pad0 != 0 or pad1 != 0:
                raise ValueError("GPU counter words are not exact uint32 halves")
            count = int(low) | (int(high) << 16)
            if index < groups:
                counts.append(count)
            elif count:
                raise ValueError("GPU counter padding contains unexpected data")
        total = sum(counts)
        wanted = expected(row["source_frame"]) if callable(expected) else expected
        if counts != row["group_counts"] or total != row["survivors"] or \
                total > records or total != wanted:
            raise ValueError("actual GPU survivors differ from the independent fixture")
        row["raw_sha256"] = sha256_file(raw)
    if any(value <= 0 for value in serials) or any(
            after <= before for before, after in zip(serials, serials[1:])):
        raise ValueError("GPU counter dispatch identity is stale or duplicated")
    return {"path": str(path), "sha256": sha256_file(path), "rows": rows,
            "scope": "actual GPU compacted model-group counters; asynchronous diagnostic only"}
