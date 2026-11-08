"""Admit actual current Web input, owned GL cleanup and bound CPU lifecycle evidence."""
from __future__ import annotations
import hashlib
from pathlib import Path
import re
import sys
from urllib.parse import parse_qsl, urlsplit
from web_evidence import ANSI, number, referenced_bytes, referenced_json, require, sha, strict_json, text
from web_performance_validation import object_value, same_json

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'tools'))
from lifecycle_trace_report import analyze_trace

GL_KINDS = ('Program', 'Buffer', 'Texture', 'Shader', 'Framebuffer', 'Renderbuffer',
            'Sampler', 'Query', 'VertexArray', 'TransformFeedback')
CPU_PHASES = ('engine.window_create', 'engine.renderer_create', 'engine.app_init',
              'engine.app_update', 'engine.app_draw', 'engine.app_exit', 'engine.async_shutdown',
              'engine.subsystems_init', 'engine.subsystems_shutdown', 'engine.renderer_destroy',
              'renderer.create', 'renderer.frame_configure', 'renderer.view_configure',
              'renderer.submit_cpu', 'renderer.destroy', 'fs.create', 'fs.mount_pak',
              'fs.read_all_bounded', 'fs.destroy')


def runtime_field(line, name):
    # Native logger SGR markers can follow the last numeric field directly.
    # Strip only that diagnostic formatting before requiring the full number.
    line = ANSI.sub('', text(line, 'runtime trace'))
    match = re.search(r'(?:^|\s)' + re.escape(name) + r'=([^\s]+)', line)
    require(match is not None, 'Missing runtime field: ' + name)
    try:
        value = float(match[1])
    except ValueError as error:
        raise ValueError('Invalid runtime field: ' + name) from error
    require(value == value and abs(value) != float('inf'), 'Nonfinite runtime field: ' + name)
    return value


