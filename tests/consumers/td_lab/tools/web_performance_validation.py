"""Admission of complete matched Web timing windows, recomputed from raw clocks."""
from __future__ import annotations
from datetime import datetime
import json
import math
from urllib.parse import parse_qsl, urlsplit
from web_evidence import number, quality_from_input, referenced_bytes, referenced_json, require, sha, text
from web_render_quality_evidence import validate_web_render_quality


PIN = '784129a685627a18f3d3f8fa594208000d9313e8'
PROTOCOLS = {
    'standard': {'id':'td-web-initial-standard-v1','repeats':3,'warmup_ms':2500,
                 'sample_ms':5000,'min_sample_intervals':2,'sample_max_ms':30000},
    'publication': {'id':'td-web-initial-publication-v2','repeats':7,'warmup_ms':3000,
                    'sample_ms':5000,'min_sample_intervals':600,'sample_max_ms':30000},
    'v3-work-consumed': {'id':'td-web-initial-publication-v3-work-consumed','repeats':7,'warmup_ms':3000,
                    'sample_ms':5000,'min_sample_intervals':600,'sample_max_ms':30000,'work_observer':True},
}
ORDER = ['source', 'jce'] * PROTOCOLS['publication']['repeats']
CAPS = ('MAX_TEXTURE_SIZE', 'MAX_RENDERBUFFER_SIZE', 'MAX_VERTEX_ATTRIBS',
        'MAX_VERTEX_UNIFORM_VECTORS', 'MAX_TEXTURE_IMAGE_UNITS', 'MAX_COMBINED_TEXTURE_IMAGE_UNITS')


def object_value(value, name):
    require(isinstance(value, dict), 'Missing object: ' + name)
    return value


def same_json(left, right):
    options = {'sort_keys': True, 'allow_nan': False, 'separators': (',', ':')}
    return json.dumps(left, **options) == json.dumps(right, **options)


def protocol_named(name):
    require(name in PROTOCOLS,'Unknown Web timing protocol')
    return PROTOCOLS[name].copy()


def validate_protocol(value):
    value=object_value(value,'Web timing protocol')
    require(any(same_json(value,known) for known in PROTOCOLS.values()),
            'Unknown, untagged or modified Web timing protocol')
    return value


def close_number(actual, expected, name):
    number(actual, name)
    require(math.isclose(actual, expected, rel_tol=1e-9, abs_tol=1e-6), 'Raw value disagrees with ' + name)


def raw_statistics(values):
    ordered = sorted(values)
    def percentile(fraction):
        return ordered[math.ceil(len(ordered) * fraction) - 1]
    return {'samples': len(values), 'mean': sum(values) / len(values), 'min': ordered[0],
            'median': percentile(.5), 'p95': percentile(.95), 'p99': percentile(.99), 'max': ordered[-1]}


def validate_statistics(actual, values, name):
    actual = object_value(actual, name)
    expected = raw_statistics(values)
    require(type(actual.get('samples')) is int and actual['samples'] == expected['samples'], 'Sample count differs: ' + name)
    for key, value in expected.items():
        if key != 'samples':
            close_number(actual.get(key), value, name + '.' + key)


def validate_windows(row, protocol):
    raw = object_value(row.get('raw'), 'raw timing')
    times, cpu, intervals = (raw.get(key) for key in ('raf_timestamps_ms', 'callback_cpu_ms', 'interval_ms'))
    require(isinstance(times, list) and len(times) > 2 and isinstance(cpu, list) and len(cpu) == len(times),
            'Actual game callback timestamps/CPU samples unavailable')
    require(isinstance(intervals, list) and len(intervals) == len(times) - 1, 'Raw intervals missing or filtered')
    require(len(intervals)>=protocol['min_sample_intervals'],'Actual game sample contains fewer than the required consecutive RAF intervals')
    for i, value in enumerate(times):
        number(value, 'RAF timestamp')
        if i:
            require(value > times[i-1], 'RAF timestamps duplicated or reversed')
            close_number(intervals[i-1], value - times[i-1], 'consecutive raw RAF interval')
    for value in cpu:
        number(value, 'callback CPU duration')
    warm = number(raw.get('warm_start_ms'), 'warmup start')
    start = number(raw.get('sample_start_ms'), 'sample start')
    close_number(start, times[0], 'sample start timestamp')
    require(start - warm >= protocol['warmup_ms'], 'Actual warmup shorter than the declared realtime protocol')
    elapsed = times[-1] - times[0]
    require(protocol['sample_ms']<=elapsed<=protocol['sample_max_ms'], 'Actual sample window short or exceeds its declared realtime bound')
    close_number(row.get('elapsed_ms'), elapsed, 'observed sample elapsed')
    close_number(row.get('fps'), len(intervals) * 1000 / elapsed, 'FPS')
    validate_statistics(row.get('interval_ms'), intervals, 'RAF summary')
    validate_statistics(row.get('cpu_ms'), cpu, 'CPU summary')
    return len(times)


