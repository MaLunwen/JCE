"""Minesweeper portable bundle reopen witness; core observation, not full parity."""
import argparse
import json
from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT))
sys.path.insert(0,str(ROOT/"tests/consumers/benchmark/tools"))
from gbuffer_asset_suite import sha256_file,write
from check_asset_pixels import read_png
from tools.perf_bench import run,read_execution_log
from tools.jce_determinism import assert_backend
from tools.benchmark_suite import read_frames,simulation_started


def validate_log(log):
    if "] ERROR -" in log or any(v in log for v in ("load failed","NO NATIVE SCRIPT MODULE","native script module loaded:","could not open","failed to instantiate")):
        raise ValueError("bundle error or native project fallback")
    if "MINES_PORTABLE_BOOT Lua rules + pointer + Canvas" not in log or "MINES_DIRECTOR Lua started" not in log:
        raise ValueError("actual portable Lua/controller director boot unavailable")
    if not simulation_started(log,"editor"):raise ValueError("actual editor Play unavailable")


def validate_scene(value):
    scene=value.get("scene",value)
    if not isinstance(scene,dict) or not isinstance(scene.get("entities"),list):
        raise ValueError("canonical scene.entities unavailable")
    entities=[e for e in scene["entities"] if isinstance(e,dict) and isinstance(e.get("id"),int)
        and isinstance(e.get("components"),list) and e.get("name")=="MinesController"]
    if len(entities)!=1 or not any(c.get("type")=="Script" and c.get("scriptPath")=="scripts/MinesController.lua"
        for c in entities[0]["components"] if isinstance(c,dict)):
        raise ValueError("actual observed scene lacks one genuine portable Lua controller")


def interaction_fixture(quit_control=False):
    # Authored 1280x720 board reference, deterministic first seed1.
    controls=[(30,640,250,1),(60,640,402,1),(90,640,218,3),(120,640,264,2)]
    if quit_control: controls.extend([(180,425,148,1),(210,640,570,1)])
    rows=[]
    for frame,x,y,button in controls:
        rows.append(f"{frame-2} M {x/1280:.9f} {y/720:.9f} 0 0")
        rows.append(f"{frame} B {x/1280:.9f} {y/720:.9f} {button} 1")
        rows.append(f"{frame+2} B {x/1280:.9f} {y/720:.9f} {button} 0")
    return "\n".join(rows)+"\n"


def validate_interaction(log,quit_control=False):
    import re
    if "MINES_PORTABLE_START width=9 height=9 mines=10 seed=1" not in log:
        raise ValueError("actual beginner pointer command unavailable")
    observed=[tuple(map(int,m)) for m in re.findall(r"MINES_PORTABLE_ACTION button=(\d+) index=(\d+) state=(\d+) flags=(\d+) revealed=(\d+)",log)]
    expected=[(1,40,1,0,54),(3,4,1,1,54),(2,13,1,1,55)]
    if observed!=expected:raise ValueError("actual pointer reveal/flag/chord state differs from deterministic fixture")
    admitted=[(int(frame),kind) for frame,kind in re.findall(r"Game event fixture frame=(\d+) type=([MB])",log)]
    expected_events=[(int(row.split()[0]),row.split()[1]) for row in interaction_fixture(quit_control).splitlines()]
    # Accepted quit stops the Game route before the final authored release.
    # This shortened admission is accepted only with independent rollback proof
    # in main(), never as standalone evidence of a successful quit.
    if quit_control:expected_events=expected_events[:-1]
    if admitted!=expected_events:
        raise ValueError("exact public Game event admission sequence unavailable")
    return {"status":"SYNTHETIC_POINTER_GAMEPLAY_OBSERVED","actions":observed,
        "scope":"injected platform-route events; not hardware input latency; deterministic seed1 core gameplay only"}


def validate_quit(log,before,during,after):
    if log.count('MINES_PORTABLE_QUIT requested')!=1:
        raise ValueError('actual accepted quit request missing or repeated')
    def widgets(value):
        scene=value.get('scene',value)
        return {e['name']:e['components'] for e in scene['entities']
            if e.get('name','').startswith('Widget')}
    a,b,c=widgets(before),widgets(during),widgets(after)
    if not a or a==b or a!=c:
        raise ValueError('actual authored widget rollback after quit unavailable')
    return {'status':'QUIT_REQUEST_AND_AUTHORED_UI_ROLLBACK_OBSERVED',
        'scope':'actual accepted request plus scene restoration; editor continues, not stock Player process exit'}


