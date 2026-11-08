"""Validate real SDK-native clock executables and their ordinary-failing control."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

SUMMARY = re.compile(r'^TD_CLOCK (PASS|FAIL) cases=(\d+) checks=(\d+) failures=(\d+) scope=actual_runtime_native_c_rules_headless$', re.MULTILINE)
REQUIRED_FAILURES = {'runtime_pause', 'zero_time_scale', 'half_time_scale',
                     'double_time_scale', 'host_pause_resume',
                     'host_interval_not_wall', 'presentation_rates'}


def run(executable, expected_exit):
    path = Path(executable).resolve(strict=True)
    before = path.read_bytes()
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(('JCE_', 'TD_'))}
    completed = subprocess.run([str(path)], cwd=path.parent, env=environment,
                               capture_output=True, timeout=60, check=False)
    after = path.read_bytes()
    if before != after:
        raise RuntimeError('Clock executable changed during execution: ' + str(path))
    output = completed.stdout + completed.stderr
    text = output.decode('utf-8', errors='strict')
    # Preserve original stream bytes/hash; Windows CRT emits CRLF for text.
    lines = text.replace('\r\n', '\n')
    matches = list(SUMMARY.finditer(lines))
    if completed.returncode != expected_exit or len(matches) != 1 or 'SETUP_FAILED' in text:
        raise RuntimeError('Clock executable did not complete its declared ordinary outcome:\n' + text)
    status, cases, checks, failures = matches[0].groups()
    if int(cases) != 13 or int(checks) < 30:
        raise RuntimeError('Incomplete native runtime clock cases:\n' + text)
    failure_rows = re.findall(r'^TD_CLOCK FAIL case=(\S+) line=', lines, re.MULTILINE)
    failed_cases = set(failure_rows)
    if expected_exit == 0:
        if status != 'PASS' or int(failures) != 0 or failed_cases:
            raise RuntimeError('Positive native runtime clock assertions failed:\n' + text)
    elif status != 'FAIL' or int(failures) != len(failure_rows) or not REQUIRED_FAILURES <= failed_cases:
        raise RuntimeError('Old wall control missed required clock failures:\n' + text)
    return {'executable': str(path), 'sha256': hashlib.sha256(before).hexdigest(),
            'bytes': len(before), 'exit_code': completed.returncode, 'cases': int(cases),
            'checks': int(checks), 'failures': int(failures),
            'failed_cases': sorted(failed_cases), 'output_sha256': hashlib.sha256(output).hexdigest()}, output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--normal', required=True, type=Path)
    parser.add_argument('--old-wall', required=True, type=Path)
    parser.add_argument('--out', type=Path, help='Fresh evidence directory; existing paths are refused.')
    args = parser.parse_args()
    if args.out is not None and args.out.exists():
        parser.error('--out must be a fresh evidence directory')
    normal, normal_output = run(args.normal, 0)
    control, control_output = run(args.old_wall, 1)
    if normal['sha256'] == control['sha256']:
        raise RuntimeError('Normal and old-wall binaries must have distinct identities')
    record = {'status': 'ACTUAL_RUNTIME_CONSUMER_CLOCK_VERIFIED', 'normal': normal,
              'old_wall_control': control,
              'scope': 'Production C controller, registered native C VM, public runtime pause/time-scale and host policy, real deterministic rules. Presentation ports and wall clock are test-controlled; no physical timing, GPU or editor UI claim.'}
    if args.out is not None:
        args.out.mkdir(parents=True)
        (args.out / 'normal.log').write_bytes(normal_output)
        (args.out / 'old-wall.log').write_bytes(control_output)
        (args.out / 'clock.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired, UnicodeError) as error:
        print('TD clock verification failed: ' + str(error), file=sys.stderr)
        sys.exit(1)
