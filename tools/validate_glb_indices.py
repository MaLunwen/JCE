"""Validate standard GLB triangle indices and accessor bounds; input is explicit."""
import json
import math
from pathlib import Path
import struct

def read_glb(path):
    raw = Path(path).read_bytes()
    magic,version,total = struct.unpack_from('<III',raw)
    if magic != 0x46546c67 or version != 2 or total != len(raw):
        raise ValueError('Invalid GLB header')
    size,kind = struct.unpack_from('<II',raw,12)
    if kind != 0x4e4f534a:
        raise ValueError('Missing JSON chunk')
    document = json.loads(raw[20:20+size])
    offset = 20+size
    binary_size,binary_kind = struct.unpack_from('<II',raw,offset)
    if binary_kind != 0x004e4942:
        raise ValueError('Missing BIN chunk')
    return document,raw[offset+8:offset+8+binary_size]

def validate(path):
    doc,binary = read_glb(path)
    for mesh in doc['meshes']:
        for primitive in mesh['primitives']:
            vertices = doc['accessors'][primitive['attributes']['POSITION']]['count']
            if 'indices' not in primitive:
                continue
            accessor = doc['accessors'][primitive['indices']]
            view = doc['bufferViews'][accessor['bufferView']]
            offset = view.get('byteOffset',0)+accessor.get('byteOffset',0)
            fmt = {5121:'B',5123:'H',5125:'I'}[accessor['componentType']]
            count = accessor['count']
            indices = struct.unpack_from('<'+fmt*count,binary,offset)
            if not indices or max(indices) >= vertices or count % 3:
                raise ValueError(f'Invalid indices: max={max(indices)}, vertices={vertices}, count={count}')
            if accessor.get('max',[max(indices)])[0] >= vertices:
                raise ValueError('Invalid accessor maximum')
    return doc

if __name__ == "__main__":
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input",type=Path)
    args=parser.parse_args(); validate(args.input); print("VALID GLB",args.input)
