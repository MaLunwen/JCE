"""Known Tower Defense native timing protocols and byte-bound raw admission."""
from __future__ import annotations
from datetime import datetime
import hashlib
import math
from pathlib import Path
import re
import sys
from web_evidence import number, referenced_bytes, require, sha, strict_json, text
from web_performance_validation import close_number, object_value, same_json
sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'tools'))
from benchmark_suite import read_frames
from jce_determinism import assert_backend

PROTOCOLS = {
    'standard': {'id':'td-native-standard-v1','warmup_frames':120,'measured_frames':300,'repeats':3},
    'publication': {'id':'td-native-publication-v2','startup_discard_frames':120,
                    'warmup_min_ms':3000,'warmup_max_frames':3000,'measured_frames':600,'repeats':7},
}
HOSTS = ('player','editor')
MODES = ('menu','combat','stress-200')
RAW_NAMES = {'frames_csv':'frames.csv','guard':'guard.json','execution':'result.json',
             'stdout':'stdout.log','stderr':'stderr.log','os_samples':'samples.json'}


def protocol_named(name):
    require(name in PROTOCOLS,'Unknown native timing protocol')
    return PROTOCOLS[name].copy()


def validate_protocol(value):
    value=object_value(value,'native protocol')
    require(any(same_json(value,known) for known in PROTOCOLS.values()),'Unknown, incomplete or modified native timing protocol')
    return value


def capture_frame_budget(protocol):
    protocol=validate_protocol(protocol)
    warmup=protocol.get('warmup_frames')
    if warmup is None:
        warmup=protocol['startup_discard_frames']+protocol['warmup_max_frames']
    return warmup+protocol['measured_frames']+30


def identity(path):
    path=Path(path).resolve();raw=path.read_bytes()
    return {'path':str(path),'sha256':hashlib.sha256(raw).hexdigest(),'bytes':len(raw)}


def utc(value,name):
    try:
        value=datetime.fromisoformat(text(value,name).replace('Z','+00:00'))
    except ValueError as error: raise ValueError('Invalid UTC timestamp: '+name) from error
    require(value.tzinfo is not None and value.utcoffset().total_seconds()==0,'UTC timestamp required: '+name)
    return value


def summary(values):
    values=[value for value in values if math.isfinite(value)]
    if not values:return None
    ordered=sorted(values)
    def percentile(fraction):
        position=(len(ordered)-1)*fraction;lower=math.floor(position);upper=math.ceil(position)
        return ordered[lower]+(ordered[upper]-ordered[lower])*(position-lower)
    return {'samples':len(values),'p50_ms':percentile(.5),'p95_ms':percentile(.95),'p99_ms':percentile(.99),
            'max_ms':ordered[-1],'mean_ms':math.fsum(values)/len(values)}


def parse_kpi(path,protocol):
    protocol=validate_protocol(protocol)
    measured=protocol['measured_frames']
    if 'warmup_frames' in protocol:
        # Preserve the standard-v1 diagnostic window and historical admission.
        warmup=protocol['warmup_frames']
        raw=read_frames(path,warmup=warmup,sample_count=measured)
        require(list(raw['frame_indices'])==list(range(warmup,warmup+measured)),
                'Actual KPI frames do not begin at zero or match the declared window')
        return {name:summary(raw[key]) for name,key in [('frame_wall','frame_ms'),('frame_cpu','cpu_frame_ms'),('frame_gpu','gpu_frame_ms')]}
    discard=protocol['startup_discard_frames'];budget=protocol['warmup_max_frames']
    raw=read_frames(path,warmup=discard,sample_count=None)
    require(list(raw['frame_indices'])==list(range(discard,discard+len(raw['frame_indices']))),
            'Actual KPI frames do not begin at zero or remain continuous')
    warm_count=None;running=0.0
    for offset,value in enumerate(raw['frame_ms'][:budget]):
        running+=value
        # Recompute the prefix with fsum near its exact threshold. This permits
        # no time tolerance: the chosen prefix must actually reach 3000 ms.
        if running>=protocol['warmup_min_ms'] or math.isclose(running,protocol['warmup_min_ms'],rel_tol=0,abs_tol=1e-7):
            actual_ms=math.fsum(raw['frame_ms'][:offset+1])
            if actual_ms>=protocol['warmup_min_ms']:
                warm_count=offset+1;break
    require(warm_count is not None, 'Publication realtime warmup did not reach 3000 ms within its frame budget')
    require(len(raw['frame_indices'])>=warm_count+measured,
            'Publication raw capture has fewer than 600 consecutive frames after realtime warmup')
    first=discard+warm_count
    window={'startup_discard_frames':discard,'warmup_first_frame':discard,'warmup_last_frame':first-1,
            'warmup_frames':warm_count,'warmup_ms':math.fsum(raw['frame_ms'][:warm_count]),
            'measured_first_frame':first,'measured_last_frame':first+measured-1,'measured_frames':measured}
    result={name:summary(raw[key][warm_count:warm_count+measured]) for name,key in
            [('frame_wall','frame_ms'),('frame_cpu','cpu_frame_ms'),('frame_gpu','gpu_frame_ms')]}
    result['measurement_window']=window
    return result


