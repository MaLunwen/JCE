import unittest
from particle_capacity_suite import CASES, asset_for, validate_counts


def witness(counts):
    rows = ["benchmark-particle-requests: source_frame=50 count=%u" % len(counts)]
    for index, count in enumerate(counts):
        rgba = count.to_bytes(3, "little").hex() + "ff"
        rows.append("benchmark-particle-alive: source_frame=50 completed_frame=53 ready_engine_frame=52 index=%u name=particles.gpu_alive_%02u rgba=%s alive=%u" % (index, index, rgba, count))
    return "\n".join(rows)


class CapacityControls(unittest.TestCase):
    def test_actual_exact_counts_and_negative_controls(self):
        for label, config in CASES.items():
            emitters, _, _, _, _, expected = config
            log = witness([expected] * emitters)
            self.assertEqual(expected * emitters, validate_counts(label, log)["total_alive"])
            for bad in (witness([expected + 1] * emitters), log.replace("ff alive", "00 alive"),
                        log + "\n" + log, "", log.replace("source_frame=50", "source_frame=49")):
                with self.subTest(label=label), self.assertRaises(ValueError):
                    validate_counts(label, bad)
        with self.assertRaises(ValueError):
            validate_counts("isolated8", witness([63, 65] + [64] * 6))

    def test_authored_capacity_and_timing_oracles(self):
        self.assertEqual(65, asset_for("rounded65")["maxParticles"])
        self.assertEqual(128, CASES["rounded65"][-1])
        self.assertEqual(0, asset_for("retired")["emitRate"])
        self.assertLess(asset_for("refill")["lifetimeMax"], .016666)
        self.assertGreater(asset_for("refill")["emitRate"] * .016666, 64)
        self.assertEqual(asset_for("refill")["lifetimeMin"], asset_for("refill")["lifetimeMax"])
        self.assertEqual(4096, asset_for("saturated")["emitBurst"])
        self.assertEqual(0, asset_for("zero")["emitBurst"])


if __name__ == "__main__":
    unittest.main()