def validate_gpu(setup):
    setup = object_value(setup, 'setup')
    require(setup.get('viewport') == [1280, 720] and type(setup.get('dpr')) in (int, float) and setup['dpr'] == 1 and
            setup.get('visibility') == 'visible', 'Actual viewport/DPR/visibility unavailable')
    number(setup.get('world_frames'), 'actual setup world callbacks', integer=True)
    require(setup['world_frames'] >= 20, 'Actual game callback readiness not observed')
    text(setup.get('callback_name'), 'actual game callback identity')
    browser = text(setup.get('browser'), 'actual browser user agent')
    contexts = setup.get('context')
    require(isinstance(contexts, list) and len(contexts) == 1, 'Actual single WebGL context required')
    gpu = object_value(contexts[0], 'actual WebGL context')
    require(gpu.get('kind') in ('webgl', 'webgl2', 'experimental-webgl') and
            text(gpu.get('version'), 'actual context version').startswith('WebGL') and
            text(gpu.get('shading_language'), 'actual shader language').startswith('WebGL'), 'Actual WebGL version unavailable')
    vendor = text(gpu.get('vendor'), 'actual GPU vendor')
    renderer = text(gpu.get('renderer'), 'actual GPU adapter')
    require(gpu.get('canvas') == [1280, 720] and gpu.get('drawing_buffer') == [1280, 720] and
            gpu.get('context_lost') is False, 'Actual canvas/drawing buffer/liveness differs')
    attributes = object_value(gpu.get('attributes'), 'actual context attributes')
    for key in ('alpha', 'depth', 'stencil', 'antialias', 'premultipliedAlpha', 'preserveDrawingBuffer'):
        require(type(attributes.get(key)) is bool, 'Actual context attribute missing: ' + key)
    require(attributes['antialias'] is False, 'Matched initial workload does not enable MSAA')
    caps = object_value(gpu.get('caps'), 'actual GPU capabilities')
    for key in CAPS:
        number(caps.get(key), key, positive=True, integer=True)
    return browser, vendor, renderer


def validate_source_quality(quality):
    quality = object_value(quality, 'actual source quality')
    require(quality.get('available') is True, 'Actual source quality unavailable')
    settings = object_value(quality.get('settings'), 'actual source settings')
    require(settings.get('advancedLight') is True and settings.get('advancedMaterials') is True and settings.get('debug') is False,
            'Source initial quality changed')
    require(quality.get('gamma_input') is False and quality.get('gamma_output') is False and
            quality.get('shadow_enabled') is True and quality.get('shadow_soft') is True, 'Actual source gamma/shadow quality changed')
    lights = quality.get('lights')
    require(isinstance(lights, list) and len(lights) == 1, 'Actual source directional light unavailable')
    light = object_value(lights[0], 'actual source light')
    require(light.get('shadow_map_width') == 512 and light.get('shadow_map_height') == 512 and light.get('cast_shadow') is True,
            'Source shadow resolution/casting changed')
    close_number(light.get('shadow_darkness'), .35, 'source shadow darkness')
    bias = light.get('shadow_bias')
    require(type(bias) in (int, float) and math.isfinite(bias) and math.isclose(bias, -.0001, rel_tol=1e-9, abs_tol=1e-9),
            'Source shadow bias changed')