def bound_raw(row):
    directory=Path(text(row.get('raw_dir'),'raw native directory'))
    require(directory.is_absolute(),'Raw native directory must be absolute')
    evidence=object_value(row.get('raw_evidence'),'native raw file identities')
    require(set(evidence)==set(RAW_NAMES),'Native raw file identity set missing or unknown')
    result={}
    for key,filename in RAW_NAMES.items():
        entry=object_value(evidence[key],'raw '+key)
        path=Path(text(entry.get('path'),'raw '+key+' path'))
        require(path.is_absolute() and path.resolve(strict=True)==(directory/filename).resolve(strict=True) and
                path.samefile(directory/filename),'Raw file path does not bind the actual run directory: '+key)
        result[key]=referenced_bytes(str(path),entry.get('sha256'))
        require(type(entry.get('bytes')) is int and entry['bytes']==len(result[key]),'Raw file byte count changed: '+key)
    return result


def validate_guard(raw,row):
    guard=object_value(strict_json(raw.decode('utf-8-sig')),'native run scheduling guard')
    start=utc(row.get('started_utc'),'native process start');end=utc(row.get('finished_utc'),'native process end')
    observations={}
    for stage in ('before','after'):
        observation=object_value(guard.get(stage),'native '+stage+' guard')
        observations[stage]=utc(observation.get('observed_utc'),stage+' process observation')
        require(isinstance(observation.get('processes'),list) and bool(observation['processes']),
                'Actual process inventory missing: '+stage)
        require(observation.get('competing_processes')==[],'Named competing build/GPU/test processes observed: '+stage)
    require(observations['before']<=start<=end<=observations['after'],'Native guard does not bracket actual process lifetime')
    text(guard.get('scope'),'named scheduling scope')


def validate_run(row,protocol,artifacts):
    row=object_value(row,'native timing repetition');validate_protocol(protocol)
    require(row.get('host') in HOSTS and row.get('mode') in MODES and type(row.get('repeat')) is int and
            0<=row['repeat']<protocol['repeats'],'Unknown host/mode/repetition')
    expected=artifacts['native' if row['host']=='player' else 'editor']
    sha(expected,'current native artifact')
    require(row.get('exe_sha256')==expected,'Native timing artifact changed')
    raw=bound_raw(row);validate_guard(raw['guard'],row)
    execution=object_value(strict_json(raw['execution'].decode('utf-8-sig')),'raw child execution')
    require(type(execution.get('returncode')) is int and execution['returncode']==0 and execution.get('timed_out') is False,
            'Actual child failed or timed out')
    require(same_json(row.get('process'),execution),'Reported child result differs from raw execution')
    log=(raw['stdout']+raw['stderr']).decode('utf-8',errors='replace');assert_backend(log,'d3d11')
    require('TD_BOOT' in log and 'actor prefab failed to spawn' not in log and 'camera proj:' not in log,'Actual game boot/errors failed')
    if row['mode']=='combat':require(re.search(r'shots=[1-9]',log) is not None,'Combat did not produce real shots')
    if row['mode']=='stress-200':require('spawned=200' in log,'Actual stress workload missing')
    actual=parse_kpi(row['raw_evidence']['frames_csv']['path'],protocol)
    declared=object_value(row.get('kpi'),'native KPI summary')
    require(set(declared)==set(actual),'Native KPI phase summaries missing or unknown')
    for phase,expected_stats in actual.items():
        if phase=='measurement_window':
            require(same_json(declared[phase],expected_stats),'Publication measured window or actual warmup differs from raw CSV');continue
        if expected_stats is None:
            require(declared[phase] is None,'Unavailable native timing was invented: '+phase);continue
        stats=object_value(declared[phase],'native '+phase+' summary')
        require(type(stats.get('samples')) is int and stats['samples']==expected_stats['samples'],'Native sample count differs: '+phase)
        for key,value in expected_stats.items():
            if key!='samples':close_number(stats.get(key),value,phase+'.'+key)
    close_number(row.get('fps'),1000/actual['frame_wall']['mean_ms'],'raw native FPS')
    return row


def clock_points(log):
    return [{'wall_s':float(w),'simulation_s':float(t),'progress':float(p),'updates':int(n),'dropped_s':float(d),'remainder_s':float(a)}
            for w,t,p,n,d,a in re.findall(r'TD_TIME wall=([\d.]+) simulation=([\d.]+) progress=([\d.]+) frames=(\d+) dropped=([\d.]+) remainder=([\d.]+)',log)]


