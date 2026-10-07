import json
from pathlib import Path
import tempfile
import unittest
from report_loads import checked, load, matched, checked_frames
from gbuffer_asset_suite import sha256_file


class LoadReportControls(unittest.TestCase):
    def test_replaced_raw_and_incomplete_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            binary=root/'consumer'
            binary.write_bytes(b'tested')
            digest=sha256_file(binary)
            checked(binary,digest)
            binary.write_bytes(b'replaced')
            with self.assertRaisesRegex(ValueError,'artifact changed'):checked(binary,digest)
            path=root/'suite.json'
            path.write_text(json.dumps({'status':'FAIL'}))
            with self.assertRaisesRegex(ValueError,'complete loader'):load(path)
            path.write_text(json.dumps({'status':'LOAD_PHASE_OBSERVATION_ONLY','exe':str(binary),
                'exe_sha256':sha256_file(binary),'inputs':{}}))
            with self.assertRaisesRegex(ValueError,'four loader'):load(path)

    def test_mixed_binary_resource_and_mode(self):
        def item(binary='same',kind=True,asset='same'):
            return ({'exe_sha256':binary,'async_cpu_phases':kind,
                     'inputs':{'glb':{'sha256':asset}}},{'backend':'Direct3D 11'})
        matched(item(kind=False),item())
        for other in (item(binary='changed'),item(asset='changed'),item(kind=False)):
            with self.assertRaises(ValueError):matched(item(kind=False),other)

    def test_all_frames_not_sliced_or_duplicated(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'frames.csv'
            content='frame_index,phase,ms\n'+''.join('%u,frame_wall,1\n'%i for i in range(200))
            path.write_text(content)
            self.assertEqual(len(checked_frames(path,200)['frame_ms']),200)
            for bad in (content+'200,frame_wall,1\n',content.replace('100,frame_wall,1\n',''),
                        content+'199,frame_wall,1\n'):
                path.write_text(bad)
                with self.assertRaises(ValueError):checked_frames(path,200)


if __name__=='__main__':unittest.main()
