"""Static native-script closure planning and attributable execution receipts."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import secrets
import shutil
import uuid

_sampler_path = Path(__file__).resolve().parents[1] / 'process_sampler.py'
_sampler_spec = importlib.util.spec_from_file_location('jce_registry_sampler', _sampler_path)
_sampler = importlib.util.module_from_spec(_sampler_spec)
_sampler_spec.loader.exec_module(_sampler)
run_process = _sampler.run_process


def _sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def _json(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True)


def _write(path, value):
    Path(path).write_text(_json(value) + '\n', encoding='utf-8', newline='\n')


def _address(value):
    if not isinstance(value, str) or not value or '\\' in value or ';' in value:
        raise ValueError('native script path must be canonical relative text')
    path = PurePosixPath(value)
    if path.is_absolute() or ':' in value or any(p in ('', '.', '..') for p in value.split('/')):
        raise ValueError('native script path must be normalized and relative')
    return value.translate(str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz'))


def plan(manifest, typed_assets, target):
    required = manifest.get('required_languages', [])
    if not isinstance(required, list) or any(not isinstance(x, str) for x in required):
        raise ValueError('required_languages must contain language names')
    closure = []
    selected = {}
    seen = set()
    for asset in typed_assets:
        address = _address(asset.get('path'))
        if address in seen:
            raise ValueError('duplicate selected asset address')
        seen.add(address)
        for key in ('asset_id', 'content_id'):
            if not isinstance(asset.get(key), str) or not re.fullmatch('[0-9a-fA-F]{16}', asset[key]):
                raise ValueError('invalid selected asset identity')
        if type(asset.get('size')) is not int or asset['size'] < 0 or not isinstance(asset.get('dependencies'), list):
            raise ValueError('invalid selected asset size or dependencies')
        closure.append(dict(asset, path=address))
        rep = asset.get('representation')
        suffix = PurePosixPath(address).suffix
        if rep in ('c.class-ref', 'cpp.class-ref') or suffix in ('.jcec', '.jcecpp'):
            expected = 'c.class-ref' if suffix == '.jcec' else 'cpp.class-ref' if suffix == '.jcecpp' else rep
            if asset.get('type') != 'script' or rep != expected:
                raise ValueError('conflicting native ScriptAsset type or representation')
            selected[address] = asset
    indexed = {a['path']: a for a in closure}
    for asset in closure:
        for dependency in asset['dependencies']:
            if not isinstance(dependency, dict):
                raise ValueError('invalid selected dependency')
            child = _address(dependency.get('address'))
            if child not in indexed or dependency.get('asset_id') != indexed[child]['asset_id']:
                raise ValueError('selected dependency outside typed closure')
    if not selected and not set(required).intersection(('c', 'cpp')):
        return None
    if not selected:
        raise ValueError('required native Script backend has no verifiable selected classes')
    factories = manifest.get('native_script_factories')
    if not isinstance(factories, list) or not factories or any(not isinstance(x, str) or not re.fullmatch('[A-Za-z_][A-Za-z0-9_]*', x) for x in factories):
        raise ValueError('native_script_factories must be nonempty C symbols')
    factories = list(dict.fromkeys(factories))
    refs = manifest.get('native_script_refs')
    if not isinstance(refs, dict):
        raise ValueError('native_script_refs must map selected addresses to authored paths')
    bindings = {}
    for address, authored in refs.items():
        canonical = _address(address)
        if address != canonical or _address(authored) != canonical or canonical not in selected:
            raise ValueError('native script binding identity or selected type mismatch')
        bindings[canonical] = authored
    if set(bindings) != set(selected):
        raise ValueError('native script bindings do not cover selected closure')
    resolved_target = target
    if not isinstance(resolved_target, str) or not resolved_target or ';' in resolved_target or any(c.isspace() for c in resolved_target):
        raise ValueError('native registry target must be explicit text')
    body = {'target': resolved_target, 'factories': factories, 'bindings': bindings,
            'assets': sorted(closure, key=lambda a: a['path'])}
    return dict(body, closure_sha256=hashlib.sha256(_json(body).encode()).hexdigest(),
                state='PENDING_STATIC_REGISTRY_EXECUTION')


def cmake_args(value):
    if value is None:
        return []
    return ['-DJCE_REQUIRED_NATIVE_FACTORIES=' + ';'.join(value['factories']),
            '-DJCE_REQUIRED_NATIVE_SCRIPT_PATHS=' + ';'.join(value['bindings'][p] for p in sorted(value['bindings'])),
            '-DJCE_NATIVE_SCRIPT_CLOSURE_SHA256=' + value['closure_sha256'],
            '-DJCE_REQUIRED_SCRIPT_TARGET=' + value['target']]


def _receipt(receipt, request):
    if not isinstance(receipt, dict):
        raise ValueError('native registry receipt missing')
    expected = {'schema': 1, 'nonce': request['nonce'], 'closure_sha256': request['closure_sha256'],
                'target': request['target'], 'selected_count': request['selected_count'],
                'resolved_count': request['selected_count'], 'factory_count': request['factory_count'],
                'registered_factory_count': request['factory_count'], 'ok': True}
    for key, value in expected.items():
        if type(receipt.get(key)) is not type(value) or receipt[key] != value:
            raise ValueError('native registry receipt mismatch: ' + key)
    return receipt


def verify(driver, args, artifact, value, target):
    if value is None:
        return None
    artifact = Path(artifact).resolve()
    digest = _sha(artifact)
    root = Path(args.project).resolve() / 'build' / 'native-registry-proof'
    root.mkdir(parents=True, exist_ok=True)
    request = {'schema': 1, 'artifact_sha256': digest, 'closure_sha256': value['closure_sha256'],
               'target': value['target'], 'selected_count': len(value['bindings']),
               'factory_count': len(value['factories']), 'nonce': secrets.token_hex(32)}
    web = artifact.suffix.lower() in ('.html', '.htm')
    if web:
        path = root / 'web-request.json'
        request['environment'] = {'JCE_NATIVE_REGISTRY_REPORT': '/jce-native-registry.json',
                                  'JCE_NATIVE_REGISTRY_NONCE': request['nonce']}
        if path.is_file():
            old = json.loads(path.read_text(encoding='utf-8'))
            if all(old.get(k) == request[k] for k in ('artifact_sha256', 'closure_sha256', 'target', 'selected_count', 'factory_count')):
                request = old
        _write(path, request)
        proof = getattr(args, 'registry_proof', None)
        if not proof:
            raise ValueError('native Web registry execution pending: ' + str(path))
        wrapper = json.loads(Path(proof).read_text(encoding='utf-8'))
        if wrapper.get('request_sha256') != _sha(path) or wrapper.get('artifact_sha256') != digest:
            raise ValueError('stale or unbound external Web registry proof')
        receipt = _receipt(wrapper.get('receipt'), request)
        return {'state': 'EXTERNAL_STATIC_REGISTRY_EXECUTION_VERIFIED', 'request': str(path),
                'request_sha256': _sha(path), 'artifact_sha256': digest, 'receipt': receipt,
                'scope': 'external actual SDK receipt; not HTML audit or browser performance'}
    out = root / uuid.uuid4().hex
    out.mkdir()
    isolated = out / 'application'
    isolated.mkdir()
    copied = isolated / artifact.name
    shutil.copy2(artifact, copied)
    receipt_path = out / 'receipt.json'
    env = {k: v for k, v in os.environ.items() if not k.startswith('JCE_')}
    profile = out / 'profile'
    profile.mkdir()
    env.update(JCE_NATIVE_REGISTRY_REPORT=str(receipt_path), JCE_NATIVE_REGISTRY_NONCE=request['nonce'],
               JCE_BACKEND='noop', JCE_WINDOW_HIDDEN='1', JCE_MAX_FRAMES='2', JCE_MULTI_INSTANCE='1',
               JCE_PROFILE_DIR=str(profile), JCE_CONFIG_DIR=str(profile), JCE_USER_DIR=str(profile),
               JCE_EDITOR_PROFILE_DIR=str(profile))
    request['environment'] = {k: v for k, v in env.items() if k.startswith('JCE_')}
    request_path = out / 'request.json'
    _write(request_path, request)
    process = run_process([str(copied)], env=env, cwd=isolated, timeout=30, output_dir=out / 'process')
    if process.get('returncode') != 0 or process.get('timed_out') is not False:
        raise ValueError('native registry relocated execution failed: ' + str(out))
    if _sha(artifact) != digest or _sha(copied) != digest:
        raise ValueError('native registry executable changed during proof')
    if not receipt_path.is_file():
        raise ValueError('native registry execution produced no receipt: ' + str(out))
    receipt = _receipt(json.loads(receipt_path.read_text(encoding='utf-8')), request)
    summary = {'state': 'STATIC_REGISTRY_EXECUTION_VERIFIED', 'artifact_sha256': digest,
               'request_sha256': _sha(request_path), 'receipt_sha256': _sha(receipt_path),
               'request': str(request_path), 'receipt_path': str(receipt_path), 'receipt': receipt,
               'process': {k: process.get(k) for k in ('returncode', 'timed_out', 'elapsed_s')},
               'scope': 'relocated EXE actual SDK registry resolution; not gameplay correctness'}
    _write(out / 'proof.json', summary)
    return summary
