"""Shared shipment policy; dependency discovery remains the public bundle packer."""
import json
import importlib.util
import re
from html.parser import HTMLParser
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
POLICY=json.loads((ROOT/'contracts/delivery-profile.json').read_text(encoding='utf-8'))
EXTENSIONS={'.lua':'lua','.jcejs':'js','.py':'python','.java':'java','.class':'java','.cs':'csharp',
            '.jcec':'c','.jcecpp':'cpp'}


def native_registry_module():
    path = Path(__file__).with_name('native_registry.py')
    spec = importlib.util.spec_from_file_location('jce_native_registry', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def native_registry_cmake_args(args):
    fields = ('JCE_REQUIRED_NATIVE_FACTORIES', 'JCE_REQUIRED_NATIVE_SCRIPT_PATHS',
              'JCE_NATIVE_SCRIPT_CLOSURE_SHA256')
    result = [part for field in fields for part in ('-U', field)]
    plan = getattr(args, '_native_registry_plan', None)
    if plan:
        result.extend(native_registry_module().cmake_args(plan))
    return result


def native_artifact_name(name, host):
    """Resolve a manifest executable name without duplicating host suffixes."""
    if not isinstance(name, str) or not name:
        raise ValueError('native artifact name must be nonempty text')
    return name + '.exe' if host == 'windows' and not name.lower().endswith('.exe') else name


def profile(args):
    value='embedded' if getattr(args,'standalone',False) else getattr(args,'delivery_profile','embedded')
    if value not in POLICY['profiles']:
        raise ValueError('unknown delivery profile: '+str(value))
    return value


def cmake_flags(value, java=True, restore_authoring=False):
    if value not in POLICY['profiles']:
        raise ValueError('unknown delivery profile: '+str(value))
    flags=['-DJCE_DELIVERY_PROFILE='+value]
    if value=='authoring' and restore_authoring:
        flags += ['-DJCE_BUILD_SCRIPT_VM_PYTHON=ON','-DJCE_BUILD_SCRIPT_CSHARP=ON',
                  '-DJCE_BUILD_SCRIPT_API_SHARED=ON','-DJCE_BUILD_SCRIPT_API_STATIC=OFF',
                  '-DJCE_BUILD_SCRIPT_JAVA='+('ON' if java else 'OFF')]
    return flags



def restore_authoring(cache):
    if not cache.is_file():
        return False
    return 'JCE_DELIVERY_PROFILE:STRING=embedded' in cache.read_text(encoding='utf-8')


def require_embedded_sdk(driver,args,sdk):
    if profile(args)!='embedded' or driver.DRY_RUN:
        return
    stamp=sdk/'delivery-profile.json'
    if not stamp.is_file() or json.loads(stamp.read_text(encoding='utf-8')).get('profile')!='embedded':
        raise ValueError('embedded application requires a stamped embedded SDK; build sdk --delivery-profile embedded first')


def admit(manifest, addresses, value, available=None, typed_assets=(), native_plan=None):
    if native_plan is not None and (not isinstance(native_plan, dict) or
            native_plan.get('state') != 'PENDING_STATIC_REGISTRY_EXECUTION' or
            not native_plan.get('factories') or not re.fullmatch('[0-9a-f]{64}', native_plan.get('closure_sha256', ''))):
        raise ValueError('invalid native registry build plan')
    allowed=set(POLICY['profiles'][value]['languages'])
    required=manifest.get('required_languages',[])
    if not isinstance(required,list) or any(not isinstance(item,str) for item in required):
        raise ValueError('required_languages must be an array of language names')
    languages=set(required)
    if value=='embedded' and languages.intersection(('c','cpp')) and not native_plan:
        raise ValueError('required native script backend lacks verified static project registry')
    for address in addresses:
        language=EXTENSIONS.get(Path(address).suffix.lower())
        if language:
            languages.add(language)
            if value=='embedded' and language in ('c','cpp') and not native_plan:
                raise ValueError('selected native script requires a verified statically linked project registry: '+address)
    representations={'lua.source':'lua','python.source':'python',
                     'java.source':'java','java.class':'java',
                     'js.source':'js','c.class-ref':'c','cpp.class-ref':'cpp',
                     'csharp.class-ref':'csharp'}
    for asset in typed_assets:
        if asset.get('type')!='script':
            continue
        address=asset['path']
        language=representations.get(asset.get('representation'))
        extension=EXTENSIONS.get(Path(address).suffix.lower())
        if not language or (extension and extension!=language):
            raise ValueError('unknown or conflicting selected ScriptAsset representation: '+address)
        languages.add(language)
        if value=='embedded' and language in ('c','cpp') and not native_plan:
            raise ValueError('selected native script requires a verified statically linked project registry: '+address)
    if not languages <= allowed:
        raise ValueError('delivery profile '+value+' cannot provide required languages: '+', '.join(sorted(languages-allowed)))
    if available is not None and not languages <= set(available):
        raise ValueError("selected project requires unavailable installed SDK languages: "+", ".join(sorted(languages-set(available))))
    return sorted(languages)


def project_bundle_paths(project, manifest):
    """Resolve ordered explicit inputs once; callers never infer a bundle catalog."""
    import os
    bundles = manifest.get('bundles', [])
    if not isinstance(bundles, list) or any(not isinstance(p, str) or not p for p in bundles):
        raise ValueError('bundles must be an array of nonempty file paths')
    paths, seen = [], set()
    for value in bundles:
        if any(c in value for c in ('\0', '\n', '\r')):
            raise ValueError('bundle path contains a control character')
        path = (Path(project) / value).resolve()
        if path.suffix.lower() != '.jbundle' or not path.is_file():
            raise ValueError('project bundle unavailable: ' + str(path))
        identity = os.path.normcase(str(path))
        if identity in seen:
            raise ValueError('duplicate project bundle: ' + str(path))
        seen.add(identity)
        paths.append(path)
    return paths


def bundle_cmake_args(project, manifest):
    """Forward explicit inputs and clear previous project cache values."""
    paths = project_bundle_paths(project, manifest)
    flags = ['-U', 'JCE_PROJECT_BUNDLES', '-U', 'JCE_PROJECT_PREBUILT_BUNDLE_DIR']
    if paths:
        flags.append('-DJCE_PROJECT_BUNDLES=' + ';'.join(
            path.as_posix().replace(';', '\\;') for path in paths))
    return flags


def shader_stock_root(sdk):
    """Return only a verified, explicit installed SDK stock directory."""
    import importlib.util
    stock = sdk / 'share/jce/shaders'
    if not stock.exists():
        return None
    source = Path(__file__).resolve().parents[1] / 'assets/shader_stock.py'
    spec = importlib.util.spec_from_file_location('jce_delivery_shader_stock', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.verify_stock(stock)
    return stock.resolve()


def bundle_platform(target):
    if target is None:
        return 'auto'
    if target.get('emscripten'):
        return 'web'
    host = target.get('host')
    if host == 'darwin':
        return 'macos'
    if host in ('android', 'ios'):
        return host
    if host in ('windows', 'linux', 'macos'):
        return host
    raise ValueError('unsupported bundle target platform')


def _admission_address(value):
    if (not isinstance(value, str) or not value or "\\" in value or ":" in value
            or any(ord(character) < 32 or ord(character) == 127 for character in value)):
        raise ValueError('invalid admission relative address')
    if any(part in ('', '.', '..') for part in value.split('/')):
        raise ValueError('invalid admission relative address')
    return value


def _single_admission_assets(output, startup):
    archive = output / 'admission.jbundle'
    sidecar = output / 'admission.jbundle.json'
    graph_path = output / 'bundle_graph.json'
    for path in (archive, sidecar, graph_path):
        if not path.is_file() or not path.resolve().is_relative_to(output.resolve()):
            raise ValueError('missing or escaped single admission output: ' + str(path))
    metadata = json.loads(sidecar.read_text(encoding='utf-8'))
    graph = json.loads(graph_path.read_text(encoding='utf-8'))
    for value, name in ((metadata, 'jce.bundle'), (graph, 'jce.bundle.graph')):
        if not isinstance(value, dict):
            raise ValueError('invalid admission metadata object')
        contract = value.get('contract', {})
        if not isinstance(contract, dict):
            raise ValueError('invalid admission metadata contract')
        if contract.get('name') != name or type(contract.get('major')) is not int or contract['major'] != 1:
            raise ValueError('invalid admission metadata contract')
    if metadata.get('id') != 'admission' or metadata.get('scene_path') != _admission_address(startup):
        raise ValueError('single admission identity or startup mismatch')
    if type(metadata.get('archive_size')) is not int or metadata['archive_size'] <= 0 or archive.stat().st_size != metadata['archive_size']:
        raise ValueError('single admission archive size mismatch')
    indexed = []
    for value, key in ((graph, 'address'), (metadata, 'path')):
        assets = value.get('assets')
        if not isinstance(assets, list) or not assets:
            raise ValueError('missing single admission assets')
        entries = {}
        for asset in assets:
            if not isinstance(asset, dict):
                raise ValueError('invalid admission asset object')
            address = _admission_address(asset.get(key))
            if address in entries:
                raise ValueError('duplicate admission asset: ' + address)
            entries[address] = asset
        indexed.append(entries)
    selected, typed = indexed
    if set(selected) != set(typed) or startup not in selected:
        raise ValueError('single admission closure mismatch')
    for address, asset in selected.items():
        other = typed[address]
        for key in ('asset_id', 'content_id', 'size', 'dependencies'):
            if key not in asset or key not in other or asset[key] != other[key]:
                raise ValueError('single admission asset identity mismatch: ' + address)
        if type(asset['size']) is not int or asset['size'] < 0:
            raise ValueError('invalid admission asset size')
        for key in ('asset_id', 'content_id'):
            if not isinstance(asset[key], str) or not re.fullmatch('[0-9a-fA-F]{16}', asset[key]):
                raise ValueError('invalid admission asset identity')
        if not isinstance(asset['dependencies'], list):
            raise ValueError('invalid admission dependencies')
        for dependency in asset['dependencies']:
            if not isinstance(dependency, dict):
                raise ValueError('invalid admission dependency object')
            child = _admission_address(dependency.get('address'))
            if child not in selected or dependency.get('asset_id') != selected[child]['asset_id']:
                raise ValueError('admission dependency outside selected closure')
        if not isinstance(other.get('type'), str) or not other['type'] or not isinstance(other.get('representation'), str) or not other['representation']:
            raise ValueError('missing typed admission asset')
    return list(selected), list(typed.values())


def _read_public_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate public inspection JSON key: ' + key)
            result[key] = value
        return result

    def invalid_constant(value):
        raise ValueError('nonfinite public inspection JSON: ' + value)

    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique,
                      parse_constant=invalid_constant)


def _typed_asset_table(assets):
    if not isinstance(assets, list):
        raise ValueError('missing public Bundle typed assets')
    table = {}
    for asset in assets:
        if not isinstance(asset, dict):
            raise ValueError('invalid public Bundle asset')
        address = _admission_address(asset.get('path'))
        if address == '__bundle__/manifest.json' or address in table:
            raise ValueError('reserved or duplicate Bundle asset: ' + address)
        if type(asset.get('size')) is not int or asset['size'] < 0:
            raise ValueError('invalid Bundle asset size')
        for field in ('asset_id', 'content_id'):
            if not isinstance(asset.get(field), str) or not re.fullmatch('[0-9a-f]{16}', asset[field]):
                raise ValueError('invalid Bundle asset identity: ' + address)
        for field in ('type', 'representation'):
            if (not isinstance(asset.get(field), str) or not asset[field]
                    or any(ord(character) < 32 or ord(character) == 127 for character in asset[field])):
                raise ValueError('missing or invalid typed Bundle asset: ' + address)
        if not isinstance(asset.get('dependencies'), list):
            raise ValueError('missing typed Bundle dependencies: ' + address)
        table[address] = asset
    for asset in table.values():
        for dependency in asset['dependencies']:
            if not isinstance(dependency, dict):
                raise ValueError('invalid Bundle dependency')
            child = _admission_address(dependency.get('address'))
            if child not in table or dependency.get('asset_id') != table[child]['asset_id']:
                raise ValueError('Bundle dependency outside validated closure: ' + child)
    return table


def merge_selected_assets(groups):
    """Typed identities constrain admission; the host packer still compares bytes."""
    merged, sources = {}, {}
    fields = ('asset_id', 'content_id', 'size', 'type', 'representation')
    for source, assets in groups:
        for asset in assets:
            address = _admission_address(asset.get('path'))
            prior = merged.get(address)
            if prior and any(prior.get(field) != asset.get(field) for field in fields):
                raise ValueError('conflicting declared asset identity: ' + address)
            if prior:
                prior_edges = {(edge['address'], edge['asset_id']) for edge in prior['dependencies']}
                edges = {(edge['address'], edge['asset_id']) for edge in asset['dependencies']}
                if prior_edges != edges:
                    raise ValueError('conflicting declared dependency metadata: ' + address)
            else:
                merged[address] = asset
            sources.setdefault(address, []).append(str(source))
    return list(merged.values()), sources


def verify_selected_pack(report, pak, embed, selected_sources, selected_assets, provenance):
    """Validate the existing public packer's actual authenticated byte receipt."""
    if not isinstance(report, dict):
        raise ValueError('invalid PAK BOM object')
    header, totals = report.get('header', {}), report.get('totals', {})
    if not isinstance(header, dict) or not isinstance(totals, dict):
        raise ValueError('invalid PAK BOM verification records')
    count = totals.get('entries')
    if (report.get('schema') != 'jce.pakbom.v1' or report.get('file') != str(pak)
            or report.get('file_size') != pak.stat().st_size
            or report.get('sources_verified') is not True
            or any(header.get(name) is not True for name in
                   ('header_verified', 'encrypted', 'secure_index', 'authenticated', 'auth_verified'))
            or type(count) is not int or count <= 0 or totals.get('verify_ran') is not True
            or totals.get('verified_count') != count or totals.get('corrupt_count') != 0
            or totals.get('skipped_count') != 0 or totals.get('encrypted_count') != count
            or totals.get('authenticated_count') != count):
        raise ValueError('PAK BOM authentication or deep verification incomplete')
    actual = report.get('selected_sources')
    if actual is None and len(selected_sources) == 1:
        actual = [report.get('selected_source')]
    if not isinstance(actual, list) or len(actual) != len(selected_sources):
        raise ValueError('PAK selected archive provenance incomplete; refresh SDK')
    for row, source in zip(actual, selected_sources):
        expected = {'path': source['path'], 'size': source['size'],
                    'content_id': source['content_id'], 'imported_count': len(source['assets'])}
        if not isinstance(row, dict) or any(type(row.get(key)) is not type(value) or row[key] != value
                                           for key, value in expected.items()):
            raise ValueError('PAK selected archive identity mismatch')
    rows = report.get('sources')
    if not isinstance(rows, list) or len(rows) != count:
        raise ValueError('PAK BOM source rows incomplete')
    selected_rows, addresses = {}, set()
    for row in rows:
        if not isinstance(row, dict) or type(row.get('selected')) is not bool:
            raise ValueError('invalid PAK BOM source row')
        address = _admission_address(row.get('path'))
        if address in addresses:
            raise ValueError('duplicate PAK BOM source address')
        addresses.add(address)
        if row['selected']:
            selected_rows[address] = row
    expected = {asset['path']: asset for asset in selected_assets}
    if set(selected_rows) != set(expected):
        raise ValueError('PAK selected sources do not match declared typed closure')
    for address, asset in expected.items():
        row = selected_rows[address]
        if (row.get('source') not in provenance[address] or type(row.get('size')) is not int
                or row['size'] != asset['size'] or row.get('content_id') != asset['content_id']):
            raise ValueError('PAK selected content identity mismatch: ' + address)
    if '__bundle__/manifest.json' in addresses or not embed.is_file():
        raise ValueError('reserved Bundle manifest leaked or embed output missing')
    return count


def verify_bundle_inputs(driver, plan):
    if not plan:
        return
    for source in plan['bundles']:
        path = Path(source['path'])
        sidecar = Path(str(path) + '.json')
        if not path.is_file() or driver._sha256(path) != source['sha256']:
            raise ValueError('declared Bundle changed after admission: ' + str(path))
        actual_sidecar = driver._sha256(sidecar) if sidecar.is_file() else None
        if actual_sidecar != source['sidecar_sha256']:
            raise ValueError('declared Bundle sidecar changed after admission: ' + str(sidecar))
        report = Path(source['inspection_path'])
        if not report.is_file() or driver._sha256(report) != source['inspection_sha256']:
            raise ValueError('public Bundle inspection changed after admission: ' + str(report))


def inspect_project_bundles(driver, project, manifest, sdk, target=None, sdk_tools=None):
    """Consume public host-tool evidence, never decode an archive or trust a sidecar."""
    paths = project_bundle_paths(project, manifest)
    if not paths:
        return None
    if driver.DRY_RUN:
        raise ValueError('declared Bundle shipment needs actual public inspection; dry-run cannot certify its scene or typed closure')
    tools = sdk_tools or {}
    tool = tools.get('jce_pak')
    if tool is None:
        name = 'jce_pak' + ('.exe' if driver.HOST == 'windows' else '')
        tool = next((sdk / folder / name for folder in ('bin', 'bin/host')
                     if (sdk / folder / name).is_file()), sdk / 'bin' / name)
    if not Path(tool).is_file():
        raise ValueError('selected SDK lacks public Bundle inspector; refresh SDK: ' + str(tool))
    output = project / 'build/delivery-admission/bundles'
    output.mkdir(parents=True, exist_ok=True)
    sources, ids, first_scene = [], set(), None
    platform = bundle_platform(target)
    for index, path in enumerate(paths):
        sidecar_path = Path(str(path) + '.json')
        before = driver._sha256(path)
        sidecar_hash = driver._sha256(sidecar_path) if sidecar_path.is_file() else None
        report_path = output / ('inspect-%d.json' % index)
        report_path.unlink(missing_ok=True)
        category = 'web' if platform == 'web' else ('mobile' if platform in ('android', 'ios') else 'desktop')
        driver.run([str(tool), '--inspect', str(path), '--verify', '--json', str(report_path),
                    '--platform', category, '--bundle-target', platform], cwd=driver.ROOT)
        if not report_path.is_file():
            raise ValueError('public Bundle inspection did not produce fresh evidence; refresh SDK')
        report = _read_public_json(report_path)
        if not isinstance(report, dict):
            raise ValueError('invalid public Bundle inspection object')
        bundle = report.get('bundle')
        header, totals = report.get('header', {}), report.get('totals', {})
        if not isinstance(header, dict) or not isinstance(totals, dict):
            raise ValueError('invalid public Bundle verification record')
        count = totals.get('entries')
        if (report.get('schema') != 'jce.pakbom.v1' or report.get('file') != str(path)
                or report.get('file_size') != path.stat().st_size
                or not isinstance(bundle, dict) or bundle.get('verified') is not True
                or header.get('header_verified') is not True or type(count) is not int or count < 1
                or totals.get('verify_ran') is not True or totals.get('verified_count') != count
                or totals.get('corrupt_count') != 0 or totals.get('skipped_count') != 0):
            raise ValueError('public Bundle contract/deep verification incomplete; refresh SDK or input Bundle')
        metadata = bundle.get('manifest')
        contract = metadata.get('contract', {}) if isinstance(metadata, dict) else {}
        if (not isinstance(contract, dict) or contract.get('name') != 'jce.bundle'
                or type(contract.get('major')) is not int or contract['major'] != 1
                or type(contract.get('minor')) is not int or contract['minor'] < 1):
            raise ValueError('declared Bundle needs validated typed contract 1.1; refresh input Bundle')
        identifier = metadata.get('id')
        if not isinstance(identifier, str) or not identifier or identifier in ids:
            raise ValueError('missing or duplicate declared Bundle identity')
        ids.add(identifier)
        if platform != 'auto' and metadata.get('target_profile') != platform:
            raise ValueError('declared Bundle target profile differs from selected platform')
        assets = _typed_asset_table(metadata.get('assets'))
        if len(assets) + 1 != count:
            raise ValueError('public Bundle assets differ from deep-verified archive')
        content_id = bundle.get('content_id')
        if not isinstance(content_id, str) or not re.fullmatch('[0-9a-f]{16}', content_id):
            raise ValueError('public Bundle archive identity unavailable; refresh SDK')
        sidecar = bundle.get('sidecar')
        if (not isinstance(sidecar, dict) or type(sidecar.get('present')) is not bool
                or type(sidecar.get('verified')) is not bool
                or type(sidecar.get('compatible')) is not bool
                or sidecar['present'] != (sidecar_hash is not None)
                or (sidecar['present'] and sidecar['compatible'] is not True)
                or (not sidecar['present'] and (sidecar['verified'] or sidecar['compatible']))):
            raise ValueError('present Bundle sidecar lacks actual reader compatibility; absent sidecars remain optional')
        scene = metadata.get('scene_path')
        if scene is not None and not isinstance(scene, str):
            raise ValueError('invalid declared Bundle scene path')
        if scene:
            scene = _admission_address(scene)
            if scene not in assets or assets[scene]['type'] != 'scene':
                raise ValueError('declared Bundle startup scene is not a validated SceneAsset')
            if first_scene is None:
                first_scene = scene
        dependencies = metadata.get('depends_on')
        if not isinstance(dependencies, list) or any(not isinstance(value, str) or not value for value in dependencies):
            raise ValueError('invalid declared Bundle dependency list')
        sources.append({'path': str(path), 'sha256': before, 'sidecar_sha256': sidecar_hash,
                        'inspection_path': str(report_path), 'inspection_sha256': driver._sha256(report_path),
                        'sidecar_verified': sidecar['verified'], 'sidecar_compatible': sidecar['compatible'],
                        'content_id': content_id, 'size': path.stat().st_size,
                        'id': identifier, 'depends_on': dependencies, 'assets': list(assets.values())})
    for source in sources:
        if any(value not in ids for value in source['depends_on']):
            raise ValueError('declared Bundle dependency is missing from the project')
    assets, provenance = merge_selected_assets((source['path'], source['assets']) for source in sources)
    table = {asset['path']: asset for asset in assets}
    startup = manifest.get('startup_scene')
    if startup is not None and startup != '':
        startup = _admission_address(startup)
    else:
        startup = first_scene
    if not startup:
        raise ValueError('declared Bundle project has no selected scene')
    resources = (project / (manifest.get('source_assets') or 'resources')).resolve()
    loose = (resources / startup).resolve()
    use_loose = bool(manifest.get('startup_scene')) and loose.is_relative_to(resources) and loose.is_file()
    if not use_loose and (startup not in table or table[startup]['type'] != 'scene'):
        raise ValueError('selected startup scene unavailable in declared Bundles or source assets')
    plan = {'bundles': sources, 'startup_scene': startup, 'use_loose_scene': use_loose,
            'assets': assets, 'provenance': provenance}
    verify_bundle_inputs(driver, plan)
    return plan


def admit_project(driver,args,project,manifest,sdk,target=None,sdk_tools=None):
    value=profile(args)
    available=["lua", *driver.sdk_shipped_script_backends(sdk)]
    # Authoring compiles SDK consumers; it is not a shipment admission.
    # Embedded delivery still resolves and validates the complete selected graph.
    args._bundle_admission = None
    if value == 'authoring' and getattr(args, 'variant', None) != 'dist':
        return admit(manifest, [], value, available)
    plan = inspect_project_bundles(driver, project, manifest, sdk, target, sdk_tools)
    args._bundle_admission = plan
    addresses=[]
    typed_assets=[]
    startup=plan['startup_scene'] if plan else manifest.get('startup_scene')
    if startup and (plan is None or plan['use_loose_scene']):
        resources=project/(manifest.get('source_assets') or 'resources')
        _admission_address(startup)
        scene=(resources/startup).resolve()
        if not scene.is_file():
            raise ValueError('selected startup scene unavailable: '+str(scene))
        output=project/'build/delivery-admission'/value
        output.mkdir(parents=True,exist_ok=True)
        tool=sdk/'bin'/('jce_bundle_pack.exe' if driver.HOST=='windows' else 'jce_bundle_pack')
        if not tool.is_file():
            tool=sdk/'bin/host'/tool.name
        if not tool.is_file():
            raise ValueError('selected SDK lacks public bundle dependency resolver: '+str(tool))
        stock = shader_stock_root(sdk)
        stock_args = ['--shader-root', str(stock)] if stock else []
        if not driver.DRY_RUN:
            for name in ('admission.jbundle', 'admission.jbundle.json', 'bundle_graph.json'):
                (output / name).unlink(missing_ok=True)
        driver.run([str(tool),'--resource-root',str(resources.resolve()),'--scene',str(scene),
                    '--out',str(output),'--single-file','--id','admission','--target',bundle_platform(target),
                    *stock_args],cwd=driver.ROOT)
        if not driver.DRY_RUN:
            addresses, typed_assets = _single_admission_assets(output, startup)
    if plan:
        groups = [(source['path'], source['assets']) for source in plan['bundles']]
        if typed_assets:
            groups.insert(0, (output / 'admission.jbundle', typed_assets))
        typed_assets, _ = merge_selected_assets(groups)
        addresses = [asset['path'] for asset in typed_assets]
    target_name = (getattr(args, 'target', None) or manifest.get('target') or
                   manifest.get('name') or project.name)
    args._native_registry_plan = native_registry_module().plan(manifest, typed_assets, target_name)
    args._native_registry_target = target or {'host': driver.HOST}
    return admit(manifest, addresses, value, available, typed_assets,
                 native_plan=args._native_registry_plan)


class _HTMLDependencies(HTMLParser):
    def __init__(self):
        super().__init__()
        self.external=[]

    def handle_starttag(self, tag, attrs):
        for key,value in attrs:
            if key in ('src','href') and value and not value.startswith(('data:','#')):
                if tag in ('script','link','img','source','video','audio'):
                    self.external.append(value)


def audit_html(artifact):
    text=Path(artifact).read_text(encoding='utf-8')
    parser=_HTMLDependencies(); parser.feed(text)
    if parser.external:
        raise ValueError('single HTML has external resource dependencies: '+', '.join(parser.external))
    if re.search(r"[\"'](?!data:)[^\"'\n]*\.(?:wasm|data)(?:[?][^\"']*)?[\"']",text):
        raise ValueError('single HTML references an external runtime payload')
    if '<script' not in text.lower() or not any(marker in text for marker in
            ('data:application/octet-stream;base64,','data:application/wasm;base64,','wasmBinary','binaryDecode')):
        raise ValueError('single HTML lacks inline runtime payload evidence')
    return {'delivery':'single-html','static_dependencies':'PASS',
            'offline_browser_relocation':'NOT VERIFIED'}


def smoke_requires_shared_api(sdk, shipped, roster_required):
    stamp=Path(sdk)/'delivery-profile.json'
    if not stamp.is_file():
        return roster_required
    metadata=json.loads(stamp.read_text(encoding='utf-8'))
    if metadata.get('profile')!='embedded':
        return roster_required
    if metadata.get('external_vm_allowed') is not False:
        raise ValueError('embedded SDK has inconsistent external VM declaration')
    if not set(shipped) <= set(POLICY['profiles']['embedded']['languages']):
        raise ValueError('embedded SDK contains an external VM backend')
    # Native C/C++ modules use the statically linked host table. The roster
    # flag describes the optional shared C ABI delivery used by authoring.
    return False


def audit_application(driver,args,artifact):
    if profile(args)!='embedded' or driver.DRY_RUN:
        return None
    if Path(artifact).suffix.lower()=='.html':
        result = audit_html(artifact)
    elif driver.HOST!='windows':
        result = {'status':'UNVERIFIED','reason':'native system-dependency and relocation audit not implemented for this host'}
    else:
        result = driver._standalone_module().verify(Path(artifact))
    plan = getattr(args, '_native_registry_plan', None)
    if plan:
        try:
            proof = native_registry_module().verify(driver, args, Path(artifact), plan,
                                                    args._native_registry_target)
        except ValueError as error:
            driver.die(str(error))
        result = {'application_audit': result, 'native_registry': proof}
    return result
