import unittest
from stream_route_metrics import process_metrics
import struct
import json
import tempfile
from pathlib import Path
import numpy as np
from stream_route_suite import original_glb, scenes, validate_points, validate_pixels, FIELDS, ROUTE, route_metrics, run_schedule

class StreamRouteControls(unittest.TestCase):
    def test_lifetime_reaches_real_perf_window_without_moving_checkpoints(self):
        for steps in range(6):
            capture, lifetime = run_schedule(steps)
            self.assertEqual(max(1, steps) * 200 - 10, capture)
            self.assertGreaterEqual(lifetime, 120 + 120 + 20)
            self.assertGreaterEqual(lifetime, capture + 40)
            if steps:
                self.assertGreater(lifetime, (steps - 1) * 200 + 199)
        self.assertEqual((190, 260), run_schedule(0))
        self.assertEqual((190, 260), run_schedule(1))
        self.assertEqual((990, 1030), run_schedule(5))
        for invalid in (-1, 6, True, 1.5, None):
            with self.assertRaises(ValueError):
                run_schedule(invalid)

    def test_original_content_and_chunks(self):
        blobs = [original_glb(i) for i in range(3)]
        self.assertEqual(len(set(blobs)),3)
        for i, blob in enumerate(blobs):
            self.assertEqual(blob,original_glb(i))
            self.assertEqual(struct.unpack_from('<III',blob),(0x46546c67,2,len(blob)))
        base,chunks=scenes()
        self.assertEqual(len(base['entities']),2)
        self.assertEqual([c['entities'][0]['components'][0]['posX'] for c in chunks],[0,100,200])

    def test_actual_point_controls_reject_missing_eviction(self):
        lines=[]
        for i in range(5):
            values=(i*200+190,i,ROUTE[i],i+1,i,1,0,1,192,192,192,i,0,192)
            lines.append('benchmark-stream-route-point: '+' '.join('%s=%u'%p for p in zip(FIELDS,values)))
        log='\n'.join(lines)+'\n'+'\n'.join('benchmark-stream-route-ready: frame=%u step=%u vertices=3 primitives=1'%(i*200+190,i) for i in range(5))
        self.assertEqual(len(validate_points(log,5)),5)
        for broken in (log.replace('model_evicted=4','model_evicted=0'),
                       log.replace('path_bytes=192','path_bytes=0'),log+'\n'+lines[0]):
            with self.assertRaises(ValueError):validate_points(broken,5)

    def test_indexed_grid_topology_and_bounds(self):
        for grid in (1, 8, 256):
            blob=original_glb(1,grid)
            size=struct.unpack_from('<I',blob,12)[0]
            doc=json.loads(blob[20:20+size])
            count=(grid+1)**2
            self.assertEqual(count,doc['accessors'][0]['count'])
            self.assertEqual(grid*grid*6,doc['accessors'][2]['count'])
            component=doc['accessors'][2]['componentType']
            self.assertEqual(5123 if count<=65536 else 5125,component)
            binary=blob[28+size:]
            view=doc['bufferViews'][2]
            indices=struct.unpack_from('<%u%s'%(grid*grid*6,'H' if component==5123 else 'I'),binary,view['byteOffset'])
            self.assertTrue(all(0<=i<count for i in indices))
            self.assertEqual(len(indices)//3,2*grid*grid)
            for a,b,c in zip(indices[::3],indices[1::3],indices[2::3]):
                self.assertEqual(len({a,b,c}),3)
        for grid in (-1,513,1.5):
            with self.assertRaises(ValueError):original_glb(0,grid)

    def test_raw_route_metrics_and_counter_negative_controls(self):
        root=Path(__file__).resolve().parents[4]/'build'/'tests'
        root.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as directory:
            path=Path(directory)/'frames.csv'
            path.write_text('frame_index,phase,ms\n'+''.join('%u,frame_wall,%u\n'%(i,40 if i==100 else 10) for i in range(1,201)))
            process={'process_samples':[{'elapsed_s':1,'read_bytes':100,'rss_bytes':1000},{'elapsed_s':3,'read_bytes':2100,'rss_bytes':3000}]}
            result=route_metrics(path,process,1)
            self.assertEqual(190,result['routes'][0]['sample_count'])
            self.assertEqual(1,result['routes'][0]['over_33_33_ms'])
            self.assertEqual(1000,result['process_io']['read_bytes_per_second'])
            self.assertEqual(3000,result['process_memory']['peak_rss_bytes'])
            self.assertEqual('UNAVAILABLE',result['cold_disk_io']['status'])
            self.assertEqual('UNAVAILABLE',result['upload_bytes_per_frame']['status'])
            process['process_samples'][1]['read_bytes']=0
            with self.assertRaises(ValueError):route_metrics(path,process,1)
            path.write_text('frame_index,phase,ms\n1,frame_wall,10\n')
            with self.assertRaises(ValueError):route_metrics(path,{},1)

    def test_memory_sampling_and_io_absence_are_explicit(self):
        base = [{'elapsed_s':0.1,'rss_bytes':100,'commit_bytes':150,'read_bytes':None},
                {'elapsed_s':0.2,'rss_bytes':200,'commit_bytes':250,'read_bytes':None}]
        result = process_metrics({'process_samples':base})
        self.assertEqual(result['process_memory']['peak_rss_bytes'], 200)
        self.assertEqual(result['process_memory']['peak_commit_bytes'], 250)
        self.assertIsNone(result['process_io'])
        for rows in ([], [dict(base[0], rss_bytes=-1)],
                     [base[0],dict(base[1],elapsed_s=0)],
                     [dict(base[0],elapsed_s=float('nan'))],
                     [dict(base[0],commit_bytes=-1)]):
            with self.assertRaises(ValueError):
                process_metrics({'process_samples':rows})

    def test_pixel_reload_controls(self):
        empty=np.zeros((720,1280,4),np.uint8)
        images=[empty]
        for color in range(3):
            p=empty.copy();p[100:200,100:200,color]=100;images.append(p)
        images.extend([images[2].copy(),images[1].copy()])
        validate_pixels(images)
        images[-1]=empty
        with self.assertRaises(ValueError):validate_pixels(images)

if __name__=='__main__':unittest.main()
