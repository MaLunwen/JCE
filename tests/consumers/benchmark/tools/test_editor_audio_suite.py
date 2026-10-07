import unittest
from editor_audio_suite import validate_paused

class AudioTests(unittest.TestCase):
    def test_actual_paused_progress_and_bad_controls(self):
        log="audio trace playing=0 time=0.000 duration=310.000 waveform=0.502 bins=1029 frame_ms=6.045\naudio trace playing=0 time=0.000 duration=310.000 waveform=1.000 bins=2038 frame_ms=6.400"
        self.assertEqual(validate_paused(log)["last"]["bins"],2038)
        for bad in (log.replace("playing=0","playing=1"),log.replace("time=0.000","time=1.000"),
            log.replace("waveform=1.000","waveform=0.800"),log.replace("bins=2038","bins=0"),
            log.replace("frame_ms=6.400","frame_ms=nan"),log.split("\n")[0],
            log.replace("duration=310.000 waveform=1.000","duration=300.000 waveform=1.000")):
            with self.assertRaises(ValueError):validate_paused(bad)

if __name__=="__main__":unittest.main()
