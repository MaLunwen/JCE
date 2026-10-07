"""Empty SDK Player startup and steady frames; no cache eviction claim."""
import argparse
import configparser
import json
from pathlib import Path
import statistics
import uuid

from culling_suite import fixture, read_png, sha256_file, source_identity
from tools.benchmark_suite import frame_summary, host_render_environment, player_counters, read_frames
from tools.perf_bench import run, read_execution_log
from tools.jce_determinism import assert_backend


def validate_empty(log, counters):
    if "] ERROR -" in log or log.count("benchmark-player: initialized viewport=") != 1:
        raise ValueError("empty Player did not initialize cleanly")
    if counters[-1]["event"] != "exit" or any(
            row["engine"]["entity_count"] != 1 for row in counters) or any(
            row["model_gpu_driven"]["active"] for row in counters[1:]):
        raise ValueError("empty scene must contain only its camera and no GPU model batch")


def validate_trial_identity(identity, exe, scene, config):
    for label, path in (("executable", exe), ("scene", scene), ("profile", config)):
        if sha256_file(path) != identity[label + "_sha256"]:
            raise ValueError(label + " changed during the trial")



PROFILE_KEYS = {"renderer.vsync", "performance.machine_class", "performance.job_workers",
    "graphics.quality", "graphics.shadows", "graphics.ssao", "graphics.bloom",
    "graphics.fog", "graphics.msaa"}
CANONICAL_DEFAULTS = {"window.width": "1280", "window.height": "720", "window.fullscreen": "false",
    "window.resizable": "true", "window.title": "JCE", "renderer.backend": "auto",
    "renderer.debug_text": "true", "renderer.clear_color": "000000FF",
    "audio.master_volume": "1.000", "audio.music_volume": "0.800", "audio.sfx_volume": "1.000",
    "app.locale": "", "logging.level": "info", "logging.colors": "true", "graphics.shadow_quality": "1"}


def profile_values(path):
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.read_string(Path(path).read_text(encoding="utf-8"))
    if parser.defaults():
        raise ValueError("profile defaults unsupported")
    return {section+"."+key: value.strip() for section in parser.sections()
            for key, value in parser.items(section)}