def validate_common(data, artifact, mode):
    data = object_value(data, 'actual Web acceptance')
    require(type(data.get('schema')) is int and data['schema'] == 1 and
            data.get('status') == 'OBSERVED_CHECKS_PASSED' and data.get('mode') == mode and data.get('errors') == [],
            'Current successful browser acceptance required')
    sha(artifact, 'current Web artifact')
    require(data.get('artifact_sha256') == artifact and data.get('served_artifact_sha256') == artifact,
            'Actual served Web bytes changed or unavailable')
    request_text = text(data.get('registry_request_json'), 'exact registry request bytes')
    require(hashlib.sha256(request_text.encode('utf-8')).hexdigest() == sha(data.get('request_sha256'), 'registry request'),
            'Registry request bytes changed')
    request = object_value(strict_json(request_text), 'registry request')
    require(type(request.get('schema')) is int and request['schema'] == 1 and request.get('artifact_sha256') == artifact,
            'Registry request does not bind the current Web artifact')
    nonce = sha(request.get('nonce'), 'registry nonce')
    closure = sha(request.get('closure_sha256'), 'selected native closure')
    target = text(request.get('target'), 'registry target')
    environment = object_value(request.get('environment'), 'registry environment')
    require(environment.get('JCE_NATIVE_REGISTRY_NONCE') == nonce and
            text(environment.get('JCE_NATIVE_REGISTRY_REPORT'), 'registry MEMFS report').startswith('/'),
            'Registry nonce or report environment changed')
    for key in ('selected_count', 'factory_count'):
        number(request.get(key), key, positive=True, integer=True)
    receipt = object_value(data.get('receipt'), 'actual browser registry receipt')
    require(type(receipt.get('schema')) is int and receipt['schema'] == 1 and receipt.get('ok') is True and
            receipt.get('nonce') == nonce and receipt.get('closure_sha256') == closure and receipt.get('target') == target,
            'Actual browser registry receipt is stale or unsuccessful')
    for key, expected in (('selected_count', request['selected_count']), ('resolved_count', request['selected_count']),
                          ('factory_count', request['factory_count']), ('registered_factory_count', request['factory_count'])):
        require(type(receipt.get(key)) is int and receipt[key] == expected, 'Registry receipt count mismatch: ' + key)
    text(data.get('browser'), 'actual browser identity')
    number(data.get('world_draw_calls'), 'actual world draws', positive=True, integer=True)
    require(data['world_draw_calls'] >= 3, 'Actual game readiness was not observed')
    require(isinstance(data.get('logs'), list) and all(isinstance(line, str) for line in data['logs']), 'Actual runtime logs missing')
    network = object_value(data.get('network'), 'actual browser network')
    require(type(network.get('restricted')) is bool and network.get('unexpected') == [], 'Actual network isolation unavailable/failed')
    base = urlsplit(text(data.get('artifact_url'), 'artifact HTTP URL'))
    run = urlsplit(text(data.get('run_url'), 'actual browser run URL'))
    require(base.scheme in ('http', 'https') and bool(base.netloc) and not base.query and not base.fragment and
            (run.scheme, run.netloc, run.path) == (base.scheme, base.netloc, base.path), 'Browser endpoint identity changed')
    queries = dict(parse_qsl(run.query, keep_blank_values=True))
    require(queries.get('qa') == '' and (queries.get('fixture') == 'initial' if mode == 'input' else queries.get('autodefense') == ''),
            'Actual acceptance query missing')
    requests = network.get('requests')
    require(isinstance(requests, list) and bool(requests), 'Actual HTTP request observations unavailable')
    identity_fetch = False
    browser_navigation = False
    for row in requests:
        row = object_value(row, 'actual HTTP request')
        url = urlsplit(text(row.get('url'), 'request URL'))
        text(row.get('method'), 'request method')
        number(row.get('status'), 'observed HTTP response', positive=True, integer=True)
        require(row['status'] < 400 and not row.get('blocked') and not row.get('failure'), 'Actual HTTP dependency failed')
        if row.get('source') == 'artifact_identity_http':
            require(row['url'] == data['artifact_url'] and row['method'] == 'GET', 'Artifact identity request changed')
            identity_fetch = True
        if row.get('source') == 'browser' and row['url'] == data['run_url'] and row['method'] == 'GET':
            browser_navigation = True
        if network['restricted']:
            require((url.scheme, url.netloc, url.path) == (base.scheme, base.netloc, base.path), 'Unexpected restricted HTTP dependency')
    require(identity_fetch and browser_navigation, 'Actual HTML identity fetch or browser navigation was not recorded')
    events = data.get('input_events')
    require(isinstance(events, list) and type(data.get('input_events_lost')) is int and data['input_events_lost'] == 0,
            'Actual browser event capture unavailable/lost')
    for event in events:
        event = object_value(event, 'actual input event')
        require(type(event.get('trusted')) is bool, 'Actual event trust classification unavailable')
        # Lifecycle trace downloads use anchor.click() and retain that false
        # trust flag; only the actual input test requires trusted user events.
        if mode == 'input':
            require(event['trusted'] is True, 'Synthetic DOM events cannot certify browser input')
        number(event.get('timestamp_ms'), 'actual browser input timestamp')
    return data


