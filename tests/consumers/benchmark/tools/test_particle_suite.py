import unittest
import numpy as np
from particle_suite import particle_asset, scene, validate, alive_records, validate_alive_controls


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


if __name__ == "__main__":
    unittest.main()