def validate_jce_quality(quality, artifact):
    quality = object_value(quality, 'actual JCE quality')
    require(quality.get('available') is True and quality.get('artifact_sha256') == artifact, 'Same-artifact actual JCE quality unavailable')
    actual = quality_from_input(referenced_bytes(quality.get('source'), quality.get('sha256')))
    require(actual['artifact_sha256'] == artifact, 'Current successful input quality source unavailable')
    declared = referenced_json(quality.get('snapshot_path'), quality.get('snapshot_sha256'))
    require(same_json(declared, actual), 'Preparsed quality snapshot differs from the single canonical parser')
    require(same_json(actual['effective'], quality.get('effective')) and actual['protocol'] == quality.get('protocol'),
            'Reported quality differs from raw render snapshot')
    effective = object_value(actual['effective'], 'actual render settings')
    fog = object_value(effective.get('fog'), 'JCE fog')
    require(type(effective.get('version')) is int and effective['version'] == 1 and fog.get('enabled') is False and
            effective.get('iblEnabled') is False and type(effective.get('dynamicGI')) is int and effective['dynamicGI'] == 0,
            'JCE lighting/fog quality changed or missing')
    shadows = object_value(effective.get('shadows'), 'JCE shadows')
    require(all(type(shadows.get(key)) is int for key in ('resolution', 'cascades', 'soft')) and
            shadows.get('resolution') == 512 and shadows.get('cascades') == 1 and shadows.get('soft') == 1,
            'JCE shadow quality changed or missing')
    close_number(shadows.get('distance'), 250, 'JCE shadow distance')
    postfx = object_value(effective.get('postfx'), 'JCE postfx')
    for key in ('tonemap', 'bloom', 'fxaa', 'vignette', 'chromatic', 'grayscale', 'custom', 'ssao', 'ssr'):
        require(postfx.get(key) is False, 'JCE postfx quality changed or unavailable: ' + key)
    close_number(postfx.get('exposure'), 1, 'JCE exposure')
    close_number(postfx.get('gamma'), 1, 'JCE gamma')



def validate_actual_render_quality(quality, artifact):
    quality = object_value(quality, 'actual linked WebGL quality wrapper')
    require(quality.get('available') is True and quality.get('artifact_sha256') == artifact,
            'Same-artifact actual linked WebGL quality unavailable')
    observed = referenced_json(quality.get('source'), quality.get('sha256'))
    require(same_json(observed, quality.get('observed')), 'Reported actual WebGL quality differs from raw evidence')
    validate_web_render_quality(observed, artifact)
    return observed['contexts'][0]


def utc_time(value, name):
    value = text(value, name)
    try:
        observed = datetime.fromisoformat(value.replace('Z', '+00:00'))
    except ValueError as error:
        raise ValueError('Invalid ISO timestamp: ' + name) from error
    require(observed.tzinfo is not None and observed.utcoffset().total_seconds() == 0, 'UTC timestamp required: ' + name)
    return observed


def validate_conditions(conditions, generated, completed):
    conditions = object_value(conditions, 'exclusive scheduling conditions')
    require(conditions.get('contended') is False, 'Formal Web timings were contended or conditions unavailable')
    evidence = referenced_json(conditions.get('evidence_path'), conditions.get('evidence_sha256'))
    evidence = object_value(evidence, 'raw scheduling evidence')
    require(evidence.get('contended') is False, 'Raw scheduling evidence does not declare an uncontended run')
    observations = {}
    for name in ('before', 'after'):
        observation = object_value(evidence.get(name), 'actual ' + name + ' scheduling observation')
        observations[name] = utc_time(observation.get('observed_utc'), name + ' observation timestamp')
        require(isinstance(observation.get('processes'), list) and bool(observation['processes']), 'Original process inventory missing: ' + name)
        require(observation.get('competing_build_test_processes') == [] and observation.get('competing_gpu_processes') == [],
                'Actual CPU/GPU competition or unavailable classification: ' + name)
    require(observations['before'] <= generated <= completed <= observations['after'],
            'Actual scheduling observations do not bracket the complete timing run')


def census_words(value, enabled):
    require(isinstance(value,list) and len(value)==8 and all(type(word) is int and 0<=word<=0xffffffff for word in value),
            'Actual TD census requires eight uint32 words')
    require(value[0]==1 and value[1]>0 and value[2]==1 and value[3]==enabled and value[6]==value[7]==0,
            'Actual TD census version/generation/alive/mode/overflow/invalidated rejected')
    return value