def validate_input(data, artifact):
    data = validate_common(data, artifact, 'input')
    for name, delay, selected in (('quickClick', 0, 60), ('ordinaryClick', 100, 60)):
        row = object_value(data.get(name), name)
        require(row.get('passed') is True and type(row.get('delay_ms')) is int and row['delay_ms'] == delay and
                type(row.get('select_events')) is int and row['select_events'] == 1 and type(row.get('selected')) is int and row['selected'] == selected,
                'Actual single-dispatch selection failed: ' + name)
    prior = object_value(data['ordinaryClick'].get('prior_same_tile_deselect'), 'ordinary prior deselection')
    require(prior.get('passed') is True and prior.get('delay_ms') == 100 and prior.get('select_events') == 1 and prior.get('selected') == -1,
            'Repeated tile deselection failed')
    selected = [runtime_field(line, 'selected') for line in data['logs'] if 'TD_TRACE action=select ' in line]
    require(selected == [60, -1, 60], 'Raw select dispatches differ from observed single clicks')
    placement = object_value(data.get('placement'), 'actual tower placement')
    require(placement.get('passed') is True and placement.get('cost') == 1 and placement.get('resources') == 554 and placement.get('towers') == 1,
            'Basic tower resource/object behavior changed')
    placed = [line for line in data['logs'] if 'TD_TRACE action=place ' in line]
    require(len(placed) == 1 and placement.get('trace') == placed[0] and runtime_field(placed[0], 'resources') == 554 and
            runtime_field(placed[0], 'towers') == 1, 'Raw placement does not support claimed result')
    drag = object_value(data.get('drag'), 'actual camera drag')
    require(drag.get('from') == [640,360] and drag.get('to') == [700,330] and drag.get('steps') == 6 and
            drag.get('pose_changed') is True and drag.get('pictures_differ') is True, 'Camera drag observation missing')
    require(data.get('before') in data['logs'] and data.get('after') in data['logs'] and
            any(runtime_field(data['before'], key) != runtime_field(data['after'], key) for key in ('x','y','z')),
            'Actual camera poses did not change')
    keyboard = object_value(data.get('keyboard'), 'actual key hold/release')
    require(type(keyboard.get('held')) is int and keyboard['held'] == 0 and type(keyboard.get('released')) is int and keyboard['released'] == 1,
            'Key hold/release result unavailable')
    require(data.get('resize') in data['logs'] and runtime_field(data['resize'], 'width') == 896 and
            runtime_field(data['resize'], 'height') == 504 and data.get('canvas') == [896,504], 'Actual resize result missing')
    events = data['input_events']
    for kind in ('mousedown', 'mouseup', 'mousemove', 'click', 'keydown', 'keyup', 'resize'):
        require(any(event.get('type') == kind for event in events), 'Actual browser event missing: ' + kind)
    clicks = [event for event in events if event.get('type') == 'click']
    require(len(clicks) >= 4 and [(event.get('x'), event.get('y')) for event in clicks[:4]] ==
            [(640,360)]*3 + [(535,665)], 'Actual click coordinates unavailable')
    require(any(event.get('type') == 'keydown' and event.get('key') == 'e' for event in events) and
            any(event.get('type') == 'keyup' and event.get('key') == 'e' for event in events) and
            any(event.get('type') == 'resize' and [event.get('width'),event.get('height')] == [896,504] for event in events),
            'Actual key/resize event values missing')
    pictures = data.get('screenshots')
    require(isinstance(pictures, list) and len(pictures) == 3, 'Actual before/after/resize pictures missing')
    for picture in pictures:
        picture = object_value(picture, 'actual screenshot')
        raw = referenced_bytes(picture.get('path'), picture.get('sha256'))
        require(raw.startswith(b'\x89PNG\r\n\x1a\n') and type(picture.get('bytes')) is int and len(raw) == picture['bytes'],
                'Actual screenshot bytes changed or unavailable')
    require(pictures[0]['sha256'] != pictures[1]['sha256'], 'Camera pictures are identical')


