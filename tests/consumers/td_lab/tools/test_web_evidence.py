"""Contract controls only: generated data never counts as actual browser timing."""
from __future__ import annotations
import copy
import hashlib
import json
import math
import shutil
import subprocess
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import web_render_quality_evidence as render_quality_admission
from web_evidence import parse_render_snapshot, quality_from_input
from web_performance_validation import PIN, ORDER, protocol_named, raw_statistics, validate_actual_render_quality, validate_web_performance
from test_web_render_quality import DIRECT_SOURCE, SCOPE, ARCHIVED_WEB09
import write_verification

ARTIFACT = 'a' * 64
ROOT = Path(__file__).resolve().parents[1]


def chunks(value):
    raw = json.dumps(value, separators=(',', ':'), ensure_ascii=True).encode('utf-8')
    total = (len(raw) + 383) // 384
    return [f'TD_RENDER_CHUNK part={index} total={total} bytes={len(raw)} payload=' +
            raw[index * 384:(index + 1) * 384].decode('ascii') + ' TD_RENDER_END at controller.c:1'
            for index in range(total)]


def store_json(path, value):
    raw = (json.dumps(value, separators=(',', ':')) + '\n').encode('utf-8')
    path.write_bytes(raw)
    return str(path), hashlib.sha256(raw).hexdigest()


def synthetic_linked_quality(directory, gpu):
    """CPU fixture for file/context binding; never an actual GPU observation."""
    raw = DIRECT_SOURCE.encode('utf-8')
    source = directory / 'synthetic-linked-source.glsl'
    source.write_bytes(raw)
    length = math.hypot(66, 84, 60)
    uniforms = {'u_pbrParams': [1 / 15, 1, 1, .5], 'u_baseColorFactor': [1, 1, 1, 1],
                'u_lookHemiGround': [247 / 255, 1, 243 / 255, 1],
                'u_ambientColor': [243 / 255, 253 / 255, 1, .6],
                'u_dirLights': [66 / length, -84 / length, 60 / length, .7, 1, .96, .9, 0] + [0] * 8,
                'u_lightCounts': [1, 0, 0, 1], 'u_csmParams': [1 / 512, 0, .012, 1.4],
                'u_shadowQuality': [0, 0, 8, 0]}
    observed = {'schema': 1, 'status': 'ACTUAL_WEB_GROUND_QUALITY_OBSERVED', 'errors': [],
                'fixture_tag': 'SYNTHETIC_CPU_CONTRACT_CONTROL_NO_GPU',
                'artifact_sha256': ARTIFACT, 'served_artifact_sha256': ARTIFACT, 'executed_artifact_sha256': ARTIFACT,
                'artifact_url': 'http://127.0.0.1:8785/synthetic.html', 'run_id': 'synthetic_linked_quality',
                'scope': SCOPE, 'executed_transport': 'Synthetic CPU contract; no browser execution occurred',
                'generated_utc': '2026-10-08T00:00:00Z', 'completed_utc': '2026-10-08T00:00:10Z',
                'contexts': [{key: gpu[key] for key in ('kind', 'version', 'shading_language', 'vendor', 'renderer')}],
                'lost': 0, 'draw_calls': 30, 'draws36': 2,
                'ground_draws': [{'method': 'drawElements', 'count': 36, 'timestamp_ms': 5000,
                                 'linked': True, 'fragment_compiled': True, 'gl_error': 0, 'uniforms': uniforms,
                                 'fragment_source': {'path': str(source), 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}}],
                'program_observations': [{'names': list(uniforms), 'uniforms': copy.deepcopy(uniforms),
                                          'source_bytes': len(DIRECT_SOURCE.encode('utf-16-le')) // 2,
                                          'source_start': DIRECT_SOURCE.encode('utf-16-le')[:600].decode('utf-16-le', errors='surrogatepass'),
                                          'contains_finite_helper': True}]}
    observed['contexts'][0]['lost'] = False
    path, identity = store_json(directory / 'synthetic-linked-quality.json', observed)
    return {'available': True, 'artifact_sha256': ARTIFACT, 'source': path, 'sha256': identity, 'observed': observed}


