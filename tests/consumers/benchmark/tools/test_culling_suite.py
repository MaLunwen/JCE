"""A requested culling switch or empty image cannot impersonate known work."""
import unittest

from culling_suite import fixture, validate_counts, validate_process, check_resources


def records(count, enabled):
    visible = (count + 2) // 3 + 3 if enabled else count + 3
    return "benchmark-player: entered Play\n" \
        "benchmark-asset: still controls taa=0 ssao=0 ssr=0 motion_blur=0\n" + "".join(
        "benchmark-culling: frame=%u enabled=%u candidates=%u visible=%u culled=%u expected_visible=%u\n"
        % (frame, enabled, count + 3, visible, count + 3 - visible, visible)
        for frame in (20, 50))


class CullingTests(unittest.TestCase):
    def test_actual_process_and_missing_resources(self):
        from pathlib import Path
        import tempfile
        log='jce_renderer: renderer: Direct3D 11'
        good={'process':{'returncode':0,'timed_out':False},'backend_actual':'Direct3D 11'}
        validate_process(good,log,'d3d11')
        for bad in [dict(good,process={'returncode':1,'timed_out':False}),dict(good,process={'returncode':0,'timed_out':True})]:
            with self.assertRaises(ValueError):validate_process(bad,log,'d3d11')
        with self.assertRaises(RuntimeError):validate_process(good,log,'vulkan')
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'empty.jsonl';p.write_text('')
            with self.assertRaises((ValueError,RuntimeError)):check_resources(p,200000,True)

    def test_fixed_camera_faces_the_front_group(self):
        camera = fixture(200000)["entities"][0]["components"]
        self.assertEqual((camera[0]["posY"], camera[0]["posZ"], camera[0]["rotX"]), (0, 10, 0))
        self.assertFalse(camera[1]["orthographic"])

    def test_exact_population_and_actual_enabled_state_are_required(self):
        validate_counts(records(200000, True), 200000, True)
        validate_counts(records(200000, False), 200000, False)
        for text in (records(200000, False), records(199999, True),
                     records(200000, True).replace("visible=66670", "visible=200003"),
                     records(200000, True).replace("frame=50", "frame=20"),
                     records(200000, True) + "] ERROR - failed", ""):
            with self.assertRaises(ValueError):
                validate_counts(text, 200000, True)


if __name__ == "__main__":
    unittest.main()
