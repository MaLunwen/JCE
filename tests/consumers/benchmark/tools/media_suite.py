"""Reuse the native public-video playback/seek probe; no audible device by default."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import uuid

from culling_suite import sha256_file, source_identity
from tools.process_sampler import run_process

TIMELINE = re.compile(r"timeline wall=([\d.]+) media=([\d.]+) picture=([-\d.]+) audio=([\d.]+) displayed=(\d+) dropped=(\d+) queue=(\d+)")
SUMMARY = re.compile(r"probe (\w+) (\d+)x(\d+) ([\d.]+)fps elapsed=(\d+) ms media=([\d.]+) dec=(\d+) disp=(\d+) drop=(\d+) decode=([\d.]+) ms convert=([\d.]+) ms pop=([\d.]+) ms queue=(\d+)")
SEEK = re.compile(r"probe seek ([\d.]+)s call=(\d+) ms.*?probe seek frame=(\d+) wait=(\d+) ms pts=([\d.]+)", re.S)
INPUT = re.compile(r"probe input size=(\d+) backend_reads=(\d+) bytes_read=(\d+) owned=(\d+)")
RESUME = re.compile(r"resume wall=([\d.]+) media=([\d.]+) picture=([-\d.]+) audio=([\d.]+) frame=(\d+)")
RESUMED = re.compile(r"probe resumed seconds=(\d+) start=([\d.]+) end=([\d.]+)")
# The retained file source owns its 64 KiB cache plus bookkeeping, not zero
# bytes. This declared consumer budget detects whole-input copies in our
# multi-megabyte streaming fixtures without mislabelling that bounded cache.
SOURCE_OWNED_BUDGET = 128 * 1024


def validate_resume(log, seconds, target, landed, picture_budget):
    rows = [dict(zip(("wall", "media", "picture", "audio", "frame"), map(float, row)))
            for row in RESUME.findall(log)]
    summary = RESUMED.search(log)
    if not summary or int(summary[1]) != seconds or len(rows) < seconds * 10 or \
            rows[0]["wall"] > 0.1 or rows[-1]["wall"] < seconds - 0.1 or \
            abs(float(summary[2]) - target) > 0.00001 or float(summary[3]) < target + seconds - 0.3:
        raise ValueError("complete post-seek device-resume evidence missing")
    if not all(math.isfinite(value) for row in rows for value in row.values()) or any(
            after["wall"] <= before["wall"] or any(after[key] < before[key]
                for key in ("media", "picture", "audio", "frame"))
            for before, after in zip(rows, rows[1:])) or any(
                row["media"] < target - 0.002 or row["audio"] < target - 0.02 or
                row["picture"] < landed - 0.002 for row in rows):
        raise ValueError("post-seek transport or picture returned to an older position")
    picture = max(abs(row["media"] - row["picture"]) for row in rows)
    audio = max(abs(row["media"] - row["audio"]) for row in rows)
    wall = max(abs(row["media"] - target - row["wall"]) for row in rows)
    if picture > picture_budget or audio > 0.1 or wall > 0.25 or rows[-1]["frame"] <= rows[0]["frame"]:
        raise ValueError("post-seek picture/PCM/transport stopped or fell out of synchronization")
    return {"resume_timeline": rows, "resume_max_picture_lag_s": picture,
            "resume_max_audio_lag_s": audio, "resume_max_wall_lag_s": wall}


def validate(log, seconds, tick_ms, audio_device=False, resume_seconds=0):
    timeline = [dict(zip(("wall", "media", "picture", "audio", "displayed", "dropped", "queue"),
                        map(float, row))) for row in TIMELINE.findall(log)]
    summary, seek, source = SUMMARY.search(log), SEEK.search(log), INPUT.search(log)
    if not summary or not seek or not source or len(timeline) < seconds - 1 or \
            "probe paused preview frame=" not in log:
        raise ValueError("complete paused/playback/seek/streaming evidence missing")
    if not all(math.isfinite(value) for row in timeline for value in row.values()) or any(
            after["wall"] <= before["wall"] or after["media"] < before["media"] or
            after["picture"] < before["picture"] or after["displayed"] < before["displayed"]
            for before, after in zip(timeline, timeline[1:])):
        raise ValueError("non-finite or regressing playback timeline")
    if any(row["picture"] < 0 or row["queue"] < 0 for row in timeline):
        raise ValueError("missing published picture or invalid frame queue")
    fps = float(summary[4])
    target, landed = float(seek[1]), float(seek[5])
    seek_budget = 1 / fps + 0.002 if fps > 0 else 0
    if not fps > 0 or abs(landed - target) > seek_budget or int(source[4]) > SOURCE_OWNED_BUDGET:
        raise ValueError("seek picture or file-source ownership is incorrect")
    # Only pre-EOF samples measure sustained cadence. A ended clip holding its
    # last picture must not become a fabricated playback slowdown.
    duration = target * 2
    steady = [row for row in timeline if 2 <= row["wall"] < duration - 0.25]
    if len(steady) < 2:
        raise ValueError("clip too short for sustained playback evidence")
    picture_lag = max(abs(row["media"] - row["picture"]) for row in steady)
    clock_lag = max(abs(row["wall"] - row["media"]) for row in steady)
    lag_budget = max(0.1, 2 / fps + 2 * tick_ms / 1000)
    if picture_lag > lag_budget or clock_lag > 0.25:
        raise ValueError("sustained picture/transport clock fell behind wall time")
    audio_lag = max(abs(row["media"] - row["audio"]) for row in steady) if audio_device else None
    if audio_device and ("probe audio device=" not in log or steady[-1]["audio"] <= 0 or
                         audio_lag > 0.1):
        raise ValueError("real-device consumption clock differs from the video transport")
    resumed = validate_resume(log, resume_seconds, target, landed, lag_budget) if resume_seconds else {}
    return dict({"codec": summary[1], "width": int(summary[2]), "height": int(summary[3]), "fps": fps,
            "timeline": timeline, "max_picture_lag_s": picture_lag, "max_wall_lag_s": clock_lag,
            "picture_lag_budget_s": lag_budget, "seek_target_s": target, "seek_picture_s": landed,
            "seek_ready_ms": int(seek[4]), "seek_picture_budget_s": seek_budget,
            "decoded": int(summary[7]), "displayed": int(summary[8]),
            "dropped": int(summary[9]), "decode_ema_ms": float(summary[10]),
            "convert_ema_ms": float(summary[11]), "pop_ema_ms": float(summary[12]),
            "source_size": int(source[1]), "source_reads": int(source[2]),
            "source_bytes_read": int(source[3]), "source_owned_bytes": int(source[4]),
            "source_owned_budget_bytes": SOURCE_OWNED_BUDGET,
            "max_audio_consumption_lag_s": audio_lag}, **resumed)


def validate_seek_trace(log, target):
    records = []
    generations = set()
    keys = {"event", "generation", "request_pts", "exact", "keyframe_known",
            "keyframe_pts", "reused", "pictures", "preroll_pictures", "index_ms",
            "flush_ms", "decode_ms", "worker_elapsed_ms", "picture_pts"}
    for raw in re.findall(r"seek-trace ([^\r\n]+)", log):
        fields = re.sub(r"\x1b\[[0-9;]*m", "", raw)
        fields = re.sub(r" at [A-Za-z0-9_.-]+\.(?:c|cpp|h):[0-9]+$", "", fields)
        pairs = [part.split("=", 1) for part in fields.split()]
        if any(len(pair) != 2 for pair in pairs):
            raise ValueError("malformed seek trace")
        row = dict(pairs)
        if set(row) != keys or len(pairs) != len(keys):
            raise ValueError("incomplete or duplicate seek trace fields")
        event = row["event"]
        if event not in ("queue_ready", "cancel", "failed"):
            raise ValueError("unknown seek trace outcome")
        for key in keys - {"event", "request_pts", "keyframe_pts", "picture_pts"}:
            row[key] = int(row[key])
            if row[key] < 0:
                raise ValueError("negative seek trace integer")
        for key in ("request_pts", "keyframe_pts", "picture_pts"):
            row[key] = float(row[key])
            if not math.isfinite(row[key]):
                raise ValueError("nonfinite seek trace PTS")
        if row["generation"] in generations or row["generation"] == 0 or any(
                row[key] not in (0, 1) for key in ("exact", "keyframe_known", "reused")) or \
                row["preroll_pictures"] > row["pictures"]:
            raise ValueError("invalid generation or seek trace counters")
        generations.add(row["generation"])
        if row["keyframe_known"] and row["keyframe_pts"] > row["request_pts"] + 0.0005:
            raise ValueError("keyframe follows requested target")
        if event == "queue_ready" and (row["pictures"] == 0 and not row["reused"] or
                row["exact"] and row["picture_pts"] + 0.0005 < row["request_pts"]):
            raise ValueError("queue-ready trace has no current target picture")
        if not row["keyframe_known"]:
            row["keyframe_pts"] = None
        row["raw"] = raw
        records.append(row)
    if not any(row["event"] == "queue_ready" and row["exact"] and
               abs(row["request_pts"] - target) <= 0.00001 for row in records):
        raise ValueError("exact midpoint queue-ready seek trace missing")
    return {"scope": "worker queue-ready; not UI upload or PCM readiness",
            "records": records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True, help="built native public-video probe")
    parser.add_argument("--file", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--preview", type=int, default=0)
    parser.add_argument("--seek-trace", action="store_true", help="require native worker seek stage diagnostics")
    parser.add_argument("--audio-device", action="store_true", help="consume PCM through a muted real device")
    parser.add_argument("--resume-seconds", type=int, default=0, help="opt-in post-seek muted-device interval, 1..5 seconds")
    args = parser.parse_args()
    if not args.exe.is_file() or any(not path.is_file() for path in args.file) or \
            not 5 <= args.seconds <= 120 or not 1 <= args.repeats <= 7 or args.preview == 1 or args.preview < 0 or \
            not 0 <= args.resume_seconds <= 5 or (args.resume_seconds and not args.audio_device):
        parser.error("existing inputs and bounded seconds/repeats/preview required")
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True, exist_ok=False)
    identity = {"source": source_identity(), "executable_sha256": sha256_file(args.exe),
                "seconds": args.seconds, "preview": args.preview, "tick_ms": 16, "audio_device": args.audio_device,
                "resume_seconds": args.resume_seconds, "seek_trace_enabled": args.seek_trace,
                "scope": "native public-video decode/publication/transport/seek; " +
                         ("muted real-device PCM consumption" if args.audio_device else "no GPU upload or audio device"),
                "limitations": ["one sampled exact midpoint seek, not exhaustive scrub/seek latency",
                                "no GPU upload or editor preview",
                                "consumed PCM clock is not acoustic speaker presentation latency",
                                "without audio device, clips with embedded audio may not reach complete AV EOF",
                                "warm/cache-uncontrolled host inputs; assets are not redistributed"]}
    if not args.resume_seconds:
        identity["limitations"].append("no post-seek device-resume witness")
    base = {key: value for key, value in os.environ.items() if not key.startswith("JCE_")}
    bad = root / "invalid.mp4"
    bad.write_bytes(b"deliberately invalid MP4 negative control")
    control = run_process([str(args.exe.resolve())], env=dict(base, JCE_TEST_VIDEO_PROBE=str(bad)),
                          cwd=str(root), timeout=30, output_dir=root / "negative")
    result = dict(identity, status="FAILED", runs=[], negative=control)
    if control["returncode"] != 3 or control["timed_out"]:
        result["error"] = "invalid container negative control was not rejected by video load"
    else:
        for index in range(args.repeats):
            for file_index, file in enumerate(args.file):
                directory = root / ("%02u-%02u-%s" % (index, file_index, file.name))
                digest = sha256_file(file)
                env = dict(base, JCE_TEST_VIDEO_PROBE=str(file.resolve()),
                           JCE_TEST_VIDEO_SECONDS=str(args.seconds), JCE_TEST_VIDEO_PREVIEW_MAX=str(args.preview),
                           JCE_TEST_VIDEO_TICK_MS="16")
                if args.seek_trace:
                    env["JCE_VIDEO_SEEK_TRACE"] = "1"
                if args.resume_seconds:
                    env["JCE_TEST_VIDEO_RESUME_SECONDS"] = str(args.resume_seconds)
                if args.audio_device:
                    env.update(JCE_TEST_VIDEO_AUDIO_DEVICE="1", JCE_TEST_VIDEO_AUDIO_MUTE="1")
                entry = {"file": str(file.resolve()), "asset_sha256": digest, "directory": str(directory),
                         "index": index, "status": "FAILED", "environment": env}
                # Persist only explicit JCE controls, never inherited credentials.
                entry["environment"] = {key: value for key, value in env.items() if key.startswith("JCE_")}
                process = run_process([str(args.exe.resolve())], env=env, cwd=str(root),
                                      timeout=args.seconds + 45, output_dir=directory)
                entry["process"] = process
                try:
                    if process["returncode"] != 0 or process["timed_out"] or sha256_file(file) != digest or \
                            sha256_file(args.exe) != identity["executable_sha256"]:
                        raise ValueError("playback probe failed or measured input changed")
                    if args.seek_trace:
                        seek = SEEK.search(process["stdout"])
                        if not seek:
                            raise ValueError("public seek target evidence missing")
                        entry["seek_trace"] = validate_seek_trace(
                            process["stdout"] + "\n" + process["stderr"], float(seek[1]))
                    entry["observations"] = validate(process["stdout"], args.seconds, 16, args.audio_device, args.resume_seconds)
                    entry["status"] = "PASS"
                except ValueError as error:
                    entry["error"] = str(error)
                result["runs"].append(entry)
                print(json.dumps({"index": index, "file": str(file), "status": entry["status"], "error": entry.get("error")}), flush=True)
                (root / "suite.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
        if all(row["status"] == "PASS" for row in result["runs"]):
            result["status"] = "PASS"
    output = root / "suite.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(output), "status": result["status"]}))
    if result["status"] != "PASS":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
