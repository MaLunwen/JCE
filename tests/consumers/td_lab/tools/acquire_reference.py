"""Pin the upstream reference, inventory its bytes and import selected MIT assets."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[4]/"tools"))
from validate_glb_indices import validate

PROJECT = Path(__file__).resolve().parents[1]
REPO = PROJECT.parents[2]
COMMIT = '784129a685627a18f3d3f8fa594208000d9313e8'
URL = 'https://github.com/Casmo/tower-defense'

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--reference', type=Path, default=REPO/'_workspace/references/phase1_tower_defense')
    ap.add_argument('--cook', type=Path, default=REPO/'build/host/tools/jce_cook.exe')
    args = ap.parse_args()
    ref = args.reference
    if not ref.exists():
        subprocess.run(['git', 'clone', URL+'.git', str(ref)], check=True)
    identity = subprocess.check_output(['git', '-C', str(ref), 'rev-parse', 'HEAD'], text=True).strip()
    if identity != COMMIT:
        raise RuntimeError(f'Reference identity mismatch: {identity}; expected {COMMIT}')
    subprocess.run(['git', '-C', str(ref), 'diff', '--exit-code'], check=True)
    files = []
    for file in sorted(ref.rglob('*')):
        if not file.is_file() or '.git' in file.relative_to(ref).parts:
            continue
        path = file.relative_to(ref).as_posix()
        classification = 'REFERENCE_ONLY'
        if path.startswith(('assets/towers/', 'assets/enemies/', 'assets/levels/', 'assets/ui/')) and '/css/' not in path:
            classification = 'SAFE_WITH_NOTICE'
        if '/fonts/' in path:
            classification = 'UNCLEAR'
        files.append({'path': path, 'bytes': file.stat().st_size,
                      'sha256': hashlib.sha256(file.read_bytes()).hexdigest(),
                      'license_basis': 'separate ShyFonts font terms; excluded from runtime' if '/fonts/' in path else 'repository MIT; no per-file override found',
                      'classification': classification})
    out = PROJECT/'build/reference'
    out.mkdir(parents=True, exist_ok=True)
    manifest = {'repository': URL, 'commit': COMMIT, 'branch': 'master',
                'retrieved_at': datetime.now(timezone.utc).isoformat(),
                'license': 'MIT', 'license_hash': hashlib.sha256((ref/'LICENSE').read_bytes()).hexdigest(),
                'files': files,
                'dependency_notes': ['Three.js r68 (MIT), browser only',
                                     'tween.js, OrbitControls, OBJLoader, Bootstrap: reference only',
                                     'javascript-astar MIT: not copied into the consumer'],
                'excluded': ['Distant Galaxy font: separate redistribution terms unverified',
                             'all upstream JavaScript and CSS: independently implemented consumer']}
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8',newline='\n')
    assets = PROJECT/'resources'
    models, textures = assets/'models', assets/'textures'
    models.mkdir(parents=True, exist_ok=True)
    textures.mkdir(parents=True, exist_ok=True)
    converted = []
    for folder, name in [('towers','tower-01'),('towers','tower-02'),('towers','tower-03'),('enemies','ghost'),('enemies','ufo')]:
        source = ref/'assets'/folder/(name+'.obj')
        imported_source = source
        if name == 'tower-02':
            # The reference explicitly takes OBJLoader.children[0]. Retain
            # the first OBJ object, including its global attribute tables.
            lines = source.read_text(encoding='utf-8').splitlines(True)
            objects = [i for i,line in enumerate(lines) if line.startswith('o ')]
            imported_source = out/(name+'-first-object.obj')
            imported_source.write_text(''.join(lines[:objects[1]]),encoding='utf-8',newline='\n')
        target = models/(name+'.glb')
        subprocess.run([str(args.cook), '--convert-model', str(imported_source), '--geometry-only', '--out', str(target)], check=True)
        if target.read_bytes()[:4] != b'glTF':
            raise RuntimeError(f'Invalid conversion: {target}')
        validate(target)
        converted.append({'source': source.relative_to(ref).as_posix(),
                          'output': target.relative_to(assets).as_posix(),
                          'sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
                          'object_policy':'first OBJ object' if name=='tower-02' else 'whole object',
                          'converter':'jce_cook --geometry-only', 'fallback':None})
    for source in sorted((ref/'assets').rglob('*')):
        if source.suffix.lower() in ('.png','.jpg'):
            shutil.copyfile(source,textures/source.name)
    licenses = assets/'licenses'
    licenses.mkdir(exist_ok=True)
    shutil.copyfile(ref/'LICENSE', licenses/'Casmo-MIT.txt')
    (out/'imports.json').write_text(json.dumps(converted, indent=2)+'\n', encoding='utf-8',newline='\n')
    print(f'REFERENCE PINNED {identity}; inventoried {len(files)} files; converted {len(converted)} GLBs')

if __name__ == '__main__':
    main()