def validate_runtime_profile(authored, runtime, backend):
    intended, actual = profile_values(authored), profile_values(runtime)
    if not set(intended) <= PROFILE_KEYS or not {"renderer.vsync", "performance.machine_class", "performance.job_workers"} <= set(intended):
        raise ValueError("unsupported or missing authored profile keys")
    for key, value in intended.items():
        if actual.get(key) != value:
            raise ValueError("intended profile control changed or missing: " + key)
    for key in set(actual)-set(intended):
        if key == "renderer.backend_resolved":
            if actual[key] != backend:
                raise ValueError("resolved backend differs")
        elif key not in CANONICAL_DEFAULTS or actual[key] != CANONICAL_DEFAULTS[key]:
            raise ValueError("unexpected runtime config mutation: " + key)
    if actual.get("renderer.backend_resolved") != backend:
        raise ValueError("resolved backend unavailable")
    return {"intended": intended, "canonical_effective": actual,
            "scope": "recognized config intent and canonical persistence; not independent GPU quality counters"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--backend", choices=("d3d11", "d3d12", "gl", "vulkan"), default="d3d11")
    parser.add_argument("--repeats", type=int, default=20)
    parser.add_argument("--steady-repeats", type=int, default=7)
    parser.add_argument("--profile", choices=("auto", "low"), default="auto")
    parser.add_argument("--vsync", choices=("on", "off"), default="on")
    args = parser.parse_args()
    if not args.exe.is_file() or not 1 <= args.repeats <= 50 or not 1 <= args.steady_repeats <= 20:
        parser.error("existing binary and bounded positive repetitions required")
    root = args.out.resolve() / uuid.uuid4().hex[:12]
    root.mkdir(parents=True, exist_ok=False)
    scene = root / "scene.json"
    document = fixture(1)
    document["entities"] = document["entities"][:1]
    scene.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8", newline="\n")
    config = root / "profile.ini"
    config.write_text("[renderer]\nvsync=%s\n\n" % ("true" if args.vsync == "on" else "false") +
        "[performance]\nmachine_class=%s\njob_workers=%u\n" %
        ("low" if args.profile == "low" else "auto", 1 if args.profile == "low" else 0) +
        ("\n[graphics]\nquality=low\nshadows=false\nssao=false\nbloom=false\nfog=false\nmsaa=1\n"
         if args.profile == "low" else ""), encoding="utf-8", newline="\n")
    identity = {"source": source_identity(), "executable_sha256": sha256_file(args.exe),
                "scene_sha256": sha256_file(scene), "backend": args.backend,
                "package_bytes": args.exe.stat().st_size,
                "profile": args.profile, "vsync": args.vsync, "profile_sha256": sha256_file(config),
                "package_scope": "single SDK-consumer executable; no symbols or external assets"}
    startup, steady = [], []
    for phase, repetitions in (("startup", args.repeats), ("steady", args.steady_repeats)):
        for index in range(repetitions):
            if sha256_file(args.exe) != identity["executable_sha256"]:
                raise ValueError("executable changed during the matrix")
            target = root / ("%s-%02u" % (phase, index))
            target.mkdir()
            trial_config = target / "profile.ini"
            trial_config.write_bytes(config.read_bytes())
            (target / "profile.before.ini").write_bytes(config.read_bytes())
            env = {"JCE_SCENE": str(scene), "JCE_BACKEND": args.backend,
                   "JCE_BENCH_CONFIG_FILE": str(trial_config),
                   "JCE_GPU_SCENE": "0", "JCE_BENCH_ASSET_STILL": "1",
                   **host_render_environment("player", target, witness=phase == "startup")}
            markers = None
            if phase == "startup":
                env.update(JCE_BENCH_SHOT_FRAME="1", JCE_BENCH_TEXT_CAPTURE="1")
                markers = {"application_ready": (target / "engine_counters.jsonl", b'"event":"initialized"'),
                           "first_gpu_readback": (target / "frame.png", b'\x00\x00\x00\x00IEND\xaeB`\x82')}
            intent = dict(identity, phase=phase, index=index, environment=env,
                          warmup_intervals=120, measured_intervals=600 if phase == "steady" else 60)
            (target / "intent.json").write_text(json.dumps(intent, indent=2) + "\n",
                                                encoding="utf-8", newline="\n")
            process = run(env, target / "engine.log", frames=721 if phase == "steady" else 181,
                          exe=args.exe.resolve(), samples_path=target / "frames.csv", live_markers=markers,
                          sample_interval=0.02 if phase == "startup" else 0.1)
            validate_trial_identity(identity, args.exe, scene, config)
            validate_trial_identity(identity, args.exe, scene, target / "profile.before.ini")
            effective = validate_runtime_profile(config, trial_config, args.backend)
            log = read_execution_log(target / "engine.log", process["process"])
            assert_backend(log, args.backend)
            if "unknown config key:" in log:
                raise ValueError("profile contains ignored controls")
            worker_marker = ("shared job pool: auto" if args.profile == "auto" else "shared job pool: 1 worker(s) (pinned via [performance] job_workers)")
            if worker_marker not in log:
                raise ValueError("worker config application unavailable")
            if args.profile == "low" and "applied [graphics] overrides from user config (quality=0)" not in log:
                raise ValueError("low graphics config application unavailable")
            counters = player_counters((target / "engine_counters.jsonl").read_text(encoding="utf-8"))
            validate_empty(log, counters)
            entry = {"status": "PASS", "index": index, "directory": str(target), "process": process,
                     "mode": "empty",
                     "profile_before_sha256": sha256_file(target / "profile.before.ini"),
                     "profile_after_sha256": sha256_file(trial_config),
                     "effective_profile": effective,
                     "resources": counters, "raw_sha256": sha256_file(target / "frames.csv")}
            if phase == "startup":
                if any(value is None for value in process["process"]["live_markers_s"].values()):
                    raise ValueError("live readiness/readback marker unavailable; shutdown is not startup")
                image = read_png(target / "frame.png")
                if image[:2] != (1280, 720) or len(image[3]) != 1280 * 720 * image[2]:
                    raise ValueError("first GPU readback is not a complete viewport")
                entry["image_sha256"] = sha256_file(target / "frame.png")
                startup.append(entry)
            else:
                entry["frame"] = frame_summary(read_frames(target / "frames.csv", warmup=120, sample_count=600))
                steady.append(entry)
            print(json.dumps({"phase": phase, "index": index, "status": "PASS"}), flush=True)
    result = dict(identity, status="PASS", startup=startup, runs=steady, count=0,
        comparison_name="Empty SDK Player", scope="camera-only 1280x720 rendered consumer; complete default engine lifecycle",
        median_frame_ms=statistics.median(row["frame"]["frame_median_ms"] for row in steady),
        limitations=["first run is cache-uncontrolled, not a guaranteed cold boot",
                     "low profile limits pools/workers on this host; not physical legacy-device validation",
                     "startup includes first-frame PNG diagnostics; steady frames exclude readback",
                     "GPU readback completion is not onscreen presentation latency",
                     "live marker latency is bounded by the recorded polling interval",
                     "total elapsed startup trials include 181 frames and clean shutdown"])
    output = root / "suite.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(output), "median_frame_ms": result["median_frame_ms"]}))


if __name__ == "__main__":
    main()
