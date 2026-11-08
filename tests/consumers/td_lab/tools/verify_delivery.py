"""Audit assets and reopen a content Bundle with explicit native authoring context.

The Bundle is not an executable and does not embed the TdController machine code.
Independent single-EXE/HTML relocation is certified by separate artifact checks.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import verify_game as v
sys.path.insert(0, str(v.REPO / 'tools'))
from validate_glb_indices import validate
from compare_captures import compare
from PIL import Image
from editor_lifecycle_probe import validate_scenes


def pack():
    sdk = Path(os.environ.get('TD_VERIFY_SDK', str(v.REPO / 'dist/sdk/win32-x86_64-dist-single'))).resolve()
    producer = sdk / 'bin/jce_bundle_pack.exe'
    stock = sdk / 'share/jce/shaders'
    assert producer.is_file() and stock.is_dir(), 'Verified installed SDK required'
    destination = v.OUT / 'content-bundle'
    destination.mkdir(parents=True, exist_ok=True)
    stage = v.PROJECT / 'resources/_cooked'
    scene = stage / 'scenes/td_lab.scene.json'
    args = [str(producer), '--resource-root', str(stage), '--scene', str(scene),
            '--out', str(destination), '--single-file', '--id', 'tower-defense',
            '--target', 'windows', '--shader-root', str(stock)]
    result = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', errors='replace')
    (v.OUT / 'bundle-pack.log').write_text(result.stdout + result.stderr, encoding='utf-8', newline='\n')
    assert result.returncode == 0, (result.stdout + result.stderr)[-3000:]
    bundle = destination / 'tower-defense.jbundle'
    assert bundle.is_file(), 'Producer did not emit the selected content package'
    # A missing selected scene is an actual packer failure, not a mocked check.
    bad = args.copy()
    bad[bad.index('--scene') + 1] = str(v.OUT / '__required_missing__.scene.json')
    bad[bad.index('--out') + 1] = str(v.OUT / 'negative-bundle')
    negative = subprocess.run(bad, capture_output=True, text=True, encoding='utf-8', errors='replace')
    (v.OUT / 'bundle-negative.log').write_text(negative.stdout + negative.stderr, encoding='utf-8', newline='\n')
    assert negative.returncode != 0, 'Packer accepted a missing selected scene'
    return bundle, {'bundle': str(bundle), 'bundle_sha256': hashlib.sha256(bundle.read_bytes()).hexdigest(),
                    'producer': str(producer), 'producer_sha256': hashlib.sha256(producer.read_bytes()).hexdigest(),
                    'stock_shader_root': str(stock), 'target': 'windows/d3d11',
                    'missing_selected_scene_rejected': True,
                    'scope': 'Content package; Editor Play uses the project native authoring module. Not a stock-host portable gameplay or independent executable proof.',
                    'required_native_class': 'TdController', 'required_native_factory': 'td_module'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle-only', action='store_true', help='Reopen the actual content Bundle with native authoring context; earlier input checks run separately')
    parser.add_argument('--pack-only', action='store_true', help='CPU content-package audit; no graphical acceptance claim')
    args = parser.parse_args()
    v.OUT.mkdir(parents=True, exist_ok=True)
    reference = v.REPO / '_workspace/references/phase1_tower_defense'
    manifest = json.loads((v.PROJECT / 'build/reference/manifest.json').read_text())
    for item in manifest['files']:
        assert hashlib.sha256((reference / item['path']).read_bytes()).hexdigest() == item['sha256']
    raster = [x for x in (reference / 'assets').rglob('*') if x.suffix.lower() in ('.png', '.jpg')]
    for source in raster:
        assert source.read_bytes() == (v.PROJECT / 'resources/textures' / source.name).read_bytes()
    models = list((v.PROJECT / 'resources/models').glob('*.glb'))
    for model in models:
        validate(model)
    bundle, report = pack()
    report.update(reference_files=len(manifest['files']), original_raster_assets=len(raster), validated_glbs=len(models))
    if args.pack_only:
        report['graphical_acceptance'] = 'NOT_RUN'
    else:
        if not args.bundle_only:
            log = v.run('low-money-rejection', v.idle(30) + v.key(30), extra={'TD_FIXTURE': 'low-money'}, count=100)
            assert 'action=place-rejected' in log and 'resources=0 score=0 lives=50 wave=0 towers=0' in log
            log = v.run('drag-no-selection', v.idle(30) + [((),640,360,1), ((),700,330,1,60,-30,0), ((),700,330,0)] + v.idle(30), extra={'TD_FIXTURE': 'initial'}, count=100)
            assert 'action=select' not in log and 'action=destroy menu=0 selected=-1' in log

        wrong = v.OUT / 'wrong-size.png'
        Image.new('RGB', (12,12)).save(wrong)
        try:
            compare(v.OUT / 'fixture-initial.png', wrong, v.OUT / 'must-not-compare')
        except ValueError:
            report['different_size_rejected'] = True
        else:
            raise AssertionError('Comparator accepted incompatible sizes')
        module = v.PROJECT / 'native/build/td_controller.dll'
        assert module.is_file(), 'Editor requires the explicitly rebuilt authoring module'
        report['native_authoring_module_sha256'] = hashlib.sha256(module.read_bytes()).hexdigest()
        # Measurement-only context: no loose resources; native code is explicitly
        # provided by the source project's rebuilt authoring module, not shipped.
        context = v.OUT / 'bundle-authoring-context'
        context.mkdir(exist_ok=True)
        manifest = json.loads((v.PROJECT / 'jce_project.json').read_text())
        manifest['script_modules'] = [str(module.resolve())]
        (context / 'jce_project.json').write_text(json.dumps(manifest), encoding='utf-8')
        shutil.copytree(v.PROJECT / 'Settings', context / 'Settings', dirs_exist_ok=True)
        (context / '.jce').mkdir(exist_ok=True)
        shutil.copyfile(v.PROJECT / '.jce/project-settings.json', context / '.jce/project-settings.json')
        assert not (context / 'resources').exists(), 'Loose asset fallback invalidates Bundle evidence'
        report['authoring_context'] = {'project': str(context), 'module': str(module.resolve()),
            'loose_resources': False, 'delivery_product': False}
        for index in range(2):
            profile = v.OUT / ('bundle-authoring-profile-' + str(index))
            profile.mkdir(exist_ok=True)
            (profile / 'editor-session.json').write_text(json.dumps({'last_project': str(context),
                'last_scene_path': str(bundle)}), encoding='utf-8')
            capture = v.OUT / ('bundle-game-' + str(index) + '.png')
            log = v.run('bundle-authoring-reopen-' + str(index), extra={
                'JCE_EDITOR_PROFILE_DIR': str(profile), 'JCE_SCENE': str(bundle),
                'JCE_DBG_FOCUS_GAME': '1', 'JCE_EDITOR_OPERATION_METRICS': '1',
                'JCE_DBG_AUTOPLAY': '20', 'JCE_DBG_PLAY_STOP_AT': '500',
                'JCE_DBG_SCENE_OBSERVE_AT': '10,200,640', 'JCE_DBG_SCENE_OBSERVE_DIR': str(profile),
                'JCE_KPI_GAME_CAPTURE_SIZE': '1280x720',
                'JCE_KPI_GAME_SHOTS': '3.00|' + str(capture), 'TD_FIXTURE': 'combat',
                'TD_RANDOM_TAPE': str(v.PROJECT / 'build/reference/combat.random.txt')}, count=650, exe=v.EDITOR)
            snapshots = [json.loads((profile / ('scene-' + str(frame) + '.json')).read_text())
                         for frame in (10,200,640)]
            assert len(snapshots[0]['scene']['entities']) == 104
            report['restoration_' + str(index)] = validate_scenes(*snapshots, require_mutation=True)
            assert 'action=destroy' in log and 'towers=2' in log
            assert 'UI sprite load failed' not in log, 'Missing dynamic UI dependency in Bundle'
            assert 'TD_CURSOR default=1 active=1' in log, 'Bundle cursor assets did not load'
            assert 'Unsupported image format' not in log and 'texture CPU decode failed' not in log
            result = compare(v.OUT / 'fixture-combat.png', capture, v.OUT / ('bundle-parity-' + str(index)))
            report['bundle_parity_' + str(index)] = result['regions']['full']
            assert result['regions']['full']['luma_ssim'] > .99, result
        report.update(low_money_rejection='NOT_RUN' if args.bundle_only else 'PASS', drag_no_selection='NOT_RUN' if args.bundle_only else 'PASS', editor_stop_cycles=2,
                      graphical_acceptance='PASS_WITH_NATIVE_AUTHORING_CONTEXT')
    (v.OUT / 'delivery-suite.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
