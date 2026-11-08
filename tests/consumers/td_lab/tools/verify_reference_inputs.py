"""Compare recorded original OrbitControls event results with native JIRC inputs.
Reference event captures come from the independent browser evidence producer;
this never computes expected cameras using the port's own implementation.
"""
import json
import re
import hashlib
import tempfile
import time
import numpy as np
from PIL import Image
from pathlib import Path
import verify_game as v
from compare_captures import compare

def touch(points):
    return ((),0,0,0,0,0,0,[(i+1,x/1280,y/720,1) for i,(x,y) in enumerate(points)])

def observe_stable_control(kind, frames):
    """Ordered completed PNG samples in one real process; never infer GPU idle."""
    root = v.OUT / 'control-observations'
    root.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix=kind + '-', dir=root))
    seconds = [2, 4, 6, 8, 10, 12, 16, 20]
    shots = [folder / ('shot-' + str(n) + '.png') for n in seconds]
    trace = folder / 'trace.json'
    spec = ';'.join(str(n) + '|' + str(path.resolve()) for n, path in zip(seconds, shots))
    started = time.monotonic()
    log = v.run('control-' + kind + '-observed', v.idle(30) + frames + v.idle(20),
                extra={'TD_FIXTURE': 'initial', 'JCE_KPI_GAME_SHOTS': spec,
                       'JCE_TRACE': '1', 'JCE_PERF_LOG': '0',
                       'JCE_TRACE_STREAM': str(trace.resolve()),
                       'JCE_TRACE_STREAM_MODE': 'threaded'}, count=1300)
    if time.monotonic() - started > 60:
        raise RuntimeError('UNREADY: control observation exceeded 60 seconds')
    document = json.loads(trace.read_text(encoding='utf-8'))
    metadata = document['jceTrace']
    assert metadata['capture_complete'] and metadata['lost'] == 0
    assert metadata['recorded'] == metadata['captured'] == len(document['traceEvents'])
    decoded = [e for e in document['traceEvents'] if e.get('ph') == 'E'
               and e.get('name') == 'scene.model.decode'
               and e.get('args', {}).get('state') == 0]
    assert decoded, 'UNREADY: no successful model decode observed'
    resource_samples = {}
    for name in ('renderer.program_count', 'renderer.shader_count',
                 'renderer.vertex_buffer_count', 'renderer.index_buffer_count',
                 'renderer.texture_count', 'renderer.draw_count'):
        values = [e.get('args', {}).get('value') for e in document['traceEvents']
                  if e.get('ph') == 'C' and e.get('name') == name]
        resource_samples[name] = values[-3:]
    observations, previous, stable, selected = [], None, 0, None
    for n, path in zip(seconds, shots):
        if not path.is_file():
            raise RuntimeError('UNREADY: scheduled screenshot did not complete: ' + str(path))
        pixels = np.asarray(Image.open(path).convert('RGB')).astype(int)
        assert pixels.shape == (720, 1280, 3), 'Unexpected capture extent'
        board = pixels[150:670, 190:1090]
        assert int(np.count_nonzero(board.max(axis=2) > 20)) > 1000, 'UNREADY: empty output'
        delta = None if previous is None else float(np.abs(board - previous).mean())
        stable = stable + 1 if delta is not None and delta < .03 else 1
        row = {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
               'requested_playing_seconds': n, 'board_mean_rgb_delta': delta}
        observations.append(row)
        previous = board
    selected = shots[-1] if stable >= 3 else None
    evidence = {'method': 'Eight ordered completed screenshot files in one process; the final three successive sampled board images stable below mean RGB .03.',
                'scope': 'Observed model decode and visible-image stability; not all assets, GPU completion/idle or consecutive renderer frames.',
                'ready_frame': 'UNAVAILABLE: schedule PNG interface does not export exact ready frame',
                'context_generation': 'UNAVAILABLE: schedule PNG interface does not export generation',
                'trace': str(trace), 'trace_sha256': hashlib.sha256(trace.read_bytes()).hexdigest(),
                'capture': metadata, 'successful_model_decodes_observed': len(decoded),
                'auxiliary_resource_samples': resource_samples,
                'observations': observations, 'selected': str(selected) if selected else None,
                'deadline_seconds': 60, 'board_stability_threshold_mean_rgb': .03}
    (folder / 'observation.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    if selected is None:
        raise RuntimeError('UNREADY: sampled board did not settle; evidence retained at ' + str(folder))
    return log, selected, evidence


def main():
    inputs={
        'wheel':[((),640,360,0,0,0,1)],
        'right-pan':[((),640,360,4),((),655,369,4,15,9,0),((),655,370,0)],
        'middle-dolly':[((),640,360,2),((),655,370,2,15,10,0),((),655,370,0)],
        'touch-rotate':[touch([(640,360)]),touch([(700,330)]),touch([])],
        'touch-pinch':[touch([(540,360),(740,360)]),touch([(530,360),(750,360)]),touch([])],
        'touch-pan':[touch([(640,360),(700,360),(760,360)]),touch([(655,369),(715,369),(775,369)]),touch([])]}
    report={}
    failed=[]
    for kind,frames in inputs.items():
        log, observed_image, observation = observe_stable_control(kind, frames)
        values=dict(re.findall(r'(\w+)=(-?[\d.]+)',[line for line in log.splitlines() if 'TD_CAMERA' in line][-1]))
        source=json.loads((v.PROJECT/'build/reference/captures'/('control-'+kind+'.jpg.json')).read_text())['state']['camera']
        actual=[float(values[k]) for k in ['x','y','z','targetX','targetY','targetZ']]
        p=source['position'];t=source['target'];expected=[p['x'],p['z'],-p['y'],t['x'],t['z'],-t['y']]
        error=max(abs(a-b) for a,b in zip(actual,expected))
        report[kind]={'reference':expected,'native':actual,'max_abs_error':error,
            'reference_producer':'original OrbitControls event listeners','native_producer':'JIRC recording',
            'pass':error<0.0001}
        if not report[kind]['pass']: failed.append(kind + ': original camera mismatch')
        visual=compare(v.PROJECT/'build/reference/captures'/('control-'+kind+'.jpg'),observed_image,v.OUT/('control-image-'+kind),{'board':[190,150,900,520]},[[0,0,500,45],[0,680,600,40]])
        report[kind]['rendered_image']=visual['regions']
        report[kind]['asset_and_image_observation']=observation
        report[kind]['raw_reference_visual_pass']=visual['regions']['board']['foreground_iou']>.99
        report[kind]['foreground_iou_threshold']=.99
        if not report[kind]['raw_reference_visual_pass']: failed.append(kind + ': raw original foreground IoU below .99')

        print(kind,'camera_max_error',error,flush=True)
    frames=v.idle(40)+v.click(675,125)+v.idle(80)
    log=v.run('credits',frames,count=len(frames)+20)
    assert 'menu=3' in log
    log=v.run('credits-close',frames+v.click(890,299)+v.idle(25),count=len(frames)+60)
    assert 'menu=1' in log.split('action=destroy')[-1]
    (v.OUT/'reference-inputs.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    if failed:
        raise AssertionError('All available controls captured; failures retained in reference-inputs.json: ' + '; '.join(failed))
    print('Six independent original input and raw visual comparisons and Credits open/close PASS')

if __name__=='__main__':main()
