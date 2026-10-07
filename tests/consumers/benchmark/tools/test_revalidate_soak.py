import unittest
from revalidate_soak import validate_execution, validate_samples

class SavedSoakTests(unittest.TestCase):
    def test_missing_or_discontinuous_csv_frames_rejected(self):
        for ids in ([],[1,2],[0,2]):
            with self.assertRaises(ValueError):validate_samples({"frame_indices":ids},{})

    def test_execution_negative_controls(self):
        intent={"executable_sha256":"abc","host":"player","kind":"entity","count":1000,"profile":"soak"}
        process={"returncode":0,"timed_out":False,"elapsed_s":1801}
        log="renderer: initialized Direct3D 11\nbenchmark-spawn: kind=3 requested=1000 spawned=1000\nbenchmark-player: entered Play\nJCE_MAX_SECONDS reached"
        # Backend marker shape is checked by the shared production parser in integration.
        for badprocess in ({**process,"returncode":1},{**process,"elapsed_s":1799},{**process,"timed_out":True}):
            with self.assertRaises(ValueError):validate_execution(intent,badprocess,"abc",log,1800)
        with self.assertRaises(ValueError):validate_execution(intent,process,"changed",log,1800)
        with self.assertRaises(ValueError):validate_execution(intent,process,"abc","no backend",1800)

if __name__=="__main__":unittest.main()
