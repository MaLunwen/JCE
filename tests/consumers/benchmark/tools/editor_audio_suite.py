"""Actual paused audio-viewer waveform control; functional ambient evidence only."""
import argparse
import math
from pathlib import Path
import re
from editor_media_suite import sha256_file, write, read_png, source_identity, read_frames, assert_backend, run

AUDIO = re.compile(r"audio trace playing=(0|1) time=(\S+) duration=(\S+) waveform=(\S+) bins=(\d+) frame_ms=(\S+)")


def validate_paused(log):
    if "] ERROR -" in log or "load failed" in log: raise ValueError("audio viewer failure")
    rows=[]
    for line in log.splitlines():
        match=AUDIO.search(line)
        if "audio trace " in line and not match: raise ValueError("malformed actual audio trace")
        if not match: continue
        playing=int(match[1]);clock,duration,progress=map(float,match.groups()[1:4]);bins=int(match[5]);frame=float(match[6])
        if (playing!=0 or not all(math.isfinite(v) for v in (clock,duration,progress,frame))
            or abs(clock)>.0005 or duration<=0 or not 0<=progress<=1 or bins>2048 or frame<0):
            raise ValueError("invalid or playing paused-audio record")
        rows.append({"playing":playing,"time":clock,"duration":duration,"waveform":progress,"bins":bins,"frame_ms":frame})
    if len(rows)<2 or any(abs(r["duration"]-rows[0]["duration"])>.001 for r in rows):
        raise ValueError("complete same-instance paused audio samples required")
    if any(b["waveform"]+.001<a["waveform"] for a,b in zip(rows,rows[1:])):
        raise ValueError("waveform progress moved backwards")
    if rows[-1]["waveform"]!=1 or rows[-1]["bins"]<=0:
        raise ValueError("actual waveform completion unavailable")
    return {"status":"PAUSED_AUDIO_WAVEFORM_OBSERVED","samples":len(rows),"first":rows[0],"last":rows[-1],
        "scope":"actual audio-file waveform bins and paused clock; no audible playback, pixel amplitude oracle or formal CPU percentiles"}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("exe","file","out"):parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--paused",action="store_true",help="paused only; default is also paused")
    parser.add_argument("--seconds",type=float,default=25)
    parser.add_argument("--timeout",type=float,default=120)
    parser.add_argument("--backend",choices=("d3d11","d3d12","vulkan","gl"),default="d3d11")
    args=parser.parse_args()
    if not args.exe.is_file() or not args.file.is_file() or not 5<=args.seconds<=300 or not args.seconds+10<=args.timeout<=600:
        parser.error("existing inputs and bounded duration/timeout required")
    args.out=args.out.resolve();args.out.mkdir(parents=True,exist_ok=False);profile=args.out/"profile";profile.mkdir()
    hashes={"executable":sha256_file(args.exe),"audio":sha256_file(args.file)}
    env={"JCE_BACKEND":args.backend,"JCE_MULTI_INSTANCE":"1","JCE_WINDOW_HIDDEN":"1",
        "JCE_EDITOR_PROFILE_DIR":str(profile),"JCE_DBG_FILE_PREVIEW":str(args.file.resolve()),
        "JCE_DBG_FILE_PREVIEW_PAUSED":"1","JCE_AUDIO_TRACE":"1",
        "JCE_WINCAP_FRAME":"150","JCE_WINCAP_PATH":str(args.out/"editor.png")}
    result={"status":"INCOMPLETE","source":source_identity(),"environment":env,
        "input":{"executable":str(args.exe.resolve()),"audio":str(args.file.resolve()),"sha256":hashes},
        "scope":"hidden actual paused viewer, no sound requested; ambient resource observations"}
    write(args.out/"intent.json",result)
    try:
        process=run(env,args.out/"engine.log",frames=100000,max_seconds=args.seconds,timeout=args.timeout,
            exe=args.exe.resolve(),samples_path=args.out/"frames.csv",sample_interval=.1)
        if process["process"]["returncode"]!=0 or process["process"]["timed_out"]:raise ValueError("abnormal editor exit")
        log=(args.out/"engine.log").read_text(encoding="utf-8");actual=assert_backend(log,args.backend)
        if actual!=process["backend_actual"] or hashes!={"executable":sha256_file(args.exe),"audio":sha256_file(args.file)}:
            raise ValueError("backend/input identity changed")
        image=read_png(args.out/"editor.png")
        if min(image[0],image[1])<1 or len(image[3])!=image[0]*image[1]*image[2]:raise ValueError("incomplete capture")
        frames=read_frames(args.out/"frames.csv",warmup=0,sample_count=None)
        result.update(status="FUNCTIONAL_OBSERVATIONS",process=process,actual_backend=actual,
            waveform=validate_paused(log),completed_frames=len(frames["frame_ms"]))
    except Exception as error:
        result.update(status="FAIL",error=str(error));raise
    finally:
        result["raw_sha256"]={str(p.relative_to(args.out)):sha256_file(p) for p in args.out.rglob("*") if p.is_file() and p.name!="result.json"}
        write(args.out/"result.json",result)

if __name__=="__main__":main()
