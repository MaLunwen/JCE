import unittest
from pathlib import Path
from media_timing_fixtures import commands, validate_metadata, validate_dimensions

class FixtureTests(unittest.TestCase):
    def test_explicit_commands_and_font_fallback(self):
        encode, probe = commands(Path("ffmpeg"),Path("ffprobe"),Path("movie.mp4"),"av1",1920,1080,60,10)
        self.assertIn("testsrc2=size=1920x1080:rate=60:duration=10",encode)
        self.assertIn("libaom-av1",encode)
        self.assertNotIn("-vf",encode)
        self.assertIn("sine=frequency=440:sample_rate=48000:duration=10",encode)
        self.assertTrue(any(v.startswith("drawtext=") for v in commands("f","p","o","h264",1920,1080,60,10,"font")[0]))
        with self.assertRaises(ValueError): validate_dimensions(1919,1080,60,10)
    def test_actual_metadata_rejects_wrong_codec_size_rate_audio(self):
        data={"streams":[{"codec_type":"video","codec_name":"av1","width":1920,"height":1080,"avg_frame_rate":"60/1"},
            {"codec_type":"audio","codec_name":"aac","sample_rate":"48000"}],"format":{"duration":"10.0"}}
        validate_metadata(data,"av1",1920,1080,60,10)
        for key,value in (("codec_name","h264"),("width",1280),("avg_frame_rate","30/1")):
            old=data["streams"][0][key];data["streams"][0][key]=value
            with self.assertRaises(ValueError):validate_metadata(data,"av1",1920,1080,60,10)
            data["streams"][0][key]=old
        data["streams"].pop()
        with self.assertRaises(ValueError):validate_metadata(data,"av1",1920,1080,60,10)

if __name__ == "__main__": unittest.main()
