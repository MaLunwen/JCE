#!/usr/bin/env python3
"""Three original GLBs, public world-stream events and model residency/reload.

Fixed frames are correctness deadlines, not streaming performance samples.
Tracked renderer bytes are not physical VRAM, process RSS or cold disk bytes.
"""
import argparse
import json
from pathlib import Path
import re
import struct

from gbuffer_asset_suite import difference, image, sha256_file, write
from postfx_fixture import fixture
from check_asset_pixels import pack_fixture
from tools.benchmark_suite import source_identity, read_frames, frame_summary
from tools.jce_determinism import assert_backend
from tools.perf_bench import run, read_execution_log
from stream_route_metrics import route_metrics
from stream_route_payload_metrics import parse_payload_metrics

ROUTE = (0, 1, 2, 1, 0)
FIELDS = ('frame step set spawned despawned loaded pending entities path_bytes '
          'total_bytes budget model_evicted chunk_evicted world_accounted').split()


def original_glb(index, grid=0):
    """Minimal first-party triangle; equal topology, distinct shape and colour."""
    if index not in (0, 1, 2):
        raise ValueError('unknown content set')
    if grid:
        return grid_glb(index, grid)
    vertices = (-3, -2, 0, 3, -2, 0, index - 1, 3, 0)
    binary = struct.pack('<18f3H', *vertices, *(0, 0, 1)*3, 0, 1, 2)
    binary += b'\0' * (-len(binary) % 4)
    doc = {'asset': {'version': '2.0', 'generator': 'JCE first-party streaming fixture'},
        'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
        'buffers': [{'byteLength': len(binary)}],
        'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': 36, 'target': 34962},
            {'buffer': 0, 'byteOffset': 36, 'byteLength': 36, 'target': 34962},
            {'buffer': 0, 'byteOffset': 72, 'byteLength': 6, 'target': 34963}],
        'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3,
            'type': 'VEC3', 'min': [-3, -2, 0], 'max': [3, 3, 0]},
            {'bufferView': 1, 'componentType': 5126, 'count': 3, 'type': 'VEC3'},
            {'bufferView': 2, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}],
        'materials': [{'doubleSided': True, 'pbrMetallicRoughness': {
            'baseColorFactor': [[.8,.1,.1,1],[.1,.8,.1,1],[.1,.1,.8,1]][index],
            'metallicFactor': 0, 'roughnessFactor': 1}}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1},
            'indices': 2, 'material': 0}]}]}
    encoded = json.dumps(doc, separators=(',', ':')).encode()
    encoded += b' ' * (-len(encoded) % 4)
    return (struct.pack('<III', 0x46546c67, 2, 28 + len(encoded) + len(binary)) +
        struct.pack('<II', len(encoded), 0x4e4f534a) + encoded +
        struct.pack('<II', len(binary), 0x004e4942) + binary)


