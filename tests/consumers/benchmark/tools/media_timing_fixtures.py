"""Generate first-party synthetic clock/motion media; not complex-content throughput."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess

from culling_suite import sha256_file

CODECS = {
    "h264": ["-c:v", "libx264", "-preset", "ultrafast", "-crf", "23"],
    "h265": ["-c:v", "libx265", "-preset", "ultrafast", "-crf", "23", "-x265-params", "pools=1:frame-threads=1"],
    "av1": ["-c:v", "libaom-av1", "-cpu-used", "8", "-crf", "30", "-row-mt", "1"],
}


def validate_dimensions(width, height, fps, duration):
    if (width % 2 or height % 2 or not 2 <= width <= 4096 or not 2 <= height <= 4096
            or not math.isfinite(fps) or not 1 <= fps <= 120
            or not math.isfinite(duration) or not 1 <= duration <= 60):
        raise ValueError("even dimensions 2..4096, fps 1..120 and duration 1..60 required")


def commands(ffmpeg, ffprobe, output, codec, width, height, fps, duration, font=None):
    validate_dimensions(width, height, fps, duration)
    video = f"testsrc2=size={width}x{height}:rate={fps}:duration={duration}"
    encode = [str(ffmpeg), "-nostdin", "-hide_banner", "-v", "error", "-f", "lavfi", "-i", video,
        "-f", "lavfi", "-i", f"sine=frequency=440:sample_rate=48000:duration={duration}"]
    if font is not None:
        # Relative fixed alias avoids drive-colon/filter escaping and platform paths.
        encode += ["-vf", r"drawtext=fontfile=clock-font.ttf:text='frame %{n} time %{pts\:hms}':x=20:y=20:fontsize=40:fontcolor=white:box=1:boxcolor=black"]
    encode += ["-pix_fmt", "yuv420p", "-threads", "2", *CODECS[codec], "-c:a", "aac",
        "-b:a", "128k", "-shortest", "-movflags", "+faststart", str(output)]
    probe = [str(ffprobe), "-v", "error", "-show_streams", "-show_format", "-of", "json", str(output)]
    return encode, probe


def validate_metadata(data, codec, width, height, fps, duration):
    video = [s for s in data.get("streams", []) if s.get("codec_type") == "video"]
    audio = [s for s in data.get("streams", []) if s.get("codec_type") == "audio"]
    if len(video) != 1 or len(audio) != 1:
        raise ValueError("one actual video and audio stream required")
    v = video[0]; numerator, denominator = map(float, v["avg_frame_rate"].split("/"))
    actual_fps = numerator / denominator
    actual_duration = float(data["format"]["duration"])
    if (v.get("codec_name") != {"h264":"h264", "h265":"hevc", "av1":"av1"}[codec]
            or v.get("width") != width or v.get("height") != height
            or not math.isfinite(actual_fps) or abs(actual_fps-fps) > .001
            or not math.isfinite(actual_duration) or abs(actual_duration-duration) > 1/fps+.1
            or audio[0].get("codec_name") != "aac" or audio[0].get("sample_rate") != "48000"):
        raise ValueError("actual metadata differs from authored fixture")
    return {"fps":actual_fps, "duration_s":actual_duration, "video":v, "audio":audio[0]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("ffmpeg", "ffprobe", "out"):
        parser.add_argument("--"+name, type=Path, required=True)
    parser.add_argument("--duration", type=float, default=10)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--fps", type=float, default=60)
    parser.add_argument("--codec", choices=tuple(CODECS), action="append")
    parser.add_argument("--clock-font", type=Path)
    parser.add_argument("--timeout", type=float, default=1800)
    args = parser.parse_args()
    validate_dimensions(args.width,args.height,args.fps,args.duration)
    if not args.ffmpeg.is_file() or not args.ffprobe.is_file() or args.timeout<=0 or not math.isfinite(args.timeout):
        parser.error("existing reference executables and finite positive timeout required")
    if args.clock_font and not args.clock_font.is_file(): parser.error("explicit clock font missing")
    args.out = args.out.resolve(); args.out.mkdir(parents=True,exist_ok=False)
    inputs = {"ffmpeg":sha256_file(args.ffmpeg),"ffprobe":sha256_file(args.ffprobe)}
    if args.clock_font:
        font = args.out/"clock-font.ttf"; font.write_bytes(args.clock_font.read_bytes())
        inputs["font"] = sha256_file(font)
    manifest = {"status":"INCOMPLETE", "scope":"first-party testsrc2 changing frame/time pattern and 440 Hz sine; synthetic content, not complex real 4K throughput",
        "clock_overlay":bool(args.clock_font), "fallback":"testsrc2 animated temporal pattern" if not args.clock_font else None,
        "input_paths":{"ffmpeg":str(args.ffmpeg.resolve()),"ffprobe":str(args.ffprobe.resolve()),"font":str(args.clock_font.resolve()) if args.clock_font else None},
        "input_sha256":inputs,"files":[]}
    try:
        for codec in args.codec or CODECS:
            output = args.out/f"{codec}-{args.width}x{args.height}-{args.fps:g}fps.mp4"
            encode,probe = commands(args.ffmpeg.resolve(),args.ffprobe.resolve(),output,codec,args.width,args.height,args.fps,args.duration,args.clock_font)
            entry={"codec":codec,"encode_command":encode,"probe_command":probe,
                "commands_sha256":hashlib.sha256(json.dumps([encode,probe],ensure_ascii=True).encode()).hexdigest()}
            manifest["files"].append(entry)
            process = subprocess.run(encode,cwd=args.out,capture_output=True,text=True,timeout=args.timeout)
            log=args.out/(codec+"-encode.log");log.write_text(process.stderr,encoding="utf-8",newline="\n")
            entry.update(returncode=process.returncode,log_sha256=sha256_file(log))
            if process.returncode: raise ValueError("encoding failed: "+codec)
            process = subprocess.run(probe,capture_output=True,text=True,timeout=30)
            metadata=args.out/(codec+"-metadata.json");metadata.write_text(process.stdout,encoding="utf-8",newline="\n")
            if process.returncode: raise ValueError("probe failed: "+codec)
            entry.update(actual=validate_metadata(json.loads(process.stdout),codec,args.width,args.height,args.fps,args.duration),
                path=str(output),sha256=sha256_file(output),metadata_sha256=sha256_file(metadata))
        if inputs["ffmpeg"]!=sha256_file(args.ffmpeg) or inputs["ffprobe"]!=sha256_file(args.ffprobe):
            raise ValueError("reference executable changed")
        manifest["status"]="GENERATED_AND_METADATA_VERIFIED"
    except Exception as error:
        manifest.update(status="FAIL",error=str(error));raise
    finally:
        (args.out/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8",newline="\n")
    print(args.out/"manifest.json")


if __name__ == "__main__": main()
