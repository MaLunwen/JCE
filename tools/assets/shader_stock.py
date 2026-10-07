"""Stage verified current-build shader stock; no SDK/cache fallback."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct


def xxh32(data):
    """XXH32 seed zero, the JPAK content_crc contract."""
    mask = 0xffffffff
    p1, p2, p3, p4, p5 = 2654435761, 2246822519, 3266489917, 668265263, 374761393
    def rot(x, n):
        return ((x << n) | (x >> (32 - n))) & mask
    def lane(v, x):
        return (rot((v + x * p2) & mask, 13) * p1) & mask
    pos, size = 0, len(data)
    if size >= 16:
        v = [(p1 + p2) & mask, p2, 0, (-p1) & mask]
        while pos <= size - 16:
            for i, x in enumerate(struct.unpack_from('<4I', data, pos)):
                v[i] = lane(v[i], x)
            pos += 16
        h = sum(rot(v[i], n) for i, n in enumerate((1, 7, 12, 18))) & mask
    else:
        h = p5
    h = (h + size) & mask
    while pos <= size - 4:
        x = struct.unpack_from('<I', data, pos)[0]
        h = (rot((h + x * p3) & mask, 17) * p4) & mask
        pos += 4
    while pos < size:
        h = (rot((h + data[pos] * p5) & mask, 11) * p1) & mask
        pos += 1
    h ^= h >> 15
    h = (h * p2) & mask
    h ^= h >> 13
    h = (h * p3) & mask
    return h ^ (h >> 16)


def address(value):
    if not isinstance(value, str) or not value or '\\' in value or ':' in value:
        raise ValueError('unsafe shader address')
    parts = value.split('/')
    if any(part in ('', '.', '..') for part in parts) or value.startswith('/'):
        raise ValueError('unsafe shader address')
    return PurePosixPath(value)


def inside(root, name):
    candidate = root.joinpath(*address(name).parts)
    if not candidate.resolve().is_relative_to(root.resolve()):
        raise ValueError('shader address escapes root')
    return candidate


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify_stock(stock_root):
    root = Path(stock_root)
    manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or not isinstance(manifest.get('files'), list):
        raise ValueError('invalid stock manifest')
    if not re.fullmatch('[0-9a-f]{64}', manifest.get('bom_sha256', '')):
        raise ValueError('invalid BOM identity')
    if not isinstance(manifest.get('profiles'), list) or not manifest['profiles']:
        raise ValueError('missing profiles')
    seen = set()
    for entry in manifest['files']:
        name = str(address(entry['address']))
        if not name.endswith('.bin') or name.casefold() in seen:
            raise ValueError('duplicate or nonshader address')
        seen.add(name.casefold())
        raw = inside(root, name).read_bytes()
        if type(entry['size']) is not int or len(raw) != entry['size'] or digest(raw) != entry['sha256']:
            raise ValueError('stock bytes differ from manifest')
    if not seen:
        raise ValueError('empty shader stock')
    actual = {p.relative_to(root).as_posix().casefold() for p in root.rglob('*') if p.is_file() and p.suffix.lower() == '.bin'}
    if actual != seen:
        raise ValueError('unregistered shader binary')
    return manifest


def stage_stock(bom, root, out, profiles):
    root, out = Path(root).resolve(), Path(out).resolve()
    if root == out or root.is_relative_to(out) or out.is_relative_to(root):
        raise ValueError('source and stock roots overlap')
    if not profiles or any(not re.fullmatch('[A-Za-z0-9_-]+', x) for x in profiles):
        raise ValueError('invalid profiles')
    raw_bom = Path(bom).read_bytes()
    document = json.loads(raw_bom)
    pending, seen = [], set()
    for entry in document['entries']:
        source_address = str(address(entry['path']))
        if not source_address.startswith('shaders/') or not source_address.endswith('.bin'):
            raise ValueError('BOM contains non-stock address')
        name = source_address[len('shaders/'):]
        if name.casefold() in seen:
            raise ValueError('duplicate shader address')
        seen.add(name.casefold())
        data = inside(root, source_address).read_bytes()
        crc = entry.get('content_crc')
        if entry.get('verified') is not True or type(entry.get('original_size')) is not int or len(data) != entry['original_size']:
            raise ValueError('unverified shader or size mismatch')
        if not isinstance(crc, str) or not re.fullmatch('0x[0-9a-fA-F]{8}', crc) or xxh32(data) != int(crc, 16):
            raise ValueError('BOM XXH32 mismatch')
        pending.append((name, data))
    if not pending:
        raise ValueError('empty shader BOM')
    old = verify_stock(out) if (out / 'manifest.json').exists() else None
    if old is None and out.exists() and any(out.rglob('*.bin')):
        raise ValueError('unowned shader binaries')
    owned = {e['address'] for e in old['files']} if old else set()
    for name, data in pending:
        target = inside(out, name)
        if target.exists() and name not in owned:
            raise ValueError('refusing unknown output collision')
    # Validate everything before writing or deleting. Only old verified owned files retire.
    out.mkdir(parents=True, exist_ok=True)
    entries = []
    for name, data in pending:
        target = inside(out, name)
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_name(target.name + '.jce-stock-tmp')
        if temporary.exists():
            raise ValueError('temporary output collision')
        temporary.write_bytes(data)
        os.replace(temporary, target)
        entries.append({'address': name, 'size': len(data), 'sha256': digest(data)})
    for name in owned - {x[0] for x in pending}:
        inside(out, name).unlink()
    manifest = {'schema': 1, 'profiles': list(profiles), 'bom_sha256': digest(raw_bom),
                'bom_content_checksum': 'XXH32(seed=0)', 'files': entries}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
    return verify_stock(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bom', required=True, type=Path)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--profiles', required=True, nargs='+')
    args = parser.parse_args()
    stage_stock(args.bom, args.root, args.out, args.profiles)


if __name__ == '__main__':
    main()
