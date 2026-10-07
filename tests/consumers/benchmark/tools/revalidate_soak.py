"""Strict offline revalidation of saved soak evidence; never launches an executable."""
import argparse
import json
from pathlib import Path
import re
from gbuffer_asset_suite import sha256_file, write
from tools.perf_bench import read_execution_log
from tools.benchmark_suite import read_frames, frame_summary, simulation_started, player_counters, capture_info, visual_delta_files, GAME_SIZE
from tools.jce_determinism import assert_backend


def validate_execution(intent, process, executable_hash, log, seconds):
    if intent.get("executable_sha256")!=executable_hash:raise ValueError("executable identity mismatch")
    if intent.get("host")!="player" or intent.get("kind")!="entity" or intent.get("count")!=1000 or intent.get("profile")!="soak":
        raise ValueError("unsupported saved workload")
    if process.get("returncode")!=0 or process.get("timed_out") is not False or process.get("elapsed_s",0)<seconds:
        raise ValueError("abnormal or early saved execution")
    if "] ERROR -" in log or "JCE_MAX_SECONDS reached" not in log or not simulation_started(log,"player"):
        raise ValueError("missing timed-limit/Play or engine ERROR")
    actual=assert_backend(log,"d3d11")
    counts=re.findall(r"benchmark-spawn: kind=(\d+) requested=(\d+) spawned=(\d+)\b",log)
    if counts!=[("3","1000","1000")]:raise ValueError("saved actual spawn mismatch")
    return actual


def validate_samples(samples, control):
    ids=samples["frame_indices"]
    if not ids or ids[0]!=0 or any(b!=a+1 for a,b in zip(ids,ids[1:])):raise ValueError("CSV frame continuity mismatch")
    summary=frame_summary(samples)
    phase=summary["phase_median_ms"].get("ecs_move",0)
    if phase<=control["frame"]["phase_median_ms"].get("ecs_move",0)*1.25+.01:
        raise ValueError("actual ECS work below unchanged negative-control threshold")
    return summary


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("run","control","exe","out"):parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--seconds",type=float,default=1800)
    args=parser.parse_args()
    if args.seconds<1800 or args.out.exists():parser.error("minimum1800seconds and new output path required")
    args.out.mkdir(parents=True)
    result={"status":"INVALID","scope":"offline saved capture, ambient concurrent editor; not a new execution or exclusive performance acceptance",
        "validator_sha256":sha256_file(Path(__file__)),"validator_dependencies_sha256":{n:sha256_file(Path(n)) for n in ("tools/perf_bench.py","tools/benchmark_suite.py","tools/jce_determinism.py")}}
    try:
        intent=json.loads((args.run/"intent.json").read_text());process=json.loads((args.run/"engine_process/result.json").read_text())
        control=json.loads(args.control.read_text());log=read_execution_log(args.run/"engine.log",process)
        actual=validate_execution(intent,process,sha256_file(args.exe),log,args.seconds)
        samples=read_frames(args.run/"samples.csv",warmup=0,sample_count=None)
        summary=validate_samples(samples,control)
        counters=player_counters((args.run/"engine_counters.jsonl").read_text())
        if not counters or any(r["engine"]["entity_count"]<1000 for r in counters):raise ValueError("saved actual entity snapshots unavailable")
        capture=capture_info(args.run/"frame.png")
        if (capture.get("width"),capture.get("height"))!=GAME_SIZE:raise ValueError("capture dimensions mismatch")
        if sha256_file(Path(control["capture"]["file"]))!=control["capture"]["sha256"]:
            raise ValueError("negative-control capture hash changed")
        visual=visual_delta_files(control["capture"]["file"],args.run/"frame.png")
        if visual<.02:raise ValueError("unchanged visual negative-control threshold failed")
        samples_path=args.run/"engine_process/samples.json"
        resources=json.loads(samples_path.read_text()) if samples_path.exists() else None
        result.update(status="REVALIDATED_SAVED_CAPTURE",intent=intent,actual_backend=actual,process=process,frame=summary,
            visual_changed_fraction=visual,resource_samples=resources,resource_trend_status="RAW_RETAINED_NOT_LEAK_CERTIFICATE" if resources else "UNAVAILABLE",
            counter_snapshots=len(counters),executable_sha256=sha256_file(args.exe))
    except Exception as error:
        result["error"]=str(error)
    finally:
        result["raw_sha256"]={str(p.relative_to(args.run)):sha256_file(p) for p in args.run.rglob("*") if p.is_file()}
        result["control_sha256"]=sha256_file(args.control)
        write(args.out/"revalidation.json",result)
    if result["status"]!="REVALIDATED_SAVED_CAPTURE":raise SystemExit(1)
    print(args.out/"revalidation.json")

if __name__=="__main__":main()
