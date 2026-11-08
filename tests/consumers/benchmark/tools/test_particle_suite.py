import unittest
import numpy as np
from particle_suite import particle_asset, scene, validate, alive_records, validate_alive_controls, component_population, validate_burst_records


class ParticleControls(unittest.TestCase):
    def test_actual_alive_bytes_and_negative_controls(self):
        log = "benchmark-particle-requests: source_frame=50 count=1\nbenchmark-particle-alive: source_frame=50 completed_frame=53 ready_engine_frame=52 index=0 name=particles.gpu_alive_00 rgba=0a0000ff alive=10"
        self.assertEqual(alive_records(log, 1)["total_alive"], 10)
        for bad in (log.replace("alive=10", "alive=11"), log.replace("0a0000ff", "0a000000"), log.replace("count=1", "count=2"), log+"\n"+log):
            with self.assertRaises(ValueError): alive_records(bad, 1)
        validate_alive_controls({"zero": 0, "active": 10, "changed": 10, "repeat": 10})
        for counts in ({"zero": 1,"active":10,"changed":10,"repeat":10}, {"zero":0,"active":10,"changed":9,"repeat":10}):
            with self.assertRaises(ValueError): validate_alive_controls(counts)

    def test_cpu_total_and_exact_burst_negative_controls(self):
        log = "benchmark-particle-requests: source_frame=50 count=2\nbenchmark-particle-alive: source_frame=50 completed_frame=53 ready_engine_frame=52 index=0 name=particles.cpu_alive_total rgba=000100ff alive=256\nbenchmark-particle-alive: source_frame=50 completed_frame=53 ready_engine_frame=52 index=1 name=particles.cpu_drawn_total rgba=000100ff alive=256"
        self.assertEqual(256,alive_records(log,8,cpu=True)["total_alive"])
        for bad in (log.replace("cpu_alive_total","gpu_alive_00"),
                    log.replace("000100ff","00010000"),log+"\n"+log,
                    log.replace("count=2","count=8"),log.replace("alive=256","alive=255")):
            with self.assertRaises(ValueError):alive_records(bad,8,cpu=True)
        mismatch=log.rsplit("rgba=000100ff alive=256",1)
        with self.assertRaises(ValueError):
            alive_records("rgba=ff0000ff alive=255".join(mismatch),8,cpu=True)
        validate_alive_controls(dict(zero=0,active=256,changed=256,repeat=256),"burst")
        with self.assertRaises(ValueError):
            validate_alive_controls(dict(zero=0,active=255,changed=255,repeat=255),"burst")
        for emitters in (1,8):
            asset=particle_asset(emitters,mode="burst")
            self.assertEqual(256,asset["emitBurst"]*emitters)
            self.assertEqual(0,asset["emitRate"])
            self.assertEqual(1000,asset["lifetimeMin"])
            self.assertEqual(0,particle_asset(emitters,False,mode="burst")["emitBurst"])

    def test_component_population_missing_duplicates_and_wrong_route_rejected(self):
        log = "benchmark-particles: frame=50 components=8 loaded=8 cpu_handles=8 gpu_requested=0"
        self.assertEqual(component_population(log, 8, True), (8, 8, 8, 0))
        for bad in ("", log + "\n" + log, log.replace("components=8", "components=7"),
                    log.replace("gpu_requested=0", "gpu_requested=8"),
                    log.replace("loaded=8", "loaded=0"), log.replace("cpu_handles=8", "cpu_handles=0")):
            with self.subTest(log=bad), self.assertRaises(ValueError):
                component_population(bad, 8, True)
        self.assertEqual(component_population(log.replace("gpu_requested=0", "gpu_requested=8"), 8, False)[3], 8)
        with self.assertRaises(ValueError):
            component_population(log, 8, False)

    def test_equal_gpu_burst_distribution_and_zero_control(self):
        good = {"total_alive": 256, "records": [{"alive": 32} for _ in range(8)]}
        validate_burst_records(good, 8, 256, True, False)
        bad = {"total_alive": 256, "records": [{"alive": count} for count in (0, 64, 32, 32, 32, 32, 32, 32)]}
        with self.assertRaises(ValueError):
            validate_burst_records(bad, 8, 256, True, False)
        validate_burst_records({"total_alive": 0, "records": [{"alive": 0} for _ in range(8)]}, 8, 256, False, False)
        with self.assertRaises(ValueError):
            validate_burst_records(good, 8, 256, False, False)

    def test_stress_capacity_and_billboard_size(self):
        for count in (256,32768,1048576):
            for emitters in (1,8):
                asset=particle_asset(emitters,mode="burst",particles=count,size=.08)
                self.assertEqual(count,asset["emitBurst"]*emitters)
                self.assertGreaterEqual(asset["maxParticles"]*emitters,count)
                self.assertEqual(0,asset["maxParticles"]%64)
                self.assertEqual(.08,asset["sizeStart"])

    def test_budget_and_scene_authority(self):
        for count in (1, 8):
            asset = particle_asset(count)
            self.assertEqual(asset["emitRate"]*count, 256)
            self.assertEqual(asset["maxParticles"]*count, 1024)
            self.assertEqual(asset["velocityMin"], asset["velocityMax"])
            self.assertEqual(len(scene("fixture.json", count)["entities"]), count+2)
            self.assertEqual(particle_asset(count, False)["emitRate"], 0)

    def test_missing_and_identical_pixels_rejected(self):
        zero = np.zeros((720, 1280, 4), np.uint8)
        active, changed = zero.copy(), zero.copy()
        active[100:150, 100:150, 0] = 50
        changed[100:150, 100:150, 2] = 50
        images = dict(zero=zero, active=active, changed=changed, repeat=active.copy())
        validate(images)
        for label in ("active", "changed", "repeat"):
            wrong = dict(images)
            wrong[label] = active.copy() if label == "changed" else zero.copy()
            with self.assertRaises(ValueError):
                validate(wrong)


    def test_full_background_changes_cannot_count_as_particle_liveness(self):
        zero = np.full((720, 1280, 4), 118, np.uint8)
        zero[:, :, 3] = 255
        active, changed = zero.copy(), zero.copy()
        active[100:150, 100:150, 0] = 220
        changed[100:150, 100:150, 2] = 220
        good = dict(zero=zero, active=active, changed=changed, repeat=active.copy())
        self.assertEqual(validate(good)["background_corner_max_rgb8_delta"],
                         dict(active=0, changed=0, repeat=0))
        wrong = {name: pixels.copy() for name, pixels in good.items()}
        for name in ("active", "changed", "repeat"):
            wrong[name][:, :, :3] = 34
        wrong["active"][100:150, 100:150, 0] = 220
        wrong["changed"][100:150, 100:150, 2] = 220
        wrong["repeat"] = wrong["active"].copy()
        with self.assertRaisesRegex(ValueError, "background/postfx"):
            validate(wrong)
        for name in ("active", "changed", "repeat"):
            for y, x in ((0, 0), (0, -1), (-1, 0), (-1, -1)):
                wrong = dict(good)
                wrong[name] = good[name].copy()
                wrong[name][y, x, 1] -= 3
                with self.subTest(name=name, corner=(y, x)), self.assertRaises(ValueError):
                    validate(wrong)


if __name__ == "__main__":
    unittest.main()