class RenderSnapshotControls(unittest.TestCase):
    def setUp(self):
        self.value = {'version': 1, 'gamma': 1, 'detail': 'x' * 850}
        self.logs = chunks(self.value)

    def test_complete_chunk_and_legacy(self):
        result = parse_render_snapshot(self.logs)
        self.assertEqual(result['effective'], self.value)
        self.assertEqual(result['parts'], 3)
        self.assertEqual(parse_render_snapshot(['TD_RENDER {"unicode":"塔"} at controller.c:1'])['effective'], {'unicode': '塔'})

    def test_reject_broken_transport(self):
        invalid = {
            'missing': self.logs[:-1],
            'duplicate': self.logs + self.logs[:1],
            'reordered': list(reversed(self.logs)),
            'header byte disagreement': [self.logs[0].replace('bytes=', 'bytes=1')] + self.logs[1:],
            'payload truncation': [self.logs[0].replace('xxx', 'xx', 1)] + self.logs[1:],
            'missing terminator': [self.logs[0].replace(' TD_RENDER_END', '')] + self.logs[1:],
            'unexpected suffix': self.logs[:-1] + [self.logs[-1].replace(' TD_RENDER_END at controller.c:1', ' TD_RENDER_END BAD')],
            'malformed JSON': [self.logs[0].replace('payload={', 'payload=[')] + self.logs[1:],
            'legacy truncated': ['TD_RENDER {"shadows":{"resolution":512'],
            'mixed': self.logs + ['TD_RENDER {}'],
            'repeated legacy': ['TD_RENDER {}', 'TD_RENDER {}'],
            'nonstandard constant': ['TD_RENDER {"gamma":NaN}'],
            'array snapshot': ['TD_RENDER []'],
            'non-log value': [None],
        }
        for name, logs in invalid.items():
            with self.subTest(name=name), self.assertRaises(ValueError):
                parse_render_snapshot(logs)

    def test_input_quality_requires_current_success(self):
        value = {'schema': 1, 'status': 'OBSERVED_CHECKS_PASSED', 'mode': 'input',
                 'artifact_sha256': ARTIFACT, 'errors': [], 'logs': self.logs}
        raw = json.dumps(value).encode()
        self.assertEqual(quality_from_input(raw)['evidence_sha256'], hashlib.sha256(raw).hexdigest())
        for key, invalid in [('schema', True), ('status', 'FAILED'), ('mode', 'lifecycle'),
                             ('errors', [{'message': 'actual failure'}]), ('artifact_sha256', '')]:
            broken = {**value, key: invalid}
            with self.subTest(key=key), self.assertRaises(ValueError):
                quality_from_input(json.dumps(broken).encode())
        with self.assertRaises(ValueError):
            quality_from_input(b'[]')


