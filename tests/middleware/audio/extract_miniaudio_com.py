"""Generate a test include from one complete function in the selected original header."""
from pathlib import Path
import argparse
import hashlib
import json
import re


def write_changed(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--header', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    source = args.header.resolve(strict=True)
    data = source.read_bytes()
    text = data.decode('utf-8').replace('\r\n', '\n')
    starts = list(re.finditer(r'^static ma_result ma_context_get_MMDevice__wasapi\([^\n]+\)\n\{\n', text, re.MULTILINE))
    if len(starts) != 1:
        raise ValueError('Expected exactly one complete selected WASAPI function')
    start = starts[0].start()
    end = re.search(r'^\}\n', text[starts[0].end():], re.MULTILINE)
    if end is None:
        raise ValueError('Selected WASAPI function has no complete closing body')
    function = text[start:starts[0].end() + end.end()]
    # The complete untouched body is the production algorithm; only its external
    # COM/enumerator/log calls are typed ports in the C fixture.
    if not function.endswith('\n}\n') or 'ma_CoInitializeEx(' not in function or 'ma_CoUninitialize(' not in function:
        raise ValueError('Selected function no longer has the required COM boundary')
    default = re.findall(r'^#define MA_COINIT_VALUE\s+(\d+)\b', text, re.MULTILINE)
    if len(default) != 1:
        raise ValueError('Selected header default COM initialization flag unavailable')
    line = text.count('\n', 0, start) + 1
    digest = hashlib.sha256(data).hexdigest()
    function_digest = hashlib.sha256(function.encode('utf-8')).hexdigest()
    literal = json.dumps(source.as_posix())
    prefix = ('/* Generated from selected original dependency; never edit upstream. */\n'
              '#define JCE_TEST_MINIAUDIO_HEADER_PATH ' + literal + '\n'
              '#define JCE_TEST_MINIAUDIO_HEADER_SHA256 "' + digest + '"\n'
              '#define JCE_TEST_MINIAUDIO_FUNCTION_SHA256 "' + function_digest + '"\n'
              '#ifndef MA_COINIT_VALUE\n#define MA_COINIT_VALUE ' + default[0] + '\n#endif\n'
              '#line ' + str(line) + ' ' + literal + '\n')
    write_changed(args.output, (prefix + function).encode('utf-8'))
    manifest = dict(schema=1, source=str(source), bytes=len(data), sha256=digest,
                    function='ma_context_get_MMDevice__wasapi', first_line=line,
                    function_bytes=len(function.encode('utf-8')), function_sha256=function_digest,
                    function_body_unchanged=True, default_coinit_value=int(default[0]),
                    scope='Complete selected upstream function with typed external ports; no native COM/device/GPU execution')
    write_changed(args.manifest, (json.dumps(manifest, indent=2) + '\n').encode('utf-8'))
    print(json.dumps(manifest))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