def clock_errors(points):
    require(len(points)>=3,'Actual live-clock points missing')
    for index,point in enumerate(points):
        for key in ('wall_s','simulation_s','progress','dropped_s','remainder_s'):
            number(point[key],'clock '+key)
        number(point['updates'],'clock updates',integer=True)
        if index:require(point['wall_s']>points[index-1]['wall_s'],'Actual clock samples reversed/duplicated')
    accepted=lambda point:point['simulation_s']+point['dropped_s']+point['remainder_s']
    slope=abs((accepted(points[-1])-accepted(points[0]))-(points[-1]['wall_s']-points[0]['wall_s']))
    progress=max(abs(point['progress']-point['updates']*.025/22) for point in points)
    require(slope<.05 and progress<.0001,'Actual live-clock accounting failed')
    return slope,progress


def validate_live_clock(row,artifacts):
    row=object_value(row,'actual live-clock control');require(row.get('exe_sha256')==artifacts['native'],'Live-clock artifact changed')
    raw=bound_raw(row);validate_guard(raw['guard'],row)
    execution=object_value(strict_json(raw['execution'].decode('utf-8-sig')),'raw live-clock execution')
    require(type(execution.get('returncode')) is int and execution['returncode']==0 and execution.get('timed_out') is False and
            same_json(row.get('process'),execution),'Raw live-clock execution failed or differs')
    log=(raw['stdout']+raw['stderr']).decode('utf-8',errors='replace');assert_backend(log,'d3d11')
    require('TD_BOOT' in log,'Live-clock game did not initialize')
    frames=read_frames(row['raw_evidence']['frames_csv']['path'],warmup=1,sample_count=None)
    require(list(frames['frame_indices'])==list(range(1,len(frames['frame_indices'])+1)),'Live-clock CSV does not start at zero')
    points=clock_points(log);slope,progress=clock_errors(points)
    require(row.get('pass') is True and same_json(row.get('points'),points),'Declared live-clock points differ from actual log')
    close_number(row.get('elapsed_clock_error_s'),slope,'actual clock error seconds')
    close_number(row.get('simulation_step_progress_error'),progress,'actual simulation progress error')


def validate_native_performance(data,artifacts,*,allow_partial=False):
    data=object_value(data,'native performance suite');protocol=validate_protocol(data.get('protocol'))
    require(type(data.get('schema')) is int and data['schema']==1 and data.get('errors')==[], 'Current native report schema/errors required')
    require(type(data.get('complete')) is bool and (allow_partial or data['complete'] is True),'Native suite completion scope differs')
    require(data.get('status')==('COMPLETE_NATIVE_TIMING_PROTOCOL_OBSERVED' if data['complete'] else 'NATIVE_TIMING_PROTOCOL_PARTIAL'),
            'Native protocol observation status missing/unavailable')
    conditions=object_value(data.get('conditions'),'native conditions')
    require(conditions.get('contended') is False and conditions.get('resolution')==[1280,720] and
            type(conditions.get('vsync_requested')) is int and conditions['vsync_requested']==1 and conditions.get('fixed_dt') is False and
            conditions.get('stream_sync') is True and conditions.get('visible_window') is False,
            'Native timing conditions missing, contended or changed')
    if protocol['id']=='td-native-publication-v2':
        require(conditions.get('resolution_scope')=='protocol_label_only_unverified_render_surface' and
                'actual_window_size' in conditions and conditions['actual_window_size'] is None and
                'actual_game_fbo_size' in conditions and conditions['actual_game_fbo_size'] is None and
                conditions.get('same_render_surface_comparison') is False,
                'Publication cannot claim an applied or observed shared render extent without actual evidence')
    runs=data.get('runs');require(isinstance(runs,list),'Native runs missing')
    expected={(host,mode,repeat) for host in HOSTS for mode in MODES for repeat in range(protocol['repeats'])}
    keys=[];raw_paths=set();previous_end=None
    for row in runs:
        validate_run(row,protocol,artifacts);keys.append((row['host'],row['mode'],row['repeat']))
        source=Path(row['raw_evidence']['frames_csv']['path']).resolve(strict=True)
        require(source not in raw_paths, 'One raw process capture was reused as multiple repetitions')
        raw_paths.add(source)
        started=utc(row['started_utc'],'native run start');finished=utc(row['finished_utc'],'native run end')
        require(previous_end is None or started>=previous_end,'Native GPU measurement processes overlap or chronology changed')
        previous_end=finished
    require(len(set(keys))==len(keys),'Native workload repetitions duplicated')
    require(set(keys)<=expected and (allow_partial or set(keys)==expected and len(keys)==len(expected)),
            'Missing or foreign native protocol repetition')
    if data['complete']:
        validate_live_clock(data.get('live_clock'),artifacts)
        require(previous_end is None or utc(data['live_clock']['started_utc'],'live clock start')>=previous_end,
                'Live-clock control overlapped native benchmark GPU processes')