class FinalPerformanceControls(unittest.TestCase):
    def setUp(self):
        # Synthetic linked source is approved only for this test's lifetime.
        # Keep the production registry for actual-observation negative controls.
        self.production_shader_registry = render_quality_admission.REVIEWED_LINKED_SOURCE_SHA256
        synthetic_sha = hashlib.sha256(DIRECT_SOURCE.encode("utf-8")).hexdigest()
        shader_patch = mock.patch.object(
            render_quality_admission, "REVIEWED_LINKED_SOURCE_SHA256",
            self.production_shader_registry | frozenset({synthetic_sha}))
        shader_patch.start()
        self.addCleanup(shader_patch.stop)
        directory = ROOT / 'build' / 'verification'
        directory.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='web-admission-control-', dir=directory)
        self.addCleanup(self.temporary.cleanup)
        directory = Path(self.temporary.name)
        effective = {'version': 1, 'fog': {'enabled': False}, 'iblEnabled': False, 'dynamicGI': 0,
                     'shadows': {'resolution': 512, 'cascades': 1, 'soft': 1, 'distance': 250},
                     'postfx': {**{key: False for key in ('tonemap', 'bloom', 'fxaa', 'vignette',
                                'chromatic', 'grayscale', 'custom', 'ssao', 'ssr')}, 'exposure': 1, 'gamma': 1},
                     'test_only_padding': 'x' * 500}
        input_data = {'schema': 1, 'status': 'OBSERVED_CHECKS_PASSED', 'mode': 'input',
                      'artifact_sha256': ARTIFACT, 'errors': [], 'logs': chunks(effective)}
        input_path, input_sha = store_json(directory / 'input.json', input_data)
        parsed = quality_from_input(Path(input_path).read_bytes())
        snapshot_path, snapshot_sha = store_json(directory / 'quality.json', parsed)
        quality = {'available': True, 'effective': effective, 'protocol': parsed['protocol'],
                   'source': input_path, 'sha256': input_sha, 'artifact_sha256': ARTIFACT,
                   'snapshot_path': snapshot_path, 'snapshot_sha256': snapshot_sha}
        conditions = {'contended': False, **{name: {'observed_utc': stamp,
                      'processes': [{'pid': 1, 'name': 'synthetic-contract-control'}],
                      'competing_build_test_processes': [], 'competing_gpu_processes': []}
                      for name, stamp in [('before', '2026-10-08T00:00:00Z'), ('after', '2026-10-08T00:03:00Z')]}}
        condition_path, condition_sha = store_json(directory / 'conditions.json', conditions)
        source_quality = {'available': True, 'settings': {'advancedLight': True, 'advancedMaterials': True, 'debug': False},
                          'gamma_input': False, 'gamma_output': False, 'shadow_enabled': True, 'shadow_soft': True,
                          'lights': [{'shadow_map_width': 512, 'shadow_map_height': 512, 'cast_shadow': True,
                                      'shadow_darkness': .35, 'shadow_bias': -.0001}]}
        gpu = {'kind': 'webgl2', 'version': 'WebGL 2.0 synthetic contract', 'shading_language': 'WebGL GLSL ES 3.00 synthetic',
               'vendor': 'synthetic control', 'renderer': 'synthetic control', 'canvas': [1280, 720],
               'drawing_buffer': [1280, 720], 'context_lost': False,
               'attributes': {key: False for key in ('alpha', 'depth', 'stencil', 'antialias', 'premultipliedAlpha', 'preserveDrawingBuffer')},
               'caps': {key: 16 for key in ('MAX_TEXTURE_SIZE', 'MAX_RENDERBUFFER_SIZE', 'MAX_VERTEX_ATTRIBS',
                       'MAX_VERTEX_UNIFORM_VECTORS', 'MAX_TEXTURE_IMAGE_UNITS', 'MAX_COMBINED_TEXTURE_IMAGE_UNITS')}}
        times = [3000 + index * 10 for index in range(601)]
        cpu = [1 + index % 6 for index in range(601)]
        intervals = [b-a for a,b in zip(times, times[1:])]
        runs = []
        for index, host in enumerate(ORDER):
            setup = {'viewport': [1280, 720], 'dpr': 1, 'visibility': 'visible', 'world_frames': 20,
                     'callback_name': 'render' if host == 'source' else 'jce_loop', 'browser': 'synthetic control',
                     'context': [copy.deepcopy(gpu)]}
            row = {'host': host, 'repeat': index//2, 'order': index, 'status': 'ACTUAL_GAME_RAF_OBSERVED', 'errors': [],
                   'url': 'http://127.0.0.1:8770/' + host + '.html?fixture=initial',
                   'inputs': {'html_sha256': ARTIFACT if host == 'jce' else 'b'*64}, 'setup': setup,
                   'effective_quality': copy.deepcopy(quality if host == 'jce' else source_quality),
                   'elapsed_ms': 6000, 'fps': 100, 'interval_ms': raw_statistics(intervals), 'cpu_ms': raw_statistics(cpu),
                   'raw': {'raf_timestamps_ms': times.copy(), 'callback_cpu_ms': cpu.copy(), 'interval_ms': intervals.copy(),
                           'warm_start_ms': 0, 'sample_start_ms': 3000}}
            if host == 'source':
                row['inputs'].update(application_sha256='c'*64, three_sha256='d'*64, probe_sha256='e'*64)
                row['source_restore'] = {'revision': '68', 'render_restored': True, 'date_now_restored': True, 'random_restored': True}
                setup.update(compatibility={'object_observe_type': 'function', 'observer_uses_interval': True,
                             'declared_fallback_interval_ms': 50}, quality=copy.deepcopy(source_quality))
                row['normal_update_observation'] = {'before': 10, 'after': 911, 'delta': 901, 'callbacks': 901}
            else:
                setup['effective_environment'] = {'JCE_TRACE': '0', 'JCE_PERF_LOG': '0', 'trace_stream': False,
                    'trace_export': False, 'kpi': False, 'game_trace': False, 'game_timing': False}
            runs.append(row)
        self.data = {'schema': 1, 'protocol':protocol_named('publication'), 'status': 'MATCHED_INITIAL_GAME_RAF_OBSERVED', 'complete': True,
                     'final_claims_allowed': True, 'errors': [], 'jce_artifact_sha256': ARTIFACT, 'reference_commit': PIN,
                     'generated_utc': '2026-10-08T00:01:00Z', 'completed_utc': '2026-10-08T00:02:00Z',
                     'browser_version': 'synthetic contract', 'conditions': {'contended': False,
                     'evidence_path': condition_path, 'evidence_sha256': condition_sha},
                     'requested': {'viewport': [1280,720], 'dpr': 1, 'warmup_ms': 3000, 'sample_ms': 5000, 'min_sample_intervals':600,'sample_max_ms':30000,'order': ORDER.copy()},
                     'jce_effective_quality': quality, 'jce_render_quality': synthetic_linked_quality(directory, gpu), 'runs': runs}

    def test_publication_rejects_prior_three_repeats_and_2500ms_warmup(self):
        controls={
            'three per host':lambda d:d.update(runs=d['runs'][:6]),
            '2500 ms realtime warmup':lambda d:d['runs'][0]['raw'].update(warm_start_ms=500),
        }
        for name,change in controls.items():
            data=copy.deepcopy(self.data);change(data)
            with self.subTest(name=name),self.assertRaises(ValueError):
                validate_web_performance(data,ARTIFACT)

    def test_publication_minimum_intervals_and_bounded_extended_window(self):
        def rewindow(row, count, step):
            times=[3000+index*step for index in range(count)]
            cpu=[1+index%6 for index in range(count)]
            intervals=[after-before for before,after in zip(times,times[1:])]
            elapsed=times[-1]-times[0]
            row['raw'].update(raf_timestamps_ms=times,callback_cpu_ms=cpu,interval_ms=intervals,
                              sample_start_ms=times[0],warm_start_ms=0)
            row.update(elapsed_ms=elapsed,fps=len(intervals)*1000/elapsed,
                       interval_ms=raw_statistics(intervals),cpu_ms=raw_statistics(cpu))
        for name,count,step in [('only599intervals',600,10),('under5seconds',601,8),('over30seconds',601,50.01)]:
            data=copy.deepcopy(self.data);rewindow(data['runs'][0],count,step)
            with self.subTest(name=name),self.assertRaises(ValueError):
                validate_web_performance(data,ARTIFACT)
        # A slow endpoint may extend its original real loop through exactly30s.
        data=copy.deepcopy(self.data);rewindow(data['runs'][0],601,50)
        validate_web_performance(data,ARTIFACT)

    def test_tagged_standard_is_explicit_diagnostic_and_cannot_be_promoted(self):
        data=copy.deepcopy(self.data)
        data.update(protocol=protocol_named('standard'),runs=data['runs'][:6],
                    status='RAW_TIMING_OBSERVED_FINAL_SCOPE_INCOMPLETE',final_claims_allowed=False)
        data['requested'].update(warmup_ms=2500,min_sample_intervals=2,order=['source','jce']*3)
        with self.assertRaisesRegex(ValueError,'publication'):
            validate_web_performance(data,ARTIFACT)
        validate_web_performance(data,ARTIFACT,allow_standard=True)
        data['final_claims_allowed']=True
        with self.assertRaises(ValueError):validate_web_performance(data,ARTIFACT,allow_standard=True)
        for field in ('missing','unknown','modified'):
            broken=copy.deepcopy(self.data)
            if field=='missing':broken.pop('protocol')
            elif field=='unknown':broken['protocol']['id']='unknown'
            else:broken['protocol']['repeats']=3
            with self.subTest(field=field),self.assertRaises(ValueError):
                validate_web_performance(broken,ARTIFACT,allow_standard=True)

    def test_saved_browser_observer_cpu_bounds_and_unknown_prelaunch_protocol(self):
        # This executes the saved observer on Node CPU mocks: no browser, HTTP,
        # canvas context or GPU exists, and all mock application rows fail.
        node=shutil.which('node')
        if not node:self.skipTest('Node CPU syntax/observer control unavailable')
        source=Path(__file__).with_name('verify_web_performance.js')
        code=r"""
const fs=require('node:fs'), assert=require('node:assert/strict');
const tool=eval('('+fs.readFileSync(process.argv[1],'utf8')+')');
let launches=0, captured=null, capturedProtocol=null, mockReport=null;
const download={saveAs:async()=>{}};
const downloader={isClosed:()=>false,waitForEvent:async()=>download,
 evaluate:async(fn,payload)=>{mockReport=JSON.parse(payload.text);}};
const context=()=>({request:{get:async()=>({ok:()=>true,status:()=>200,body:async()=>Buffer.from('CPU MOCK ONLY'),headers:()=>({})})},
 route:async()=>{},addInitScript:async(fn,args)=>{captured=fn;capturedProtocol=args.protocol;throw Error('CPU MOCK: no application/GPU');},
 close:async()=>{},newPage:async()=>downloader});
const fakeBrowser={version:()=> 'CPU MOCK',newContext:async()=>context(),close:async()=>{}};
const page={context:()=>({browser:()=>({browserType:()=>({launch:async()=>{++launches;return fakeBrowser;}})})})};
const identity='a'.repeat(64),config={reference_commit:'784129a685627a18f3d3f8fa594208000d9313e8',
 run_id:'CPU_ONLY',output_dir:'D:/CPU_ONLY',jce_url:'http://127.0.0.1:8785/mock.html',source_url:'http://127.0.0.1:8770/mock.html',
 jce_artifact_sha256:identity,source_html_sha256:identity,source_application_sha256:identity,source_three_sha256:identity,source_probe_sha256:identity};
(async()=>{
 await assert.rejects(tool(page,{...config,protocol:'unknown'},async()=>identity),/Unknown explicit Web timing protocol/);
 assert.equal(launches,0);
 const result=await tool(page,config,async()=>identity);
 assert.equal(result.protocol.id,'td-web-initial-publication-v2');assert.equal(result.runs.length,14);
 assert.equal(result.status,'UNAVAILABLE');assert.equal(result.final_claims_allowed,false);
 assert.equal(mockReport.runs.length,14);assert(captured);
 function initialize(){
  const queue=[];globalThis.window=globalThis;globalThis.requestAnimationFrame=fn=>{queue.push(fn);return 1;};
  globalThis.HTMLCanvasElement=function(){};HTMLCanvasElement.prototype.getContext=()=>null;
  globalThis.performance={now:()=>0};globalThis.TowerDefense={counter:0};
  captured({host:'source',protocol:capturedProtocol});
  const state=window.jcePerfObservation;
  function render(){++TowerDefense.counter;requestAnimationFrame(render);}
  state.gameCallback=render;state.phase='warmup';state.warmStart=0;state.done=false;state.failed=false;
  state.resolve=()=>{state.done=true;};state.reject=()=>{state.failed=true;};
  requestAnimationFrame(render);
  return {state,tick:timestamp=>{assert(queue.length);queue.shift()(timestamp);}};
 }
 let scenario=initialize();scenario.tick(2999);assert.equal(scenario.state.phase,'warmup');
 scenario.tick(3000);assert.equal(scenario.state.phase,'sample');
 for(let n=1;n<=599;n++)scenario.tick(3000+n*10);
 assert.equal(scenario.state.done,false);assert.equal(scenario.state.timestamps.length,600);
 scenario.tick(9000);assert.equal(scenario.state.done,true);assert.equal(scenario.state.timestamps.length,601);
 scenario=initialize();scenario.tick(3000);scenario.tick(33001);
 assert.equal(scenario.state.failed,true);assert.equal(scenario.state.phase,'unavailable');
 assert.deepEqual(scenario.state.timestamps,[3000,33001]);assert.equal(scenario.state.cpu.length,2);
 console.log('CPU observer only: unknown prelaunch rejects;14 failed mock rows;3000ms/600intervals/bounded raw retained');
})().catch(error=>{console.error(error);process.exitCode=1;});
"""
        result=subprocess.run([node,'-e',code,str(source)],capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('CPU observer only',result.stdout)

    def test_valid_contract_and_entry_point(self):
        validate_web_performance(self.data, ARTIFACT)
        write_verification.verify_bound_data('web-performance', self.data, {'web': {'sha256': ARTIFACT}})

    def test_synthetic_source_approval_is_local_and_production_still_rejects_it(self):
        with mock.patch.object(render_quality_admission, "REVIEWED_LINKED_SOURCE_SHA256",
                               self.production_shader_registry):
            with self.assertRaisesRegex(ValueError, "Unknown full linked-source identity"):
                validate_web_performance(self.data, ARTIFACT)
        # The local fixture approval resumes; it never changes production policy.
        validate_web_performance(self.data, ARTIFACT)

    def test_reject_incomplete_and_unavailable(self):
        for key, invalid in [('schema', True), ('status', 'UNAVAILABLE'), ('complete', False),
                             ('final_claims_allowed', False), ('errors', [{'kind': 'failure'}]),
                             ('browser_version', ''), ('jce_artifact_sha256', 'f'*64)]:
            data = copy.deepcopy(self.data); data[key] = invalid
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_web_performance(data, ARTIFACT)

    def test_reject_actual_observation_mutations(self):
        changes = {
            'missing run': lambda d: d['runs'].pop(),
            'wrong order': lambda d: d['runs'].reverse(),
            'unavailable row': lambda d: d['runs'][0].update(status='UNAVAILABLE'),
            'no update': lambda d: d['runs'][0].pop('normal_update_observation'),
            'frozen source renderer': lambda d: d['runs'][0]['source_restore'].update(render_restored=False),
            'update skipped': lambda d: d['runs'][0]['normal_update_observation'].update(delta=199),
            'update never warm': lambda d: d['runs'][0]['normal_update_observation'].update(before=0, after=6, delta=6, callbacks=6),
            'wrong callback': lambda d: d['runs'][0]['setup'].update(callback_name='frozenRender'),
            'no context': lambda d: d['runs'][0]['setup'].update(context=[]),
            'no GPU renderer': lambda d: d['runs'][0]['setup']['context'][0].update(renderer=None),
            'adapter changed': lambda d: d['runs'][2]['setup']['context'][0].update(renderer='another adapter'),
            'context lost': lambda d: d['runs'][0]['setup']['context'][0].update(context_lost=True),
            'MSAA mismatch': lambda d: d['runs'][0]['setup']['context'][0]['attributes'].update(antialias=True),
            'DPR bool': lambda d: d['runs'][0]['setup'].update(dpr=True),
            'trace active': lambda d: d['runs'][1]['setup']['effective_environment'].update(JCE_TRACE='1'),
            'short warmup': lambda d: d['runs'][0]['raw'].update(warm_start_ms=501),
            'short sample': lambda d: d['runs'][0]['raw']['raf_timestamps_ms'].__setitem__(-1, 7999),
            'duplicate RAF': lambda d: d['runs'][0]['raw']['raf_timestamps_ms'].__setitem__(1, 3000),
            'filtered tail': lambda d: d['runs'][0]['raw']['interval_ms'].pop(),
            'CPU unavailable': lambda d: d['runs'][0]['raw'].update(callback_cpu_ms=[]),
            'nonfinite CPU': lambda d: d['runs'][0]['raw']['callback_cpu_ms'].__setitem__(0, float('nan')),
            'invented FPS': lambda d: d['runs'][0].update(fps=60),
            'invented P99': lambda d: d['runs'][0]['interval_ms'].update(p99=500),
            'quality unavailable': lambda d: d['jce_effective_quality'].update(available=False),
            'source shadows disabled': lambda d: d['runs'][0]['effective_quality'].update(shadow_enabled=False),
            'changed source bytes': lambda d: d['runs'][2]['inputs'].update(three_sha256='f'*64),
            'stale measured HTML': lambda d: d['runs'][1]['inputs'].update(html_sha256='f'*64),
            'fixture missing': lambda d: d['runs'][0].update(url='http://127.0.0.1:8770/source.html'),
            'conditions unspecified': lambda d: d.update(conditions={}),
            'conditions busy': lambda d: d['conditions'].update(contended=True),
            'timestamps not bracketed': lambda d: d.update(completed_utc='2026-10-08T00:04:00Z'),
            'timezone ambiguous': lambda d: d.update(generated_utc='2026-10-08T00:01:00'),
        }
        for name, mutate in changes.items():
            data = copy.deepcopy(self.data); mutate(data)
            with self.subTest(name=name), self.assertRaises(ValueError):
                validate_web_performance(data, ARTIFACT)

    def test_reject_changed_files_and_forged_parser_output(self):
        quality = self.data['jce_effective_quality']
        raw_path = Path(quality['source'])
        raw = raw_path.read_bytes()
        raw_path.write_bytes(raw + b' ')
        with self.assertRaises(ValueError): validate_web_performance(self.data, ARTIFACT)
        raw_path.write_bytes(raw)
        snapshot = json.loads(Path(quality['snapshot_path']).read_text())
        snapshot['effective']['postfx']['gamma'] = 2
        _, quality['snapshot_sha256'] = store_json(Path(quality['snapshot_path']), snapshot)
        with self.assertRaises(ValueError): validate_web_performance(self.data, ARTIFACT)

    def test_actual_linked_quality_is_mandatory_and_cannot_be_promoted(self):
        changes = {
            'missing witness': lambda d: d.pop('jce_render_quality'),
            'unavailable witness': lambda d: d['jce_render_quality'].update(available=False),
            'wrong witness artifact': lambda d: d['jce_render_quality'].update(artifact_sha256='f' * 64),
            'invented submitted value': lambda d: d['jce_render_quality']['observed']['ground_draws'][0]['uniforms'].update(u_lookHemiGround=[0] * 4),
        }
        for name, mutate in changes.items():
            data = copy.deepcopy(self.data); mutate(data)
            with self.subTest(name=name), self.assertRaises(ValueError):
                validate_web_performance(data, ARTIFACT)
        witness = self.data['jce_render_quality']
        path = Path(witness['source'])
        raw = path.read_bytes()
        path.write_bytes(raw + b' ')
        with self.assertRaises(ValueError): validate_web_performance(self.data, ARTIFACT)
        path.write_bytes(raw)
        observed = copy.deepcopy(witness['observed'])
        observed['ground_draws'][0]['uniforms']['u_lookHemiGround'][3] = 0
        _, witness['sha256'] = store_json(path, observed)
        witness['observed'] = observed
        with self.assertRaisesRegex(ValueError, 'ground uniform'):
            validate_web_performance(self.data, ARTIFACT)

    def test_actual_linked_quality_uses_original_source_and_same_measured_context(self):
        witness = self.data['jce_render_quality']
        original = copy.deepcopy(witness['observed'])
        observed = copy.deepcopy(original)
        observed['contexts'][0]['renderer'] = 'different synthetic GPU'
        _, witness['sha256'] = store_json(Path(witness['source']), observed)
        witness['observed'] = observed
        with self.assertRaisesRegex(ValueError, 'measured JCE context'):
            validate_web_performance(self.data, ARTIFACT)
        _, witness['sha256'] = store_json(Path(witness['source']), original)
        witness['observed'] = original
        fragment = Path(original['ground_draws'][0]['fragment_source']['path'])
        raw = fragment.read_bytes()
        fragment.write_bytes(raw.replace(b'return 1.0;', b'return 0.0;', 1))
        with self.assertRaises(ValueError): validate_web_performance(self.data, ARTIFACT)

    def test_actual_failed_web09_quality_remains_rejected(self):
        if not ARCHIVED_WEB09.is_file():
            self.skipTest('Archived actual Web09 observation unavailable; synthetic controls remain independent')
        raw = ARCHIVED_WEB09.read_bytes()
        observed = json.loads(raw)
        artifact = observed['artifact_sha256']
        wrapper = {'available': True, 'artifact_sha256': artifact, 'source': str(ARCHIVED_WEB09),
                   'sha256': hashlib.sha256(raw).hexdigest(), 'observed': observed}
        with mock.patch.object(render_quality_admission, "REVIEWED_LINKED_SOURCE_SHA256",
                               self.production_shader_registry):
            with self.assertRaisesRegex(ValueError, 'Successful current Web ground-quality'):
                validate_actual_render_quality(wrapper, artifact)

    def test_reject_observed_cpu_gpu_competition_even_if_caller_says_quiet(self):
        path = Path(self.data['conditions']['evidence_path'])
        original = json.loads(path.read_text())
        for name, key, value in [('before', 'competing_build_test_processes', [{'pid': 33}]),
                                 ('after', 'competing_gpu_processes', [{'pid': 44}]),
                                 ('before', 'processes', []), ('after', 'observed_utc', 'invalid')]:
            data = copy.deepcopy(self.data); evidence = copy.deepcopy(original)
            evidence[name][key] = value
            _, data['conditions']['evidence_sha256'] = store_json(path, evidence)
            with self.subTest(name=name, key=key), self.assertRaises(ValueError):
                validate_web_performance(data, ARTIFACT)


    def work_report(self):
        data=copy.deepcopy(self.data)
        data.update(schema=2,protocol=protocol_named('v3-work-consumed'),status='MATCHED_INITIAL_GAME_WORK_CONSUMED_OBSERVED')
        base=data['runs'];runs=[]
        fragment=data['jce_render_quality']['observed']['ground_draws'][0]['fragment_source']
        for repeat in range(7):
            for mode in ('off','on'):
                for host in ('source','jce'):
                    row=copy.deepcopy(base[repeat*2+(host=='jce')]);index=len(runs)
                    row.update(order=index,repeat=repeat,observer_mode=mode)
                    row['inputs']['executed_html_sha256']=row['inputs']['html_sha256']
                    row['world_programs']=[{'id':1,'fragment_sha256':fragment['sha256'],'fragment_bytes':fragment['bytes']}]
                    if host=='jce':
                        row['setup'].update(work_environment={'update_census':mode},census_setup=[1,7,1,int(mode=='on'),20 if mode=='on' else 0,20 if mode=='on' else 0,0,0])
                    records=[]
                    if mode=='on':
                        for n,timestamp in enumerate(row['raw']['raf_timestamps_ms']):
                            record={'update_kind':'td_census' if host=='jce' else 'source_counter',
                                'timestamp_ms':timestamp,'world_draw_calls':1,'world_program_mask':2}
                            if host=='jce':record.update(census_before=[1,7,1,1,320+n,320+n,0,0],census_after=[1,7,1,1,321+n,321+n,0,0])
                            else:record.update(source_update_before=310+n,source_update_after=311+n)
                            records.append(record)
                    row['work_observation']={'schema':1,'mode':mode,'samples':records,
                        'update_kind':'td_census' if host=='jce' else 'source_counter',
                        'scope':'Actual callback update and setup-program GL calls; not CPU submission/GPU completion'}
                    runs.append(row)
        data['runs']=runs;data['requested']['order']=['source','jce']*14
        data['submission_observation']={'available':False,'reason':'No same-callback CPU submission getter in this consumer protocol'}
        data['observer_control']={'schema':1,'pairs':[],
            'scope':'Same-artifact off/on workload delta; do not subtract observer cost or infer isolated instruction cost'}
        for index in range(0,len(runs),4):
            for host_index in range(2):
                off,on=runs[index+host_index],runs[index+2+host_index]
                data['observer_control']['pairs'].append({'host':off['host'],'repeat':off['repeat'],
                    'off_order':off['order'],'on_order':on['order'],'callback_cpu_mean_delta_ms':0,
                    'interval_mean_delta_ms':0,'callback_fps_delta':0})
        return data

    def test_v3_same_window_update_and_world_positive_preserves_v2(self):
        validate_web_performance(self.data,ARTIFACT)
        work=self.work_report()
        validate_web_performance(work,ARTIFACT)
        write_verification.verify_bound_data('web-performance',work,{'web':{'sha256':ARTIFACT}})
        # Negative raw overhead is allowed; retain it without clipping/subtraction.
        for index,row in enumerate(work['runs']):
            if row['observer_mode']=='on':
                row['raw']['callback_cpu_ms']=[value*.5 for value in row['raw']['callback_cpu_ms']]
                row['cpu_ms']=raw_statistics(row['raw']['callback_cpu_ms'])
        for pair in work['observer_control']['pairs']:
            off,on=work['runs'][pair['off_order']],work['runs'][pair['on_order']]
            pair['callback_cpu_mean_delta_ms']=on['cpu_ms']['mean']-off['cpu_ms']['mean']
            self.assertLess(pair['callback_cpu_mean_delta_ms'],0)
        validate_web_performance(work,ARTIFACT)

    def test_v3_missing_update_draw_return_generation_and_reset_are_rejected(self):
        changes={
            'forged proof kind':lambda d:d['runs'][2]['work_observation'].update(update_kind='td_census'),
            'forged source generation':lambda d:d['runs'][2]['work_observation']['samples'][7].update(census_before=[1,1,1,1,7,7,0,0]),
            'missing sample':lambda d:d['runs'][3]['work_observation']['samples'].pop(),
            'missing update':lambda d:d['runs'][3]['work_observation']['samples'][7]['census_after'].__setitem__(4,327),
            'missing normal return':lambda d:d['runs'][3]['work_observation']['samples'][7]['census_after'].__setitem__(5,327),
            'missing actual draw':lambda d:d['runs'][3]['work_observation']['samples'][7].update(world_draw_calls=0),
            'foreign draw program':lambda d:d['runs'][3]['work_observation']['samples'][7].update(world_program_mask=4),
            'generation change':lambda d:d['runs'][3]['work_observation']['samples'][7]['census_after'].__setitem__(1,8),
            'counter reset':lambda d:d['runs'][3]['work_observation']['samples'][7]['census_before'].__setitem__(4,1),
            'dead instance':lambda d:d['runs'][3]['work_observation']['samples'][7]['census_after'].__setitem__(2,0),
            'overflow':lambda d:d['runs'][3]['work_observation']['samples'][7]['census_after'].__setitem__(6,1),
            'invalidated host/multi-instance':lambda d:d['runs'][3]['work_observation']['samples'][7]['census_after'].__setitem__(7,1),
            'misaligned callback':lambda d:d['runs'][3]['work_observation']['samples'][7].update(timestamp_ms=90000),
            'same-count source reset':lambda d:d['runs'][2]['work_observation']['samples'][7].update(source_update_before=1,source_update_after=2),
        }
        baseline=self.work_report()
        for name,change in changes.items():
            data=copy.deepcopy(baseline);change(data)
            with self.subTest(name=name),self.assertRaises(ValueError):validate_web_performance(data,ARTIFACT)

    def test_v3_off_on_controls_and_artifact_binding_cannot_be_dropped(self):
        changes={
            'missing off/on pair':lambda d:d['observer_control']['pairs'].pop(),
            'reversed mode':lambda d:d['runs'][3].update(observer_mode='off'),
            'different executed artifact':lambda d:d['runs'][3]['inputs'].update(executed_html_sha256='f'*64),
            'invented overhead correction':lambda d:d['observer_control']['pairs'][1].update(callback_cpu_mean_delta_ms=-4),
            'off census is enabled':lambda d:d['runs'][1]['setup']['census_setup'].__setitem__(3,1),
            'off row collects work':lambda d:d['runs'][1]['work_observation'].update(samples=[{}]),
            'foreign ground source':lambda d:d['runs'][3]['world_programs'][0].update(fragment_sha256='f'*64),
            'fake CPU submission':lambda d:d['submission_observation'].update(available=True),
            'v2 schema relabel':lambda d:d.update(schema=1),
        }
        baseline=self.work_report()
        for name,change in changes.items():
            data=copy.deepcopy(baseline);change(data)
            with self.subTest(name=name),self.assertRaises(ValueError):validate_web_performance(data,ARTIFACT)


if __name__ == '__main__':
    unittest.main()
