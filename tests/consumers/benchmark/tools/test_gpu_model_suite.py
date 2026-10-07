"""Actual imported population and executed GPU work are required evidence."""
import copy
import unittest

from gpu_model_suite import front_cells, validate_gpu, validate_model, validate_policy
from test_culling_suite import records
from test_occlusion_suite import records as wall_records


def model_log(count=6000):
    return records(count, False) + "".join(
        "benchmark-asset: frame=%u meshes=%u animators=0 vertices=24 primitives=1 "
        "joints=0 clips=0 player=0 time=0.000000 gpu_bytes=1200\n" % (frame, count)
        for frame in (20, 50))


def counters(mode="gpu", indirect=True):
    gpu = {key: True for key in ("enabled", "active", "forced", "supported",
                                "dispatch_succeeded", "indirect_ready")}
    gpu.update(indirect_supported=indirect, hiz_enabled=mode == "hiz",
               candidate_records=6000, input_records=6000, compute_dispatches=3,
               upload_calls=2, uploaded_bytes=672016, cpu_fallback_submits=0,
               indirect_submits=int(indirect), fixed_count_submits=int(not indirect))
    if mode == "cpu":
        gpu.update(enabled=False, active=False, compute_dispatches=0)
    if mode == "fixed":
        gpu.update(indirect_ready=False, indirect_submits=0,
                   fixed_count_submits=1, compute_dispatches=1)
    return [{"event": "init"}, {"event": "exit", "engine": {"entity_count": 6003},
                               "model_gpu_driven": gpu}]


class GpuModelTests(unittest.TestCase):
    def test_public_policy_is_observed_once(self):
        for mode in ("cpu", "fixed", "gpu", "hiz"):
            text = "benchmark-asset: indirect policy=%u\n" % (mode != "fixed")
            validate_policy(text, mode)
            for bad in ("", text + text, text.replace("policy=0", "policy=1")
                        if mode == "fixed" else text.replace("policy=1", "policy=0")):
                with self.assertRaises(ValueError):
                    validate_policy(bad, mode)

    def test_front_cells_detect_missing_geometry_and_reject_unresolvable_density(self):
        width, height = 1280, 720
        empty = (width, height, 3, bytes(width * height * 3))
        bright = (width, height, 3, bytes([255]) * (width * height * 3))
        coverage = front_cells(empty, bright, 6000)
        self.assertEqual(len(coverage), 2000)
        self.assertTrue(all(coverage))
        self.assertFalse(any(front_cells(bright, bright, 6000)))
        half = (width, height, 3, bytes(255 if x >= width // 2 else 0
                for _y in range(height) for x in range(width) for _channel in range(3)))
        changes = front_cells(bright, half, 6000)
        self.assertTrue(any(changes))
        self.assertTrue(any(value == 0 for value in changes))
        with self.assertRaises(ValueError):
            front_cells(empty, bright, 200000)

    def test_imported_wall_reuses_actual_route_validation(self):
        topology = model_log(128).split("benchmark-asset: frame=20")[1]
        text = wall_records(128, enabled=False, reveal=True) + "benchmark-asset: frame=20" + topology
        validate_model(text, 128, 24, wall=True, reveal=True)
        with self.assertRaises(ValueError):
            validate_model(text.replace("reveal frame=80", "reveal frame=81"),
                           128, 24, wall=True, reveal=True)

    def test_exact_static_import_and_population(self):
        validate_model(model_log(), 6000, 24)
        for text in (model_log(5999), model_log().replace("vertices=24", "vertices=23"),
                     model_log().replace("animators=0", "animators=1"),
                     model_log().replace("frame=50", "frame=20"),
                     model_log().replace("gpu_bytes=1200", "gpu_bytes=0"),
                     model_log() + "] ERROR - failed", ""):
            with self.subTest(text=text[-100:]), self.assertRaises(ValueError):
                validate_model(text, 6000, 24)

    def test_compute_indirect_hiz_and_declared_hardware_fallback(self):
        for mode in ("cpu", "gpu", "hiz", "fixed"):
            validate_gpu(counters(mode), 6000, mode)
        validate_gpu(counters(indirect=False), 6000, "gpu")

    def test_fixed_count_policy_rejects_indirect_and_missing_work(self):
        for key, value in (("indirect_ready", True), ("indirect_submits", 1),
                           ("fixed_count_submits", 0), ("compute_dispatches", 3)):
            rows = counters("fixed")
            rows[-1]["model_gpu_driven"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_gpu(rows, 6000, "fixed")

    def test_requested_gpu_mode_cannot_impersonate_execution(self):
        for key, value in (("enabled", False), ("active", False), ("forced", False),
                           ("supported", False), ("dispatch_succeeded", False),
                           ("candidate_records", 5999), ("input_records", 5999),
                           ("compute_dispatches", 0), ("upload_calls", 0),
                           ("uploaded_bytes", 0), ("cpu_fallback_submits", 1),
                           ("indirect_ready", False), ("indirect_submits", 0),
                           ("hiz_enabled", True)):
            rows = counters()
            rows[-1]["model_gpu_driven"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_gpu(rows, 6000, "gpu")

    def test_missing_exit_population_and_fallback_draw_fail(self):
        cases = [[], counters()[:-1], counters()]
        cases[-1][-1]["engine"]["entity_count"] = 6002
        fallback = counters(indirect=False)
        fallback[-1]["model_gpu_driven"]["fixed_count_submits"] = 0
        cases.append(fallback)
        for rows in cases:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                validate_gpu(rows, 6000, "gpu")

    def test_cpu_control_rejects_gpu_activity(self):
        for key in ("enabled", "active", "compute_dispatches"):
            rows = copy.deepcopy(counters("cpu"))
            rows[-1]["model_gpu_driven"][key] = 1
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_gpu(rows, 6000, "cpu")


if __name__ == "__main__":
    unittest.main()
