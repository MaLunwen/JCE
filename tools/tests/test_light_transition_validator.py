"""Negative controls for consumer log evidence; no graphics execution."""
import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests/consumers/benchmark/tools"))
from light_transition_suite import expected, validate_log


class TransitionEvidenceTests(unittest.TestCase):
    def log(self, mode="off-on-off"):
        lines = []
        for frame in range(1, 201):
            state = expected(mode, frame)
            changed = frame in ((1, 30, 70) if mode == "off-on-off" else (1, 30))
            lines.append("benchmark-fp-transition: frame=%u mode=%s expected=%u actual=%u changed=%u" %
                         (frame, mode, state, state, changed))
        return "\n".join(lines) + "\ndrawcmd-cache: 0 hits / 100 builds\ndrawcmd-cache: 100 hits / 0 builds"

    def test_bidirectional_and_roundtrip_evidence(self):
        for mode in ("off-on", "on-off", "off-on-off"):
            self.assertEqual(1, validate_log(self.log(mode), mode, 1)["positive_hit_reports"])

    def test_rejects_missing_edge_wrong_actual_and_stale_cache(self):
        valid = self.log()
        for log in (valid.replace("frame=30", "frame=300"),
                    valid.replace("expected=1 actual=1", "expected=1 actual=0"),
                    valid + "\nHIT MISMATCH", valid.replace("100 hits /", "0 hits /")):
            with self.assertRaises(ValueError):
                validate_log(log, "off-on-off", 1)

    def test_disabled_cache_must_be_inactive(self):
        with self.assertRaises(ValueError):
            validate_log(self.log(), "off-on-off", 0)
        log = self.log().split("\ndrawcmd-cache:")[0]
        validate_log(log, "off-on-off", 0)
        with self.assertRaises(ValueError):
            validate_log(log, "off-on-off", 1)


if __name__ == "__main__":
    unittest.main()
