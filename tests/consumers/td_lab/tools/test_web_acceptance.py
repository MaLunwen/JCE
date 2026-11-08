"""CPU-only evidence contract controls; no fixture is a product observation."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from web_acceptance_validation import CPU_PHASES, GL_KINDS, validate_input, validate_cleanup, validate_web_lifecycle
from lifecycle_trace_report import analyze_trace
from test_web_evidence import ARTIFACT, ROOT, store_json
import write_verification


class AcceptanceControls(unittest.TestCase):
    def setUp(self):
        directory = ROOT / 'build' / 'verification'
        directory.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='web-acceptance-control-', dir=directory)
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        request = {'schema': 1, 'artifact_sha256': ARTIFACT, 'nonce': 'b'*64, 'closure_sha256': 'c'*64,
                   'target': 'TowerDefense', 'selected_count': 1, 'factory_count': 1,
                   'environment': {'JCE_NATIVE_REGISTRY_NONCE': 'b'*64, 'JCE_NATIVE_REGISTRY_REPORT': '/receipt.json'}}
        raw = json.dumps(request)
        self.common = {'schema': 1, 'status': 'OBSERVED_CHECKS_PASSED', 'artifact_sha256': ARTIFACT,
                       'served_artifact_sha256': ARTIFACT, 'request_sha256': hashlib.sha256(raw.encode()).hexdigest(),
                       'registry_request_json': raw, 'errors': [], 'browser': 'synthetic contract', 'world_draw_calls': 30,
                       'receipt': {'schema':1, 'ok':True, 'nonce':'b'*64, 'closure_sha256':'c'*64, 'target':'TowerDefense',
                                   'selected_count':1, 'resolved_count':1, 'factory_count':1, 'registered_factory_count':1},
                       'artifact_url':'http://127.0.0.1:8785/test.html', 'input_events':[], 'input_events_lost':0,
                       'network': {'restricted':False, 'unexpected':[], 'requests':[{'source':'artifact_identity_http',
                                   'method':'GET','url':'http://127.0.0.1:8785/test.html','status':200}]}}

    def navigation(self, data, suffix):
        data['run_url'] = data['artifact_url'] + suffix
        data['network']['requests'].append({'source':'browser','method':'GET','url':data['run_url'],'status':200})

    def input(self):
        data = copy.deepcopy(self.common); data['mode'] = 'input'
        self.navigation(data, '?qa=&fixture=initial')
        data['logs'] = ['TD_TRACE action=select selected=60', 'TD_TRACE action=select selected=-1',
                        'TD_TRACE action=select selected=60', 'TD_TRACE action=place resources=554 towers=1',
                        'TD_CAMERA x=1 y=2 z=3', 'TD_CAMERA x=4 y=2 z=3', 'TD_CAMERA width=896 height=504']
        data.update(quickClick={'passed':True,'delay_ms':0,'select_events':1,'selected':60},
                    ordinaryClick={'passed':True,'delay_ms':100,'select_events':1,'selected':60,
                    'prior_same_tile_deselect':{'passed':True,'delay_ms':100,'select_events':1,'selected':-1}},
                    placement={'passed':True,'cost':1,'resources':554,'towers':1,'trace':data['logs'][3]},
                    drag={'from':[640,360],'to':[700,330],'steps':6,'pose_changed':True,'pictures_differ':True},
                    before=data['logs'][4], after=data['logs'][5], keyboard={'held':0,'released':1},
                    resize=data['logs'][6],canvas=[896,504])
        for kind in ('mousedown','mouseup','mousemove','click','click','click','click','keydown','keyup','resize'):
            data['input_events'].append({'type':kind,'trusted':True,'timestamp_ms':100+len(data['input_events']),
                'x':640,'y':360,'key':'e','width':896,'height':504})
        clicks = [event for event in data['input_events'] if event['type']=='click']; clicks[-1].update(x=535,y=665)
        data['screenshots'] = []
        for name in ('before','after','resize'):
            # Signature-only controls check file identity; these are not rendered PNG evidence.
            raw = b'\x89PNG\r\n\x1a\n' + name.encode(); path = self.directory / (name + '.png'); path.write_bytes(raw)
            data['screenshots'].append({'path':str(path),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()})
        return data

    def cleanup(self):
        data = copy.deepcopy(self.common); data['mode'] = 'lifecycle'
        self.navigation(data, '?qa=&autodefense=&frames=1000')
        events = [{'ph':'M','name':'thread_name','pid':1,'tid':0,'args':{'name':'synthetic cooperative control'}}]
        events += [{'ph':'i','cat':'frame','name':'frame','pid':1,'tid':0,'ts':index,'args':{'id':index}} for index in range(1000)]
        for index, name in enumerate(CPU_PHASES, 1):
            common = {'cat':'task','name':name,'pid':1,'tid':0}
            events += [{**common,'ph':'i','ts':index*10,'args':{'task':index,'parent':0,'kind':2}},
                       {**common,'ph':'B','ts':index*10+1,'args':{'id':index,'parent':index}},
                       {**common,'ph':'E','ts':index*10+2,'args':{'id':index,'parent':index,'ns':1000,'state':0}}]
        metadata = {'mode':'continuous','consumer_mode':'cooperative','worker_available':False,'write_ok':True,
                    'capture_complete':True,'capture_generation_stable':True,'recorded':len(events),'captured':len(events),
                    **{key:0 for key in ('overwritten_before_capture','cursor_lost','lost','unexported','active_threads','active_work_items','active_waits')}}
        trace = {'jceTrace':metadata,'traceEvents':events}
        trace_path, trace_sha = store_json(self.directory/'trace.json',trace)
        resources = {kind:{'available':True,'created':1,'deleted':1,'delete_calls':1,'live':0,'valid_survivors':0} for kind in GL_KINDS}
        context = {'kind':'webgl2','version':'WebGL synthetic contract','shading_language':'WebGL synthetic',
                   'vendor':'synthetic','renderer':'synthetic','context_lost':False,'gl_error_after_shutdown':0,'resources':resources,
                   'attributes':{key:False for key in ('alpha','depth','stencil','antialias','premultipliedAlpha','preserveDrawingBuffer')}}
        data.update(logs=['TD_TRACE action=destroy'],shutdownWaitError=None,contexts=[context],
                    shutdown={'programsCreated':1,'programsDeleted':1,'live':0,'resources':{kind:{'created':1,'deleted':1,'live':0,'valid_survivors':0} for kind in GL_KINDS if kind!='Program'}},
                    trace_metadata=metadata,trace_path=trace_path,trace_sha256=trace_sha,
                    frame_observation={'requested':1000,'observed':1000,'first':0,'last':999},wasm_linear_memory_capacity_bytes=65536)
        return data

    def test_valid_contract_entry_points(self):
        validate_input(self.input(),ARTIFACT)
        validate_cleanup(self.cleanup(),ARTIFACT)
        write_verification.verify_bound_data('web-input',self.input(),{'web':{'sha256':ARTIFACT}})
        write_verification.verify_bound_data('web-gpu-cleanup',self.cleanup(),{'web':{'sha256':ARTIFACT}})

    def test_actual_logger_color_suffix_keeps_numeric_validation(self):
        data = self.input()
        original = data['resize']
        colored = '\x1b[93m' + original + '\x1b[0m \x1b[96mat controller.c:30\x1b[0m'
        data['logs'][-1] = data['resize'] = colored
        validate_input(data, ARTIFACT)
        for bad in ('504garbage', 'NaN', '503', 'True'):
            broken = copy.deepcopy(data)
            broken['logs'][-1] = broken['resize'] = colored.replace('height=504', 'height=' + bad)
            with self.subTest(bad=bad), self.assertRaises(ValueError): validate_input(broken, ARTIFACT)

    def test_reject_input_claims_without_observation(self):
        mutations = {
            'failed status':lambda d:d.update(status='FAILED'), 'old schema':lambda d:d.pop('schema'),
            'old request missing':lambda d:d.pop('registry_request_json'),
            'stale served bytes':lambda d:d.update(served_artifact_sha256='f'*64),
            'receipt nonce':lambda d:d['receipt'].update(nonce='f'*64),
            'receipt bool count':lambda d:d['receipt'].update(resolved_count=True),
            'lost events':lambda d:d.update(input_events_lost=1),
            'synthetic events':lambda d:d['input_events'][0].update(trusted=False),
            'no browser navigation':lambda d:d['network']['requests'].pop(),
            'network error':lambda d:d['network']['requests'][0].update(status=404),
            'duplicate dispatch':lambda d:d['logs'].append('TD_TRACE action=select selected=60'),
            'wrong cost':lambda d:d['placement'].update(cost=2),
            'no keyup':lambda d:d.update(input_events=[event for event in d['input_events'] if event['type']!='keyup']),
            'camera unchanged':lambda d:d.update(after=d['before']),
            'wrong resize':lambda d:d.update(canvas=[1280,720]),
            'stale picture':lambda d:d['screenshots'][0].update(sha256='f'*64),
        }
        for name, mutate in mutations.items():
            data=self.input();mutate(data)
            with self.subTest(name=name),self.assertRaises(ValueError):validate_input(data,ARTIFACT)

    def test_lifecycle_download_event_is_retained_without_becoming_user_input(self):
        data = self.cleanup()
        data['input_events'] = [{'type':'click', 'timestamp_ms':18596.54, 'trusted':False,
                                 'x':0, 'y':0, 'width':640, 'height':360}]
        validate_cleanup(data, ARTIFACT)
        data['input_events'][0]['trusted'] = None
        with self.assertRaises(ValueError): validate_cleanup(data, ARTIFACT)

    def test_reject_cleanup_summary_without_actual_retirement(self):
        mutations = {
            'no context':lambda d:d.update(contexts=[]), 'missing kind':lambda d:d['contexts'][0]['resources'].pop('Shader'),
            'GL error':lambda d:d['contexts'][0].update(gl_error_after_shutdown=1282),
            'context loss':lambda d:d['contexts'][0].update(context_lost=True),
            'live object':lambda d:d['contexts'][0]['resources']['Buffer'].update(live=1),
            'delete mismatch':lambda d:d['contexts'][0]['resources']['Buffer'].update(deleted=0),
            'valid survivor':lambda d:d['contexts'][0]['resources']['Shader'].update(valid_survivors=1),
            'missing validity':lambda d:d['contexts'][0]['resources']['Shader'].update(valid_survivors=None),
            'summary fabrication':lambda d:d['shutdown'].update(programsCreated=2),
            'incomplete run':lambda d:d['frame_observation'].update(observed=999),
            'trace changed':lambda d:d.update(trace_sha256='f'*64),
            'lost capture':lambda d:d['trace_metadata'].update(lost=1),
        }
        for name, mutate in mutations.items():
            data=self.cleanup();mutate(data)
            with self.subTest(name=name),self.assertRaises(ValueError):validate_cleanup(data,ARTIFACT)

    def test_reject_missing_required_cpu_phase_or_frame_gap(self):
        for name in ('missing phase','frame gap'):
            data=self.cleanup(); trace=json.loads(Path(data['trace_path']).read_text())
            if name=='missing phase':
                trace['traceEvents']=[event for event in trace['traceEvents'] if event['name']!=CPU_PHASES[-1]]
                trace['jceTrace']['recorded']=trace['jceTrace']['captured']=len(trace['traceEvents'])
            else:
                trace['traceEvents'][2]['args']['id']=0
            _,data['trace_sha256']=store_json(Path(data['trace_path']),trace);data['trace_metadata']=trace['jceTrace']
            with self.subTest(name=name),self.assertRaises(ValueError):validate_cleanup(data,ARTIFACT)

    def test_absolute_slash_alias_requires_the_same_actual_trace_file(self):
        cleanup=self.cleanup();trace_path=Path(cleanup['trace_path'])
        trace=json.loads(trace_path.read_text())
        cleanup['trace_path']=trace_path.as_posix()
        path,digest=store_json(self.directory/'cleanup-slashes.json',cleanup)
        data={**analyze_trace(trace,list(CPU_PHASES)),'trace':str(trace_path.resolve()),'sha256':cleanup['trace_sha256'],
              'cleanup_evidence_path':path,'cleanup_evidence_sha256':digest}
        validate_web_lifecycle(data,ARTIFACT)
        copy_path=self.directory/'different-file-same-bytes.json';copy_path.write_bytes(trace_path.read_bytes())
        self.assertEqual(hashlib.sha256(copy_path.read_bytes()).hexdigest(),cleanup['trace_sha256'])
        for fake_path in (str(copy_path.resolve()),'trace.json'):
            with self.subTest(fake_path=fake_path),self.assertRaises(ValueError):
                validate_web_lifecycle({**data,'trace':fake_path},ARTIFACT)

    def test_web_lifecycle_requires_same_artifact_cleanup_and_recomputed_summary(self):
        cleanup=self.cleanup(); trace=json.loads(Path(cleanup['trace_path']).read_text())
        path,digest=store_json(self.directory/'cleanup.json',cleanup)
        data={**analyze_trace(trace,list(CPU_PHASES)),'trace':cleanup['trace_path'],'sha256':cleanup['trace_sha256'],
              'cleanup_evidence_path':path,'cleanup_evidence_sha256':digest}
        validate_web_lifecycle(data,ARTIFACT)
        write_verification.verify_bound_data('web-lifecycle',data,{'web':{'sha256':ARTIFACT}})
        for key, value in [('cleanup_evidence_sha256','f'*64),('trace','someotherfile'),('required',[]),('scopes',[])]:
            broken={**data,key:value}
            with self.subTest(key=key),self.assertRaises(ValueError):validate_web_lifecycle(broken,ARTIFACT)
        with self.assertRaises(ValueError):validate_web_lifecycle(data,'f'*64)


if __name__=='__main__':unittest.main()