def validate_work_row(row, ground_identities):
    mode=row.get('observer_mode')
    require(mode in ('off','on'),'Explicit observer mode required')
    programs=row.get('world_programs')
    require(isinstance(programs,list) and 1<=len(programs)<=16,'Setup world program identities missing')
    ids=set(); identities=set(); mask=0
    for program in programs:
        program=object_value(program,'actual setup world program')
        value=program.get('id')
        require(type(value) is int and 1<=value<=16 and value not in ids,'Unique bounded world program id required')
        ids.add(value);mask|=1<<value
        identity=sha(program.get('fragment_sha256'),'actual setup fragment source')
        number(program.get('fragment_bytes'),'actual setup fragment bytes',integer=True,positive=True)
        identities.add((identity,program['fragment_bytes']))
        if row['host']=='jce':require(identity in ground_identities,'Timing world program differs from actual linked ground identity')
    require(row['inputs'].get('executed_html_sha256')==row['inputs']['html_sha256'],
            'Same-window executed/served timing HTML identity missing')
    setup=object_value(row.get('setup'),'actual work setup')
    generation=None
    if row['host']=='jce':
        require(setup.get('work_environment')=={'update_census':mode},'Census observer environment changed')
        before=census_words(setup.get('census_setup'),1 if mode=='on' else 0)
        generation=before[1]
        if mode=='off':require(before[4]==before[5]==0,'Disabled update census accumulated work')
    observation=object_value(row.get('work_observation'),'same-window work observation')
    require(type(observation.get('schema')) is int and observation['schema']==1 and observation.get('mode')==mode,
            'Work observation schema/mode changed')
    require(observation.get('scope')=='Actual callback update and setup-program GL calls; not CPU submission/GPU completion',
            'Work observation scope changed')
    kind='td_census' if row['host']=='jce' else 'source_counter'
    require(observation.get('update_kind')==kind,'Actual update proof kind changed')
    if row['host']=='source':require('census_setup' not in setup,'Source has no TD instance census')
    samples=observation.get('samples')
    require(isinstance(samples,list),'Same-window work samples missing')
    if mode=='off':
        require(samples==[],'Observer-off row retained work instrumentation')
        return identities
    times=row['raw']['raf_timestamps_ms']
    require(len(samples)==len(times),'Every sampled RAF needs its own unfiltered work record')
    previous=None
    for timestamp,sample in zip(times,samples):
        sample=object_value(sample,'callback work sample')
        require(sample.get('update_kind')==kind,'Callback update proof kind changed')
        if row['host']=='source':require('census_before' not in sample and 'census_after' not in sample,'Source cannot claim TD instance fields')
        number(sample.get('timestamp_ms'),'work timestamp')
        require(sample['timestamp_ms']==timestamp,'Work sample is outside/reordered from its RAF')
        draws,used=sample.get('world_draw_calls'),sample.get('world_program_mask')
        require(type(draws) is int and draws>0 and type(used) is int and used>0 and used&~mask==0,
                'Actual setup-identified world draw missing/foreign')
        if row['host']=='jce':
            before=census_words(sample.get('census_before'),1);after=census_words(sample.get('census_after'),1)
            require(before[1]==after[1]==generation,'TD instance generation changed during the work window')
            require(after[4]-before[4]==after[5]-before[5]==1,'Actual on_update missing or normal return not consumed once')
            require(previous is None or before==previous,'TD census reset or changed between sample callbacks')
            require(before[4]>=setup['census_setup'][4] and before[5]>=setup['census_setup'][5],'TD census reset after setup')
            previous=after
        else:
            before,after=sample.get('source_update_before'),sample.get('source_update_after')
            number(before,'actual source update before',integer=True);number(after,'actual source update after',integer=True)
            require(before<=0x1fffffffffffff and after<=0x1fffffffffffff and after-before==1 and
                    (previous is None or before==previous),'Actual source update missing/reset/overflow between callbacks')
            require(row['normal_update_observation']['before']<=before<after<=row['normal_update_observation']['after'],
                    'Source work census is outside the declared warmup/sample interval')
            previous=after
    return identities


