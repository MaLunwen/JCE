import copy
import json
import math
import struct
import unittest
from geometry_asset_suite import sphere_glb, sphere_arrays, fixture, validate_inventory, validate_exit, cell_coverage
from static_asset_suite import glb_census

class GeometryTests(unittest.TestCase):
    def test_canonical_binary_boundaries_normals_and_outward_topology(self):
        for rings,slices,verts,triangles in ((16,32,497,960),(128,256,32641,65024)):
            content,manifest=sphere_glb(rings,slices)
            self.assertEqual(sphere_glb(rings,slices),(content,manifest))
            self.assertEqual((manifest['vertices'],manifest['source_triangles']),(verts,triangles))
            length,kind=struct.unpack_from('<II',content,12)
            doc=json.loads(content[20:20+length]);binary=content[28+length:]
            self.assertEqual(len(binary),doc['buffers'][0]['byteLength'])
            strides=(12,12,8,4)
            for accessor,stride in zip(doc['accessors'],strides):
                view=doc['bufferViews'][accessor['bufferView']]
                self.assertEqual(view['byteLength'],accessor['count']*stride)
                self.assertLessEqual(view['byteOffset']+view['byteLength'],len(binary))
            positions,uv,indices=sphere_arrays(rings,slices)
            view=doc['bufferViews'][3]
            packed=struct.unpack_from('<%uI'%len(indices),binary,view['byteOffset'])
            self.assertEqual(list(packed),indices);self.assertLess(max(indices),verts)
            self.assertEqual(binary[doc["bufferViews"][0]["byteOffset"]:doc["bufferViews"][0]["byteLength"]], binary[doc["bufferViews"][1]["byteOffset"]:doc["bufferViews"][1]["byteOffset"]+doc["bufferViews"][1]["byteLength"]])
            self.assertEqual(glb_census(content)['vertices'],verts)
            for position in positions:self.assertAlmostEqual(sum(v*v for v in position),1,places=12)
            for ring in range(rings-1):
                a=1+ring*(slices+1);b=a+slices
                for x,y in zip(positions[a],positions[b]):self.assertAlmostEqual(x,y,places=12)
                self.assertEqual((uv[a][0],uv[b][0]),(0,1))
            for offset in range(0,len(indices),3):
                a,b,c=(positions[i] for i in indices[offset:offset+3])
                ab=[b[i]-a[i] for i in range(3)];ac=[c[i]-a[i] for i in range(3)]
                normal=(ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0])
                self.assertGreater(sum(normal[i]*(a[i]+b[i]+c[i]) for i in range(3)),0)
        for args in ((1,32),(16,2),(999,32),(16,999),(16.0,32)):
            with self.assertRaises(ValueError):sphere_glb(*args)

    def test_scene_populations_and_no_builtin_sphere_substitution(self):
        for count in (1,100,1000,10000):
            rows=fixture('sphere.glb',count)['entities']
            self.assertEqual(len(rows),count+2)
            self.assertEqual(len({r['id'] for r in rows}),count+2)
            self.assertTrue(all(r['components'][1]['meshPath']=='sphere.glb' for r in rows[2:]))
            self.assertEqual(len(fixture('sphere.glb',count,False)['entities']),2)

    def test_actual_inventory_and_exit_negative_controls(self):
        census={'vertices':497}
        text='benchmark-asset: still controls taa=0 ssao=0 ssr=0 motion_blur=0\nbenchmark-asset: ready elapsed_ms=10 frame=0\n'+ '\n'.join('benchmark-asset: frame=%u meshes=100 animators=0 vertices=497 primitives=1 joints=0 clips=0 player=0 time=0.000000 gpu_bytes=100'%f for f in (20,50))
        validate_inventory(text,census,100,readiness=True)
        for bad in (text.replace('meshes=100','meshes=99'),text.replace('vertices=497','vertices=496'),text.replace('frame=0','frame=1'),text+'\n'+text.splitlines()[-1],text+'\n] ERROR - failure'):
            with self.assertRaises(ValueError):validate_inventory(bad,census,100,readiness=True)
        with self.assertRaises(ValueError):validate_inventory(text,census,0,readiness=True)
        validate_exit({'process':{'returncode':0,'timed_out':False}})
        for process in ({},{'process':{'returncode':1,'timed_out':False}},{'process':{'returncode':0,'timed_out':True}}):
            with self.assertRaises(ValueError):validate_exit(process)

    def test_per_cell_missing_geometry_is_rejected(self):
        width,height=128,72;empty=(width,height,3,bytes(width*height*3));pixels=bytearray(empty[3])
        # One sphere centre, independent top-down projection.
        for y in range(30,42):
            for x in range(58,70):pixels[(y*width+x)*3]=100
        self.assertGreater(cell_coverage(empty,(width,height,3,bytes(pixels)),1)[0],0)
        with self.assertRaises(ValueError):cell_coverage(empty,empty,1)
        with self.assertRaises(ValueError):cell_coverage(empty,(128,71,3,bytes(pixels)),1)

if __name__=='__main__':unittest.main()
