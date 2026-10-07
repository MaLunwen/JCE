"""Generate small synthetic SDR color controls with an explicit FFmpeg input."""
import argparse
import json
from pathlib import Path
import subprocess

from culling_suite import sha256_file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--size", default="66x50", help="explicit synthetic picture dimensions")
    parser.add_argument("--codec", choices=("av1", "vp9", "hevc"), action="append")
    args = parser.parse_args()
    if not args.ffmpeg.is_file():
        parser.error("existing FFmpeg reference executable required")
    try:
        width, height = map(int, args.size.split("x"))
    except ValueError:
        parser.error("picture dimensions must be WIDTHxHEIGHT")
    if not 2 <= width <= 4096 or not 2 <= height <= 4096:
        parser.error("picture dimensions must be within 2..4096")
    args.out.mkdir(parents=True, exist_ok=False)
    manifest = {"ffmpeg": str(args.ffmpeg.resolve()), "ffmpeg_sha256": sha256_file(args.ffmpeg),
                "scope": "first-party synthetic color bars; no user or licensed assets", "files": []}
    for codec, extension, options in (
        ("av1", "mp4", ["-c:v", "libaom-av1", "-cpu-used", "8", "-crf", "20"]),
        ("vp9", "webm", ["-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "8", "-lossless", "1"]),
        ("hevc", "mp4", ["-c:v", "libx265", "-preset", "ultrafast", "-x265-params", "pools=1:frame-threads=1:lossless=1"])):
        if args.codec and codec not in args.codec:
            continue
        for matrix in ("bt470bg", "bt709", "bt2020nc"):
            for signal_range in ("tv", "pc"):
                path = args.out / ("%s-%s-%s.%s" % (codec, matrix, signal_range, extension))
                command = [str(args.ffmpeg.resolve()), "-nostdin", "-hide_banner", "-v", "error",
                           "-f", "lavfi", "-i", "smptebars=size=%ux%u:rate=30:duration=2" % (width, height),
                           "-an", "-pix_fmt", "yuv420p", "-threads", "2", "-colorspace", matrix,
                           "-color_range", signal_range, *options, str(path.resolve())]
                process = subprocess.run(command, capture_output=True, text=True, timeout=60)
                (args.out / (path.name + ".log")).write_text(process.stderr, encoding="utf-8", newline="\n")
                if process.returncode:
                    raise ValueError("fixture encoder failed: " + path.name)
                manifest["files"].append({"file": str(path.resolve()), "sha256": sha256_file(path),
                                          "matrix": matrix, "range": signal_range, "codec": codec,
                                          "command": command})
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(args.out / "manifest.json")


if __name__ == "__main__":
    main()
