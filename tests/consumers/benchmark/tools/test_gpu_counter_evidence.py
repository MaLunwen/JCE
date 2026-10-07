"""Counter fixtures exercise exact words, completion identity and failed controls."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
import subprocess
import sys

from gpu_counter_evidence import read_survivors

SCRATCH = Path(__file__).resolve().parents[4] / "build/tests/benchmark-tool-regressions"


class GpuCounterTests(unittest.TestCase):
    def test_direct_import_from_unrelated_cwd(self):
        helper = Path(__file__).resolve().with_name("gpu_counter_evidence.py")
        SCRATCH.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=SCRATCH) as directory:
            result = subprocess.run([sys.executable, "-I", "-B", str(helper)],
                                    cwd=directory, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


    def fixture(self, root, endian="little"):
        counts = [2 ** 24 + 37, 65536, 0]
        blob = struct.pack(("<" if endian == "little" else ">") + "16f", *[
            value for count in counts + [0]
            for value in (count & 65535, count >> 16, 0, 0)])
        rows = []
        for frame in (20, 50):
            raw = root / ("frame-%u.bin" % frame)
            raw.write_bytes(blob)
            rows.append(dict(source_frame=frame, completed_frame=frame + 2,
                dispatch_serial=str(frame), ready_engine_frame=frame + 1,
                input_records=sum(counts), survivors=sum(counts), groups=3,
                width=2, height=2, byte_size=len(blob), byte_order=endian,
                raw_words=str(raw), group_counts=counts))
        path = root / "survivors.jsonl"
        self.write(path, rows)
        return path, rows, sum(counts)

    def write(self, path, rows):
        path.write_text("".join(json.dumps(row) + "\n" for row in rows),
                        encoding="utf-8", newline="\n")

    def test_exact_large_uint32_and_both_native_byte_orders(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        for endian in ("little", "big"):
            with tempfile.TemporaryDirectory(dir=SCRATCH) as scratch:
                path, rows, count = self.fixture(Path(scratch), endian)
                actual = read_survivors(path, count, count, frames=(20, 50))
                self.assertEqual(actual["rows"][0]["survivors"], count)
                self.assertEqual(len(actual["rows"][0]["raw_sha256"]), 64)

    def test_forged_missing_and_stale_counters_are_rejected(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        for defect in ("missing", "serial", "total", "completion", "words", "padding", "fraction"):
            with self.subTest(defect=defect), tempfile.TemporaryDirectory(dir=SCRATCH) as scratch:
                path, rows, count = self.fixture(Path(scratch))
                if defect == "missing":
                    rows.pop()
                elif defect == "serial":
                    rows[1]["dispatch_serial"] = rows[0]["dispatch_serial"]
                elif defect == "total":
                    rows[0]["survivors"] = count - 1
                elif defect == "completion":
                    rows[0]["completed_frame"] = 19
                else:
                    raw = Path(rows[0]["raw_words"])
                    blob = bytearray(raw.read_bytes())
                    offset, value = {"words": (0, 99), "padding": (56, 1), "fraction": (0, .5)}[defect]
                    struct.pack_into("<f", blob, offset, value)
                    raw.write_bytes(blob)
                self.write(path, rows)
                with self.assertRaises(ValueError):
                    read_survivors(path, count, count, frames=(20, 50))


if __name__ == "__main__":
    unittest.main()
