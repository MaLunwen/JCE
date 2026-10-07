import unittest
from particle_timing_suite import timing_records, validate_process


class TimingCutoffTests(unittest.TestCase):
    def test_real_nested_process_schema(self):
        good = {"frame": 1.2, "backend_actual": "Direct3D 11",
                "process": {"returncode": 0, "timed_out": False, "elapsed_s": 2.0}}
        validate_process(good, "d3d11")
        for bad in ({"returncode": 0, "backend_actual": "Direct3D 11"},
                    {**good, "process": {"returncode": 1, "timed_out": False}},
                    {**good, "process": {"returncode": 0, "timed_out": True}},
                    {**good, "process": {"returncode": 0}},
                    {**good, "process": {"returncode": False, "timed_out": False}},
                    {**good, "backend_actual": "Vulkan"}):
            with self.assertRaises(ValueError):
                validate_process(bad, "d3d11")

    def test_cutoff_and_population(self):
        log = ("benchmark-particle-requests: source_frame=50 count=1\n"
               "benchmark-particle-alive: source_frame=50 completed_frame=53 ready_engine_frame=54 "
               "index=0 name=particles.gpu_alive_00 rgba=000100ff alive=256\n"
               "benchmark-particle-stats-disabled: frame=64 requested=1 completed=1")
        self.assertEqual(timing_records(log, 1, False, 256)["disabled_frame"], 64)
        self.assertEqual(timing_records(log+"\nWARN - unrelated warning", 1, False, 256)["disabled_frame"], 64)
        for bad in (log.replace("frame=64", "frame=120"), log.replace("completed=1", "completed=0"),
                    log+"\n"+log.splitlines()[-1], log+"\nWARN - CPU particle upload unavailable", log.replace("rgba=000100ff", "rgba=00010000"),
                    log.replace("alive=256", "alive=255"), log.split("benchmark-particle-stats-disabled")[0]):
            with self.assertRaises(ValueError):
                timing_records(bad, 1, False, 256)
        with self.assertRaises(ValueError):
            timing_records(log, 1, False, 512)


if __name__ == "__main__":
    unittest.main()
