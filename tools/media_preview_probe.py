"""Measure an actual editor media preview, preserving user settings bytes.

Uses the explicit asset-open route, real wall time and a real hidden GPU window.
No build, fixed timestep, backend fallback, or user media redistribution.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time

from jce_determinism import (DETERMINISM, actual_backend, assert_backend,
                             assert_has_content, read_png)

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("media", type=Path)
    ap.add_argument("--editor", type=Path,
                    default=ROOT / "build/desktop/windows-x64/release/jce_editor.exe")
    ap.add_argument("--scene", type=Path)
    ap.add_argument("--backend")
    ap.add_argument("--frames", type=int, default=3800)
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--capture-frame", type=int)
    ap.add_argument("--expected-frames", type=int,
                    help="known full-clip picture count for EOF regression checks")
    ap.add_argument("--expected-final-pts", type=float,
                    help="known final video PTS when audio/container duration differs")
    ap.add_argument("--seeks", type=float, nargs="+",
                    help="seek targets driven through real viewer scrub/release")
    ap.add_argument("--max-seek-wait-ms", type=float,
                    help="optional device-specific seek latency budget")
    ap.add_argument("--paused", action="store_true",
                    help="open without autoplay to measure idle waveform work")
    ap.add_argument("--verify-audio", action="store_true",
                    help="require waveform publication and responsive editor frames")
    ap.add_argument("--verify-playback", action="store_true",
                    help="fail slow playback, A/V drift or long presentation gaps")
    ap.add_argument("--require-eof", action="store_true",
                    help="require a completed clip with its final picture")
    args = ap.parse_args()
    media, editor = args.media.resolve(), args.editor.resolve()
    if not media.is_file() or not editor.is_file():
        ap.error("media and editor must be existing files")
    if args.frames < 31 or args.timeout <= 0:
        ap.error("frames must exceed the open frame (30); timeout must be positive")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    folders = {Path.home() / ".jce", ROOT / ".jce", editor.parent / ".jce"}
    if args.scene:
        scene = args.scene.resolve(strict=True)
        folders.update(p / ".jce" for p in scene.parents if p != p.parent)
    saved = {d: {p: p.read_bytes() for p in d.iterdir() if p.is_file()}
             if d.exists() else {} for d in folders}
    env = os.environ.copy()
    env.update(DETERMINISM)
    # Media cadence needs wall-clock time and actual asynchronous workers.
    # Keep shared capture/window pins, deliberately override fixed-step pins.
    for key in ("JCE_STREAM_SYNC", "JCE_DBG_FILE_PREVIEW_PAUSED", "JCE_FRAME_DT_FIXED", "JCE_DBG_AUTOPLAY", "JCE_SHOT_FRAME",
                "JCE_WINCAP_FRAME", "JCE_WINCAP_PATH"):
        env.pop(key, None)
    env.update(JCE_DBG_FILE_PREVIEW=str(media), JCE_VIDEO_TRACE="1",
               JCE_AUDIO_TRACE="1", JCE_WINDOW_HIDDEN="1", JCE_MULTI_INSTANCE="1",
               JCE_MAX_FRAMES=str(args.frames))
    if args.seeks:
        env["JCE_DBG_FILE_PREVIEW_SEEKS"] = ",".join(map(str, args.seeks))
    else:
        env.pop("JCE_DBG_FILE_PREVIEW_SEEKS", None)
    if args.paused:
        env["JCE_DBG_FILE_PREVIEW_PAUSED"] = "1"
    if args.scene:
        env["JCE_SCENE"] = str(scene)
    if args.backend:
        env["JCE_BACKEND"] = args.backend
    if args.capture_frame is not None:
        env.update(JCE_WINCAP_FRAME=str(args.capture_frame),
                   JCE_WINCAP_PATH=str(output.with_suffix(".png")))
    timeout = False
    start = time.monotonic()
    try:
        with output.open("wb") as log:
            process = subprocess.Popen([str(editor)], cwd=editor.parent, env=env,
                                       stdout=log, stderr=subprocess.STDOUT)
            try:
                code = process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                timeout = True
                process.terminate()
                code = process.wait(timeout=10)
    finally:
        for folder, files in saved.items():
            if folder.exists():
                for path in folder.iterdir():
                    if path.is_file() and path not in files:
                        path.unlink()
            for path, data in files.items():
                path.write_bytes(data)
            assert all(p.read_bytes() == b for p, b in files.items())
    text = output.read_text(encoding="utf-8", errors="replace")
    backend = actual_backend(text)
    if args.backend:
        assert_backend(text, args.backend)
    if args.capture_frame is not None:
        assert_has_content(read_png(output.with_suffix(".png")), str(output))
    rows = []
    audio_rows = []
    for line in text.splitlines():
        if "viewer trace wall=" in line:
            rows.append({key: float(value) for key, value in
                         re.findall(r"(\w+)=(-?\d+(?:\.\d+)?)", line)})
        if "audio trace playing=" in line:
            audio_rows.append({key: float(value) for key, value in
                               re.findall(r"(\w+)=(-?\d+(?:\.\d+)?)", line)})
    failures = []
    seek_rows = {kind: [{key: float(value) for key, value in
                        re.findall(r"(\w+)=(-?\d+(?:\.\d+)?)", line)}
                       for line in text.splitlines() if f"seek {kind} target=" in line]
                 for kind in ("requested", "waiting", "ready")}
    if args.seeks:
        if len(seek_rows["requested"]) != len(args.seeks) or len(seek_rows["ready"]) != len(args.seeks):
            failures.append("seek sequence did not fully resume")
        for row in seek_rows["waiting"] + seek_rows["ready"]:
            if abs(row["media"] - row["target"]) > .00001 or row["voice"]:
                failures.append("seek advanced the clock or audio before readiness")
                break
        if args.max_seek_wait_ms is not None and any(
                row["wait_ms"] > args.max_seek_wait_ms for row in seek_rows["ready"]):
            failures.append("seek exceeded the device-specific latency budget")
        playing = [r for r in rows if r.get("playing")]
        if any(abs(r["pts"] - r["media"]) > .10 for r in playing):
            failures.append("picture lagged after seek resume")
        for row in seek_rows["ready"]:
            if abs(row["pts"] - row["target"]) > .05 or abs(row["audio"] - row["target"]) > .05:
                failures.append("seek resumed with mismatched audio/picture time")
                break
    if args.verify_audio:
        if len(audio_rows) < 4 or not any(r["bins"] > 0 for r in audio_rows):
            failures.append("audio waveform did not publish")
        else:
            steady = audio_rows[2:]
            frame_ms = sorted(r["frame_ms"] for r in steady)
            if frame_ms[len(frame_ms) // 2] > 20 or max(frame_ms) > 50:
                failures.append("audio preview causes slow editor frames")
            if args.paused and any(r["playing"] or r["time"] > .01 for r in audio_rows):
                failures.append("paused audio advanced or played")
    match = re.search(r"loaded " + re.escape(media.name)
                      + r" \d+x\d+ ([0-9.]+)fps .*?dur=([0-9.]+)s", text)
    # A capped transport waiting for tail drain is no longer steady playback.
    # Unequal audio/video tails need the independent video PTS oracle.
    tail = float(match.group(2)) if match else float("inf")
    if args.expected_final_pts is not None and match:
        tail = min(tail, args.expected_final_pts + 1 / float(match.group(1)))
    active = [r for r in rows if r.get("playing") == 1 and r.get("wall", 0) >= 1
              and r.get("media", 0) < tail - .02]
    if args.verify_playback or args.require_eof:
        if not match or len(active) < min(5, max(2, int(float(match.group(2))) - 1)):
            failures.append("insufficient actual playback samples")
        else:
            fps, duration = map(float, match.groups())
            first, last = active[0], active[-1]
            interval = last["media"] - first["media"]
            cadence = (last["disp"] - first["disp"]) / interval if interval > 0 else 0
            wall_interval = last["wall"] - first["wall"]
            speed = interval / wall_interval if wall_interval > 0 else 0
            if not .97 <= speed <= 1.03:
                failures.append(f"media/wall-clock rate {speed:.3f}")
            if cadence < fps * 0.90:
                failures.append(f"presentation cadence {cadence:.2f} below {fps * .90:.2f}")
            drift = max(abs(r["media"] - r["pts"]) for r in active)
            if drift > max(.10, 2 / fps):
                failures.append(f"picture/clock drift {drift:.3f}s")
            if last["max_gap_ms"] > 100:
                failures.append(f"presentation gap {last['max_gap_ms']:.0f}ms")
            if args.require_eof:
                ended = [r for r in rows if not r.get("playing")
                         and r.get("media", 0) >= duration - .02]
                if not ended:
                    failures.append("clip did not reach EOF")
                elif args.expected_final_pts is not None:
                    if abs(ended[-1]["pts"] - args.expected_final_pts) > .5 / fps + .001:
                        failures.append("final picture PTS differs from fixture")
                elif ended[-1]["pts"] < duration - 1.5 / fps - .01:
                    failures.append("delayed final pictures missing at EOF")
                if ended and args.expected_frames is not None:
                    last = ended[-1]
                    if (last["dec"] != args.expected_frames or
                            last["disp"] + last["drop"] != args.expected_frames):
                        failures.append("decoded/presented picture count differs from fixture")
    result = {"media": str(media), "bytes": media.stat().st_size,
              "exit_code": code, "timeout": timeout, "backend": backend,
              "wall_seconds": time.monotonic() - start, "settings_restored": True,
              "video_trace": rows, "audio_trace": audio_rows, "seek_trace": seek_rows,
              "validation_failures": failures, "log": str(output)}
    output.with_suffix(".json").write_text(json.dumps(result, indent=2) + "\n",
                                          encoding="utf-8", newline="\n")
    print(json.dumps({k: v for k, v in result.items() if k not in {"video_trace", "audio_trace", "seek_trace"}}))
    print("video snapshots:", len(rows), "last:", rows[-1] if rows else None)
    if audio_rows:
        print("audio snapshots:", len(audio_rows), "last:", audio_rows[-1])
    if failures:
        print("FAIL:", "; ".join(failures))
    if code or timeout or not backend or failures:
        return 1
    if media.suffix.lower() in {".mp4", ".webm", ".mkv", ".ivf"} and not rows:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
