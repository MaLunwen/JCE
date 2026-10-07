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


def bundle_cmake_args(project, manifest):
    """Forward explicit bundle inputs and clear previous project cache values."""
    import os
    bundles = manifest.get('bundles', [])
    if not isinstance(bundles, list) or any(not isinstance(p, str) or not p for p in bundles):
        raise ValueError('bundles must be an array of nonempty file paths')
    paths = []
    seen = set()
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
        paths.append(path.as_posix().replace(';', '\\;'))
    flags = ['-U', 'JCE_PROJECT_BUNDLES', '-U', 'JCE_PROJECT_PREBUILT_BUNDLE_DIR']
    if paths:
        flags.append('-DJCE_PROJECT_BUNDLES=' + ';'.join(paths))
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
    if host in ('android', 'ios'):
        return host
    if host in ('windows', 'linux', 'macos'):
        return host
    raise ValueError('unsupported bundle target platform')


def _admission_address(value):
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
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


def admit_project(driver,args,project,manifest,sdk,target=None):
    value=profile(args)
    available=["lua", *driver.sdk_shipped_script_backends(sdk)]
    # Authoring compiles SDK consumers; it is not a shipment admission.
    # Embedded delivery still resolves and validates the complete selected graph.
    if value == 'authoring':
        return admit(manifest, [], value, available)
    addresses=[]
    typed_assets=[]
    startup=manifest.get('startup_scene')
    if startup:
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
