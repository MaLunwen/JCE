"""Independent seek-picture comparison using retained raw RGBA and FFmpeg PTS."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import uuid

from culling_suite import sha256_file, source_identity
from tools.process_sampler import run_process

PICTURE = re.compile(r"probe seek picture=(\d+)x(\d+) pts=([\d.]+) bytes=(\d+)")


def reference_yuv420(data, width, height, metadata):
    """Independent floating-point inverse with explicit nearest chroma policy."""
    import numpy as np
    if width <= 0 or height <= 0 or metadata.get("pix_fmt") != "yuv420p":
        raise ValueError("nearest oracle requires decoded 8-bit planar YUV420")
    matrix = metadata.get("color_space")
    if matrix not in (None, "unknown", "bt470bg", "smpte170m", "bt709", "bt2020nc"):
        raise ValueError("unsupported reference matrix")
    signal_range = metadata.get("color_range")
    if signal_range not in (None, "unknown", "tv", "pc"):
        raise ValueError("unsupported reference signal range")
    kr, kb = {"bt709": (0.2126, 0.0722), "bt2020nc": (0.2627, 0.0593)}.get(matrix, (0.299, 0.114))
    uw, uh = (width + 1) // 2, (height + 1) // 2
    size, chroma_size = width * height, uw * uh
    if len(data) != size + 2 * chroma_size:
        raise ValueError("incomplete independently decoded YUV planes")
    planes = np.frombuffer(data, np.uint8)
    y = planes[:size].reshape(height, width)
    u = planes[size:size + chroma_size].reshape(uh, uw)
    v = planes[size + chroma_size:].reshape(uh, uw)
    rgba = np.full((height, width, 4), 255, dtype=np.uint8)
    # Bound temporary floating-point storage even for large reference pictures.
    full = signal_range == "pc"
    columns = np.arange(width) // 2
    for start in range(0, height, 32):
        stop = min(start + 32, height)
        rows = np.arange(start, stop) // 2
        yy = (y[start:stop].astype(np.float64) - (0 if full else 16)) * (1 if full else 255 / 219)
        uu = (u[rows[:, None], columns].astype(np.float64) - 128) * (1 if full else 255 / 224)
        vv = (v[rows[:, None], columns].astype(np.float64) - 128) * (1 if full else 255 / 224)
        red = yy + 2 * (1 - kr) * vv
        blue = yy + 2 * (1 - kb) * uu
        green = (yy - kr * red - kb * blue) / (1 - kr - kb)
        for channel, values in enumerate((red, green, blue)):
            rgba[start:stop, :, channel] = np.floor(values + 0.5).clip(0, 255).astype(np.uint8)
    return rgba


def compare_rgba(native, reference):
    """Keep the opaque-alpha guard and RGB budget independent of decoding."""
    import numpy as np
    if native.shape != reference.shape or native.ndim != 3 or native.shape[2] != 4:
        raise ValueError("RGBA dimensions disagree")
    if np.any(native[:, :, 3] != 255) or np.any(reference[:, :, 3] != 255):
        raise ValueError("video conversion produced non-opaque alpha")
    delta = np.abs(native[:, :, :3].astype(np.int16) - reference[:, :, :3].astype(np.int16))
    return {"changed_fraction": float((np.max(delta, axis=2) > 5).mean()),
            "mean_rgb_error": float(delta.mean()), "max_rgb_error": int(delta.max())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--file", type=Path, action="append", required=True)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--ffprobe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--audio-device", action="store_true", help="consume embedded PCM through a muted device")
    parser.add_argument("--oracle", choices=("ffmpeg", "nearest-yuv"), default="ffmpeg",
                        help="declare chroma reconstruction; no implicit tolerance changes")
    args = parser.parse_args()
    if any(not path.is_file() for path in (args.exe, args.ffmpeg, args.ffprobe, *args.file)):
        parser.error("existing native probe, reference tools and media required")
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True, exist_ok=False)
    result = {"status": "FAILED", "source": source_identity(), "runs": [], "oracle": args.oracle,
              "executables": {str(path.resolve()): sha256_file(path) for path in (args.exe, args.ffmpeg, args.ffprobe)},
              "scope": "separate raw CPU seek-picture witness; not playback/timing/GPU evidence",
              "limitations": ["one seek picture per clip; no exhaustive motion or editor presentation",
                              "RGB differences may reflect color conversion; decoded PTS/content must be assessed separately"]}
    base = {key: value for key, value in os.environ.items() if not key.startswith("JCE_")}
    import numpy as np
    from PIL import Image
    for index, file in enumerate(args.file):
        directory = root / ("%02u" % index)
        directory.mkdir()
        raw = directory / "native.rgba"
        entry = {"file": str(file.resolve()), "asset_sha256": sha256_file(file), "status": "FAILED"}
        env = dict(base, JCE_TEST_VIDEO_PROBE=str(file.resolve()), JCE_TEST_VIDEO_SECONDS="5",
                   JCE_TEST_VIDEO_SEEK_RGBA=str(raw))
        if args.audio_device:
            env.update(JCE_TEST_VIDEO_AUDIO_DEVICE="1", JCE_TEST_VIDEO_AUDIO_MUTE="1")
        try:
            process = run_process([str(args.exe.resolve())], env=env, cwd=str(root),
                                  timeout=45, output_dir=directory / "native")
            entry["process"] = process
            picture = PICTURE.search(process["stdout"])
            if process["returncode"] != 0 or process["timed_out"] or not picture:
                raise ValueError("native picture was not published and retained")
            width, height, pts, size = int(picture[1]), int(picture[2]), float(picture[3]), int(picture[4])
            if size != width * height * 4 or raw.stat().st_size != size:
                raise ValueError("native picture dimensions/bytes disagree")
            command = [str(args.ffprobe.resolve()), "-v", "error", "-select_streams", "v:0",
                       "-show_frames", "-show_entries",
                       "frame=best_effort_timestamp_time,width,height,pix_fmt,color_range,color_space,color_primaries,color_transfer",
                       "-of", "json", str(file.resolve())]
            frames = subprocess.run(command, capture_output=True, text=True, check=True, timeout=180)
            (directory / "reference-frames.json").write_text(frames.stdout, encoding="utf-8", newline="\n")
            entries = json.loads(frames.stdout)["frames"]
            origin = float(entries[0]["best_effort_timestamp_time"])
            selected = min(range(len(entries)), key=lambda i: abs(float(entries[i]["best_effort_timestamp_time"]) - origin - pts))
            reference_pts = float(entries[selected]["best_effort_timestamp_time"]) - origin
            if abs(reference_pts - pts) > 0.00001 or int(entries[selected]["width"]) != width or int(entries[selected]["height"]) != height:
                raise ValueError("native PTS has no matching independently decoded reference frame")
            reference = directory / "reference.rgba"
            decoded = directory / "reference.yuv" if args.oracle == "nearest-yuv" else reference
            command = [str(args.ffmpeg.resolve()), "-nostdin", "-v", "error", "-threads", "4",
                       "-i", str(file.resolve()), "-vf", "select=eq(n\\,%u)" % selected,
                       "-frames:v", "1", "-vsync", "0", "-pix_fmt",
                       "yuv420p" if args.oracle == "nearest-yuv" else "rgba", "-f", "rawvideo", str(decoded)]
            subprocess.run(command, capture_output=True, text=True, check=True, timeout=180)
            if args.oracle == "nearest-yuv":
                oracle = reference_yuv420(decoded.read_bytes(), width, height, entries[selected])
                reference.write_bytes(oracle.tobytes())
                entry.update(reference_yuv_sha256=sha256_file(decoded), oracle=args.oracle)
            if reference.stat().st_size != size:
                raise ValueError("reference RGBA is incomplete")
            native = np.frombuffer(raw.read_bytes(), dtype=np.uint8).reshape(height, width, 4)
            oracle = np.frombuffer(reference.read_bytes(), dtype=np.uint8).reshape(height, width, 4)
            comparison = compare_rgba(native, oracle)
            for label, pixels in (("native", native), ("reference", oracle)):
                Image.fromarray(pixels).save(directory / (label + ".png"))
            entry.update(width=width, height=height, native_pts=pts, reference_pts=reference_pts,
                         reference_frame_index=selected, raw_sha256=sha256_file(raw),
                         reference_sha256=sha256_file(reference), **comparison,
                         reference_color={key: value for key, value in entries[selected].items()
                                          if key.startswith("color_")},
                         directory=str(directory))
            if sha256_file(file) != entry["asset_sha256"]:
                raise ValueError("asset changed during comparison")
            if entry["changed_fraction"] > 0.001:
                raise ValueError("seek picture RGB differs from the independently decoded reference")
            entry["status"] = "PASS"
        except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
            entry["error"] = str(error)
        result["runs"].append(entry)
        (root / "suite.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(json.dumps({key: value for key, value in entry.items() if key != "process"}, default=str), flush=True)
    if all(row["status"] == "PASS" for row in result["runs"]):
        result["status"] = "PASS"
    output = root / "suite.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(output), "status": result["status"]}))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
