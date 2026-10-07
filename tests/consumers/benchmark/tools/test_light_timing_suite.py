import unittest
from light_timing_suite import timing_scene,validate_inventory

class LightTimingControls(unittest.TestCase):
    def test_real_component_population_preserves_geometry(self):
        for kind in ('point','spot'):
            geometry=None
            for count in (0,10,100):
                entities=timing_scene(kind,count)['entities']
                lights=[c for e in entities for c in e['components'] if c['type'].endswith('Light')]
                self.assertEqual(len(lights),count)
                self.assertTrue(all(c['intensity']==3 for c in lights))
                meshes=[e for e in entities if any(c['type']=='MeshRenderer' for c in e['components'])]
                self.assertEqual(len(meshes),100)
                if geometry is not None:self.assertEqual(meshes,geometry)
                geometry=meshes

    def test_component_and_authority_negative_controls(self):
        marker='JCE_FORWARDPLUS set -> r.forwardplus ON\nbenchmark-light-timing: frame=100 kind=point requested=10 point=10 spot=0 directional=0 area=0 positive=10'
        validate_inventory(marker,'point',10)
        for wrong in (marker.replace('positive=10','positive=0'),marker.replace('point=10','point=100'),
                      marker.replace('directional=0','directional=1'),marker+'\n'+marker,
                      marker.replace('frame=100','frame=200'),marker.replace('ON','OFF')):
            with self.assertRaises(ValueError):validate_inventory(wrong,'point',10)

if __name__=='__main__':unittest.main()
