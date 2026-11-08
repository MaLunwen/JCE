"""CPU-only native protocol controls; all artifacts and measurements are synthetic."""
import copy
from datetime import datetime,timedelta,timezone
import hashlib
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from native_performance_evidence import HOSTS, MODES, RAW_NAMES, capture_frame_budget, clock_errors, clock_points, identity, parse_kpi, protocol_named, validate_native_performance, validate_run
from test_web_evidence import ROOT
import write_verification
import verify_performance

ARTIFACTS={'native':'a'*64,'editor':'b'*64}


def save(path,value):path.write_text(json.dumps(value)+'\n',encoding='utf-8',newline='\n')


class NativeTimingControls(unittest.TestCase):
    def setUp(self):
        base=ROOT/'build'/'verification';base.mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(prefix='native-protocol-controls-',dir=base)
        self.addCleanup(self.temp.cleanup);self.directory=Path(self.temp.name)

    def fixture(self,name):
        protocol=protocol_named(name);runs=[]
        maximum=capture_frame_budget(protocol)
        csv='frame_index,phase,ms\n'+''.join(f'{index},{phase},{value}\n' for index in range(maximum)
             for phase,value in [('frame_wall',99 if index==400 else 16+index%3),('frame_cpu',2+index%2),('frame_gpu',3)])
        epoch=datetime(2026,10,8,tzinfo=timezone.utc)
        for host in HOSTS:
            for mode in MODES:
                for repeat in range(protocol['repeats']):
                    folder=self.directory/(name+'-'+host+'-'+mode+'-'+str(repeat));folder.mkdir(exist_ok=True)
                    (folder/'frames.csv').write_text(csv,encoding='utf-8',newline='\n')
                    started=epoch+timedelta(seconds=len(runs)*3+1);finished=started+timedelta(seconds=1)
                    observation=lambda stamp:{'observed_utc':stamp.isoformat(),'processes':[{'pid':1,'name':'synthetic contract'}],'competing_processes':[]}
                    save(folder/'guard.json',{'scope':'Synthetic contract inventory, not actual process observation',
                         'before':observation(started-timedelta(seconds=.5)),'after':observation(finished+timedelta(seconds=.5))})
                    execution={'returncode':0,'timed_out':False,'elapsed_s':1,'sampling_available':False,'peak_rss_bytes':None}
                    save(folder/'result.json',execution);save(folder/'samples.json',[])
                    (folder/'stdout.log').write_text('jce_renderer: renderer: Direct3D 11\nTD_BOOT\nshots=1 spawned=200\n',encoding='utf-8')
                    (folder/'stderr.log').write_text('',encoding='utf-8')
                    stats=parse_kpi(folder/'frames.csv',protocol)
                    runs.append({'host':host,'mode':mode,'repeat':repeat,'exe_sha256':ARTIFACTS['native' if host=='player' else 'editor'],
                        'raw_dir':str(folder),'raw_evidence':{key:identity(folder/file) for key,file in RAW_NAMES.items()},
                        'started_utc':started.isoformat(),'finished_utc':finished.isoformat(),'process':execution,
                        'kpi':stats,'fps':1000/stats['frame_wall']['mean_ms']})
        folder=self.directory/(name+'-live-clock');folder.mkdir(exist_ok=True)
        clock=copy.deepcopy(runs[-1]);clock.update(exe_sha256=ARTIFACTS['native'],raw_dir=str(folder),started_utc=(epoch+timedelta(seconds=len(runs)*3+1)).isoformat(),finished_utc=(epoch+timedelta(seconds=len(runs)*3+2)).isoformat())
        clock_log='jce_renderer: renderer: Direct3D 11\nTD_BOOT\n'+''.join(f'TD_TIME wall={second} simulation={second} progress={second*60*.025/22:.6f} frames={second*60} dropped=0 remainder=0\n' for second in (1,2,3))
        for key,file in RAW_NAMES.items():
            (folder/file).write_bytes(Path(runs[-1]['raw_evidence'][key]['path']).read_bytes())
        (folder/'stdout.log').write_text(clock_log,encoding='utf-8')
        save(folder/'guard.json',{'scope':'Synthetic clock scheduling contract','before':observation(epoch+timedelta(seconds=len(runs)*3+.5)),'after':observation(epoch+timedelta(seconds=len(runs)*3+2.5))})
        clock.update(raw_evidence={key:identity(folder/file) for key,file in RAW_NAMES.items()},points=clock_points(clock_log))
        slope,progress=clock_errors(clock['points']);clock.update(elapsed_clock_error_s=slope,simulation_step_progress_error=progress,**{'pass':True})
        return {'schema':1,'protocol':protocol,'complete':True,'status':'COMPLETE_NATIVE_TIMING_PROTOCOL_OBSERVED','errors':[],
                'conditions':{'resolution_scope':'protocol_label_only_unverified_render_surface','actual_window_size':None,'actual_game_fbo_size':None,'same_render_surface_comparison':False,'contended':False,'resolution':[1280,720],'vsync_requested':1,'fixed_dt':False,'stream_sync':True,'visible_window':False},'runs':runs,'live_clock':clock}

    def test_publication_short_realtime_warmup_is_rejected(self):
        # Old realtime 120-frame warmup is only 0.6 s in this known control.
        # The 600 measured frames cannot make the preceding warmup longer.
        path = self.directory / 'synthetic-short-realtime-warmup.csv'
        path.write_text('frame_index,phase,ms\n' + ''.join(
            f'{index},frame_wall,5\n' for index in range(750)), encoding='utf-8', newline='\n')
        with self.assertRaises(ValueError):
            parse_kpi(path, protocol_named('publication'))

    def test_publication_earliest_realtime_boundary_excludes_startup_stalls(self):
        protocol=protocol_named('publication')
        self.assertEqual(capture_frame_budget(protocol),3750)
        self.assertEqual(capture_frame_budget(protocol_named('standard')),450)
        path=self.directory/'synthetic-dynamic-publication.csv'
        path.write_text('frame_index,phase,ms\n'+''.join(
            f'{index},frame_wall,{4000 if index==0 else 9000 if index==119 else 99 if index==800 else 7.5}\n'
            for index in range(3750)),encoding='utf-8',newline='\n')
        actual=parse_kpi(path,protocol)
        self.assertEqual(actual['measurement_window'],{
            'startup_discard_frames':120,'warmup_first_frame':120,'warmup_last_frame':519,
            'warmup_frames':400,'warmup_ms':3000.0,
            'measured_first_frame':520,'measured_last_frame':1119,'measured_frames':600})
        self.assertEqual(actual['frame_wall']['samples'],600)
        self.assertEqual(actual['frame_wall']['max_ms'],99)
        # Values just below three seconds must not borrow a numeric tolerance.
        values=[1000,1000,999.99999999,.00000002]+[10]*600
        path.write_text('frame_index,phase,ms\n'+''.join(
            f'{index},frame_wall,{value}\n' for index,value in enumerate([9000]*120+values)),encoding='utf-8',newline='\n')
        window=parse_kpi(path,protocol)['measurement_window']
        self.assertEqual(window['warmup_frames'],4)
        self.assertEqual(window['measured_first_frame'],124)
        self.assertGreaterEqual(window['warmup_ms'],3000)

    def test_publication_budget_cannot_borrow_later_frames_or_measurement(self):
        protocol=protocol_named('publication')
        path=self.directory/'synthetic-exhausted-warm-budget.csv'
        path.write_text('frame_index,phase,ms\n'+''.join(
            f'{index},frame_wall,{9000 if index<120 else .999 if index<3120 else 1000}\n'
            for index in range(3750)),encoding='utf-8',newline='\n')
        with self.assertRaisesRegex(ValueError,'frame budget'):
            parse_kpi(path,protocol)
        path.write_text('frame_index,phase,ms\n'+''.join(
            f'{index},frame_wall,10\n' for index in range(1019)),encoding='utf-8',newline='\n')
        # Warm 120..419 = 3 seconds, but only 599 frames remain to measure.
        with self.assertRaisesRegex(ValueError,'600 consecutive'):
            parse_kpi(path,protocol)

    def test_publication_window_is_recomputed_after_resigned_warm_raw_changes(self):
        data=self.fixture('publication');row=data['runs'][0]
        self.assertGreaterEqual(row['kpi']['measurement_window']['warmup_ms'],3000)
        for key,value in [('warmup_ms',1),('warmup_frames',1),('measured_first_frame',120),('measured_frames',599)]:
            broken=copy.deepcopy(row);broken['kpi']['measurement_window'][key]=value
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'window'):
                validate_run(broken,data['protocol'],ARTIFACTS)
        path=Path(row['raw_evidence']['frames_csv']['path'])
        original=path.read_bytes()
        path.write_bytes(original.replace(b'120,frame_wall,16\n',b'120,frame_wall,1\n',1))
        changed=copy.deepcopy(row);changed['raw_evidence']['frames_csv']=identity(path)
        with self.assertRaisesRegex(ValueError,'window'):
            validate_run(changed,data['protocol'],ARTIFACTS)
        path.write_bytes(original)
        gap=copy.deepcopy(row)
        path.write_bytes(original.replace(b'125,',b'126,'))
        gap['raw_evidence']['frames_csv']=identity(path)
        with self.assertRaises(ValueError):validate_run(gap,data['protocol'],ARTIFACTS)

    def test_publication_v1_cannot_resume_as_v2_and_no_trace_budget_is_added(self):
        data=self.fixture('publication')
        data['protocol']={'id':'td-native-publication-v1','warmup_frames':120,'measured_frames':600,'repeats':7}
        data.update(complete=False,status='NATIVE_TIMING_PROTOCOL_PARTIAL')
        with self.assertRaisesRegex(ValueError,'protocol'):
            validate_native_performance(data,ARTIFACTS,allow_partial=True)
        with patch.dict(verify_performance.os.environ, {
            'JCE_TRACE':'1','JCE_TRACE_STREAM':'inherited-stream.json',
            'JCE_TRACE_EXPORT':'inherited-export.json','JCE_PERF_LOG':'1',
            'JCE_FRAME_DT_FIXED':'0.01','JCE_TRACE_CAPACITY':'99999'}):
            environment=verify_performance.measurement_environment(self.directory,capture_frame_budget(protocol_named('publication')))
        self.assertEqual(environment.get('JCE_TRACE'),'0')
        self.assertEqual(environment.get('JCE_TRACE_STREAM'),'')
        self.assertEqual(environment.get('JCE_TRACE_EXPORT'),'')
        self.assertEqual(environment['TD_TRACE'],'1')
        self.assertEqual(environment['JCE_MAX_FRAMES'],'3750')
        self.assertNotIn('JCE_FRAME_DT_FIXED',environment)
        self.assertNotIn('JCE_TRACE_CAPACITY',environment)

    def test_named_guard_records_gpu_tools_and_retains_ordinary_background(self):
        records=[SimpleNamespace(pid=100+index,info={'name':name,'cmdline':arguments}) for index,(name,arguments) in enumerate([
            ('QRenderDoc.exe',['qrenderdoc.exe']),('renderdoccmd.exe',['renderdoccmd.exe','capture']),
            ('python.exe',['python','verify_reference_inputs.py']),('python.exe',['python','verify_delivery.py']),
            ('node.exe',['node','verify_web_performance.js']),('node.exe',['node','verify_web_render_quality.js']),
            ('node.exe',['node','verify_reference_shader_boundary.js']),('node.exe',['node','verify_web_acceptance.js']),
            ('python.exe',['python','reference_fixture_server.py']),('node.exe',['node','verify_web_hash.js']),
            ('ordinary.exe',['ordinary.exe']),
            ('Native Archived 13.exe',['elsewhere/Native Archived 13.exe']),
            ('Packaged-JCE-Editor.exe',['relocated/Packaged-JCE-Editor.exe'])])]
        with patch('psutil.process_iter',return_value=records),patch('verify_performance.os.getpid',return_value=99999), \
             patch.object(verify_performance.v,'GAME',Path('new location/Native Archived 13.exe')), \
             patch.object(verify_performance.v,'EDITOR',Path('another location/Packaged-JCE-Editor.exe')):
            observed=verify_performance.observe_competition()
        self.assertEqual(len(observed['processes']),13)
        self.assertEqual([row['pid'] for row in observed['competing_processes']],list(range(100,108))+[111,112])
        self.assertTrue(observed['observed_utc'])

    def test_both_complete_protocols_and_publication_spike(self):
        for name,count,samples in [('standard',18,300),('publication',42,600)]:
            data=self.fixture(name);validate_native_performance(data,ARTIFACTS)
            write_verification.verify_bound_data('performance-suite',data,{key:{'sha256':value} for key,value in ARTIFACTS.items()})
            self.assertEqual(len(data['runs']),count)
            self.assertEqual(data['runs'][0]['kpi']['frame_wall']['samples'],samples)
            self.assertEqual(data['runs'][0]['kpi']['frame_wall']['max_ms'],99)

    def test_publication_render_extent_labels_do_not_become_actual_observations(self):
        original=self.fixture('publication')
        validate_native_performance(original,ARTIFACTS)
        for key,value in [('resolution_scope','actual_render_size'),('actual_window_size',[1280,720]),
                          ('actual_game_fbo_size',[1280,720]),('same_render_surface_comparison',True)]:
            broken=copy.deepcopy(original);broken['conditions'][key]=value
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'render extent'):
                validate_native_performance(broken,ARTIFACTS)
        broken=copy.deepcopy(original);broken['conditions'].pop('resolution_scope')
        with self.assertRaisesRegex(ValueError,'render extent'):
            validate_native_performance(broken,ARTIFACTS)

    def test_protocol_shape_cardinality_and_identity_controls(self):
        original=self.fixture('standard')
        changes={
            'no protocol':lambda d:d.pop('protocol'), 'unknown protocol':lambda d:d['protocol'].update(id='unknown'),
            'bool repeat':lambda d:d['protocol'].update(repeats=True), 'modified sample':lambda d:d['protocol'].update(measured_frames=600),
            'unavailable status':lambda d:d.update(status='UNAVAILABLE'), 'incomplete':lambda d:d.update(complete=False),
            'actual errors':lambda d:d.update(errors=['actual failure']), 'missing repetition':lambda d:d['runs'].pop(),
            'duplicate keys':lambda d:d['runs'].__setitem__(-1,copy.deepcopy(d['runs'][0])),
            'foreign repeat':lambda d:d['runs'][0].update(repeat=3), 'bool row repeat':lambda d:d['runs'][0].update(repeat=True),
            'artifact changed':lambda d:d['runs'][0].update(exe_sha256='f'*64),
            'sample summary shortened':lambda d:d['runs'][0]['kpi']['frame_wall'].update(samples=299),
            'spike removed from summary':lambda d:d['runs'][0]['kpi']['frame_wall'].update(max_ms=18),
            'invented FPS':lambda d:d['runs'][0].update(fps=1000),
            'raw path reused':lambda d:d['runs'][1].update(raw_dir=d['runs'][0]['raw_dir'],raw_evidence=copy.deepcopy(d['runs'][0]['raw_evidence'])),
            'run overlap':lambda d:d['runs'][1].update(started_utc=d['runs'][0]['started_utc']),
            'invented live clock':lambda d:d['live_clock'].update(points=[]),
            'failed live clock':lambda d:d['live_clock'].update(**{'pass':False}),
            'fixed dt':lambda d:d['conditions'].update(fixed_dt=True), 'contended':lambda d:d['conditions'].update(contended=True),
        }
        for name,change in changes.items():
            data=copy.deepcopy(original);change(data)
            with self.subTest(name=name),self.assertRaises(ValueError):validate_native_performance(data,ARTIFACTS)

    def test_raw_csv_mutation_continuity_and_phase_checks(self):
        data=self.fixture('standard');row=data['runs'][0]
        path=Path(row['raw_evidence']['frames_csv']['path']);original=path.read_bytes()
        cases={'stale identity':original+b' ',
               'gap':original.replace(b'130,',b'131,'),
               'duplicate phase':original.replace(b'130,frame_cpu,2\n',b'130,frame_cpu,2\n130,frame_cpu,2\n'),
               'nonfinite':original.replace(b'400,frame_wall,99',b'400,frame_wall,nan'),
               'first frame shifted':original.replace(b'0,frame_wall,16',b'1,frame_wall,16',1),
               'short measurement':b'\n'.join(original.splitlines()[:400])+b'\n'}
        for name,raw in cases.items():
            path.write_bytes(raw);broken=copy.deepcopy(row)
            if name!='stale identity':broken['raw_evidence']['frames_csv']=identity(path)
            with self.subTest(name=name),self.assertRaises(ValueError):validate_run(broken,data['protocol'],ARTIFACTS)
        path.write_bytes(original)

    def test_guard_and_execution_cannot_be_overridden_by_summary(self):
        data=self.fixture('standard');row=data['runs'][0]
        guard_path=Path(row['raw_evidence']['guard']['path']);original=json.loads(guard_path.read_text())
        for name,change in [('before busy',lambda d:d['before'].update(competing_processes=[{'pid':2,'name':'ninja.exe'}])),
                             ('after busy',lambda d:d['after'].update(competing_processes=[{'pid':3,'name':'towerdefense.exe'}])),
                             ('empty inventory',lambda d:d['before'].update(processes=[])),
                             ('no lifetime bracket',lambda d:d['after'].update(observed_utc='2026-10-07T00:00:00Z'))]:
            guard=copy.deepcopy(original);change(guard);save(guard_path,guard)
            broken=copy.deepcopy(row);broken['raw_evidence']['guard']=identity(guard_path)
            with self.subTest(name=name),self.assertRaises(ValueError):validate_run(broken,data['protocol'],ARTIFACTS)
        save(guard_path,original);row['raw_evidence']['guard']=identity(guard_path)
        broken=copy.deepcopy(row);broken['process']['timed_out']=True
        with self.assertRaises(ValueError):validate_run(broken,data['protocol'],ARTIFACTS)

    def test_partial_resume_requires_same_known_protocol_current_artifacts_and_raw(self):
        data=self.fixture('publication');data['runs']=data['runs'][:2]
        data.update(complete=False,status='NATIVE_TIMING_PROTOCOL_PARTIAL')
        validate_native_performance(data,ARTIFACTS,allow_partial=True)
        with self.assertRaises(ValueError):validate_native_performance(data,ARTIFACTS)
        with self.assertRaises(ValueError):validate_native_performance(data,{**ARTIFACTS,'native':'f'*64},allow_partial=True)
        broken=copy.deepcopy(data);broken['runs'][0]['raw_evidence']['frames_csv']['sha256']='f'*64
        with self.assertRaises(ValueError):validate_native_performance(broken,ARTIFACTS,allow_partial=True)
        broken=copy.deepcopy(data);broken['protocol']=protocol_named('standard')
        with self.assertRaises(ValueError):validate_native_performance(broken,ARTIFACTS,allow_partial=True)


if __name__=='__main__':unittest.main()
