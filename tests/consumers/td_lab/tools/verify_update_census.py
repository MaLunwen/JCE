"""Actual SDK-native TD update-census controls. No browser/GPU observation."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

SUMMARY=re.compile(r'^TD_CENSUS (PASS|FAIL) cases=(\d+) checks=(\d+) failures=(\d+) scope=actual_runtime_native_c_update_census_headless$',re.MULTILINE)
FAILURES={'missing_update':'census_initial_consumption','missing_entry':'census_initial_consumption',
          'missing_return':'census_initial_consumption','reset':'census_instance_generation','overflow':'census_counter_overflow'}


def run(path,variant):
    path=path.resolve(strict=True);before=path.read_bytes()
    env={key:value for key,value in os.environ.items() if not key.startswith(('JCE_','TD_'))}
    result=subprocess.run([str(path)],cwd=path.parent,env=env,capture_output=True,timeout=60,check=False)
    if path.read_bytes()!=before:raise ValueError('Actual census executable changed')
    raw=result.stdout+result.stderr;output=raw.decode('utf-8').replace('\r\n','\n')
    summaries=list(SUMMARY.finditer(output));expected=0 if variant=='normal' else 1
    if result.returncode!=expected or len(summaries)!=1 or 'SETUP_FAILED' in output:
        raise ValueError('Census owner did not finish its ordinary outcome:\n'+output)
    status,cases,checks,failures=summaries[0].groups()
    failed=re.findall(r'^TD_CLOCK FAIL case=(\S+) line=',output,re.MULTILINE)
    if int(cases)!=8 or int(checks)<12 or int(failures)!=len(failed):raise ValueError('Incomplete actual census owner cases')
    if variant=='normal':
        if status!='PASS' or int(failures)!=0:raise ValueError('Actual census owner failed')
    elif status!='FAIL' or FAILURES[variant] not in failed:raise ValueError('Whole-owner negative missed its ordinary assertion')
    return {'variant':variant,'executable':str(path),'artifact_sha256':hashlib.sha256(before).hexdigest(),
            'bytes':len(before),'exit_code':result.returncode,'cases':int(cases),'checks':int(checks),
            'failures':int(failures),'failed_cases':failed,'output_sha256':hashlib.sha256(raw).hexdigest()},raw


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for variant in ['normal',*FAILURES]:parser.add_argument('--'+variant.replace('_','-'),type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    if args.out.exists():parser.error('--out must be a fresh evidence directory')
    rows=[];outputs=[]
    for variant in ['normal',*FAILURES]:
        row,raw=run(getattr(args,variant),variant);rows.append(row);outputs.append(raw)
    if len({row['artifact_sha256'] for row in rows})!=len(rows):raise ValueError('Whole-owner controls are identical artifacts')
    args.out.mkdir(parents=True)
    for row,raw in zip(rows,outputs):(args.out/(row['variant']+'.log')).write_bytes(raw)
    report={'schema':1,'status':'ACTUAL_NATIVE_CENSUS_CONTROLS_PASSED','runs':rows,
            'scope':'Actual runtime/native C callback consumption and lifecycle; no Web export, browser work, pixels or GPU completion proof'}
    (args.out/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps(report,separators=(',',':')))


if __name__=='__main__':main()