def validate_background(image, expected=(24,45,64)):
    width,height,channels,pixels=image
    if channels<3 or width<160 or height<160:raise ValueError("game capture too small")
    regions=[(20,20),(width-40,20),(20,height-40),(width-40,height-40)]
    counts=[]
    for x,y in regions:
        matching=0
        for dy in range(20):
            for dx in range(20):
                offset=((y+dy)*width+x+dx)*channels
                if all(abs(pixels[offset+c]-expected[c])<=6 for c in range(3)):matching+=1
        counts.append(matching)
    if min(counts)<380:raise ValueError("rendered Game background corner ROI mismatch")
    return {"status":"GAME_BACKGROUND_ROI_OBSERVED","expected_rgb":expected,"roi_matching_pixels":counts,
        "scope":"four bounded Game-image corner regions, not whole-window theme or full visual parity"}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("exe","bundle","out"):parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--backend",choices=("d3d11","d3d12","vulkan","gl"),default="d3d11")
    parser.add_argument("--interaction",action="store_true",help="inject deterministic public Game pointer events")
    parser.add_argument("--quit-control",action="store_true",help="after pointer gameplay click menu then exit and verify authored UI rollback")
    parser.add_argument("--timeout",type=float,default=120)
    args=parser.parse_args()
    if args.quit_control and not args.interaction:parser.error("--quit-control requires --interaction")
    if not args.exe.is_file() or not args.bundle.is_file() or not 10<=args.timeout<=600:parser.error("existing inputs/bounded timeout required")
    args.out=args.out.resolve();args.out.mkdir(parents=True,exist_ok=False);(args.out/"profile").mkdir()
    write(args.out/"profile/editor-preferences.json", {"startup_mode":1})
    hashes={"executable":sha256_file(args.exe),"bundle":sha256_file(args.bundle)}
    env={"JCE_SCENE":str(args.bundle.resolve()),"JCE_BACKEND":args.backend,"JCE_WINDOW_HIDDEN":"1","JCE_MULTI_INSTANCE":"1",
        "JCE_EDITOR_PROFILE_DIR":str(args.out/"profile"),"JCE_FRAME_DT_FIXED":"0.016666666667",
        "JCE_KPI_GAME_SHOTS":"1.0|"+str(args.out/"game.png"),"JCE_KPI_GAME_CAPTURE_SIZE":"1280x720","JCE_DBG_AUTOPLAY":"20","JCE_DBG_FOCUS_GAME":"1",
        "JCE_WINCAP_FRAME":"150","JCE_WINCAP_PATH":str(args.out/"editor.png"),
        "JCE_EDITOR_OPERATION_METRICS":"1","JCE_DBG_SCENE_OBSERVE_DIR":str(args.out),"JCE_DBG_SCENE_OBSERVE_AT":"150"}
    if args.interaction:
        event_path=args.out/"pointer-events.txt"
        event_path.write_text(interaction_fixture(args.quit_control),encoding="utf-8",newline="\n")
        env["JCE_KPI_GAME_EVENTS"]=str(event_path)
    if args.quit_control:env["JCE_DBG_SCENE_OBSERVE_AT"]="10,150,350"
    result={"status":"INCOMPLETE","environment":env,"input_sha256":hashes,"scope":"core Lua bundle reopen and Play witness; not full feature parity or input/gameplay verification",
        "validator_sha256":sha256_file(Path(__file__))}
    write(args.out/"intent.json",result)
    try:
        process=run(env,args.out/"engine.log",exe=args.exe.resolve(),frames=400,timeout=args.timeout,samples_path=args.out/"frames.csv")
        if process["process"]["returncode"]!=0 or process["process"]["timed_out"]:raise ValueError("abnormal editor exit")
        log=read_execution_log(args.out/"engine.log",process["process"]);actual=assert_backend(log,args.backend)
        if actual!=process["backend_actual"] or hashes!={"executable":sha256_file(args.exe),"bundle":sha256_file(args.bundle)}:raise ValueError("identity/backend changed")
        validate_log(log);validate_scene(json.loads((args.out/"scene-150.json").read_text()))
        image=read_png(args.out/"editor.png")
        if min(image[0],image[1])<1 or len(image[3])!=image[0]*image[1]*image[2]:raise ValueError("incomplete PNG")
        interaction=validate_interaction(log,args.quit_control) if args.interaction else None
        background=validate_background(read_png(args.out/"game.png"))
        quit_result=validate_quit(log,*[json.loads((args.out/f"scene-{f}.json").read_text()) for f in (10,150,350)]) if args.quit_control else None
        frames=read_frames(args.out/"frames.csv",warmup=0,sample_count=None)
        if len(frames["frame_ms"])<399:raise ValueError("incomplete400frame capture")
        result.update(status="PORTABLE_CORE_REOPEN_OBSERVED",process=process,actual_backend=actual,complete_frames=len(frames["frame_ms"]),background=background,interaction=interaction,quit_control=quit_result)
    except Exception as error:
        result.update(status="FAIL",error=str(error));raise
    finally:
        result["raw_sha256"]={str(p.relative_to(args.out)):sha256_file(p) for p in args.out.rglob("*") if p.is_file() and p.name!="result.json"}
        write(args.out/"result.json",result)

if __name__=="__main__":main()
