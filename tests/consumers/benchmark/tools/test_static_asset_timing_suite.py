"""CPU controls for fixed static timing; never launches a Player."""
import unittest
from unittest.mock import patch
import static_asset_timing_suite as suite


class StaticTimingControls(unittest.TestCase):
    def test_world_requires_actual_membership_mesh_and_camera(self):
        log=('stock-world: entities=3 meshes=1 cameras=1 lights=1 valid=1\n'
             'stock-mesh: scale=1.000000,1.000000,1.000000 path=model.glb\n'
             'stock-authored-camera: position=0.000000,0.700000,1.000000 fov=40.000000 near=0.010000 far=100.000000\n'
             'stock-camera: position=0.000000,0.700000,1.000000 forward=0.000000,0.000000,-1.000000 fov_deg=40.000000 near=0.010000 far=100.000000')
        suite.validate_world(log,'model.glb',1,[0,.7,1],0)
        for bad in (log.replace('entities=3','entities=4'),log.replace('path=model.glb','path=stock.glb'),
                    log.replace('fov=40.000000','fov=60.000000'),
                    log.replace('fov_deg=40.000000','fov_deg=0.698132'),log.replace('0.700000','0.800000'),log+'\n'+log):
            with self.assertRaises(ValueError):suite.validate_world(bad,'model.glb',1,[0,.7,1],0)

    def test_readiness_selects_exact_post_ready_warmup(self):
        self.assertEqual(suite.readiness_warmup('stock-ready: raw_frame=321 elapsed_ms=400'),440)
        self.assertEqual(suite.readiness_warmup('stock-ready: raw_frame=19460 elapsed_ms=55000'),19579)
        for log in ('','stock-ready: raw_frame=0 elapsed_ms=1',
                    'stock-ready: raw_frame=100001 elapsed_ms=1',
                    'stock-ready: raw_frame=1 elapsed_ms=60000',
                    'stock-ready: raw_frame=1 elapsed_ms=1\nstock-ready: raw_frame=2 elapsed_ms=2'):
            with self.assertRaises(ValueError):suite.readiness_warmup(log)

    def test_measured_deltas_require_full_repeats(self):
        def records(value):return [{'summary':{'frame_median_ms':value}} for _ in range(7)]
        result=suite.effect_deltas({'base':records(10),'ssao':records(13)})
        self.assertEqual(result['ssao']['delta_from_base_ms'],3)
        for runs in ({'base':records(10)[:6]}, {'ssao':records(10)},
                     {'base':records(float('nan'))}):
            with self.assertRaises(ValueError):suite.effect_deltas(runs)

    def test_actual_effect_and_topology_witnesses_required(self):
        with patch.object(suite,'assert_backend',side_effect=ValueError('wrong backend')), \
             patch.object(suite,'validate_log') as effects:
            with self.assertRaises(ValueError):suite.validate_base('log',{},'vulkan')
            effects.assert_not_called()
        with patch.object(suite,'assert_backend'),patch.object(suite,'validate_log',return_value={32:'SSAO'}):
            log='stock-model: frame=120 vertices=3 primitives=1'
            result=suite.validate_base(log,{'vertices':3,'primitives':1},'gl','ssao')
            self.assertEqual(result['topology'],[('3','1')])
            for broken in ('',log+'\n'+log,log.replace('vertices=3','vertices=0')):
                with self.assertRaises(ValueError):suite.validate_base(broken,{'vertices':3,'primitives':1},'gl','ssao')
        with patch.object(suite,'assert_backend'),patch.object(suite,'validate_log',side_effect=ValueError('effect not submitted')):
            with self.assertRaises(ValueError):suite.validate_base(log,{'vertices':3,'primitives':1},'gl','ssao')

    def test_timing_scope_excludes_readback_and_keeps_fixed_budget(self):
        from pathlib import Path
        source=Path(suite.__file__).read_text(encoding='utf-8')
        for required in ('range(7)','warmup=readiness_warmup(log),sample_count=600','sample_interval=.02'):
            self.assertIn(required,source)
        self.assertIn('pak=pack_fixture(args.packer,packed_asset,args.out)',source)
        self.assertIn("'JCE_BENCH_ASSET_PAK':str(pak)",source)
        self.assertIn('frames=100000,timeout=210,max_seconds=180',source)
        self.assertNotIn('JCE_BENCH_RT_TARGET',source)
        self.assertNotIn('JCE_KPI_GAME_SHOTS',source)


if __name__=='__main__':unittest.main()
