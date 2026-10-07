import tempfile
import json
import io
from pathlib import Path
import unittest
import wave
from PIL import Image
from load_suite import generate_inputs,validate_marker,io_observation,prepared_bundle
from stream_route_suite import original_glb
from static_asset_suite import glb_census

class LoadControls(unittest.TestCase):
    def test_original_repeated_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            first=generate_inputs(root/'a',side=32,seconds=1)
            second=generate_inputs(root/'b',side=32,seconds=1)
            for kind in first:self.assertEqual(first[kind].read_bytes(),second[kind].read_bytes())
            with Image.open(first['texture']) as image:self.assertEqual(image.size,(32,32))
            with wave.open(str(first['audio'])) as sound:
                self.assertEqual((sound.getnframes(),sound.getnchannels(),sound.getframerate()),(48000,2,48000))

    def test_malformed_input_negative_controls(self):
        blob=original_glb(0)
        glb_census(blob)
        for bad in (blob[:-1], b"not a GLB"):
            with self.assertRaises(ValueError):glb_census(bad)
        with self.assertRaises((OSError, ValueError)):
            Image.open(io.BytesIO(b"not a PNG")).load()
        with self.assertRaises((wave.Error, EOFError)):
            wave.open(io.BytesIO(b"RIFF truncated"), "rb")

    def test_phase_sum_and_malformed_controls(self):
        marker='benchmark-load: frame=20 kind=glb ok=1 read_bytes=100 packed_entry_bytes=0 payload_bytes=100 read_ns=2 open_ns=0 decompress_verify_ns=0 decode_ns=3 submit_ns=5 total_ns=10'
        validate_marker(marker,'glb',100)
        for bad in ('',marker+'\n'+marker,marker.replace('total_ns=10','total_ns=9'),
                    marker.replace('ok=1','ok=0'),marker.replace('read_bytes=100','read_bytes=99'),
                    marker.replace('payload_bytes=100','payload_bytes=0')):
            with self.assertRaises(ValueError):validate_marker(bad,'glb',100)
        validate_marker(marker.replace('ok=1','ok=0'),'glb',100,success=False)

    def test_prepared_catalog_and_actual_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            (root/'scene.jbundle').write_bytes(b'actual bundle artifact')
            size=(root/'scene.jbundle').stat().st_size
            catalog={'contract':{'name':'jce.bundle.catalog'},'bundles':{'scene':{'file':'scene.jbundle','size':size}}}
            report={'bundles':[{'name':'scene','file':'scene.jbundle','size_bytes':size,'entries':[{'path':'set_0.glb','size_bytes':99}]}]}
            (root/'bundle_catalog.json').write_text(json.dumps(catalog))
            (root/'build_report.json').write_text(json.dumps(report))
            self.assertEqual(prepared_bundle(root,99),(root/'scene.jbundle','set_0.glb'))
            for bad in (100,0):
                with self.assertRaises(ValueError):prepared_bundle(root,bad)
            catalog['bundles']['scene']['file']='../scene.jbundle'
            (root/'bundle_catalog.json').write_text(json.dumps(catalog))
            with self.assertRaises(ValueError):prepared_bundle(root,99)

    def test_os_io_scope_missing_is_unavailable(self):
        self.assertIsNone(io_observation([])['read_bytes_last'])
        observation=io_observation([dict(read_bytes=12,write_bytes=0),dict(read_bytes=24,write_bytes=3)])
        self.assertEqual(observation['read_bytes_sampled_delta'],12)
        self.assertIn('not physical',observation['scope'])

if __name__=='__main__':unittest.main()
