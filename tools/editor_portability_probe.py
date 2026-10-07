"""Relocated standalone Windows editor correctness witness, not a timing test."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import uuid

from benchmark_suite import read_frames
from jce_determinism import assert_has_content, read_png
from process_sampler import run_process

_spec = importlib.util.spec_from_file_location(
    "jce_portability_standalone", Path(__file__).parent / "build/standalone.py")
_standalone = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_standalone)


def sha256(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            value.update(block)
    return value.hexdigest()


def validate_process(process):
    if process["timed_out"] or process["returncode"] != 0:
        raise ValueError("relocated editor did not complete normally")


def validate_image(image, path):
    width, height, channels, pixels = image
    if width < 320 or height < 240 or channels not in (3, 4) or len(pixels) != width * height * channels:
        raise ValueError("incomplete editor window capture")
    assert_has_content(image, path, y0=0, y1=height, x0=0, x1=width)
    return [width, height]


def frame_count(value):
    try:
        count = int(value)
    except (TypeError, ValueError) as error:
        raise argparse.ArgumentTypeError("frames must be an integer") from error
    if isinstance(value, bool) or str(count) != str(value) or not 240 <= count <= 100000:
        raise argparse.ArgumentTypeError("frames must be an integer in 240..100000")
    return count


def validate_frames(path, expected=240):
    expected = frame_count(expected)
    frames = read_frames(path, warmup=0, sample_count=None)
    if list(frames["frame_indices"]) != list(range(expected)):
        raise ValueError(f"relocated editor must record exactly frames 0 through {expected - 1}")
    return expected


def probe_environment(scene, root, frames, low_memory):
    frames = frame_count(frames)
    env = {key: value for key, value in os.environ.items() if not key.startswith("JCE_")}
    env.update(JCE_SCENE=str(scene), JCE_WINDOW_HIDDEN="1", JCE_MULTI_INSTANCE="1",
               JCE_MAX_FRAMES=str(frames), JCE_WINCAP_FRAME="120", JCE_WINCAP_PATH=str(root / "window.png"),
               JCE_KPI_FRAME_LOG=str(root / "frames.csv"), JCE_EDITOR_PROFILE_DIR=str(root / "profile"),
               JCE_LOG_FILE=str(root / "engine.log"))
    if low_memory:
        env["JCE_LOW_MEM"] = "1"
    return env


def probe(exe, scene, out, timeout=180, frames=240, low_memory=False):
    requested_frames = frame_count(frames)
    exe, scene = Path(exe).resolve(), Path(scene).resolve()
    imports = _standalone.verify(exe)
    identity = {"exe": sha256(exe), "scene": sha256(scene)}
    root = Path(out).resolve() / uuid.uuid4().hex
    root.mkdir(parents=True, exist_ok=False)
    relocated = root / "relocated_editor.exe"
    shutil.copyfile(exe, relocated)
    if sha256(relocated) != identity["exe"]:
        raise ValueError("relocated executable copy differs")
    capture, frames = root / "window.png", root / "frames.csv"
    env = probe_environment(scene, root, requested_frames, low_memory)
    result = {"status": "FAIL", "identity": identity, "imports": imports,
              "requested_frames": requested_frames, "low_memory": bool(low_memory),
              "scope": "PE system imports and relocated editor window/CSV correctness only",
              "limitations": ["backend actual unavailable with compiled-out INFO logs",
                              "not startup or throughput timing, nor all driver-language validation",
                              "low-memory mode alone does not qualify the 512MiB/legacy-device target"],
              "environment": {key: value for key, value in env.items() if key.startswith("JCE_")}}
    report = root / "probe.json"
    try:
        process = run_process([str(relocated)], env=env, cwd=str(root), timeout=timeout,
                              output_dir=root / "process", marker_path=frames)
        result["process"] = process
        validate_process(process)
        if sha256(exe) != identity["exe"] or sha256(relocated) != identity["exe"] or sha256(scene) != identity["scene"]:
            raise ValueError("executable or scene changed during the witness")
        result["window_size"] = validate_image(read_png(capture), capture)
        result["complete_frame_records"] = validate_frames(frames, requested_frames)
        result["artifacts"] = {name: sha256(path) for name, path in
                               (("window", capture), ("frames", frames),
                                ("process_result", root / "process/result.json"),
                                ("process_samples", root / "process/samples.json"),
                                ("stdout", root / "process/stdout.log"),
                                ("stderr", root / "process/stderr.log"))}
        result["status"] = "PASS"
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("exe", "scene", "out"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--frames", type=frame_count, default=240)
    parser.add_argument("--low-memory", action="store_true", help="explicitly request JCE_LOW_MEM=1")
    args = parser.parse_args()
    print(probe(args.exe, args.scene, args.out, args.timeout, args.frames, args.low_memory))


if __name__ == "__main__":
    main()
