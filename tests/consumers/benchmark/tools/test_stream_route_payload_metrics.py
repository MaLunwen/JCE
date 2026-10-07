import unittest
from stream_route_payload_metrics import parse_payload_metrics


def records():
    lines = []
    for frame, phase, vertex, index in ((0, "begin", 20, 10), (190, "loaded", 84, 22), (199, "frozen", 84, 22)):
        lines.append(f"benchmark-stream-payload: frame={frame} step=0 phase={phase} valid=1 generation=9 renderer_frame={frame} vertex={vertex} index={index} texture_create=7 texture_update=0 saturated=0")
    return "\n".join(lines)


class PayloadMetricsTests(unittest.TestCase):
    def test_actual_deltas_and_frozen_scope(self):
        result = parse_payload_metrics(records(), 1)
        self.assertEqual(result["phases"][0]["load_delta_bytes"]["vertex"], 64)
        self.assertEqual(result["phases"][0]["frozen_delta_bytes"]["vertex"], 0)

    def test_missing_does_not_become_zero(self):
        with self.assertRaises(ValueError):
            parse_payload_metrics("", 1)

    def test_invalid_controls(self):
        controls = (
            records() + "\n" + records().splitlines()[0],
            records().replace("generation=9 renderer_frame=190", "generation=10 renderer_frame=190"),
            records().replace("vertex=84 index=22", "vertex=19 index=22", 1),
            records().replace("phase=frozen valid=1", "phase=frozen valid=0"),
            records().replace("texture_update=0 saturated=0", "texture_update=0 saturated=1", 1),
            records().replace("frame=199 step=0", "frame=198 step=0"),
            records().replace("vertex=84 index=22", "vertex=20 index=22"),
            records().replace("renderer_frame=199 vertex=84", "renderer_frame=199 vertex=85"),
        )
        for log in controls:
            with self.subTest(log=log):
                with self.assertRaises(ValueError):
                    parse_payload_metrics(log, 1)

    def test_disabled_route_has_no_fabricated_samples(self):
        self.assertEqual(parse_payload_metrics("", 0)["records"], [])
        with self.assertRaises(ValueError):
            parse_payload_metrics(records(), 0)


if __name__ == "__main__":
    unittest.main()
