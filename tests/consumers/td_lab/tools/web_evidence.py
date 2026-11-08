"""Shared consumer QA parsing and strict byte identity helpers; no product state."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys


ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')
CHUNK = re.compile(r'TD_RENDER_CHUNK part=(0|[1-9][0-9]*) total=([1-9][0-9]*) bytes=([1-9][0-9]*) payload=')
CHUNK_BYTES = 384
MAX_RENDER_BYTES = 16 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value, name, *, positive=False, integer=False):
    require(type(value) in (int, float) and math.isfinite(value) and value >= 0,
            'Missing/nonfinite/negative ' + name)
    if positive:
        require(value > 0, 'Nonpositive ' + name)
    if integer:
        require(type(value) is int, 'Noninteger ' + name)
    return value


def text(value, name):
    require(isinstance(value, str) and bool(value.strip()), 'Missing ' + name)
    return value


def sha(value, name):
    require(isinstance(value, str) and re.fullmatch(r'[a-f0-9]{64}', value), 'Invalid SHA-256: ' + name)
    return value


def invalid_constant(value):
    raise ValueError('Nonstandard JSON constant: ' + value)


def strict_json(raw):
    return json.loads(raw, parse_constant=invalid_constant)


def referenced_bytes(path, expected_sha):
    path = Path(text(path, 'evidence path'))
    require(path.is_absolute(), 'Evidence path must be absolute')
    raw = path.read_bytes()
    require(hashlib.sha256(raw).hexdigest() == sha(expected_sha, 'referenced evidence'), 'Referenced evidence bytes changed')
    return raw


def referenced_json(path, expected_sha):
    return strict_json(referenced_bytes(path, expected_sha).decode('utf-8-sig'))


def parse_render_snapshot(logs):
    """One complete legacy object or one exact ordered 384-byte chunk set."""
    require(isinstance(logs, list) and all(isinstance(line, str) for line in logs), 'Actual log lines required')
    chunks, legacy = [], []
    for raw_line in logs:
        line = ANSI.sub('', raw_line)
        if 'TD_RENDER_CHUNK' in line:
            match = CHUNK.search(line)
            require(match is not None, 'Malformed render chunk header')
            payload, marker, trailer = line[match.end():].rpartition(' TD_RENDER_END')
            require(bool(marker), 'Missing render chunk end marker')
            require(not trailer.strip() or trailer.lstrip().startswith('at '), 'Unexpected render chunk trailer')
            chunks.append((int(match[1]), int(match[2]), int(match[3]), payload))
        elif 'TD_RENDER ' in line:
            content = line.split('TD_RENDER ', 1)[1]
            decoder = json.JSONDecoder(parse_constant=invalid_constant)
            value, end = decoder.raw_decode(content)
            trailer = content[end:]
            require(not trailer.strip() or trailer.lstrip().startswith('at '), 'Unexpected legacy render trailer')
            require(isinstance(value, dict), 'Legacy render snapshot must be an object')
            legacy.append(value)
    require(not (chunks and legacy), 'Mixed render snapshot protocols are ambiguous')
    if legacy:
        require(len(legacy) == 1, 'Repeated legacy render snapshots are ambiguous')
        return {'effective': legacy[0], 'protocol': 'complete-legacy-json'}
    require(bool(chunks), 'No actual render snapshot')
    total, size = chunks[0][1:3]
    require(size <= MAX_RENDER_BYTES and total == (size + CHUNK_BYTES - 1) // CHUNK_BYTES,
            'Render chunk count/byte contract invalid')
    require(len(chunks) == total, 'Missing or repeated render chunks')
    payloads = []
    for expected, (part, count, byte_count, payload) in enumerate(chunks):
        require(part == expected and count == total and byte_count == size, 'Render chunks reordered, repeated or inconsistent')
        encoded = payload.encode('utf-8')
        expected_bytes = CHUNK_BYTES if part < total - 1 else size - CHUNK_BYTES * (total - 1)
        require(len(encoded) == expected_bytes, 'Render chunk UTF-8 byte length mismatch')
        payloads.append(encoded)
    raw = b''.join(payloads)
    require(len(raw) == size, 'Render snapshot byte length mismatch')
    value = strict_json(raw.decode('utf-8'))
    require(isinstance(value, dict), 'Render snapshot must be a JSON object')
    return {'effective': value, 'protocol': 'ordered-384-byte-chunks', 'parts': total, 'bytes': size}


def quality_from_input(raw):
    data = strict_json(raw.decode('utf-8-sig'))
    require(isinstance(data, dict), 'Input quality must be an object')
    require(type(data.get('schema')) is int and data['schema'] == 1 and data.get('status') == 'OBSERVED_CHECKS_PASSED' and data.get('mode') == 'input',
            'Successful current input schema required for quality')
    require(data.get('errors') == [], 'Input quality evidence errors/unavailable')
    artifact = sha(data.get('artifact_sha256'), 'quality artifact')
    snapshot = parse_render_snapshot(data.get('logs'))
    return {'available': True, 'artifact_sha256': artifact,
            'evidence_sha256': hashlib.sha256(raw).hexdigest(), **snapshot}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stdin-quality', action='store_true', help='Parse exact UTF-8 input evidence bytes from stdin')
    args = parser.parse_args()
    require(args.stdin_quality, '--stdin-quality is required')
    try:
        result = quality_from_input(sys.stdin.buffer.read())
    except (ValueError, UnicodeError) as error:
        print('QUALITY_UNAVAILABLE: ' + str(error), file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=True, separators=(',', ':')))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