def validate_cleanup(data, artifact):
    data = validate_common(data, artifact, 'lifecycle')
    require(data.get('shutdownWaitError') is None, 'Shutdown was unavailable')
    contexts = data.get('contexts')
    require(isinstance(contexts, list) and len(contexts) == 1, 'Actual single WebGL context missing')
    totals = {kind: {'created': 0, 'deleted': 0, 'live': 0, 'valid_survivors': 0} for kind in GL_KINDS}
    for context in contexts:
        context = object_value(context, 'actual shutdown context')
        require(context.get('kind') in ('webgl','webgl2','experimental-webgl') and
                text(context.get('version'), 'actual context version').startswith('WebGL') and
                text(context.get('shading_language'), 'actual shader language').startswith('WebGL') and
                context.get('context_lost') is False and type(context.get('gl_error_after_shutdown')) is int and
                context['gl_error_after_shutdown'] == 0, 'Actual graphics context unavailable/lost/errors')
        text(context.get('vendor'), 'actual graphics vendor'); text(context.get('renderer'), 'actual graphics adapter')
        attributes = object_value(context.get('attributes'), 'actual context attributes')
        for name in ('alpha','depth','stencil','antialias','premultipliedAlpha','preserveDrawingBuffer'):
            require(type(attributes.get(name)) is bool, 'Actual context attribute unavailable: ' + name)
        resources = object_value(context.get('resources'), 'actual owned WebGL resources')
        require(set(resources) == set(GL_KINDS), 'Actual WebGL resource kinds missing')
        for kind, row in resources.items():
            row = object_value(row, 'actual ' + kind + ' ownership')
            require(type(row.get('available')) is bool, 'Resource availability missing: ' + kind)
            for key in ('created','deleted','delete_calls','live'):
                number(row.get(key), kind + '.' + key, integer=True)
            require(row['created'] == row['deleted'] and row['live'] == 0 and row['delete_calls'] >= row['deleted'],
                    'Actual owned WebGL objects did not retire: ' + kind)
            if row['available']:
                require(type(row.get('valid_survivors')) is int and row['valid_survivors'] == 0,
                        'Actual WebGL validity inspection missing/nonzero: ' + kind)
            else:
                require(row['created'] == row['delete_calls'] == 0 and row.get('valid_survivors') is None,
                        'Unavailable WebGL kind has invented measurements: ' + kind)
            for key in ('created','deleted','live','valid_survivors'):
                totals[kind][key] += row[key] or 0
    shutdown = object_value(data.get('shutdown'), 'actual shutdown summary')
    require(shutdown.get('programsCreated') == totals['Program']['created'] and
            shutdown.get('programsDeleted') == totals['Program']['deleted'] and shutdown.get('live') == 0 and
            same_json(shutdown.get('resources'), {kind: row for kind,row in totals.items() if kind != 'Program'}),
            'Shutdown summary differs from actual context observations')
    trace = referenced_json(data.get('trace_path'), data.get('trace_sha256'))
    trace = object_value(trace, 'raw lifecycle trace')
    require(same_json(trace.get('jceTrace'), data.get('trace_metadata')), 'Trace footer differs from original file')
    metadata = object_value(trace.get('jceTrace'), 'actual trace footer')
    require(metadata.get('mode') == 'continuous' and metadata.get('consumer_mode') == 'cooperative' and
            metadata.get('worker_available') is False, 'Actual cooperative Web capture unavailable')
    frame = object_value(data.get('frame_observation'), 'actual game frame observation')
    number(frame.get('requested'), 'requested game frames', positive=True, integer=True)
    require(all(type(frame.get(key)) is int for key in ('observed','first','last')) and
            1000 <= frame['requested'] <= 10000 and frame.get('observed') == frame['requested'] and
            frame.get('first') == 0 and frame.get('last') == frame['requested']-1, 'Actual full frame run missing')
    events = trace.get('traceEvents')
    require(isinstance(events, list) and all(isinstance(event, dict) for event in events), 'Actual trace events missing')
    frames = [event for event in events if event.get('name') == 'frame' and event.get('cat') == 'frame']
    require(all(isinstance(event.get('args'), dict) and type(event['args'].get('id')) is int for event in frames),
            'Actual game frame identity must be an integer')
    require([event['args']['id'] for event in frames] == list(range(frame['requested'])),
            'Raw game frame IDs missing, duplicated or reordered')
    analyze_trace(trace, list(CPU_PHASES))
    number(data.get('wasm_linear_memory_capacity_bytes'), 'actual linear-memory capacity', positive=True, integer=True)
    return trace


def validate_web_lifecycle(data, artifact):
    data = object_value(data, 'Web lifecycle summary')
    require(data.get('status') == 'REQUIRED_CPU_LIFECYCLE_SCOPES_OBSERVED', 'Actual Web lifecycle summary unavailable')
    cleanup = referenced_json(data.get('cleanup_evidence_path'), data.get('cleanup_evidence_sha256'))
    trace = validate_cleanup(cleanup, artifact)
    summary_path = Path(text(data.get('trace'), 'summary trace path'))
    cleanup_path = Path(text(cleanup.get('trace_path'), 'cleanup trace path'))
    require(summary_path.is_absolute() and cleanup_path.is_absolute(), 'Both bound trace paths must be absolute')
    require(summary_path.resolve(strict=True) == cleanup_path.resolve(strict=True) and summary_path.samefile(cleanup_path) and
            data.get('sha256') == cleanup['trace_sha256'],
            'Web lifecycle summary does not bind the same-artifact actual cleanup trace')
    required = data.get('required')
    require(isinstance(required, list) and len(required) == len(CPU_PHASES) and set(required) == set(CPU_PHASES),
            'Required Web CPU lifecycle phases changed or missing')
    actual = analyze_trace(trace, required)
    for key, value in actual.items():
        require(same_json(data.get(key), value), 'Web lifecycle summary differs from raw independent parser: ' + key)