def grid_glb(index, grid):
    if not isinstance(grid, int) or not 1 <= grid <= 512:
        raise ValueError('grid resolution must be in [1,512]')
    positions = [(-3 + 6*x/grid + (index-1)*.25*y/grid, -2+5*y/grid, 0)
                 for y in range(grid+1) for x in range(grid+1)]
    indices = []
    for y in range(grid):
        for x in range(grid):
            a = y*(grid+1)+x
            indices.extend((a,a+1,a+grid+2,a,a+grid+2,a+grid+1))
    count = len(positions)
    width = 2 if count <= 65536 else 4
    binary = struct.pack('<%uf' % (count*3), *(v for p in positions for v in p))
    binary += struct.pack('<%uf' % (count*3), *((0,0,1)*count))
    binary += struct.pack('<%u%s' % (len(indices), 'H' if width == 2 else 'I'), *indices)
    views = [{'buffer':0,'byteOffset':0,'byteLength':count*12,'target':34962},
             {'buffer':0,'byteOffset':count*12,'byteLength':count*12,'target':34962},
             {'buffer':0,'byteOffset':count*24,'byteLength':len(indices)*width,'target':34963}]
    binary += b'\0' * (-len(binary)%4)
    doc = {'asset':{'version':'2.0','generator':'JCE first-party indexed grid'},
           'scene':0,'scenes':[{'nodes':[0]}],'nodes':[{'mesh':0}],
           'buffers':[{'byteLength':len(binary)}],'bufferViews':views,
           'accessors':[{'bufferView':0,'componentType':5126,'count':count,'type':'VEC3',
                         'min':[min(p[a] for p in positions) for a in range(3)],
                         'max':[max(p[a] for p in positions) for a in range(3)]},
                        {'bufferView':1,'componentType':5126,'count':count,'type':'VEC3'},
                        {'bufferView':2,'componentType':5123 if width==2 else 5125,
                         'count':len(indices),'type':'SCALAR'}],
           'materials':[{'doubleSided':True,'pbrMetallicRoughness':{
               'baseColorFactor':[[.8,.1,.1,1],[.1,.8,.1,1],[.1,.1,.8,1]][index],
               'metallicFactor':0,'roughnessFactor':1}}],
           'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1},'indices':2,'material':0}]}]}
    encoded=json.dumps(doc,separators=(',',':')).encode()
    encoded += b' ' * (-len(encoded)%4)
    return struct.pack('<III',0x46546c67,2,28+len(encoded)+len(binary))+struct.pack('<II',len(encoded),0x4e4f534a)+encoded+struct.pack('<II',len(binary),0x004e4942)+binary



def scenes():
    base = fixture(empty=True)
    base['entities'][0]['components'][0].update(posX=0, posY=0, posZ=12,
        rotX=0, rotY=0, rotZ=0)
    base['entities'][0]['components'][1].update(orthographic=True, orthoSize=12)
    base['entities'][1]['components'][1].update(castsShadow=False)
    base['rendering']['postfx'].update(ssao=False, ssr=False, bloom=False)
    chunks = []
    for index in range(3):
        chunks.append({'entities': [{'id': 1, 'name': 'StreamSet%u' % index,
            'components': [{'type': 'Transform', 'posX': index*100, 'posY': 0,
                'posZ': 0, 'scaleX': 1, 'scaleY': 1, 'scaleZ': 1},
                {'type': 'MeshRenderer', 'meshPath': 'set_%u.glb' % index,
                    'visible': True, 'baseColorR': 1, 'baseColorG': 1, 'baseColorB': 1}]}]})
    return base, chunks


def validate_points(log, steps, vertices=3, primitives=1):
    pattern = r'benchmark-stream-route-point: ' + ' '.join(key+r'=(\d+)' for key in FIELDS)
    points = [dict(zip(FIELDS, map(int, match))) for match in re.findall(pattern, log)]
    if len(points) != steps:
        raise ValueError('missing or duplicate route points')
    ready = re.findall(r'benchmark-stream-route-ready: frame=(\d+) step=(\d+) vertices=(\d+) primitives=(\d+)', log)
    if len(ready) != steps or any(tuple(map(int,row)) != (i*200+190,i,vertices,primitives) for i,row in enumerate(ready)):
        raise ValueError('rendered model topology unavailable')
    if not steps:
        return points
    budget = points[0]['budget']
    for step, point in enumerate(points):
        expected = dict(frame=step*200+190, step=step, set=ROUTE[step],
            spawned=step+1, despawned=step, loaded=1, pending=0, entities=1)
        if any(point[key] != value for key, value in expected.items()) or not budget \
                or point['budget'] != budget or not point['path_bytes'] \
                or point['total_bytes'] != point['path_bytes'] \
                or point['total_bytes'] > budget or point['model_evicted'] < step:
            raise ValueError('route residency, eviction or reload control failed')
    return points


def validate_pixels(images):
    if len(images) != 6 or any(p.shape != (720,1280,4) for p in images):
        raise ValueError('missing route pictures')
    metrics = {'liveness': [difference(images[0], p) for p in images[1:]],
        'reverse_one': difference(images[2], images[4]),
        'reverse_zero': difference(images[1], images[5]),
        'distinct': [difference(images[1], images[2]), difference(images[2], images[3])]}
    if min(metrics['liveness'] + metrics['distinct']) <= .001 \
            or max(metrics['reverse_one'], metrics['reverse_zero']) > .0001:
        raise ValueError('route rendered liveness or reload image parity failed')
    return metrics


def run_schedule(steps):
    if isinstance(steps, bool) or not isinstance(steps, int) or not 0 <= steps <= len(ROUTE):
        raise ValueError("route prefix must be an integer in [0,5]")
    capture_frame = max(1, steps) * 200 - 10
    # The existing perf harness requires a real rolling report: the engine
    # discards120 warmup frames before collecting its120-frame log window.
    # Keep checkpoints fixed; extend only lifetime, including capture drain.
    process_frames = max(120 + 120 + 20, capture_frame + 40)
    return capture_frame, process_frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--packer', type=Path, required=True)
    parser.add_argument('--backend', choices=('d3d11','d3d12','vulkan','gl'), required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--grid-resolution', type=int, default=0)
    args = parser.parse_args()
    if not 0 <= args.grid_resolution <= 512:
        parser.error('grid resolution must be in [0,512]')
    expected_vertices = (args.grid_resolution+1)**2 if args.grid_resolution else 3
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    assets = args.out/'assets'
    assets.mkdir()
    for index in range(3):
        (assets/('set_%u.glb' % index)).write_bytes(original_glb(index, args.grid_resolution))
    pak = pack_fixture(args.packer, assets/'set_0.glb', args.out)
    base, chunks = scenes()
    for index, chunk in enumerate(chunks):
        write(assets/('cell_%u.scene.json' % index), chunk)
    binary_hash = sha256_file(args.exe)
    inputs = {str(p): sha256_file(p) for p in assets.iterdir()}
    inputs[str(pak)] = sha256_file(pak)
    result = {'status':'INCOMPLETE', 'source':source_identity(), 'cases':{},
        'exe':str(args.exe.resolve()), 'exe_sha256':binary_hash, 'inputs':inputs,
        'backend':args.backend, 'route':list(ROUTE), 'grid_resolution':args.grid_resolution, 'expected_vertices':expected_vertices,
        'scope':'Actual route frame timing, sampled process RSS and OS read counters; tracked model budget is not physical VRAM; cold disk unavailable; actual CPU-submitted payload deltas exclude PCIe/GPU completion'}
    pictures = []
    try:
        for steps in range(6):
            directory = args.out/('prefix_%u' % steps)
            directory.mkdir()
            write(directory/'scene.json', base)
            frame, process_frames = run_schedule(steps)
            env = {'JCE_BACKEND':args.backend, 'JCE_GPU_TIER':'high',
                'JCE_SCENE':str(directory/'scene.json'), 'JCE_BENCH_ASSET_STILL':'1',
                'JCE_BENCH_ASSET_PAK':str(pak), 'JCE_BENCH_SHOT_FRAME':str(frame),
                'JCE_BENCH_PLAYER_SHOT':str(directory/'target.png')}
            if steps:
                env.update(JCE_BENCH_STREAM_ROOT=str(assets), JCE_BENCH_STREAM_STEPS=str(steps),
                           JCE_BENCH_STREAM_VERTICES=str(expected_vertices), JCE_BENCH_STREAM_PRIMITIVES='1')
            process = run(env, directory/'engine.log', frames=process_frames, exe=args.exe.resolve(),
                          samples_path=directory/'frames.csv', sample_interval=.02)
            log = read_execution_log(directory/'engine.log', process)
            assert_backend(process.get('stdout','')+'\n'+log, args.backend)
            if '] ERROR -' in log:
                raise ValueError('stream consumer logged an error')
            points = validate_points(log, steps, expected_vertices, 1)
            pictures.append(image(directory/'target.png'))
            if sha256_file(args.exe) != binary_hash or any(sha256_file(Path(p)) != h for p,h in inputs.items()):
                raise ValueError('source inputs changed')
            result['cases'][str(steps)] = {'process':process,'environment':env,'points':points, 'capture_frame':frame, 'process_frame_limit':process_frames, 'timing':route_metrics(directory/'frames.csv',process,steps),
                'submitted_payload':parse_payload_metrics(log,steps),
                'files':{name:sha256_file(directory/name) for name in ('scene.json','engine.log','target.png','frames.csv')}}
            write(args.out/'suite.json',result)
        result.update(status='RENDERED_ROUTE_CONTROLS_PASS',metrics=validate_pixels(pictures))
    except Exception as error:
        result.update(status='FAIL',error=str(error))
        raise
    finally:
        write(args.out/'suite.json',result)


if __name__ == '__main__':
    main()
