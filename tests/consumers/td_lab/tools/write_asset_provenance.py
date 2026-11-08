"""Write/check Git-visible selected-asset provenance from immutable reference records."""
import argparse
import hashlib
import json
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
REPO = PROJECT.parents[2]
PIN = '784129a685627a18f3d3f8fa594208000d9313e8'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def generate():
    reference = REPO / '_workspace/references/phase1_tower_defense'
    manifest = json.loads((PROJECT / 'build/reference/manifest.json').read_text())
    assert manifest['commit'] == PIN
    originals = {item['path']: item for item in manifest['files']}
    for name, item in originals.items():
        assert sha(reference / name) == item['sha256'], 'Changed immutable reference: ' + name
    imports = {item['output']: item for item in json.loads((PROJECT / 'build/reference/imports.json').read_text())}
    raster = {Path(name).name: name for name in originals if Path(name).suffix.lower() in ('.png','.jpg','.jpeg')}
    font = json.loads((PROJECT / 'resources/licenses/ui-font-provenance.json').read_text())
    assert font['provider'] == 'Pillow bundled default' and font['license'] == 'No Rights Reserved'
    entries = []
    resources = PROJECT / 'resources'
    for path in sorted(resources.rglob('*')):
        if not path.is_file() or '_cooked' in path.relative_to(resources).parts:
            continue
        name = path.relative_to(resources).as_posix()
        entry = {'path':'resources/' + name, 'bytes':path.stat().st_size, 'sha256':sha(path)}
        if name in imports:
            item = imports[name]
            assert entry['sha256'] == item['sha256'], 'Changed converted model: ' + name
            original = originals[item['source']]
            entry.update(kind='converted-model', source=item['source'], source_sha256=original['sha256'],
                         transform=item['converter'], object_policy=item['object_policy'], terms='Casmo MIT')
        elif path.parent.name == 'textures' and path.name in raster:
            source = raster[path.name]
            assert entry['sha256'] == originals[source]['sha256'], 'Changed original image: ' + name
            entry.update(kind='original-image', source=source, source_sha256=originals[source]['sha256'], terms='Casmo MIT')
        elif path.parent.name == 'ui' and path.suffix == '.png':
            entry.update(kind='generated-ui', producer='tools/author_ui.py', terms='First-party artwork; default-font glyphs No Rights Reserved',
                         font_sha256=font['font_sha256'])
        elif name == 'textures/white.png':
            entry.update(kind='generated-texture', producer='tools/author_reference_scene.py', basis='One opaque white RGBA pixel')
        elif name == 'models/ground.glb':
            entry.update(kind='reconstructed-geometry', producer='tools/author_ground.py',
                         parameters='ground-reference.json', parameters_sha256=sha(PROJECT/'ground-reference.json'),
                         basis='Pinned original Three.js BoxGeometry after normal recomputation')
        elif name == 'licenses/Casmo-MIT.txt':
            entry.update(kind='license-notice', source='LICENSE', source_sha256=originals['LICENSE']['sha256'], transform='CRLF to LF only')
        else:
            entry.update(kind='authored-metadata', terms='First-party project data; imported artwork retains its separate notice')
        entries.append(entry)
    return {'schema':1, 'reference':{'repository':manifest['repository'],'commit':PIN},
            'font':font, 'entries':entries,
            'excluded': [{'source':name, 'sha256':item['sha256'], 'scope':'Reference-only original browser/runtime/font software; not shipped'}
                         for name,item in originals.items() if name not in {e.get('source') for e in entries}],
            'scope':'Selected file identity and declared provenance; not a blanket legal determination or visual/performance certification'}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check',action='store_true')
    args=parser.parse_args()
    document=json.dumps(generate(),indent=2,ensure_ascii=False)+'\n'
    path=PROJECT/'ASSET_PROVENANCE.json'
    if args.check:
        assert path.read_text(encoding='utf-8') == document, 'Selected asset provenance is stale'
    else:
        path.write_text(document,encoding='utf-8',newline='\n')
    print('ASSET PROVENANCE', 'CHECK PASS' if args.check else 'WRITTEN', path)

if __name__=='__main__':
    main()
