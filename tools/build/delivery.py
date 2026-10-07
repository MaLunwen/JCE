"""Shared shipment policy; dependency discovery remains the public bundle packer."""
import json
import re
from html.parser import HTMLParser
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
POLICY=json.loads((ROOT/'contracts/delivery-profile.json').read_text(encoding='utf-8'))
EXTENSIONS={'.lua':'lua','.jcejs':'js','.py':'python','.java':'java','.class':'java','.cs':'csharp',
            '.jcec':'c','.jcecpp':'cpp'}


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
                  '-DJCE_BUILD_SCRIPT_API_SHARED=ON',
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


def admit(manifest, addresses, value, available=None, typed_assets=()):
    allowed=set(POLICY['profiles'][value]['languages'])
    required=manifest.get('required_languages',[])
    if not isinstance(required,list) or any(not isinstance(item,str) for item in required):
        raise ValueError('required_languages must be an array of language names')
    languages=set(required)
    if value=='embedded' and languages.intersection(('c','cpp')):
        raise ValueError('required native script backend lacks verified static project registry')
    for address in addresses:
        language=EXTENSIONS.get(Path(address).suffix.lower())
        if language:
            languages.add(language)
            if value=='embedded' and language in ('c','cpp'):
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
        if value=='embedded' and language in ('c','cpp'):
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


def admit_project(driver,args,project,manifest,sdk):
    value=profile(args)
    available=["lua", *driver.sdk_shipped_script_backends(sdk)]
    admit(manifest,[],value,available)
    # Authoring compiles SDK consumers; it is not a shipment admission.
    # Embedded delivery still resolves and validates the complete selected graph.
    if value == 'authoring':
        return admit(manifest, [], value, available)
    addresses=[]
    typed_assets=[]
    startup=manifest.get('startup_scene')
    if startup:
        resources=project/(manifest.get('source_assets') or 'resources')
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
        driver.run([str(tool),'--resource-root',str(resources.resolve()),'--scene',str(scene),
                    '--out',str(output),'--single-file','--target','auto'],cwd=driver.ROOT)
        if not driver.DRY_RUN:
            graph=json.loads((output/'bundle_graph.json').read_text(encoding='utf-8'))
            addresses=[entry['address'] for entry in graph['assets']]
            catalog=json.loads((output/'bundle_catalog.json').read_text(encoding='utf-8'))
            selected=set(addresses)
            for record in catalog['bundles'].values():
                sidecar=(output/(record['file']+'.json')).resolve()
                if not sidecar.is_relative_to(output.resolve()):
                    raise ValueError('bundle sidecar outside admission output')
                metadata=json.loads(sidecar.read_text(encoding='utf-8'))
                typed_assets.extend(asset for asset in metadata['assets']
                                    if asset['path'] in selected)
    return admit(manifest,addresses,value,available,typed_assets)


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
        return audit_html(artifact)
    if driver.HOST!='windows':
        return {'status':'UNVERIFIED','reason':'native system-dependency and relocation audit not implemented for this host'}
    return driver._standalone_module().verify(Path(artifact))