def validate_observer_control(data, runs):
    control=object_value(data.get('observer_control'),'observer off/on control')
    require(type(control.get('schema')) is int and control['schema']==1 and
            control.get('scope')=='Same-artifact off/on workload delta; do not subtract observer cost or infer isolated instruction cost',
            'Observer control scope changed')
    pairs=control.get('pairs')
    require(isinstance(pairs,list) and len(pairs)==len(runs)//2,'All same-artifact off/on controls required')
    for n,pair in enumerate(pairs):
        off=runs[(n//2)*4+n%2];on=runs[(n//2)*4+2+n%2]
        require(isinstance(pair,dict) and pair.get('host')==off['host']==on['host'] and
                type(pair.get('repeat')) is int and pair['repeat']==off['repeat']==on['repeat'] and
                type(pair.get('off_order')) is int and pair['off_order']==off['order'] and
                type(pair.get('on_order')) is int and pair['on_order']==on['order'],
                'Off/on observer pairing changed')
        require(off['inputs']==on['inputs'] and off['url']==on['url'],'Off/on control artifact or workload changed')
        require({(p['fragment_sha256'],p['fragment_bytes']) for p in off['world_programs']}==
                {(p['fragment_sha256'],p['fragment_bytes']) for p in on['world_programs']},'Off/on linked world identity changed')
        for key,expected in [('callback_cpu_mean_delta_ms',on['cpu_ms']['mean']-off['cpu_ms']['mean']),
                             ('interval_mean_delta_ms',on['interval_ms']['mean']-off['interval_ms']['mean']),
                             ('callback_fps_delta',on['fps']-off['fps'])]:
            actual=pair.get(key)
            require(type(actual) in (int,float) and math.isfinite(actual) and
                    math.isclose(actual,expected,rel_tol=1e-9,abs_tol=1e-6),
                    'Raw signed off/on value disagrees with '+key)


def validate_web_performance(data, artifact, *, allow_standard=False):
    data = object_value(data, 'Web performance report')
    protocol=validate_protocol(data.get('protocol'))
    work=same_json(protocol,PROTOCOLS['v3-work-consumed'])
    publication=same_json(protocol,PROTOCOLS['publication']) or work
    require(publication or allow_standard,'Final Web timing requires the publication protocol')
    status=('MATCHED_INITIAL_GAME_WORK_CONSUMED_OBSERVED' if work else 'MATCHED_INITIAL_GAME_RAF_OBSERVED') if publication else 'RAW_TIMING_OBSERVED_FINAL_SCOPE_INCOMPLETE'
    require(type(data.get('schema')) is int and data['schema'] == (2 if work else 1) and data.get('status') == status and
            data.get('complete') is True and data.get('final_claims_allowed') is publication and data.get('errors') == [],
            'Incomplete/unavailable Web timing cannot enter its declared scope')
    order=['source','jce']*protocol['repeats']*(2 if work else 1)
    require(data.get('jce_artifact_sha256') == artifact and data.get('reference_commit') == PIN, 'Web artifact or reference pin changed')
    sha(artifact, 'actual Web artifact')
    generated = utc_time(data.get('generated_utc'), 'run generation')
    completed = utc_time(data.get('completed_utc'), 'run completion')
    validate_conditions(data.get('conditions'), generated, completed)
    text(data.get('browser_version'), 'actual browser version')
    requested = object_value(data.get('requested'), 'declared matched windows')
    require(requested.get('viewport') == [1280, 720] and type(requested.get('dpr')) in (int, float) and requested['dpr'] == 1 and
            all(type(requested.get(key)) is int and requested[key]==protocol[key] for key in
                ('warmup_ms','sample_ms','min_sample_intervals','sample_max_ms')) and requested.get('order') == order,
            'Declared matched workload/window differs')
    quality = data.get('jce_effective_quality')
    validate_jce_quality(quality, artifact)
    actual_quality_gpu = validate_actual_render_quality(data.get('jce_render_quality'), artifact)
    runs = data.get('runs')
    require(isinstance(runs, list) and len(runs) == len(order) and all(isinstance(row, dict) for row in runs) and [row.get('host') for row in runs] == order,
            'Expected every strictly alternating source/JCE repetition')
    fingerprint, source_identities, source_quality = None, None, None
    for index, row in enumerate(runs):
        require(row.get('status') == 'ACTUAL_GAME_RAF_OBSERVED' and row.get('errors') == [] and
                type(row.get('order')) is int and row['order'] == index and
                type(row.get('repeat')) is int and row['repeat'] == index // (4 if work else 2), 'Unavailable, reordered or duplicated Web run')
        url = urlsplit(text(row.get('url'), 'actual workload URL'))
        require(url.scheme in ('http', 'https') and bool(url.netloc) and parse_qsl(url.query, keep_blank_values=True) == [('fixture', 'initial')],
                'Web timing workload/query changed')
        inputs = object_value(row.get('inputs'), 'actual HTTP byte identities')
        sha(inputs.get('html_sha256'), 'served HTML')
        if row['host'] == 'jce':
            require(inputs['html_sha256'] == artifact, 'Measured HTML bytes are stale')
            require(same_json(row.get('effective_quality'), quality), 'JCE repeat quality source changed')
            environment = object_value(object_value(row.get('setup'), 'actual JCE setup').get('effective_environment'), 'actual trace-off environment')
            require(environment == {'JCE_TRACE': '0', 'JCE_PERF_LOG': '0', 'trace_stream': False,
                                   'trace_export': False, 'kpi': False, 'game_trace': False, 'game_timing': False},
                    'Trace/KPI/game timing contaminated formal window')
        else:
            identities = tuple(sha(inputs.get(key), key) for key in ('html_sha256', 'application_sha256', 'three_sha256', 'probe_sha256'))
            if source_identities is None:
                source_identities = identities
            require(identities == source_identities, 'Reference bytes changed between repeats')
            restored = object_value(row.get('source_restore'), 'normal source restoration')
            require(str(restored.get('revision')) == '68' and all(restored.get(key) is True for key in
                    ('render_restored', 'date_now_restored', 'random_restored')), 'Frozen/foreign source loop was measured')
            setup = object_value(row.get('setup'), 'actual source setup')
            require(setup.get('callback_name') == 'render', 'Original source callback identity unavailable')
            compatibility = object_value(setup.get('compatibility'), 'actual source compatibility')
            require(compatibility.get('object_observe_type') == 'function' and compatibility.get('observer_uses_interval') is True and
                    compatibility.get('declared_fallback_interval_ms') == 50, 'Disclosed source compatibility shim unavailable')
            observed_quality = row.get('effective_quality')
            validate_source_quality(observed_quality)
            require(same_json(observed_quality, setup.get('quality')), 'Source setup and repeat quality disagree')
            if source_quality is None:
                source_quality = observed_quality
            require(same_json(observed_quality, source_quality), 'Source quality changed between repeats')
        current = validate_gpu(row.get('setup'))
        if row['host'] == 'jce':
            timing_gpu = row['setup']['context'][0]
            require(all(timing_gpu.get(key) == actual_quality_gpu.get(key) for key in
                        ('kind', 'version', 'shading_language', 'vendor', 'renderer')),
                    'Actual WebGL quality GPU/context differs from the measured JCE context')
        if fingerprint is None:
            fingerprint = current
        require(current == fingerprint, 'Actual browser/GPU adapter changed between repeats')
        count = validate_windows(row,protocol)
        if row['host'] == 'source':
            update = object_value(row.get('normal_update_observation'), 'actual normal source updates')
            for key in ('before', 'after', 'delta', 'callbacks'):
                number(update.get(key), 'source update ' + key, integer=True)
            require(update['after'] - update['before'] == update['delta'] == update['callbacks'] and update['callbacks'] > count,
                    'Normal source update did not run once per warmup/sample game callback')
    if work:
        require(data.get('submission_observation',{}).get('available') is False and
                data['submission_observation'].get('reason')=='No same-callback CPU submission getter in this consumer protocol',
                'Work census cannot claim CPU submission or GPU completion')
        ground={draw['fragment_source']['sha256'] for draw in data['jce_render_quality']['observed']['ground_draws']}
        identities={}
        for index,row in enumerate(runs):
            require(row.get('observer_mode')==('off' if index%4<2 else 'on'),'Off/off/on/on observer order changed')
            current=validate_work_row(row,ground)
            if row['host'] not in identities:identities[row['host']]=current
            require(identities[row['host']]==current,'Linked world source changed between timing repeats')
        validate_observer_control(data,runs)
