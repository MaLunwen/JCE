"""CPU-only canonical RP staging controls using an explicit installed SDK packer."""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/assets'))
from shader_stock import xxh32


def digest(path):
    raw=path.read_bytes()
    return {'path':str(path.resolve()),'sha256':hashlib.sha256(raw).hexdigest(),'bytes':len(raw)}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('packer','source','canonical','out'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    for path in (args.packer,args.source,args.canonical):
        if not path.is_file():parser.error('Explicit input missing: '+str(path))
    out=args.out.resolve()
    if out.exists() and any(out.iterdir()):parser.error('Evidence output must be new or empty')
    out.mkdir(parents=True,exist_ok=True)
    source_bytes=args.source.read_bytes();old_bytes=args.canonical.read_bytes()
    if source_bytes==old_bytes:parser.error('Negative control requires distinct authored and stale canonical input bytes')
    spec=importlib.util.spec_from_file_location('canonical_rp_probe_driver',ROOT/'tools/build/jce.py')
    driver=importlib.util.module_from_spec(spec);spec.loader.exec_module(driver)
    project=out/'fixture';cooked=project/'resources/_cooked';boot=project/'boot'
    source=project/'Settings/RenderPipeline.rp.json';canonical=cooked/'settings/render_pipeline.rp.json'
    source.parent.mkdir(parents=True);canonical.parent.mkdir(parents=True);boot.mkdir(parents=True)
    source.write_bytes(source_bytes);canonical.write_bytes(old_bytes)
    user=cooked/'unrelated-user-data.bin';user.write_bytes(b'Independent cooked user data must remain unchanged')
    report={'schema':1,'scope':'Actual SDK archive packing/CRC only over isolated copies; no application, GPU, SDK modification or user asset edit',
            'packer':digest(args.packer),'source':digest(args.source),'before_canonical':digest(args.canonical),'cases':[]}
    def pack(label,expected):
        archive=out/(label+'.pak');bom=out/(label+'-bom.json')
        command=[str(args.packer.resolve()),'--resource-dir',str(cooked),'--resource-dir',str(boot),
                 '--pak-file',str(archive),'--header-file',str(out/(label+'.h')),'--manifest-file',str(out/(label+'.cmake')),
                 '--obj-format','none','--level','1','--json',str(bom)]
        run=subprocess.run(command,capture_output=True)
        (out/(label+'-stdout.log')).write_bytes(run.stdout);(out/(label+'-stderr.log')).write_bytes(run.stderr)
        if run.returncode:raise ValueError('Actual packer failed: '+label+'; retained original logs')
        data=json.loads(bom.read_text(encoding='utf-8-sig'))
        entries=[entry for entry in data['entries'] if entry.get('path')=='settings/render_pipeline.rp.json']
        if len(entries)!=1:raise ValueError('Canonical runtime key is absent/repeated: '+label)
        entry=entries[0];crc=f'0x{xxh32(expected):08x}'
        if entry['original_size']!=len(expected) or entry['content_crc']!=crc or entry['verified'] is not True:
            raise ValueError('Actual packed canonical content differs: '+label)
        if canonical.read_bytes()!=expected or (boot/'settings/render_pipeline.rp.json').exists() or user.read_bytes()!=b'Independent cooked user data must remain unchanged':
            raise ValueError('Stage ownership/data preservation failed: '+label)
        report['cases'].append({'case':label,'canonical_sha256':hashlib.sha256(expected).hexdigest(),'content_crc':crc,
                                'archive':digest(archive),'bom':digest(bom),'verified':entry['verified'],'original_size':entry['original_size']})
    pack('unstaged-negative',old_bytes)
    driver._stage_runtime_boot_settings(project,cooked,boot);pack('staged-source',source_bytes)
    before_time=canonical.stat().st_mtime_ns
    driver._stage_runtime_boot_settings(project,cooked,boot)
    if canonical.stat().st_mtime_ns!=before_time:raise ValueError('Unchanged stage rewrote canonical')
    pack('repeat-unchanged',source_bytes)
    modified=json.loads(source_bytes.decode('utf-8-sig'));modified['post_quality']='high'
    changed=(json.dumps(modified,sort_keys=True)+'\n').encode('utf-8');source.write_bytes(changed)
    driver._stage_runtime_boot_settings(project,cooked,boot);pack('changed-root',changed)
    source.unlink();driver._stage_runtime_boot_settings(project,cooked,boot);pack('root-absent-preserves-canonical',changed)
    fresh=out/'fresh';fresh_source=fresh/'Settings/RenderPipeline.rp.json';fresh_source.parent.mkdir(parents=True);fresh_source.write_bytes(source_bytes)
    driver._stage_runtime_boot_settings(fresh,fresh/'cooked',fresh/'boot')
    if (fresh/'cooked/settings/render_pipeline.rp.json').read_bytes()!=source_bytes:raise ValueError('Fresh canonical stage failed')
    report['fresh_canonical']=digest(fresh/'cooked/settings/render_pipeline.rp.json')
    report['status']='ACTUAL_CANONICAL_RP_ARCHIVE_CONTROLS_PASSED'
    target=out/'result.json';target.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(report['status'],len(report['cases']),str(target))
    for row in report['cases']:print(row['case'],row['original_size'],row['content_crc'])


if __name__=='__main__':main()
